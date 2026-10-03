#ifndef IMAGEPROCESSOR_H
#define IMAGEPROCESSOR_H

#include <Arduino.h>
#include <FS.h>
#include <SdFat.h>
#include "PNGdec.h"
#include "JPEGDEC.h"


class ImageProcessor {
public:
    enum ErrorCode {
        SUCCESS = 0,                 ///< 处理成功
        ERROR_FILE_NOT_FOUND,        ///< 文件未找到或读取失败
        ERROR_MEMORY_ALLOCATION,     ///< PSRAM内存分配失败（空间不足或分配失败）
        ERROR_IMAGE_DECODE,          ///< 图像解码失败（PNG/JPEG解析错误）
        ERROR_IMAGE_FORMAT,          ///< 不支持的图像格式（仅支持PNG/JPG/JPEG）
        ERROR_SCALE_FAILED,          ///< 图像缩放/裁剪/旋转失败
        ERROR_PROGRESSIVE_JPEG       ///< 不支持渐进式JPEG格式
    };

    typedef void (*ProgressCallback)(int percent);
    typedef void (*ErrorCallback)(ErrorCode error, const char* message);
    ImageProcessor();
    ~ImageProcessor();
    ErrorCode loadAndProcessImage(const char* imagePath, uint16_t** outputBuffer, size_t* outputSize, int targetWidth = 240, int targetHeight = 280);
    void setProgressCallback(ProgressCallback callback);
    void setErrorCallback(ErrorCallback callback); 
    void getImageInfo(const char** format, int* originalWidth, int* originalHeight);
    size_t getFreePSRAM();
    bool checkPSRAM(size_t requiredSize, const char* operation);

private:
    uint16_t* rawImage;        
    uint16_t* scaledImage;      
    uint32_t rawWidth;          
    uint32_t rawHeight;        
    uint32_t scaledWidth;      
    uint32_t scaledHeight;      
    PNG png;                   
    JPEGDEC jpeg;               
    ProgressCallback progressCallback; 
    ErrorCallback errorCallback;       
    char lastImageFormat[8];   
    int lastOriginalWidth;       
    int lastOriginalHeight;      
    int targetWidth;            
    int targetHeight;            
    int lastDecodeDestY;         

    static ImageProcessor* instance;
    ErrorCode loadFileToBuffer(const char* imagePath, uint8_t** buffer, size_t* fileSize);
    ErrorCode processPNG(uint8_t* fileBuffer, size_t fileSize);
    ErrorCode processJPEG(uint8_t* fileBuffer, size_t fileSize);
    bool scaleAndCropImage(uint16_t* src, int srcWidth, int srcHeight);
    uint16_t* rotateImage90(uint16_t* src, int srcWidth, int srcHeight);
    void cleanup();
    static void staticPngDrawCallback(PNGDRAW *pDraw);
    static int staticJpegDecodeCallback(JPEGDRAW *pDraw);
    void pngDrawCallback(PNGDRAW *pDraw);
    int jpegDecodeCallback(JPEGDRAW *pDraw);
    void reportProgress(int percent);
    void reportError(ErrorCode error, const char* message);
};

#endif
