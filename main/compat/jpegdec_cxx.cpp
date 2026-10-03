/*
 * Arduino JPEGDEC C++ 类补齐
 *
 * bitbank2/jpegdec 的 ESP-IDF 组件 (managed_components/bitbank2__jpegdec)
 * 只导出 C API (JPEG_openRAM / JPEG_decode / ...), 头文件里的 class JPEGDEC
 * 只有声明没有定义 —— 因为它的 JPEGDEC.c 是 C 文件, 而 Arduino 版是由
 * JPEGDEC.cpp 提供这些成员函数的。
 *
 * 参考工程用的是 Arduino 的 C++ 类, 所以这里按 Arduino 库
 * (D:\Arduino\libraries\JPEGDEC\src\JPEGDEC.cpp) 的语义,
 * 用组件的 C API 把类方法补齐, 使参考工程源码原样可用。
 *
 * 说明: JPEGDEC.h 在 __cplusplus 分支里不声明 C API, 所以下面自行 extern "C" 声明。
 */

#include <JPEGDEC.h>
#include <string.h>

extern "C" {
int  JPEG_openRAM(JPEGIMAGE *pJPEG, uint8_t *pData, int iDataSize, JPEG_DRAW_CALLBACK *pfnDraw);
void JPEG_setFramebuffer(JPEGIMAGE *pJPEG, void *pFramebuffer);
void JPEG_setCropArea(JPEGIMAGE *pJPEG, int x, int y, int w, int h);
void JPEG_getCropArea(JPEGIMAGE *pJPEG, int *x, int *y, int *w, int *h);
int  JPEG_getWidth(JPEGIMAGE *pJPEG);
int  JPEG_getHeight(JPEGIMAGE *pJPEG);
int  JPEG_decode(JPEGIMAGE *pJPEG, int x, int y, int iOptions);
int  JPEG_decodeDither(JPEGIMAGE *pJPEG, uint8_t *pDither, int iOptions);
void JPEG_close(JPEGIMAGE *pJPEG);
int  JPEG_getLastError(JPEGIMAGE *pJPEG);
int  JPEG_getOrientation(JPEGIMAGE *pJPEG);
int  JPEG_getBpp(JPEGIMAGE *pJPEG);
int  JPEG_getSubSample(JPEGIMAGE *pJPEG);
int  JPEG_hasThumb(JPEGIMAGE *pJPEG);
int  JPEG_getThumbWidth(JPEGIMAGE *pJPEG);
int  JPEG_getThumbHeight(JPEGIMAGE *pJPEG);
void JPEG_setPixelType(JPEGIMAGE *pJPEG, int iType);
void JPEG_setMaxOutputSize(JPEGIMAGE *pJPEG, int iMaxMCUs);
}

/* ─────────────── 初始化 ─────────────── */
int JPEGDEC::openRAM(uint8_t *pData, int iDataSize, JPEG_DRAW_CALLBACK *pfnDraw)
{
    return JPEG_openRAM(&_jpeg, pData, iDataSize, pfnDraw);
}

/* ESP32 上 FLASH 与 RAM 统一编址, memcpy_P == memcpy, 故与 openRAM 等价 */
int JPEGDEC::openFLASH(const uint8_t *pData, int iDataSize, JPEG_DRAW_CALLBACK *pfnDraw)
{
    return openRAM((uint8_t *)pData, iDataSize, pfnDraw);
}

void JPEGDEC::close()
{
    JPEG_close(&_jpeg);
}

/* ─────────────── 参数 ─────────────── */
void JPEGDEC::setPixelType(int iType)
{
    if (iType >= 0 && iType < INVALID_PIXEL_TYPE) {
        _jpeg.ucPixelType = (uint8_t)iType;
    } else {
        _jpeg.iError = JPEG_INVALID_PARAMETER;
    }
}

void JPEGDEC::setMaxOutputSize(int iMaxMCUs)
{
    JPEG_setMaxOutputSize(&_jpeg, iMaxMCUs);
}

void JPEGDEC::setFramebuffer(void *pFramebuffer)
{
    JPEG_setFramebuffer(&_jpeg, pFramebuffer);
}

void JPEGDEC::setCropArea(int x, int y, int w, int h)
{
    JPEG_setCropArea(&_jpeg, x, y, w, h);
}

void JPEGDEC::getCropArea(int *x, int *y, int *w, int *h)
{
    JPEG_getCropArea(&_jpeg, x, y, w, h);
}

void JPEGDEC::setUserPointer(void *p)
{
    _jpeg.pUser = p;
}

/* ─────────────── 解码 ─────────────── */
int JPEGDEC::decode(int x, int y, int iOptions)
{
    return JPEG_decode(&_jpeg, x, y, iOptions);
}

int JPEGDEC::decodeDither(uint8_t *pDither, int iOptions)
{
    return JPEG_decodeDither(&_jpeg, pDither, iOptions);
}

int JPEGDEC::decodeDither(int x, int y, uint8_t *pDither, int iOptions)
{
    _jpeg.iXOffset = x;
    _jpeg.iYOffset = y;
    return JPEG_decodeDither(&_jpeg, pDither, iOptions);
}

/* ─────────────── 查询 ─────────────── */
int JPEGDEC::getWidth()       { return JPEG_getWidth(&_jpeg); }
int JPEGDEC::getHeight()      { return JPEG_getHeight(&_jpeg); }
int JPEGDEC::getBpp()         { return JPEG_getBpp(&_jpeg); }
int JPEGDEC::getSubSample()   { return JPEG_getSubSample(&_jpeg); }
int JPEGDEC::getOrientation() { return JPEG_getOrientation(&_jpeg); }
int JPEGDEC::getLastError()   { return JPEG_getLastError(&_jpeg); }
int JPEGDEC::hasThumb()       { return JPEG_hasThumb(&_jpeg); }
int JPEGDEC::getThumbWidth()  { return JPEG_getThumbWidth(&_jpeg); }
int JPEGDEC::getThumbHeight() { return JPEG_getThumbHeight(&_jpeg); }

/* 0 = 基线, 1 = 渐进式 (与 Arduino 库 JPEGDEC.cpp 一致) */
int JPEGDEC::getJPEGType()
{
    return (_jpeg.ucMode == 0xc2) ? JPEG_MODE_PROGRESSIVE : JPEG_MODE_BASELINE;
}
