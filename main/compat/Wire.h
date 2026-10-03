/*
 * Arduino Wire (TwoWire) → ESP-IDF i2c_master 嫁接层
 *
 * 参考工程 RTCManager.cpp 用 Wire1 (I2C port 1) 访问 RX8130CE:
 *     TwoWire rtcWire = TwoWire(1);
 *     rtcWire.begin(41, 42, 100000);
 *     rtcWire.beginTransmission(0x32); rtcWire.write(reg); rtcWire.endTransmission();
 *     rtcWire.beginTransmission(0x32); rtcWire.write(reg); rtcWire.endTransmission(false);
 *     rtcWire.requestFrom(0x32, (uint8_t)len); rtcWire.available(); rtcWire.read();
 *
 * 映射要点:
 *   begin(sda,scl,freq)          → i2c_new_master_bus()          (端口号由构造函数给出)
 *   beginTransmission/write      → 缓存待发字节 (Arduino 语义)
 *   endTransmission(true)        → i2c_master_transmit()         返回 0=成功 / 非0=失败
 *   endTransmission(false)       → 只记 repeated-start 标志, 真正的传输留给 requestFrom
 *   requestFrom(addr,len)        → i2c_master_transmit_receive()  (write reg + read len 合成一次事务)
 *   available()/read()           → 从接收缓冲读
 *
 * 触摸 (CST816S) 用的是 I2C_NUM_0, 这里默认 I2C_NUM_1, 互不影响。
 */

#pragma once

#include <stdint.h>
#include <stddef.h>
#include <string.h>

#include "esp_err.h"
#include "driver/i2c_master.h"
#include "driver/gpio.h"

#define WIRE_TX_BUF_SIZE  32
#define WIRE_RX_BUF_SIZE  32
#define WIRE_TIMEOUT_MS   100
#define WIRE_MAX_DEVICES  4

class TwoWire {
public:
    explicit TwoWire(uint8_t bus_num = 0) : _port(bus_num) {}

    /* Arduino: bool begin(int sda, int scl, uint32_t frequency = 0) */
    bool begin(int sda, int scl, uint32_t frequency = 100000)
    {
        if (_bus) return true;

        i2c_master_bus_config_t bus_cfg = {};
        bus_cfg.i2c_port                    = (i2c_port_num_t)_port;
        bus_cfg.sda_io_num                  = (gpio_num_t)sda;
        bus_cfg.scl_io_num                  = (gpio_num_t)scl;
        bus_cfg.clk_source                  = I2C_CLK_SRC_DEFAULT;
        bus_cfg.glitch_ignore_cnt           = 7;
        bus_cfg.intr_priority               = 0;
        bus_cfg.trans_queue_depth           = 0;
        bus_cfg.flags.enable_internal_pullup = 1;

        if (i2c_new_master_bus(&bus_cfg, &_bus) != ESP_OK) {
            _bus = nullptr;
            return false;
        }
        _freq = frequency ? frequency : 100000;
        return true;
    }

    void beginTransmission(uint8_t address)
    {
        _addr    = address;
        _tx_len  = 0;
        _no_stop = false;
        _rx_len  = 0;
        _rx_idx  = 0;
    }

    size_t write(uint8_t data)
    {
        if (_tx_len < WIRE_TX_BUF_SIZE) {
            _tx_buf[_tx_len++] = data;
            return 1;
        }
        return 0;
    }

    size_t write(const uint8_t *data, size_t len)
    {
        size_t n = 0;
        while (n < len && write(data[n])) n++;
        return n;
    }

    /* 返回 0 = 成功 (Arduino 语义: 0 成功, 2 = 收到 NACK, 4 = 其他错误) */
    uint8_t endTransmission(bool sendStop = true)
    {
        if (!_bus || _addr == 0) return 4;

        if (!sendStop) {
            /* 不发 STOP: 留给随后的 requestFrom 合成一次 repeated-start 事务 */
            _no_stop = true;
            return 0;
        }

        if (_tx_len == 0) {
            /* 零字节发送 = 仅探测从机地址是否 ACK (Arduino endTransmission() 语义).
             * 不能走 i2c_master_transmit(size=0): 该 API 强制要求 size>0, 否则报
             * "i2c transmit buffer or size invalid". RX8130CE 探测就是这条路径. */
            esp_err_t perr = i2c_master_probe(_bus, _addr, WIRE_TIMEOUT_MS);
            return (perr == ESP_OK) ? 0 : 2;
        }

        i2c_master_dev_handle_t dev = get_device(_addr);
        if (!dev) return 4;

        esp_err_t err = i2c_master_transmit(dev, _tx_buf, _tx_len, WIRE_TIMEOUT_MS);
        _tx_len = 0;
        return (err == ESP_OK) ? 0 : 2;
    }

    /* 从从机读取 len 字节; 返回实际读到的字节数 (Arduino 语义) */
    uint8_t requestFrom(uint8_t address, uint8_t len)
    {
        _rx_len = 0;
        _rx_idx = 0;
        if (!_bus || len == 0) return 0;
        if (len > WIRE_RX_BUF_SIZE) len = WIRE_RX_BUF_SIZE;

        i2c_master_dev_handle_t dev = get_device(address);
        if (!dev) return 0;

        esp_err_t err;
        if (_tx_len > 0) {
            /* write reg bytes + read, 一次事务 (repeated start) */
            err = i2c_master_transmit_receive(dev, _tx_buf, _tx_len,
                                              _rx_buf, len, WIRE_TIMEOUT_MS);
        } else {
            err = i2c_master_receive(dev, _rx_buf, len, WIRE_TIMEOUT_MS);
        }
        _tx_len  = 0;
        _no_stop = false;
        if (err != ESP_OK) return 0;

        _rx_len = len;
        return len;
    }

    int available() { return (int)(_rx_len - _rx_idx); }

    int read()
    {
        if (_rx_idx >= _rx_len) return -1;
        return (int)_rx_buf[_rx_idx++];
    }

private:
    i2c_master_dev_handle_t get_device(uint8_t address)
    {
        for (int i = 0; i < WIRE_MAX_DEVICES; i++) {
            if (_dev_addr[i] == address) return _dev[i];
        }
        for (int i = 0; i < WIRE_MAX_DEVICES; i++) {
            if (_dev[i] == nullptr) {
                i2c_device_config_t dev_cfg = {};
                dev_cfg.dev_addr_length = I2C_ADDR_BIT_LEN_7;
                dev_cfg.device_address  = address;
                dev_cfg.scl_speed_hz    = _freq;
                if (i2c_master_bus_add_device(_bus, &dev_cfg, &_dev[i]) != ESP_OK) {
                    _dev[i] = nullptr;
                    return nullptr;
                }
                _dev_addr[i] = address;
                return _dev[i];
            }
        }
        return nullptr;
    }

    uint8_t _port;
    uint32_t _freq = 100000;

    i2c_master_bus_handle_t _bus = nullptr;

    i2c_master_dev_handle_t _dev[WIRE_MAX_DEVICES] = {nullptr, nullptr, nullptr, nullptr};
    uint8_t _dev_addr[WIRE_MAX_DEVICES] = {0, 0, 0, 0};

    uint8_t _addr    = 0;
    uint8_t _tx_buf[WIRE_TX_BUF_SIZE] = {0};
    size_t  _tx_len  = 0;
    bool    _no_stop = false;

    uint8_t _rx_buf[WIRE_RX_BUF_SIZE] = {0};
    size_t  _rx_len = 0;
    size_t  _rx_idx = 0;
};
