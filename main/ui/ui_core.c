/*
 * LVGL 底层移植 (对应参考工程 watch.ino)
 *
 * 参考 watch.ino:
 *   - my_disp_flush()      : LGFX 刷屏 → 这里改为 esp_lcd (lcd_draw_bitmap_sync)
 *   - my_touchpad_read()   : LGFX getTouch → 这里改为 CST816S (touch_read)
 *   - my_tick()            : millis()
 *   - setup() 中 lv_init/lv_disp_drv_register/lv_indev_drv_register
 *   - loop() 中 lv_timer_handler()
 */

#include "ui_core.h"

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "lcd.h"
#include "touch.h"
#include "watchdog.h"

#define TAG "UI_CORE"

/* 绘制缓冲: 单个缓冲 = 1/3 屏像素, 双缓冲 (内部 DMA 内存)
 * LVGL v8.3 的 partial 双缓冲语义: 渲染下一段与上一段 DMA 并行, 且在发下一段前等待上一段完成 */
#define DRAW_BUF_SIZE  (TFT_HOR_RES * TFT_VER_RES / 3)

static lv_color_t        *draw_buf1 = NULL;
static lv_color_t        *draw_buf2 = NULL;
static lv_disp_draw_buf_t draw_buf_dsc;
static lv_disp_drv_t      disp_drv;
static lv_disp_t         *disp = NULL;
static SemaphoreHandle_t   s_flush_sem = NULL;   /* DMA 完成信号 (供 wait_cb 阻塞等待) */

static lv_indev_drv_t     indev_drv;
static lv_indev_t        *indev = NULL;

/* SPI DMA 传输完成回调 (在 esp_lcd 的 SPI ISR 中执行) */
static void lvgl_flush_done_cb(void *user_ctx)
{
    (void)user_ctx;
    BaseType_t hp_task_woken = pdFALSE;
    lv_disp_flush_ready(&disp_drv);                       /* 通知 LVGL 本段刷屏完成 */
    if (s_flush_sem) {
        xSemaphoreGiveFromISR(s_flush_sem, &hp_task_woken);
    }
    if (hp_task_woken == pdTRUE) {
        portYIELD_FROM_ISR();
    }
}

/* LVGL 等待回调: 阻塞等待上一段 DMA 完成, 避免在 lvgl 任务里忙等空转 */
static void my_disp_wait_cb(lv_disp_drv_t *drv)
{
    (void)drv;
    if (s_flush_sem) {
        xSemaphoreTake(s_flush_sem, portMAX_DELAY);
    }
}

/* LVGL 刷新回调: 异步发起 DMA 后立即返回, 不在此调用 lv_disp_flush_ready
 * (完成由 lvgl_flush_done_cb 触发, 从而让 LVGL 在传输时渲染另一帧) */
static void my_disp_flush(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *color_p)
{
    (void)drv;
    lcd_draw_bitmap_async(area->x1, area->y1, area->x2 + 1, area->y2 + 1, color_p);
}

/* LVGL 触摸读取回调 (对应 watch.ino my_touchpad_read, 底层换为 CST816S) */
static void my_touchpad_read(lv_indev_drv_t *indev_drv, lv_indev_data_t *data)
{
    int x = 0, y = 0;
    bool touched = touch_read(&x, &y);
    data->state = touched ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
    if (touched) {
        data->point.x = (lv_coord_t)x;
        data->point.y = (lv_coord_t)y;
    }
}

/* 系统时基 (对应 watch.ino my_tick, 当前 LV_TICK_CUSTOM 已直接用 esp_timer) */
static uint32_t __attribute__((unused)) my_tick(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000LL);
}

void ui_core_display_init(void)
{
    /* 双绘制缓冲 (内部 DMA): 各 1/3 屏, 支持渲染与刷屏并行 */
    draw_buf1 = (lv_color_t *)heap_caps_malloc(DRAW_BUF_SIZE * sizeof(lv_color_t), MALLOC_CAP_DMA);
    draw_buf2 = (lv_color_t *)heap_caps_malloc(DRAW_BUF_SIZE * sizeof(lv_color_t), MALLOC_CAP_DMA);
    if (!draw_buf1 || !draw_buf2) {
        ESP_LOGE(TAG, "draw buffer alloc failed");
        return;
    }

    s_flush_sem = xSemaphoreCreateBinary();
    if (!s_flush_sem) {
        ESP_LOGE(TAG, "flush sem create failed");
        return;
    }

    lv_init();

    /* 触摸 (CST816S) */
    touch_init();

    /* 显示驱动 (双缓冲 + 异步 DMA 刷屏) */
    lv_disp_draw_buf_init(&draw_buf_dsc, draw_buf1, draw_buf2, DRAW_BUF_SIZE);
    lv_disp_drv_init(&disp_drv);
    disp_drv.hor_res   = TFT_HOR_RES;
    disp_drv.ver_res   = TFT_VER_RES;
    disp_drv.flush_cb  = my_disp_flush;
    disp_drv.wait_cb   = my_disp_wait_cb;
    disp_drv.draw_buf  = &draw_buf_dsc;
    disp_drv.sw_rotate = 1;
    disp_drv.rotated   = LV_DISP_ROT_NONE;
    disp = lv_disp_drv_register(&disp_drv);

    /* 注册底层异步刷屏完成回调 -> 触发 lv_disp_flush_ready */
    lcd_set_flush_done_cb(lvgl_flush_done_cb, NULL);

    /* 触摸驱动 */
    lv_indev_drv_init(&indev_drv);
    indev_drv.type    = LV_INDEV_TYPE_POINTER;
    indev_drv.read_cb = my_touchpad_read;
    indev = lv_indev_drv_register(&indev_drv);

    /* 屏幕背景 (对应 watch.ino) */
    lv_obj_set_style_bg_color(lv_scr_act(), lv_color_black(), LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(lv_scr_act(), LV_OPA_COVER, LV_STATE_DEFAULT);

    ESP_LOGI(TAG, "LVGL display/indev ready (%dx%d)", TFT_HOR_RES, TFT_VER_RES);
}

/* LVGL 主循环 (对应 watch.ino loop()) */
static void ui_core_task(void *arg)
{
    while (1) {
        /* 喂狗 (对应 watch.ino loop() 的 timerWrite(watchdog_timer, 0)) */
        watchdog_feed();
        lv_timer_handler();
        vTaskDelay(pdMS_TO_TICKS(2));
    }
}

void ui_core_start(void)
{
    xTaskCreatePinnedToCore(ui_core_task, "lvgl", 8192, NULL, 2, NULL, 1);
}

lv_disp_t *ui_core_get_disp(void)
{
    return disp;
}
