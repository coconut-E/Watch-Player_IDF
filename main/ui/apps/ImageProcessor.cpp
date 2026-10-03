#include "ImageProcessor.h"
#include <esp_heap_caps.h>

#ifndef MAX
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#endif

#ifndef MIN
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#endif

#ifndef CLAMP
#define CLAMP(v, min, max) ((v) < (min) ? (min) : ((v) > (max) ? (max) : (v)))
#endif

// 静态成员初始化
ImageProcessor* ImageProcessor::instance = nullptr;

ImageProcessor::ImageProcessor() 
    : rawImage(nullptr), scaledImage(nullptr), rawWidth(0), rawHeight(0), 
      scaledWidth(0), scaledHeight(0), progressCallback(nullptr), errorCallback(nullptr), 
      lastOriginalWidth(0), lastOriginalHeight(0), targetWidth(240), targetHeight(280),
      lastDecodeDestY(-1)
{
    strcpy(lastImageFormat, "N/A");
    instance = this;
}

ImageProcessor::~ImageProcessor() {
    cleanup();
    if (instance == this) {
        instance = nullptr;
    }
}

void ImageProcessor::cleanup() {
    if (rawImage) {
        free(rawImage);
        rawImage = nullptr;
    }
    if (scaledImage) {
        free(scaledImage);
        scaledImage = nullptr;
    }
    rawWidth = 0;
    rawHeight = 0;
    scaledWidth = 0;
    scaledHeight = 0;
}

size_t ImageProcessor::getFreePSRAM() {
    return ESP.getFreePsram();
}

bool ImageProcessor::checkPSRAM(size_t requiredSize, const char* operation) {
    // 获取最大连续可用块大小
    size_t largestBlock = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);
    // 额外预留10%安全空间
    size_t safeRequired = requiredSize + (requiredSize / 10); 
    
    if (largestBlock < safeRequired) {
        Serial.printf("连续内存检查失败[%s]: 需要%u字节, 最大连续可用%u字节\n", operation, safeRequired, largestBlock);
        return false;
    }
    return true;
}

void ImageProcessor::setProgressCallback(ProgressCallback callback) {
    progressCallback = callback;
}

void ImageProcessor::setErrorCallback(ErrorCallback callback) {
    errorCallback = callback;
}

void ImageProcessor::reportProgress(int percent) {
    if (progressCallback) {
        progressCallback(percent);
    } else {
        Serial.printf("[进度] %d%%\n", percent);
    }
}

void ImageProcessor::reportError(ErrorCode error, const char* message) {
    if (errorCallback) {
        errorCallback(error, message);
    } else {
        Serial.printf("[错误] 代码: %d, 消息: %s\n", error, message);
    }
}

void ImageProcessor::getImageInfo(const char** format, int* originalWidth, int* originalHeight) {
    if (format) *format = lastImageFormat;
    if (originalWidth) *originalWidth = lastOriginalWidth;
    if (originalHeight) *originalHeight = lastOriginalHeight;
}

// PNG解码回调静态函数
void ImageProcessor::staticPngDrawCallback(PNGDRAW *pDraw) {
    if (instance) {
        instance->pngDrawCallback(pDraw);
    }
}

// PNG解码回调成员函数
void ImageProcessor::pngDrawCallback(PNGDRAW *pDraw) {
    if (rawImage == nullptr || pDraw->y >= rawHeight) return;

    // 从 pUser 中提取外部申请好的缓冲区指针
    uint16_t* usPixels = (uint16_t*)pDraw->pUser;
    if (!usPixels) return;

    // 计算当前行对应的目标行索引 (近邻缩放丢弃)
    int destY = (pDraw->y * scaledHeight) / rawHeight;

    // 如果与上一行目标索引相同，说明该行需要被丢弃
    if (destY == lastDecodeDestY) {
        return;
    }
    lastDecodeDestY = destY;

    // PNG解码进度占总进度的80%
    if (pDraw->y % (rawHeight / 10) == 0) {
        int progress = 20 + (pDraw->y * 60) / rawHeight;
        reportProgress(progress);
    }

    // 直接使用传进来的 usPixels，不再动态申请和释放
    png.getLineAsRGB565(pDraw, usPixels, PNG_RGB565_LITTLE_ENDIAN, 0xffffffff);

    // 近邻缩放：将 rawWidth 压缩到目标宽度 scaledWidth
    uint16_t *pDest = rawImage + (destY * scaledWidth);
    for (uint32_t x = 0; x < scaledWidth; x++) {
        uint32_t srcX = (x * rawWidth) / scaledWidth;
        pDest[x] = usPixels[srcX];
    }
}


// JPEG解码回调静态函数
int ImageProcessor::staticJpegDecodeCallback(JPEGDRAW *pDraw) {
    if (instance) {
        return instance->jpegDecodeCallback(pDraw);
    }
    return 0;
}

// JPEG解码回调成员函数
int ImageProcessor::jpegDecodeCallback(JPEGDRAW *pDraw) {
    if (rawImage == nullptr) return 0;
    
    //JPEG解码进度占总进度的80%
    static int lastReportedProgress = 0;
    int progress = 20 + (pDraw->y * 60) / rawHeight; 
    if (progress != lastReportedProgress) {
        reportProgress(progress);
        lastReportedProgress = progress;
    }

    // 将解码数据复制到原始图像缓冲区
    uint16_t* dest = rawImage + pDraw->y * rawWidth + pDraw->x;
    uint16_t* src = pDraw->pPixels;
    for (int y = 0; y < pDraw->iHeight; y++) {
        memcpy(dest, src, pDraw->iWidth * sizeof(uint16_t));
        dest += rawWidth;
        src += pDraw->iWidth;
    }
    return 1;
}

// 加载文件到缓冲区
ImageProcessor::ErrorCode ImageProcessor::loadFileToBuffer(const char* imagePath, uint8_t** buffer, size_t* fileSize) {
    FsFile file;
    if (!file.open(imagePath, O_RDONLY)) {
        reportError(ERROR_FILE_NOT_FOUND, "无法打开文件");
        return ERROR_FILE_NOT_FOUND;
    }

    *fileSize = file.size();
    //Serial.printf("读取文件大小: %u 字节\n", *fileSize);

    // 检查PSRAM空间
    if (!checkPSRAM(*fileSize, "文件缓冲区")) {
        reportError(ERROR_MEMORY_ALLOCATION, "PSRAM连续空间不足");
        file.close();
        return ERROR_MEMORY_ALLOCATION;
    }

    // 分配内存
    *buffer = (uint8_t*)ps_malloc(*fileSize);
    if (!*buffer) {
        reportError(ERROR_MEMORY_ALLOCATION, "无法分配文件缓冲区");
        file.close();
        return ERROR_MEMORY_ALLOCATION;
    }

    // 分块读取，每块 32KB
    size_t bytesRead = 0;
    size_t totalBytes = *fileSize;
    size_t chunkSize = 32 * 1024; 
    uint8_t* ptr = *buffer;
    while (bytesRead < totalBytes) {
        size_t toRead = (totalBytes - bytesRead) > chunkSize ? chunkSize : (totalBytes - bytesRead);
        if (file.read(ptr, toRead) != toRead) {
            reportError(ERROR_FILE_NOT_FOUND, "读取文件过程中断");
            free(*buffer);
            *buffer = nullptr;
            file.close();
            return ERROR_FILE_NOT_FOUND;
        }
        ptr += toRead;
        bytesRead += toRead;
        int readProgress = (int)((bytesRead * 20) / totalBytes);
        reportProgress(readProgress);
        vTaskDelay(10);
    }
    file.close();
    Serial.printf("文件加载成功，已分配 %u 字节内存          ", *fileSize);
    return SUCCESS;
}

// 旋转图像90度
uint16_t* ImageProcessor::rotateImage90(uint16_t* src, int srcWidth, int srcHeight) {
    int rotatedWidth = srcHeight;
    int rotatedHeight = srcWidth;
    size_t requiredMemory = rotatedWidth * rotatedHeight * sizeof(uint16_t);
    
    if (!checkPSRAM(requiredMemory, "旋转图像")) {
        reportError(ERROR_MEMORY_ALLOCATION, "PSRAM空间不足，无法分配旋转图像内存");
        return nullptr;
    }

    uint16_t* rotatedImage = (uint16_t*)ps_malloc(requiredMemory);
    if (!rotatedImage) {
        reportError(ERROR_MEMORY_ALLOCATION, "无法分配旋转图像内存");
        return nullptr;
    }

    // 执行90度旋转
    for (int y = 0; y < srcHeight; y++) {
        for (int x = 0; x < srcWidth; x++) {
            int newX = srcHeight - 1 - y;
            int newY = x;
            rotatedImage[newY * rotatedWidth + newX] = src[y * srcWidth + x];
        }
    }
    return rotatedImage;
}

// RGB565 颜色混合，mix 范围 0~256（0=全c2，256=全c1）
static inline uint16_t rgb565_mix_256(uint16_t c1, uint16_t c2, uint16_t mix) {
    uint32_t r1 = (c1 >> 11) & 0x1F;
    uint32_t g1 = (c1 >> 5) & 0x3F;
    uint32_t b1 = c1 & 0x1F;
    uint32_t r2 = (c2 >> 11) & 0x1F;
    uint32_t g2 = (c2 >> 5) & 0x3F;
    uint32_t b2 = c2 & 0x1F;
    uint32_t r = (r1 * mix + r2 * (256 - mix)) >> 8;
    uint32_t g = (g1 * mix + g2 * (256 - mix)) >> 8;
    uint32_t b = (b1 * mix + b2 * (256 - mix)) >> 8;
    return (r << 11) | (g << 5) | b;
}

static inline uint16_t rgb565_bilinear(uint16_t c00, uint16_t c01, uint16_t c10, uint16_t c11, uint16_t x_frac, uint16_t y_frac) {
    uint16_t row0 = rgb565_mix_256(c01, c00, x_frac);
    uint16_t row1 = rgb565_mix_256(c11, c10, x_frac);
    return rgb565_mix_256(row1, row0, y_frac);
}

// 区域平均采样：计算源图像矩形区域内所有像素的平均值
static inline uint16_t rgb565_area_average(const uint16_t* src, int srcWidth, int x0, int x1, int y0, int y1) {
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 >= srcWidth) x1 = srcWidth - 1; 

    uint32_t r_sum = 0, g_sum = 0, b_sum = 0;
    int count = 0;
    for (int y = y0; y <= y1; y++) {
        for (int x = x0; x <= x1; x++) {
            uint16_t p = src[y * srcWidth + x];
            r_sum += (p >> 11) & 0x1F;
            g_sum += (p >> 5) & 0x3F;
            b_sum += p & 0x1F;
            count++;
        }
    }
    if (count == 0) return 0;
    uint32_t r = (r_sum + count / 2) / count;
    uint32_t g = (g_sum + count / 2) / count;
    uint32_t b = (b_sum + count / 2) / count;
    return (r << 11) | (g << 5) | b;
}

// 缩放和裁剪图像，自动判断横竖屏处理
bool ImageProcessor::scaleAndCropImage(uint16_t* src, int srcWidth, int srcHeight) {
    // 自动判断是否为横屏图像
    bool isLandscape = (srcWidth > srcHeight); 

    // 目标尺寸始终为240x280
    scaledWidth = targetWidth;  // 240
    scaledHeight = targetHeight; // 280

    // 计算裁剪区域
    int cropWidth, cropHeight, cropX, cropY;
    if (isLandscape) {
        // 横屏图像：先裁剪到280x240（保持6:7比例）
        float targetAspect = 7.0f / 6.0f; 
        float srcAspect = (float)srcWidth / srcHeight;
        if (srcAspect > targetAspect) {
            cropHeight = srcHeight;
            cropWidth = (int)(srcHeight * targetAspect);
        } else {
            cropWidth = srcWidth;
            cropHeight = (int)(srcWidth / targetAspect);
        }
        cropX = (srcWidth - cropWidth) / 2;
        cropY = (srcHeight - cropHeight) / 2;
    } else {
        // 竖屏图像：直接裁剪到240x280
        float targetAspect = 6.0f / 7.0f; 
        float srcAspect = (float)srcWidth / srcHeight;
        if (srcAspect > targetAspect) {
            cropHeight = srcHeight;
            cropWidth = (int)(srcHeight * targetAspect);
        } else {
            cropWidth = srcWidth;
            cropHeight = (int)(srcWidth / targetAspect);
        }
        cropX = (srcWidth - cropWidth) / 2;
        cropY = (srcHeight - cropHeight) / 2;
    }

    // 确保裁剪区域在图片范围内
    if (cropX < 0) cropX = 0;
    if (cropY < 0) cropY = 0;
    if (cropX + cropWidth > srcWidth) cropWidth = srcWidth - cropX;
    if (cropY + cropHeight > srcHeight) cropHeight = srcHeight - cropY;

    // 分配中间图像内存（竖屏直接240x280，横屏先280x240）
    int intermediateWidth = isLandscape ? 280 : 240;
    int intermediateHeight = isLandscape ? 240 : 280;
    size_t intermediateMemory = intermediateWidth * intermediateHeight * sizeof(uint16_t);
    
    if (!checkPSRAM(intermediateMemory, "中间图像")) {
        reportError(ERROR_MEMORY_ALLOCATION, "PSRAM空间不足，无法分配中间图像内存");
        return false;
    }

    uint16_t* intermediateImage = (uint16_t*)ps_malloc(intermediateMemory);
    if (!intermediateImage) {
        reportError(ERROR_MEMORY_ALLOCATION, "无法分配中间图像内存");
        return false;
    }

    // 计算缩放比例
    float scaleX = (float)cropWidth / intermediateWidth;
    float scaleY = (float)cropHeight / intermediateHeight;
    bool isDownscale = (scaleX < 1.0f) && (scaleY < 1.0f);

    if (isDownscale) {
        // ---------- 区域平均采样----------
        const float fx = (float)cropWidth / intermediateWidth;
        const float fy = (float)cropHeight / intermediateHeight;
        for (int y = 0; y < intermediateHeight; y++) {
            float y_center = cropY + (y + 0.5f) * fy;
            float y_half = fy / 2.0f;
            int y_start = (int)floor(y_center - y_half);
            int y_end = (int)ceil(y_center + y_half);
            y_start = MAX(y_start, cropY);
            y_end = MIN(y_end, cropY + cropHeight);
            y_start = MIN(y_start, srcHeight - 1);
            y_end = MIN(y_end, srcHeight);
            y_end = MAX(y_end, y_start + 1);

            for (int x = 0; x < intermediateWidth; x++) {
                float x_center = cropX + (x + 0.5f) * fx;
                float x_half = fx / 2.0f;
                int x_start = (int)floor(x_center - x_half);
                int x_end = (int)ceil(x_center + x_half);
                x_start = MAX(x_start, cropX);
                x_end = MIN(x_end, cropX + cropWidth);
                x_start = MIN(x_start, srcWidth - 1);
                x_end = MIN(x_end, srcWidth);
                x_end = MAX(x_end, x_start + 1);

                intermediateImage[y * intermediateWidth + x] = rgb565_area_average(
                    src, srcWidth, x_start, x_end - 1, y_start, y_end - 1
                );
            }
            if (y % (intermediateHeight / 10) == 0) {
                int scaleProgress = 80 + (y * 15) / intermediateHeight; 
                reportProgress(scaleProgress);
            }
        }
    } else {
        // ---------- 双线性插值 ----------
        uint32_t scaleX_fixed = (cropWidth << 8) / intermediateWidth;
        uint32_t scaleY_fixed = (cropHeight << 8) / intermediateHeight;
        for (int y = 0; y < intermediateHeight; y++) {
            uint32_t srcY_fixed = (cropY << 8) + y * scaleY_fixed;
            int srcY_int = srcY_fixed >> 8; 
            srcY_int = CLAMP(srcY_int, 0, srcHeight - 1);
            int y_frac = srcY_fixed & 0xFF;
            int srcY2_int = MIN(srcY_int + 1, srcHeight - 1);

            for (int x = 0; x < intermediateWidth; x++) {
                uint32_t srcX_fixed = (cropX << 8) + x * scaleX_fixed;
                int srcX_int = srcX_fixed >> 8;
                srcX_int = CLAMP(srcX_int, 0, srcWidth - 1);
                int x_frac = srcX_fixed & 0xFF;
                int srcX2_int = MIN(srcX_int + 1, srcWidth - 1);

                uint16_t c00 = src[srcY_int * srcWidth + srcX_int];
                uint16_t c01 = src[srcY_int * srcWidth + srcX2_int];
                uint16_t c10 = src[srcY2_int * srcWidth + srcX_int];
                uint16_t c11 = src[srcY2_int * srcWidth + srcX2_int];
                
                intermediateImage[y * intermediateWidth + x] = rgb565_bilinear(c00, c01, c10, c11, x_frac, y_frac);
            }
            if (y % (intermediateHeight / 10) == 0) {
                int scaleProgress = 80 + (y * 15) / intermediateHeight; 
                reportProgress(scaleProgress);
            }
        }
    }

    // 如果是横屏图像，需要旋转90度
    if (isLandscape) {
        reportProgress(95);
        if (rawImage) {
            free(rawImage);
            rawImage = nullptr;
        }
        scaledImage = rotateImage90(intermediateImage, intermediateWidth, intermediateHeight);
        free(intermediateImage);
        if (!scaledImage) {
            reportError(ERROR_SCALE_FAILED, "图像旋转失败");
            return false;
        }
        reportProgress(98);
    } else {
        scaledImage = intermediateImage;
        reportProgress(98);
    }
    return true;
}

// 处理PNG图像
ImageProcessor::ErrorCode ImageProcessor::processPNG(uint8_t* fileBuffer, size_t fileSize) {
    reportProgress(0);
    // 打开 PNG 文件头获取原始元数据，以判断真实的横竖屏状态
    if (png.openRAM(fileBuffer, fileSize, staticPngDrawCallback) != PNG_SUCCESS) {
        free(fileBuffer); 
        reportError(ERROR_IMAGE_DECODE, "PNG打开失败");
        return ERROR_IMAGE_DECODE;
    }

    rawWidth = png.getWidth();
    rawHeight = png.getHeight();
    strcpy(lastImageFormat, "PNG");
    lastOriginalWidth = rawWidth;
    lastOriginalHeight = rawHeight;

    Serial.printf("PNG图像规格: (%d x %d)", rawWidth, rawHeight);

    //获取最大连续可用内存，并预留10%安全余量
    size_t maxContiguousMem = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);
    size_t safeMem = maxContiguousMem - (maxContiguousMem / 10);

    // 最小输出要求：240*280*2字节
    size_t minRequiredBytes = 240 * 280 * 2;
    if (safeMem < minRequiredBytes) {
        free(fileBuffer);
        png.close();
        reportError(ERROR_MEMORY_ALLOCATION, "PSRAM连续空间不足，无法满足最小输出要求(240x280)");
        return ERROR_MEMORY_ALLOCATION;
    }

    //计算保持原始比例的最大输出尺寸
    size_t maxPixels = safeMem / 2;
    size_t originalPixels = (size_t)rawWidth * rawHeight;
    if (maxPixels > originalPixels) {
        maxPixels = originalPixels;
    }

    float ratio = (float)rawWidth / rawHeight;
    int outH = (int)sqrt((float)maxPixels / ratio);
    int outW = (int)(outH * ratio);
    
    //确保 outW * outH <= maxPixels
    while (outW > 0 && outH > 0 && (size_t)(outW * outH) > maxPixels) {
        outH--;
        outW = (int)(outH * ratio);
    }

    if (outW <= 0 || outH <= 0) {
        free(fileBuffer);
        png.close();
        reportError(ERROR_MEMORY_ALLOCATION, "无法计算有效的输出尺寸");
        return ERROR_MEMORY_ALLOCATION;
    }

    Serial.printf("自动调整PNG解码输出尺寸: %d x %d\n", outW, outH); 

    //分配解码输出缓冲区
    size_t decodeMemory = outW * outH * sizeof(uint16_t);
    rawImage = (uint16_t*)ps_malloc(decodeMemory);
    if (!rawImage) {
        free(fileBuffer);
        png.close();
        reportError(ERROR_MEMORY_ALLOCATION, "无法分配PNG解码输出内存");
        return ERROR_MEMORY_ALLOCATION;
    }

    // 将计算出的输出尺寸暂存供回调函数使用
    scaledWidth = outW;
    scaledHeight = outH;
    lastDecodeDestY = -1; // 重置行去重标记

    size_t lineBufferSize = rawWidth * sizeof(uint16_t);
    uint16_t* pngLineBuffer = (uint16_t*)heap_caps_aligned_alloc(16, lineBufferSize + 1024, MALLOC_CAP_8BIT);
    if (!pngLineBuffer) {
        free(fileBuffer);
        free(rawImage);
        rawImage = nullptr;
        png.close();
        reportError(ERROR_MEMORY_ALLOCATION, "无法分配PNG行缓冲内存");
        return ERROR_MEMORY_ALLOCATION;
    }

    //解码PNG 
    if (png.decode(pngLineBuffer, 0) != PNG_SUCCESS) {
        heap_caps_free(pngLineBuffer); // 失败时记得释放
        pngLineBuffer = nullptr; 
        free(fileBuffer);
        free(rawImage);
        rawImage = nullptr;
        png.close();
        reportError(ERROR_IMAGE_DECODE, "PNG解码失败");
        return ERROR_IMAGE_DECODE;
    }

    //释放临时内存
    heap_caps_free(pngLineBuffer);
    pngLineBuffer = nullptr; 

    //解码完成后到80%
    reportProgress(80);
    png.close(); 

    // 释放文件缓冲区
    free(fileBuffer);
    fileBuffer = nullptr;

    // 缩放图像 (自动判断旋转模式)
    if (!scaleAndCropImage(rawImage, outW, outH)) {
        free(rawImage);
        rawImage = nullptr;
        reportError(ERROR_SCALE_FAILED, "PNG图像缩放失败");
        return ERROR_SCALE_FAILED;
    }

    // 释放原始图像内存
    if (rawImage) {
        free(rawImage);
        rawImage = nullptr;
    }

    //进度到100%
    reportProgress(100);
    return SUCCESS;
}

// 处理JPEG图像
ImageProcessor::ErrorCode ImageProcessor::processJPEG(uint8_t* fileBuffer, size_t fileSize) {
    reportProgress(0);

    // 打开JPEG获取图像信息
    if (!jpeg.openRAM(fileBuffer, fileSize, staticJpegDecodeCallback)) {
        free(fileBuffer); 
        reportError(ERROR_IMAGE_DECODE, "JPEG打开失败");
        return ERROR_IMAGE_DECODE;
    }

    // 获取原始尺寸
    int origWidth = jpeg.getWidth();
    int origHeight = jpeg.getHeight();
    int imgType = jpeg.getJPEGType();
    strcpy(lastImageFormat, "JPEG");
    lastOriginalWidth = origWidth;
    lastOriginalHeight = origHeight;

    // 自动选择缩小比例
    int decodeOptions = 0;
    rawWidth = origWidth;
    rawHeight = origHeight;
    if (origWidth / 8 >= 241 && origHeight / 8 >= 281) {
        decodeOptions = JPEG_SCALE_EIGHTH;
        rawWidth = origWidth / 8;
        rawHeight = origHeight / 8;
    } else if (origWidth / 4 >= 241 && origHeight / 4 >= 281) {
        decodeOptions = JPEG_SCALE_QUARTER;
        rawWidth = origWidth / 4;
        rawHeight = origHeight / 4;
    } else if (origWidth / 2 >= 241 && origHeight / 2 >= 281) {
        decodeOptions = JPEG_SCALE_HALF;
        rawWidth = origWidth / 2;
        rawHeight = origHeight / 2;
    }

    if (decodeOptions > 0) {
        Serial.printf("JPEG解码缩小: %dx%d -> %dx%d (1/%d)", origWidth, origHeight, rawWidth, rawHeight, decodeOptions);
    } else {
        Serial.printf("JPEG原始尺寸解码: %dx%d\n", origWidth, origHeight);
    }

    // 检查是否为渐进式JPEG
    if (imgType == 1) {
        free(fileBuffer); 
        jpeg.close();
        reportError(ERROR_PROGRESSIVE_JPEG, "不支持渐进式JPEG格式");
        return ERROR_PROGRESSIVE_JPEG;
    }

    // 检查原始图像内存 (宽高各+16作为MCU对齐的安全缓冲，避免边界溢出)
    size_t rawImageMemory = (rawWidth + 16) * (rawHeight + 16) * sizeof(uint16_t);
    if (!checkPSRAM(rawImageMemory, "原始JPEG图像")) {
        free(fileBuffer); 
        jpeg.close();
        reportError(ERROR_MEMORY_ALLOCATION, "PSRAM空间不足，无法分配原始JPEG图像内存");
        return ERROR_MEMORY_ALLOCATION;
    }

    // 分配原始图像内存
    rawImage = (uint16_t*)ps_malloc(rawImageMemory);
    if (!rawImage) {
        free(fileBuffer); 
        jpeg.close();
        reportError(ERROR_MEMORY_ALLOCATION, "无法分配原始JPEG图像内存");
        return ERROR_MEMORY_ALLOCATION;
    }
    Serial.printf("已分配原始JPEG图像内存: %u 字节\n", rawImageMemory);

    // 配置解码参数
    jpeg.setPixelType(RGB565_LITTLE_ENDIAN); 

    // 解码JPEG (传入计算出的缩小选项)
    if (!jpeg.decode(0, 0, decodeOptions)) {
        free(fileBuffer); 
        free(rawImage);
        rawImage = nullptr;
        jpeg.close();
        reportError(ERROR_IMAGE_DECODE, "JPEG解码失败");
        return ERROR_IMAGE_DECODE;
    }

    //解码完成后到80%
    reportProgress(80);
    jpeg.close(); 

    // 释放文件缓冲区
    free(fileBuffer);
    fileBuffer = nullptr;

    // 缩放图像 (自动判断旋转)
    if (!scaleAndCropImage(rawImage, rawWidth, rawHeight)) {
        free(rawImage);
        rawImage = nullptr;
        reportError(ERROR_SCALE_FAILED, "JPEG图像缩放失败");
        return ERROR_SCALE_FAILED;
    }

    // 释放原始图像内存
    if (rawImage) {
        free(rawImage);
        rawImage = nullptr;
    }

    //最终进度到100%
    reportProgress(100);
    return SUCCESS;
}

// 主函数：加载并处理图像
ImageProcessor::ErrorCode ImageProcessor::loadAndProcessImage(const char* imagePath, uint16_t** outputBuffer, size_t* outputSize, int targetWidth, int targetHeight) {
    // 设置目标尺寸
    this->targetWidth = targetWidth;
    this->targetHeight = targetHeight;

    // 清理之前的资源
    cleanup();

    // 检查文件扩展名
    String path = String(imagePath);
    String lpath = path;
    lpath.toLowerCase();
    bool isPNG = lpath.endsWith(".png");
    bool isJPEG = lpath.endsWith(".jpg") || lpath.endsWith(".jpeg"); 

    if (!isPNG && !isJPEG) {
        reportError(ERROR_IMAGE_FORMAT, "不支持的图像格式");
        return ERROR_IMAGE_FORMAT;
    }

    // 加载文件到缓冲区
    uint8_t* fileBuffer = nullptr;
    size_t fileSize = 0;
    ErrorCode error = loadFileToBuffer(imagePath, &fileBuffer, &fileSize);
    if (error != SUCCESS) {
        return error;
    }

    // 处理图像
    ErrorCode result = SUCCESS;
    if (isPNG) {
        result = processPNG(fileBuffer, fileSize);
    } else if (isJPEG) {
        result = processJPEG(fileBuffer, fileSize);
    }

    // 设置输出参数
    if (result == SUCCESS && scaledImage) {
        *outputBuffer = scaledImage;
        *outputSize = scaledWidth * scaledHeight * sizeof(uint16_t);
        scaledImage = nullptr; // 转移所有权给调用者
    } else {
        *outputBuffer = nullptr;
        *outputSize = 0;
    }

    return result;
}
