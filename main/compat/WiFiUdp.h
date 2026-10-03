/*
 * Arduino WiFiUDP → 空壳
 *
 * 参考工程只是把 WiFiUDP 当 NTPClient 的构造参数传进去
 * (NTPClient 内部才用 UDP 发包)。本移植的 NTPClient 直接用 esp_sntp,
 * 不需要 UDP socket, 所以这里只需提供一个能编译过的空类型。
 */

#pragma once

#include <stdint.h>
#include <stddef.h>

class WiFiUDP {
public:
    WiFiUDP() {}
    ~WiFiUDP() {}

    bool begin(uint16_t port = 0) { (void)port; return true; }
    void stop() {}
};
