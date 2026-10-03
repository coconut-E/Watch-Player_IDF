#ifndef RTC_MANAGER_H
#define RTC_MANAGER_H

#include <Wire.h>
#include <Arduino.h>

struct DateTime {
    uint8_t second;
    uint8_t minute;
    uint8_t hour;
    uint8_t week;
    uint8_t day;
    uint8_t month;
    uint8_t year;
};

class RTCManager {
public:
    RTCManager();
    bool begin();
    bool isAvailable();
    bool readTime(DateTime* dt);
    bool setTime(const DateTime* dt);
    bool syncToSystem();
    bool syncFromSystem();
    
private:
    bool initialized;
    bool rtcAvailable;
    
    // RX8130CE I2C 地址与寄存器
    static const uint8_t RX8130_I2C_ADDR = 0x32; 
    
    // 时间寄存器地址从 0x10 开始 
    static const uint8_t REG_SEC   = 0x10;
    static const uint8_t REG_MIN   = 0x11;
    static const uint8_t REG_HOUR  = 0x12;
    static const uint8_t REG_WEEK  = 0x13;
    static const uint8_t REG_DAY   = 0x14;
    static const uint8_t REG_MON   = 0x15;
    static const uint8_t REG_YEAR  = 0x16;
    
    // 扩展寄存器与控制寄存器
    static const uint8_t REG_EXT   = 0x1D; // Extension Register
    static const uint8_t REG_FLAG  = 0x1E; // Flag Register
    static const uint8_t REG_CTRL  = 0x1F; // Control Register

    // 引脚定义
    static const uint8_t SDA_PIN = 41;
    static const uint8_t SCL_PIN = 42;
    
    uint8_t writeByte(uint8_t reg, uint8_t data);
    uint8_t readByte(uint8_t reg);
    uint8_t readBytes(uint8_t reg, uint8_t *data, uint8_t len);
    uint8_t bcdToHex(uint8_t bcd);
    uint8_t hexToBcd(uint8_t hex);
};

extern RTCManager rtcManager;

#endif