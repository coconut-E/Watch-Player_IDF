/*
 * CST816S 电容触摸 (I2C) — 底层驱动
 * 引脚/地址与参考工程 lgfx_config.h 一致: SDA=3, SCL=4, INT=2, addr=0x15
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 触摸原始坐标范围 */
#define TOUCH_X_MAX   240
#define TOUCH_Y_MAX   280

/* 初始化 I2C 总线 + CST816S */
esp_err_t touch_init(void);

/* 读取触摸点: 返回 true 表示按下, 并输出坐标 (0..TOUCH_X_MAX-1 / 0..TOUCH_Y_MAX-1) */
bool touch_read(int *x, int *y);

#ifdef __cplusplus
}
#endif
