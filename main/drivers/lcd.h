/*
 * SPDX-License-Identifier: CC0-1.0
 */

#pragma once

#include <stdint.h>
#include "esp_err.h"
#include "esp_lcd_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 屏幕可见分辨率 (ST7789V3, 240x280, 顶部 20 行为假像素) */
#define LCD_H_RES   240
#define LCD_V_RES   280

/* 初始化 SPI + esp_lcd ST7789 面板 + 背光 */
esp_err_t lcd_init(void);

/* 获取面板句柄 (供上层绘图/LVGL 使用) */
esp_lcd_panel_handle_t lcd_get_panel(void);

/* 背光亮度: level 0~255 */
void lcd_set_backlight(uint8_t level);

/* 整屏填充 RGB565 颜色 (刷屏) */
esp_err_t lcd_fill_screen(uint16_t color);

/* 同步绘制位图 (LVGL flush 用): 阻塞到 DMA 传输完成再返回 */
esp_err_t lcd_draw_bitmap_sync(int x_start, int y_start, int x_end, int y_end,
                               const void *color_data);

/* 异步刷屏完成回调 (在 SPI DMA 完成中断里被调用) */
typedef void (*lcd_flush_done_cb_t)(void *user_ctx);

/* 注册异步刷屏完成回调 (单槽, 传 NULL 取消) */
void lcd_set_flush_done_cb(lcd_flush_done_cb_t cb, void *user_ctx);

/* 异步绘制位图 (LVGL 双缓冲用): 入队即返回, 传输完成时回调 cb */
esp_err_t lcd_draw_bitmap_async(int x_start, int y_start, int x_end, int y_end,
                                const void *color_data);

#ifdef __cplusplus
}
#endif
