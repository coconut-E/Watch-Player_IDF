/*
 * SPDX-License-Identifier: CC0-1.0
 */

#include "lcd.h"

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "driver/spi_master.h"
#include "driver/ledc.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_st7789.h"

#define TAG "LCD"

/* ─────────────── SPI 总线 / 引脚 (ST7789V3) ─────────────── */
#define LCD_SPI_HOST    SPI2_HOST
#define PIN_LCD_SCLK    12
#define PIN_LCD_MOSI    13
#define PIN_LCD_MISO    -1
#define PIN_LCD_DC      14
#define PIN_LCD_CS      10
#define PIN_LCD_RST     11
#define PIN_LCD_BL      9

#define LCD_PCLK_HZ     (80 * 1000 * 1000)

/* 屏幕顶部 20 行为假像素, 纵向偏移 20 跳过 */
#define LCD_GAP_X       0
#define LCD_GAP_Y       20

/* 刷屏分块行数 (内部 DMA 内存, 避免占用大块 PSRAM) */
#define LCD_FLUSH_LINES 60

/* ─────────────── 背光 PWM ─────────────── */
#define BL_PWM_FREQ_HZ  44100
#define BL_LEDC_MODE    LEDC_LOW_SPEED_MODE
#define BL_LEDC_TIMER   LEDC_TIMER_0
#define BL_LEDC_CHANNEL LEDC_CHANNEL_0
#define BL_LEDC_RES     LEDC_TIMER_8_BIT

static esp_lcd_panel_io_handle_t s_panel_io   = NULL;
static esp_lcd_panel_handle_t    s_panel      = NULL;
static uint16_t                 *s_line_buf   = NULL;   /* 分块刷屏缓冲 (DMA) */
static SemaphoreHandle_t         s_flush_done = NULL;

/* 异步刷屏 (LVGL 双缓冲) 完成回调 */
static lcd_flush_done_cb_t       s_done_cb       = NULL;
static void                     *s_done_cb_ctx   = NULL;
static volatile bool             s_async_pending = false;

static bool lcd_color_trans_done(esp_lcd_panel_io_handle_t io,
                                 esp_lcd_panel_io_event_data_t *edata,
                                 void *user_ctx)
{
    BaseType_t hp_task_woken = pdFALSE;
    if (s_async_pending) {
        /* 异步路径: 通知 LVGL 该段刷屏完成 (ISR 中置位/给信号量) */
        s_async_pending = false;
        if (s_done_cb) {
            s_done_cb(s_done_cb_ctx);
        }
    } else if (s_flush_done) {
        /* 同步路径: 唤醒 lcd_draw_bitmap_sync */
        xSemaphoreGiveFromISR(s_flush_done, &hp_task_woken);
    }
    return hp_task_woken == pdTRUE;
}

static esp_err_t lcd_backlight_init(void)
{
    ledc_timer_config_t timer_cfg = {
        .speed_mode      = BL_LEDC_MODE,
        .duty_resolution = BL_LEDC_RES,
        .timer_num       = BL_LEDC_TIMER,
        .freq_hz         = BL_PWM_FREQ_HZ,
        .clk_cfg         = LEDC_AUTO_CLK,
    };
    ESP_RETURN_ON_ERROR(ledc_timer_config(&timer_cfg), TAG, "ledc timer failed");

    ledc_channel_config_t ch_cfg = {
        .gpio_num   = PIN_LCD_BL,
        .speed_mode = BL_LEDC_MODE,
        .channel    = BL_LEDC_CHANNEL,
        .intr_type  = LEDC_INTR_DISABLE,
        .timer_sel  = BL_LEDC_TIMER,
        .duty       = 0,
        .hpoint     = 0,
    };
    return ledc_channel_config(&ch_cfg);
}

void lcd_set_backlight(uint8_t level)
{
    ledc_set_duty(BL_LEDC_MODE, BL_LEDC_CHANNEL, level);
    ledc_update_duty(BL_LEDC_MODE, BL_LEDC_CHANNEL);
}

esp_err_t lcd_init(void)
{
    /* 1. SPI 总线 */
    /* max_transfer_sz 至少覆盖 LVGL 单段刷屏缓冲 (1/3 屏), 保证一次 flush = 一次 SPI 事务,
     * 否则 esp_lcd 会拆块, 在 trans_queue_depth=1 下第 2 块入队会中途阻塞而破坏异步 */
    size_t lvgl_flush_bytes = (LCD_H_RES * LCD_V_RES / 3) * sizeof(uint16_t);
    size_t line_buf_bytes   = LCD_H_RES * LCD_FLUSH_LINES * sizeof(uint16_t);
    size_t max_transfer_sz  = (lvgl_flush_bytes > line_buf_bytes) ? lvgl_flush_bytes
                                                                  : line_buf_bytes;

    spi_bus_config_t buscfg = {
        .sclk_io_num     = PIN_LCD_SCLK,
        .mosi_io_num     = PIN_LCD_MOSI,
        .miso_io_num     = PIN_LCD_MISO,
        .quadwp_io_num   = -1,
        .quadhd_io_num   = -1,
        .max_transfer_sz = (int)max_transfer_sz,
    };
    ESP_RETURN_ON_ERROR(spi_bus_initialize(LCD_SPI_HOST, &buscfg, SPI_DMA_CH_AUTO),
                        TAG, "spi bus init failed");

    /* 2. Panel IO (命令/数据时序) */
    s_flush_done = xSemaphoreCreateBinary();
    ESP_RETURN_ON_FALSE(s_flush_done, ESP_ERR_NO_MEM, TAG, "sem create failed");

    esp_lcd_panel_io_spi_config_t io_config = {
        .cs_gpio_num         = PIN_LCD_CS,
        .dc_gpio_num         = PIN_LCD_DC,
        .spi_mode            = 0,
        .pclk_hz             = LCD_PCLK_HZ,
        .trans_queue_depth   = 1,
        .on_color_trans_done = lcd_color_trans_done,
        .lcd_cmd_bits        = 8,
        .lcd_param_bits      = 8,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_SPI_HOST,
                                                 &io_config, &s_panel_io),
                        TAG, "panel io init failed");

    /* 3. ST7789 面板 */
    esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = PIN_LCD_RST,
        .rgb_ele_order  = LCD_RGB_ELEMENT_ORDER_RGB,
        .data_endian    = LCD_RGB_DATA_ENDIAN_LITTLE,   /* 面板接受小端 RGB565 (图标数据无需翻转) */
        .bits_per_pixel = 16,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_st7789(s_panel_io, &panel_config, &s_panel),
                        TAG, "new panel failed");

    ESP_RETURN_ON_ERROR(esp_lcd_panel_reset(s_panel), TAG, "reset failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(s_panel), TAG, "init failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_invert_color(s_panel, true), TAG, "invert failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_set_gap(s_panel, LCD_GAP_X, LCD_GAP_Y), TAG, "set gap failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_disp_on_off(s_panel, true), TAG, "disp on failed");

    /* 4. 分块刷屏缓冲 (内部 DMA 内存) */
    s_line_buf = heap_caps_malloc(LCD_H_RES * LCD_FLUSH_LINES * sizeof(uint16_t),
                                  MALLOC_CAP_DMA);
    ESP_RETURN_ON_FALSE(s_line_buf, ESP_ERR_NO_MEM, TAG, "line buf alloc failed");

    /* 4.1 清屏为黑: ST7789 上电 GRAM 为随机值, 背光淡入前先刷黑,
     * 避免 LVGL 首帧完成前露出花屏 (随后 LVGL 首帧会覆盖)。
     * 此时 s_done_cb 尚未注册, lcd_fill_screen 走同步 DMA 路径。 */
    lcd_fill_screen(0x0000);

    /* 5. 背光 (先关, 由上层点亮) */
    ESP_RETURN_ON_ERROR(lcd_backlight_init(), TAG, "backlight init failed");
    lcd_set_backlight(0);

    ESP_LOGI(TAG, "LCD init done (%dx%d, gap=%d,%d, clk=%dMHz)",
             LCD_H_RES, LCD_V_RES, LCD_GAP_X, LCD_GAP_Y, LCD_PCLK_HZ / 1000000);
    return ESP_OK;
}

esp_err_t lcd_fill_screen(uint16_t color)
{
    ESP_RETURN_ON_FALSE(s_panel && s_line_buf && s_flush_done,
                        ESP_ERR_INVALID_STATE, TAG, "not initialized");

    size_t line_px = LCD_H_RES * LCD_FLUSH_LINES;
    for (size_t i = 0; i < line_px; i++) {
        s_line_buf[i] = color;
    }

    for (int y = 0; y < LCD_V_RES; y += LCD_FLUSH_LINES) {
        int y_end = y + LCD_FLUSH_LINES;
        if (y_end > LCD_V_RES) {
            y_end = LCD_V_RES;
        }

        xSemaphoreTake(s_flush_done, 0);   /* 清除残留信号 */
        ESP_RETURN_ON_ERROR(esp_lcd_panel_draw_bitmap(s_panel, 0, y, LCD_H_RES, y_end,
                                                      s_line_buf),
                            TAG, "draw bitmap failed");
        /* 等待本块 DMA 传输完成后再复用缓冲 */
        if (xSemaphoreTake(s_flush_done, pdMS_TO_TICKS(1000)) != pdTRUE) {
            ESP_LOGW(TAG, "flush timeout @y=%d", y);
            return ESP_ERR_TIMEOUT;
        }
    }

    return ESP_OK;
}

esp_lcd_panel_handle_t lcd_get_panel(void)
{
    return s_panel;
}

esp_err_t lcd_draw_bitmap_sync(int x_start, int y_start, int x_end, int y_end,
                               const void *color_data)
{
    ESP_RETURN_ON_FALSE(s_panel && s_flush_done, ESP_ERR_INVALID_STATE, TAG, "not initialized");

    xSemaphoreTake(s_flush_done, 0);   /* 清除残留信号 */
    ESP_RETURN_ON_ERROR(esp_lcd_panel_draw_bitmap(s_panel, x_start, y_start, x_end, y_end,
                                                  color_data),
                        TAG, "draw bitmap failed");
    /* 等待 DMA 传输完成 (LVGL 缓冲复用前必须完成) */
    if (xSemaphoreTake(s_flush_done, pdMS_TO_TICKS(1000)) != pdTRUE) {
        ESP_LOGW(TAG, "draw bitmap timeout");
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

void lcd_set_flush_done_cb(lcd_flush_done_cb_t cb, void *user_ctx)
{
    s_done_cb     = cb;
    s_done_cb_ctx = user_ctx;
}

esp_err_t lcd_draw_bitmap_async(int x_start, int y_start, int x_end, int y_end,
                                const void *color_data)
{
    ESP_RETURN_ON_FALSE(s_panel && s_done_cb, ESP_ERR_INVALID_STATE, TAG, "not initialized");

    /* 清理可能残留的同步信号, 标记本次为异步事务, 入队后立即返回 */
    xSemaphoreTake(s_flush_done, 0);
    s_async_pending = true;
    esp_err_t err = esp_lcd_panel_draw_bitmap(s_panel, x_start, y_start, x_end, y_end,
                                              color_data);
    if (err != ESP_OK) {
        s_async_pending = false;
        ESP_LOGW(TAG, "draw bitmap async failed");
    }
    return err;
}
