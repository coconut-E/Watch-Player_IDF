/*
 * SPDX-FileCopyrightText: 2010-2022 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: CC0-1.0
 */

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_sleep.h"

#include "board.h"
#include "lcd.h"
#include "sd_task.h"
#include "ui_core.h"
#include "watchdog.h"
#include "console.h"

static const char *TAG = "APP";

/* 外设电源使能 (低有效): 屏幕 + SD 共用 (对应参考 watch.ino setup() 开头)
 *   参考: gpio_hold_dis(5); gpio_deep_sleep_hold_dis(); pinMode(5,OUTPUT); digitalWrite(5,LOW);
 * 必须先把深睡时锁存的电平放开, 否则唤醒后 GPIO5 会一直保持高电平(断电态),
 * 屏幕和 SD 永远不上电。 */
static void periph_power_enable(void)
{
    gpio_hold_dis((gpio_num_t)PERIPH_PWR_PIN);
    gpio_deep_sleep_hold_dis();

    gpio_config_t pwr_conf = {
        .pin_bit_mask = (1ULL << PERIPH_PWR_PIN),
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&pwr_conf);
    gpio_set_level((gpio_num_t)PERIPH_PWR_PIN, 0);
    vTaskDelay(pdMS_TO_TICKS(20));   /* 等待外设电源稳定 */
}

static const char *wakeup_cause_str(esp_sleep_wakeup_cause_t cause)
{
    switch (cause) {
        case ESP_SLEEP_WAKEUP_EXT0:  return "EXT0 (BUTTON_2)";
        case ESP_SLEEP_WAKEUP_EXT1:  return "EXT1";
        case ESP_SLEEP_WAKEUP_TIMER: return "TIMER";
        case ESP_SLEEP_WAKEUP_TOUCHPAD: return "TOUCHPAD";
        case ESP_SLEEP_WAKEUP_ULP:   return "ULP";
        case ESP_SLEEP_WAKEUP_UNDEFINED:
        default:                     return "上电/复位 (非深睡唤醒)";
    }
}

void app_main(void)
{
    esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();
    ESP_LOGI(TAG, "唤醒原因: %s", wakeup_cause_str(cause));

    ESP_LOGI(TAG, "使能外设电源 (GPIO%d 拉低, 并释放深睡锁存)", PERIPH_PWR_PIN);
    periph_power_enable();

    board_init();

    ESP_LOGI(TAG, "启动 LCD 初始化");
    if (lcd_init() != ESP_OK) {
        ESP_LOGE(TAG, "LCD 初始化失败");
    }

    ESP_LOGI(TAG, "启动 LVGL + 主界面");
    ui_start_all();

    /* 硬件看门狗 (对应 watch.ino setup() 里 timerBegin/timerAlarm,
     * 由 LVGL 任务循环喂狗) —— 必须在 ui_start_all() 之后启动,
     * 否则 LVGL 任务尚未开始喂狗就可能被 10s 超时复位 */
    watchdog_init();

    /* 串口控制台 (USB-Serial-JTAG): stats/ram/psram/nvs/vbat/temp/bt/taskmem */
    ESP_LOGI(TAG, "启动串口控制台");
    console_init();

    /* 注意: 背光淡入 (boot_anim_start) 已移入 ui_start_all() 的最后一步,
     * 确保 UI 初始化完成后才开始淡入, 不再早于 LVGL 首帧 */

    ESP_LOGI(TAG, "启动 SD 卡检测/扫描 (GPIO15)");
    sd_task_start();
}
