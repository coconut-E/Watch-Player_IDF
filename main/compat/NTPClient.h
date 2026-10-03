/*
 * Arduino NTPClient → esp_sntp 嫁接层
 *
 * 参考工程用法:
 *     static WiFiUDP ntpUDP;
 *     static NTPClient timeClient(ntpUDP, "ntp.aliyun.com", 28800, 60000);
 *     timeClient.begin();
 *     if (timeClient.update()) {
 *         struct timeval tv = { .tv_sec = (time_t)timeClient.getEpochTime() };
 *         settimeofday(&tv, NULL);
 *     }
 *
 * 语义对齐要点:
 *   - NTPClient 是一次性查询, 不改系统时钟; 参考工程随后自己 settimeofday。
 *   - esp_sntp 会直接把系统时钟设成真实 UTC, 所以 update() 成功后会立刻
 *     esp_sntp_stop() 停掉后台轮询, 否则一小时后 SNTP 再同步会把
 *     参考工程"把北京时间当 UTC 存"的偏移抹掉 (时钟会倒退 8 小时)。
 *   - getEpochTime() = time(NULL) + offset, 与 Arduino NTPClient 一致
 *     (offset = 28800 = UTC+8), 交给调用方 settimeofday。
 */

#pragma once

#include <stdint.h>
#include <time.h>

#include "esp_sntp.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "WiFiUdp.h"
#include "arduino_compat.h"   /* millis / delay */

#define NTP_SYNC_TIMEOUT_MS 15000

class NTPClient {
public:
    NTPClient(WiFiUDP &udp, const char *poolServerName,
              long timeOffset = 0, unsigned long updateInterval = 60000)
        : _server(poolServerName), _offset(timeOffset), _interval(updateInterval)
    {
        (void)udp;
    }

    void begin()
    {
        if (_server) esp_sntp_setservername(0, _server);
        esp_sntp_setoperatingmode(ESP_SNTP_OPMODE_POLL);
        esp_sntp_init();
        _started = true;
    }

    /* 返回 true 表示本次拿到时间 (阻塞等待首次同步完成, 最多 10s) */
    bool update()
    {
        if (!_started) return false;

        uint32_t t0 = millis();
        bool ok = false;
        while (millis() - t0 < NTP_SYNC_TIMEOUT_MS) {
            if (esp_sntp_get_sync_status() == SNTP_SYNC_STATUS_COMPLETED) {
                ok = true;
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(100));
        }

        if (ok) {
            /* 一次性同步: 停止后台轮询, 避免之后把系统时间改回 UTC
             * (参考工程是把"北京时间"当 UTC 存进系统时钟的) */
            esp_sntp_stop();
            _started = false;
        } else {
            /* 超时也要停掉: 否则 SNTP 之后某次轮询成功会把系统时钟悄悄改回 UTC,
             * 而调用方不会再执行 settimeofday, 时钟就会倒退 8 小时 */
            esp_sntp_stop();
            _started = false;
        }
        return ok;
    }

    /* 与 Arduino NTPClient 一致: 返回 epoch + 时区偏移 */
    unsigned long getEpochTime()
    {
        time_t now = time(nullptr);
        return (unsigned long)((long)now + _offset);
    }

    void setTimeOffset(long offset) { _offset = offset; }
    long getTimeOffset() { return _offset; }
    void setUpdateInterval(unsigned long ms) { _interval = ms; }

private:
    const char   *_server;
    long          _offset;
    unsigned long _interval;
    bool          _started = false;
};
