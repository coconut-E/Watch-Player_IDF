/*
 * Arduino → ESP-IDF 兼容层 (C++)
 *
 * 目的: 让参考工程 (Arduino) 的源码 (ui_manager.cpp 等) 尽量原样移植,
 *       只替换底层实现 (显示/触摸/时间/持久化)。
 * 仅提供参考工程实际用到的子集。
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include <ctype.h>
#include <time.h>
#include <string>
#include <type_traits>

#include "esp_timer.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_random.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ─────────────── Arduino 常量/宏 ─────────────── */
#ifndef HIGH
#define HIGH 1
#endif
#ifndef LOW
#define LOW 0
#endif
#ifndef INPUT
#define INPUT 0x01
#endif
#ifndef OUTPUT
#define OUTPUT 0x03
#endif
#ifndef INPUT_PULLUP
#define INPUT_PULLUP 0x05
#endif
#ifndef ANALOG
#define ANALOG 0x02
#endif
#ifndef PROGMEM
#define PROGMEM
#endif

/* Arduino ADC 衰减宏 → IDF ADC_ATTEN_DB_12 */
#define ADC_11db 3
#define ADC_6db  2
#define ADC_2_5db 1
#define ADC_0db  0

#ifndef constrain
#define constrain(amt, low, high) ((amt) < (low) ? (low) : ((amt) > (high) ? (high) : (amt)))
#endif

/* ─────────────── 时间 ─────────────── */
static inline uint32_t millis(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000LL);
}

static inline void delay(uint32_t ms)
{
    vTaskDelay(pdMS_TO_TICKS(ms));
}

/* ─────────────── GPIO ─────────────── */
static inline void pinMode(int pin, int mode)
{
    gpio_set_direction((gpio_num_t)pin,
                       (mode == OUTPUT) ? GPIO_MODE_OUTPUT : GPIO_MODE_INPUT);
    if (mode == INPUT_PULLUP) {
        gpio_set_pull_mode((gpio_num_t)pin, GPIO_PULLUP_ONLY);
    }
}

static inline int digitalRead(int pin)
{
    return gpio_get_level((gpio_num_t)pin);
}

static inline void digitalWrite(int pin, int level)
{
    gpio_set_level((gpio_num_t)pin, level);
}

/* ─────────────── ADC (Arduino analog* 兼容, 底层 esp_adc) ─────────────── */
void     analogReadResolution(int bits);
void     analogSetPinAttenuation(int pin, int attenuation);
uint32_t analogReadMilliVolts(int pin);

#ifdef __cplusplus
}
#endif

/* ─────────────── Arduino random(min,max) ───────────────
 * newlib 的 random(void) 与 Arduino 的 random(howbig)/random(min,max) 同名,
 * 这里按 C++ 重载补上后两个 (Arduino WMath.cpp 的语义: 上限不含)。
 * 注意: 必须放在 extern "C" 块之外, 否则会被当成 C 函数而冲突。 */
static inline long random(long howbig)
{
    if (howbig <= 0) return 0;
    return (long)(esp_random() % (uint32_t)howbig);
}

static inline long random(long howsmall, long howbig)
{
    if (howbig <= howsmall) return howsmall;
    return howsmall + random(howbig - howsmall);
}

/* ─────────────── Arduino getLocalTime ───────────────
 * 参考工程用它读本地时间; 本项目不设 TZ (参考工程靠 NTPClient 的 +28800 偏移
 * 把北京时间当 UTC 存), 所以这里等价于 localtime。 */
static inline bool getLocalTime(struct tm *info, uint32_t ms = 5000)
{
    (void)ms;
    if (!info) return false;
    time_t now = time(nullptr);
    struct tm *t = localtime(&now);
    if (!t) return false;
    *info = *t;
    return true;
}

/* ─────────────── Arduino min/max (WMath.h) ───────────────
 * 参考工程用 min((int)array.size(), 7)。这里用函数模板而不是宏,
 * 避免污染 <algorithm> 等系统头 (Arduino 用宏是因为 C 兼容)。
 * 注意: 返回类型必须用 common_type (值类型); 若写成 decltype(a<b?a:b)
 * 会得到引用类型 int&, 返回局部变量引用 -> GCC 报 dangling-pointer。 */
template <typename T, typename U>
static inline typename std::common_type<T, U>::type min(T a, U b) { return (a < b) ? a : b; }
template <typename T, typename U>
static inline typename std::common_type<T, U>::type max(T a, U b) { return (a > b) ? a : b; }

/* 说明: strlcpy/strlcat 由工具链的 newlib(picolibc) string.h 提供, 无需自己实现 */

/* ─────────────── Serial ─────────────── */
class String;   /* 前置声明: Serial 需要 String 重载, 定义见下方 */

class SerialClass {
public:
    void begin(unsigned long baud = 115200) { (void)baud; }
    /* 对应 Arduino Serial.flush(): 本移植用 stdout 打日志, 所以刷 stdout */
    void flush(void) { fflush(stdout); }

    void print(const char *s) { printf("%s", s ? s : ""); }
    void print(char c) { printf("%c", c); }
    void print(int v) { printf("%d", v); }
    void print(unsigned int v) { printf("%u", v); }
    void print(long v) { printf("%ld", v); }
    void print(unsigned long v) { printf("%lu", v); }
    void print(double v) { printf("%f", v); }

    void println(void) { printf("\n"); }
    void println(const char *s) { printf("%s\n", s ? s : ""); }
    void println(char c) { printf("%c\n", c); }
    void println(int v) { printf("%d\n", v); }
    void println(unsigned int v) { printf("%u\n", v); }
    void println(long v) { printf("%ld\n", v); }
    void println(unsigned long v) { printf("%lu\n", v); }
    void println(double v) { printf("%f\n", v); }

    /* String 重载 (定义在 String 类之后, 见下方 inline) */
    void print(const String &s);
    void println(const String &s);

    int printf(const char *fmt, ...)
    {
        va_list ap;
        va_start(ap, fmt);
        int n = vprintf(fmt, ap);
        va_end(ap);
        return n;
    }
};

extern SerialClass Serial;

/* ─────────────── PSRAM 分配 (Arduino-ESP32 的 ps_malloc 系列) ─────────────── */
static inline void *ps_malloc(size_t n) { return heap_caps_malloc(n, MALLOC_CAP_SPIRAM); }
static inline void *ps_calloc(size_t n, size_t s) { return heap_caps_calloc(n, s, MALLOC_CAP_SPIRAM); }
static inline void *ps_realloc(void *p, size_t n) { return heap_caps_realloc(p, n, MALLOC_CAP_SPIRAM); }

/* ─────────────── String (Arduino String 子集, 基于 std::string) ─────────────── */
class String {
public:
    String() {}
    String(const char *s) : _s(s ? s : "") {}
    String(const std::string &s) : _s(s) {}
    String(const String &o) : _s(o._s) {}
    String(char c) : _s(1, c) {}

    const char *c_str() const { return _s.c_str(); }
    size_t length() const { return _s.length(); }
    size_t size() const { return _s.size(); }
    bool isEmpty() const { return _s.empty(); }
    char charAt(size_t i) const { return (i < _s.size()) ? _s[i] : '\0'; }

    void trim()
    {
        size_t b = 0, e = _s.size();
        while (b < e && isspace((unsigned char)_s[b])) b++;
        while (e > b && isspace((unsigned char)_s[e - 1])) e--;
        _s = _s.substr(b, e - b);
    }
    void toLowerCase()
    {
        for (size_t i = 0; i < _s.size(); i++)
            _s[i] = (char)tolower((unsigned char)_s[i]);
    }
    void toUpperCase()
    {
        for (size_t i = 0; i < _s.size(); i++)
            _s[i] = (char)toupper((unsigned char)_s[i]);
    }

    bool startsWith(const String &p) const
    {
        return (p._s.size() <= _s.size()) && (_s.compare(0, p._s.size(), p._s) == 0);
    }
    bool startsWith(const char *p) const { return startsWith(String(p)); }
    bool endsWith(const String &p) const
    {
        if (p._s.size() > _s.size()) return false;
        return _s.compare(_s.size() - p._s.size(), p._s.size(), p._s) == 0;
    }
    bool endsWith(const char *p) const { return endsWith(String(p)); }

    int indexOf(char c) const
    {
        size_t p = _s.find(c);
        return (p == std::string::npos) ? -1 : (int)p;
    }
    int indexOf(const String &v) const
    {
        size_t p = _s.find(v._s);
        return (p == std::string::npos) ? -1 : (int)p;
    }

    String substring(size_t from) const
    {
        return (from >= _s.size()) ? String() : String(_s.substr(from));
    }
    String substring(size_t from, size_t to) const
    {
        if (from >= _s.size()) return String();
        if (to > _s.size()) to = _s.size();
        if (to <= from) return String();
        return String(_s.substr(from, to - from));
    }

    /* Arduino String::remove —— 删掉 index 起的 count 个字符 */
    void remove(unsigned int index)
    {
        if (index >= _s.size()) return;
        _s.erase(index);
    }
    void remove(unsigned int index, unsigned int count)
    {
        if (index >= _s.size()) return;
        _s.erase(index, count);
    }

    /* Arduino String::concat —— ArduinoJson 的 Writer<::String> 会用到 */
    bool concat(const String &o) { _s += o._s; return true; }
    bool concat(const char *s) { if (s) _s += s; return true; }
    bool concat(char c) { _s += c; return true; }

    long  toInt() const { return strtol(_s.c_str(), nullptr, 10); }
    float toFloat() const { return strtof(_s.c_str(), nullptr); }

    String &operator=(const char *s) { _s = s ? s : ""; return *this; }
    String &operator=(const String &o) { _s = o._s; return *this; }
    String &operator+=(const String &o) { _s += o._s; return *this; }
    String &operator+=(const char *s) { if (s) _s += s; return *this; }
    String &operator+=(char c) { _s += c; return *this; }
    String &operator+=(int v)
    {
        char b[24];
        snprintf(b, sizeof(b), "%d", v);
        _s += b;
        return *this;
    }

    bool operator==(const String &o) const { return _s == o._s; }
    bool operator==(const char *s) const { return _s == (s ? s : ""); }
    bool operator!=(const String &o) const { return !(*this == o); }
    bool operator!=(const char *s) const { return !(*this == s); }

    /* Arduino String::equals / equalsIgnoreCase */
    bool equals(const String &o) const { return _s == o._s; }
    bool equals(const char *s) const { return _s == (s ? s : ""); }
    bool equalsIgnoreCase(const String &o) const
    {
        if (_s.size() != o._s.size()) return false;
        for (size_t i = 0; i < _s.size(); i++) {
            if (tolower((unsigned char)_s[i]) != tolower((unsigned char)o._s[i])) return false;
        }
        return true;
    }
    bool equalsIgnoreCase(const char *s) const { return equalsIgnoreCase(String(s)); }

    const std::string &str() const { return _s; }

private:
    std::string _s;
};

/* Serial 的 String 重载 (String 定义完成后才能实现) */
inline void SerialClass::print(const String &s)   { printf("%s", s.c_str()); }
inline void SerialClass::println(const String &s) { printf("%s\n", s.c_str()); }

inline String operator+(const String &a, const String &b) { return String(a.str() + b.str()); }
inline String operator+(const String &a, const char *b) { return String(a.str() + std::string(b ? b : "")); }
inline String operator+(const char *a, const String &b) { return String(std::string(a ? a : "") + b.str()); }
inline bool operator==(const char *a, const String &b) { return b == a; }

/* ─────────────── ESP (Arduino-ESP32 的 ESP 全局对象子集) ─────────────── */
class ESPClass {
public:
    size_t   getFreePsram() { return heap_caps_get_free_size(MALLOC_CAP_SPIRAM); }
    size_t   getPsramSize() { return heap_caps_get_total_size(MALLOC_CAP_SPIRAM); }
    size_t   getFreeHeap() { return heap_caps_get_free_size(MALLOC_CAP_DEFAULT); }
    size_t   getHeapSize() { return heap_caps_get_total_size(MALLOC_CAP_DEFAULT); }
    uint32_t getChipId();
};

extern ESPClass ESP;

/* ─────────────── Preferences (NVS) ─────────────── */
class Preferences {
public:
    Preferences();
    ~Preferences();

    bool begin(const char *name, bool readOnly = false);
    void end(void);

    uint8_t getUChar(const char *key, uint8_t defaultValue = 0);
    size_t putUChar(const char *key, uint8_t value);
    int32_t getInt(const char *key, int32_t defaultValue = 0);
    size_t putInt(const char *key, int32_t value);
    float getFloat(const char *key, float defaultValue = 0.0f);
    size_t putFloat(const char *key, float value);
    bool getBool(const char *key, bool defaultValue = false);
    size_t putBool(const char *key, bool value);
    String getString(const char *key, const String &defaultValue = String());
    size_t putString(const char *key, const String &value);
    bool isKey(const char *key);
    bool remove(const char *key);

private:
    nvs_handle_t _handle;
    bool _opened;
};

extern Preferences preferences;

/* ─────────────── 显示 (对应参考工程 LGFX display) ─────────────── */
class LGFX {
public:
    void init(void) {}
    void setRotation(int r) { (void)r; }
    void setColorDepth(int d) { (void)d; }
    void fillScreen(uint32_t color) { (void)color; }
    void setBrightness(uint8_t brightness);
    bool getTouch(uint16_t *x, uint16_t *y);
};

extern LGFX display;
