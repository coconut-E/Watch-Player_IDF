#include "RTCManager.h"
#include <sys/time.h>

// 使用 ESP32 的第二个 I2C 控制器 (Wire1)
TwoWire rtcWire = TwoWire(1); 

RTCManager rtcManager;

RTCManager::RTCManager() : initialized(false), rtcAvailable(false) {}

bool RTCManager::begin() {
    if (initialized) return rtcAvailable;
    
    if (!rtcWire.begin(SDA_PIN, SCL_PIN, 100000)) {
        Serial.println("I2C 控制器初始化失败");
        return false;
    }
    
    // 检查设备是否存在
    rtcWire.beginTransmission(RX8130_I2C_ADDR);
    if (rtcWire.endTransmission() == 0) {
        Serial.println("检测到 RX8130CE RTC");
        rtcAvailable = true;
    
        writeByte(0x08, 0x00); // 禁用充电位
        
        // 清除电压下降/停止振荡标志位
        uint8_t flags = readByte(REG_FLAG);
        writeByte(REG_FLAG, flags & 0x00); 
        
    } else {
        Serial.println("未检测到 RX8130CE");
        rtcAvailable = false;
    }
    
    initialized = true;
    return rtcAvailable;
}

bool RTCManager::isAvailable() {
    return rtcAvailable;
}

bool RTCManager::readTime(DateTime* dt) {
    if (!rtcAvailable) return false;
    
    uint8_t data[7];
    if (readBytes(REG_SEC, data, 7) != 0) {
        return false;
    }
    
    dt->second = bcdToHex(data[0] & 0x7F);
    dt->minute = bcdToHex(data[1] & 0x7F);
    dt->hour   = bcdToHex(data[2] & 0x3F); 
    dt->week   = bcdToHex(data[3] & 0x07);
    dt->day    = bcdToHex(data[4] & 0x3F);
    dt->month  = bcdToHex(data[5] & 0x1F);
    dt->year   = bcdToHex(data[6]);
    
    return true;
}

bool RTCManager::setTime(const DateTime* dt) {
    if (!rtcAvailable) return false;
    
    rtcWire.beginTransmission(RX8130_I2C_ADDR);
    rtcWire.write(REG_SEC);
    rtcWire.write(hexToBcd(dt->second));
    rtcWire.write(hexToBcd(dt->minute));
    rtcWire.write(hexToBcd(dt->hour));
    rtcWire.write(hexToBcd(dt->week));
    rtcWire.write(hexToBcd(dt->day));
    rtcWire.write(hexToBcd(dt->month));
    rtcWire.write(hexToBcd(dt->year));
    
    return (rtcWire.endTransmission() == 0);
}

// 系统时间同步逻辑
bool RTCManager::syncToSystem() {
    if (!rtcAvailable) return false;
    DateTime rtcTime;
    if (!readTime(&rtcTime)) return false;
    
    struct tm sysTime;
    sysTime.tm_sec = rtcTime.second;
    sysTime.tm_min = rtcTime.minute;
    sysTime.tm_hour = rtcTime.hour;
    sysTime.tm_mday = rtcTime.day;
    sysTime.tm_mon = rtcTime.month - 1;
    sysTime.tm_year = rtcTime.year + 100;
    
    time_t epochTime = mktime(&sysTime);
    struct timeval tv = {epochTime, 0};
    return (settimeofday(&tv, NULL) == 0);
}

bool RTCManager::syncFromSystem() {
    if (!rtcAvailable) return false;
    time_t now;
    time(&now);
    struct tm* sysTime = localtime(&now);
    
    DateTime rtcTime;
    rtcTime.second = sysTime->tm_sec;
    rtcTime.minute = sysTime->tm_min;
    rtcTime.hour = sysTime->tm_hour;
    rtcTime.day = sysTime->tm_mday;
    rtcTime.month = sysTime->tm_mon + 1;
    rtcTime.year = sysTime->tm_year - 100;
    rtcTime.week = sysTime->tm_wday == 0 ? 7 : sysTime->tm_wday;
    
    return setTime(&rtcTime);
}

uint8_t RTCManager::writeByte(uint8_t reg, uint8_t data) {
    rtcWire.beginTransmission(RX8130_I2C_ADDR);
    rtcWire.write(reg);
    rtcWire.write(data);
    return rtcWire.endTransmission();
}

uint8_t RTCManager::readByte(uint8_t reg) {
    rtcWire.beginTransmission(RX8130_I2C_ADDR);
    rtcWire.write(reg);
    rtcWire.endTransmission(false);
    rtcWire.requestFrom(RX8130_I2C_ADDR, (uint8_t)1);
    return rtcWire.read();
}

uint8_t RTCManager::readBytes(uint8_t reg, uint8_t *data, uint8_t len) {
    rtcWire.beginTransmission(RX8130_I2C_ADDR);
    rtcWire.write(reg);
    if (rtcWire.endTransmission(false) != 0) return 1;
    
    rtcWire.requestFrom(RX8130_I2C_ADDR, len);
    for (uint8_t i = 0; i < len; i++) {
        if (rtcWire.available()) data[i] = rtcWire.read();
        else return 1;
    }
    return 0;
}

uint8_t RTCManager::bcdToHex(uint8_t bcd) { return (bcd >> 4) * 10 + (bcd & 0x0F); }
uint8_t RTCManager::hexToBcd(uint8_t hex) { return ((hex / 10) << 4) | (hex % 10); }