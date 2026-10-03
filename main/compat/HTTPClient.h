/*
 * Arduino HTTPClient → ESP-IDF 官方 esp_http_client 嫁接层
 *
 * 只实现参考工程 (ui_manager.cpp 的 weather_task) 实际用到的子集:
 *     HTTPClient http;
 *     http.setTimeout(8000);
 *     http.setConnectTimeout(5000);
 *     if (http.begin(url)) {          // url 为 String 或 const char*
 *         int httpCode = http.GET();  // 返回 HTTP 状态码, 失败为负
 *         String payload = http.getString();
 *         http.end();
 *     }
 *
 * 底层: IDF 官方内置组件 esp_http_client (无需从组件市场下载)。
 * 语义对齐: begin() 只创建句柄; GET() 时才真正连接/请求/读取整个响应体。
 */

#pragma once

#include <stdint.h>
#include <string>

#include "esp_http_client.h"

#include "arduino_compat.h"   /* String */

class HTTPClient {
public:
    HTTPClient() {}
    ~HTTPClient() { end(); }

    /* Arduino: setTimeout(ms) —— 整体超时 (连接+读写) */
    void setTimeout(int ms) { if (ms > 0) _timeout_ms = ms; }

    /* Arduino: setConnectTimeout(ms) —— esp_http_client 无独立连接超时字段,
     * 取二者较小值作为整体 timeout, 与参考工程的意图等价 */
    void setConnectTimeout(int ms) { if (ms > 0 && ms < _timeout_ms) _timeout_ms = ms; }

    /* Arduino: begin(url) */
    bool begin(const String &url) { return begin(url.c_str()); }
    bool begin(const char *url)
    {
        end();
        if (!url || !url[0]) return false;

        esp_http_client_config_t cfg = {};
        cfg.url        = url;
        cfg.method     = HTTP_METHOD_GET;
        cfg.timeout_ms = _timeout_ms;
        cfg.buffer_size = 2048;

        _client = esp_http_client_init(&cfg);
        return _client != nullptr;
    }

    /* Arduino: GET() —— 返回 HTTP 状态码; 失败返回负值 */
    int GET()
    {
        if (!_client) return -1;

        _body.clear();

        if (esp_http_client_open(_client, 0) != ESP_OK) {
            return -1;
        }

        esp_http_client_fetch_headers(_client);   /* 返回体长度或 -1(chunked) */
        int status = esp_http_client_get_status_code(_client);

        char buf[512];
        int n;
        while ((n = esp_http_client_read(_client, buf, sizeof(buf))) > 0) {
            _body.append(buf, (size_t)n);
        }

        esp_http_client_close(_client);
        return status;
    }

    /* Arduino: getString() —— 整个响应体 */
    String getString() { return String(_body); }

    /* Arduino: end() */
    void end()
    {
        if (_client) {
            esp_http_client_cleanup(_client);
            _client = nullptr;
        }
    }

private:
    esp_http_client_handle_t _client = nullptr;
    int         _timeout_ms = 5000;
    std::string _body;
};
