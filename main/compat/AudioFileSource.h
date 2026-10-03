/*
 * Arduino/ESP8266Audio 的 AudioFileSource 抽象基类 → 最小替身
 *
 * 参考工程的 AudioFileSourceSdFat 继承自 ESP8266Audio 的 AudioFileSource,
 * 只覆写了 7 个虚函数。这里提供一个最小基类, 让 AudioFileSourceSdFat.h/.cpp
 * 可以原样编译 —— 它本身只是个 SdFat 文件读取器, 正好给新的
 * micro-mp3 + I2S 后端当"压缩数据源"用。
 *
 * 注意: 本移植不移植 ESP8266Audio 的 AudioGenerator / AudioOutput 体系,
 *       所以这里只声明 AudioFileSourceSdFat 实际覆写的那几个虚函数。
 */

#pragma once

#include <stdint.h>
#include <stddef.h>

class AudioFileSource {
public:
    AudioFileSource() {}
    virtual ~AudioFileSource() {}

    /* 返回 true 表示打开成功 */
    virtual bool open(const char *filename) = 0;

    /* 读取 len 字节, 返回实际读到的字节数 */
    virtual uint32_t read(void *data, uint32_t len) = 0;

    /* dir 取 SEEK_SET / SEEK_CUR / SEEK_END (与 SdFat 一致) */
    virtual bool seek(int32_t pos, int dir) = 0;

    virtual bool close() = 0;
    virtual bool isOpen() = 0;

    /* 文件总字节数 / 当前读取位置 (用于进度条) */
    virtual uint32_t getSize() = 0;
    virtual uint32_t getPos() = 0;
};
