/*
 * Arduino WiFi → ESP-IDF esp_wifi 嫁接层
 *
 * 只实现参考工程实际用到的子集 (fs_settings.cpp 的 WiFi 扫描/连接/保存,
 * ui_manager.cpp 的 connect_saved_wifi):
 *     WiFi.mode(WIFI_STA / WIFI_OFF)
 *     WiFi.disconnect() / WiFi.disconnect(true)
 *     WiFi.scanNetworks()  → 阻塞式扫描, 结果缓存在本类里
 *     WiFi.SSID(i)
 *     WiFi.setTxPower(WIFI_POWER_11dBm)   (单位 0.25dBm, 11dBm = 44)
 *     WiFi.begin(ssid, pass) / WiFi.status() / WL_CONNECTED
 *
 * 底层: esp_netif + esp_event + esp_wifi (STA 模式)。
 * 首次使用时惰性初始化, 之后一直复用。
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#include <string>
#include <vector>

#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_err.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "arduino_compat.h"   /* String / millis */

/* ─────────────── Arduino WiFi 常量 ─────────────── */
#ifndef WIFI_OFF
#define WIFI_OFF     0
#endif
#ifndef WIFI_STA
#define WIFI_STA     1
#endif
#ifndef WIFI_AP
#define WIFI_AP      2
#endif
#ifndef WIFI_AP_STA
#define WIFI_AP_STA  3
#endif

#define WL_IDLE_STATUS     0
#define WL_NO_SSID_AVAIL   1
#define WL_SCAN_COMPLETED  2
#define WL_CONNECTED       3
#define WL_CONNECT_FAILED  4
#define WL_CONNECTION_LOST 5
#define WL_DISCONNECTED    6

/* Arduino: WIFI_POWER_11dBm = 44 (步进 0.25dBm) */
#define WIFI_POWER_11dBm  44

#define WIFI_MAX_SCAN_RESULTS 32

class WiFiClass {
public:
    /* Arduino: WiFi.mode(WIFI_STA / WIFI_OFF / ...) */
    bool mode(int m)
    {
        if (!ensure_init()) return false;

        wifi_mode_t target;
        switch (m) {
            case WIFI_OFF:    target = WIFI_MODE_NULL;   break;
            case WIFI_AP:     target = WIFI_MODE_AP;     break;
            case WIFI_AP_STA: target = WIFI_MODE_APSTA;  break;
            case WIFI_STA:
            default:          target = WIFI_MODE_STA;    break;
        }

        if (target == WIFI_MODE_NULL) {
            esp_wifi_stop();
            esp_wifi_set_mode(WIFI_MODE_NULL);
            _connected = false;
            _got_ip = false;
            _mode = WIFI_OFF;
            return true;
        }

        if (_mode == WIFI_OFF || !_started) {
            if (esp_wifi_set_mode(target) != ESP_OK) return false;
            if (esp_wifi_start() != ESP_OK) return false;
            _started = true;
        } else {
            esp_wifi_set_mode(target);
        }
        _mode = m;
        return true;
    }

    /* Arduino: WiFi.disconnect(wifioff=false) */
    bool disconnect(bool wifioff = false, bool eraseap = false)
    {
        (void)eraseap;
        if (!_started) return true;
        esp_wifi_disconnect();
        if (wifioff) {
            esp_wifi_stop();
            esp_wifi_set_mode(WIFI_MODE_NULL);
            _started = false;
            _mode = WIFI_OFF;
        }
        _connected = false;
        _got_ip = false;
        return true;
    }

    /* Arduino: WiFi.scanNetworks() —— 阻塞式扫描, 返回发现的 AP 数 */
    int scanNetworks(bool async = false, bool show_hidden = false)
    {
        (void)async;
        _scan_ssid.clear();
        if (!ensure_init()) return -1;
        if (_mode == WIFI_OFF) mode(WIFI_STA);

        wifi_scan_config_t scan_cfg = {};
        scan_cfg.show_hidden = show_hidden;
        if (esp_wifi_scan_start(&scan_cfg, true) != ESP_OK) return -1;

        uint16_t ap_num = 0;
        esp_wifi_scan_get_ap_num(&ap_num);
        if (ap_num > WIFI_MAX_SCAN_RESULTS) ap_num = WIFI_MAX_SCAN_RESULTS;
        if (ap_num == 0) return 0;

        wifi_ap_record_t *recs = (wifi_ap_record_t *)calloc(ap_num, sizeof(wifi_ap_record_t));
        if (!recs) return -1;
        if (esp_wifi_scan_get_ap_records(&ap_num, recs) != ESP_OK) {
            free(recs);
            return -1;
        }
        for (uint16_t i = 0; i < ap_num; i++) {
            _scan_ssid.push_back(String((const char *)recs[i].ssid));
        }
        free(recs);
        return (int)_scan_ssid.size();
    }

    /* Arduino: WiFi.SSID(i) */
    String SSID(int i)
    {
        if (i < 0 || i >= (int)_scan_ssid.size()) return String();
        return _scan_ssid[i];
    }

    /* Arduino: WiFi.setTxPower(WIFI_POWER_11dBm) —— 单位 0.25dBm */
    bool setTxPower(int p)
    {
        if (!ensure_init()) return false;
        if (p > 78) p = 78;      /* 上限约 19.5dBm */
        if (p < 8)  p = 8;
        return esp_wifi_set_max_tx_power((int8_t)p) == ESP_OK;
    }

    /* Arduino: WiFi.begin(ssid, pass) */
    bool begin(const char *ssid, const char *passphrase = nullptr)
    {
        if (!ensure_init()) return false;
        if (_mode == WIFI_OFF || !_started) mode(WIFI_STA);

        wifi_config_t cfg = {};
        if (ssid) strlcpy((char *)cfg.sta.ssid, ssid, sizeof(cfg.sta.ssid));
        if (passphrase) strlcpy((char *)cfg.sta.password, passphrase, sizeof(cfg.sta.password));
        cfg.sta.threshold.authmode = (passphrase && passphrase[0]) ? WIFI_AUTH_WPA_PSK
                                                                   : WIFI_AUTH_OPEN;

        if (esp_wifi_set_config(WIFI_IF_STA, &cfg) != ESP_OK) return false;
        _connected = false;
        _got_ip = false;
        return esp_wifi_connect() == ESP_OK;
    }

    /* Arduino: WiFi.status() */
    int status()
    {
        if (!_started || _mode == WIFI_OFF) return WL_DISCONNECTED;
        return _got_ip ? WL_CONNECTED : (_connected ? WL_IDLE_STATUS : WL_DISCONNECTED);
    }

    /* 本移植新增: 彻底关闭并释放 WiFi 资源 (esp_wifi_deinit + 销毁 netif),
     * 避免 "开启并关闭 WiFi 之后内存少了一部分没释放"。
     * 下次 mode()/scanNetworks()/begin() 会自动重新初始化。 */
    bool end()
    {
        if (!_inited) return true;

        esp_wifi_disconnect();
        esp_wifi_stop();
        esp_wifi_set_mode(WIFI_MODE_NULL);

        if (_wifi_evt) {
            esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, _wifi_evt);
            _wifi_evt = nullptr;
        }
        if (_ip_evt) {
            esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, _ip_evt);
            _ip_evt = nullptr;
        }

        esp_wifi_deinit();

        if (_sta_netif) {
            esp_netif_destroy(_sta_netif);
            _sta_netif = nullptr;
        }

        _inited    = false;
        _started   = false;
        _connected = false;
        _got_ip    = false;
        _mode      = WIFI_OFF;
        return true;
    }

private:
    static void event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
    {
        WiFiClass *self = (WiFiClass *)arg;
        if (!self) return;
        if (base == WIFI_EVENT && id == WIFI_EVENT_STA_CONNECTED) {
            self->_connected = true;
        } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
            self->_connected = false;
            self->_got_ip = false;
        } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
            self->_got_ip = true;
        }
    }

    bool ensure_init()
    {
        if (_inited) return true;

        if (esp_netif_init() != ESP_OK) return false;
        esp_err_t err = esp_event_loop_create_default();
        if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) return false;
        _sta_netif = esp_netif_create_default_wifi_sta();
        if (!_sta_netif) return false;

        wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
        if (esp_wifi_init(&cfg) != ESP_OK) return false;

        esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                           &WiFiClass::event_handler, this, &_wifi_evt);
        esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                           &WiFiClass::event_handler, this, &_ip_evt);
        _inited = true;
        return true;
    }

    bool _inited    = false;
    bool _started   = false;
    bool _connected = false;
    bool _got_ip    = false;
    int  _mode      = WIFI_OFF;

    esp_event_handler_instance_t _wifi_evt = nullptr;
    esp_event_handler_instance_t _ip_evt   = nullptr;
    esp_netif_t                 *_sta_netif = nullptr;

    std::vector<String> _scan_ssid;
};

extern WiFiClass WiFi;
