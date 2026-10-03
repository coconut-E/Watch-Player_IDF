/*
 * 板级全局/函数 (对应参考工程 watch.ino)
 */

#include "board.h"

#include <math.h>
#include <string.h>
#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "driver/rtc_io.h"
#include "esp_sleep.h"
#include "esp_log.h"
#include "nvs_flash.h"

#include "lcd.h"
#include "battery.h"

#define TAG "BOARD"

/* 外部 RTC 初始化 (实现在 app/rtc_init.cpp)
 * 注意: 不能叫 rtc_init, 会和 IDF esp_hw_support 里的 rtc_init() 撞名 */
extern void rtc_manager_init(void);

/* ─────────────── SD 卡状态 ─────────────── */
bool sd_card_inserted = false;
bool sd_card_initialized = false;
int  sd_card_insertion_event = 0;
bool sd_card_init_event = false;
bool sd_card_scan_event = false;
bool sd_force_scan = false;
bool sd_card_warming = false;
SemaphoreHandle_t sd_state_mutex = NULL;
QueueHandle_t scan_queue = NULL;
SemaphoreHandle_t spi_bus_sem = NULL;

/* ─────────────── 电池监测 ─────────────── */
float battery_voltage = 0.0f;
int   battery_percentage = 0;
volatile float Calibration = 100;
volatile bool is_charging = false;

/* ─────────────── 启动动画 ─────────────── */
volatile int slow_start_exit_flag = 0;
volatile int slow_start_completed = 0;
volatile int current_brightness = 0;

/* ─────────────── 倒计时 ─────────────── */
/* g_countdown_active / g_countdown_remaining / g_countdown_total / g_countdown_paused
 * 定义在 fs_stopwatch.cpp (对应参考工程) */

/* 扫描进度 → 队列 (参考 watch.ino onScanProgress) */
void onScanProgress(int categoryIndex, int currentCount, const char* categoryName)
{
    ScanData scan_data;
    scan_data.category_index = categoryIndex;
    scan_data.current_count = currentCount;
    if (categoryName) {
        strncpy(scan_data.category_name, categoryName, sizeof(scan_data.category_name) - 1);
        scan_data.category_name[sizeof(scan_data.category_name) - 1] = '\0';
    } else {
        scan_data.category_name[0] = '\0';
    }
    if (scan_queue) xQueueSend(scan_queue, &scan_data, 0);
}

/* 亮度映射: 线性输入 → 指数输出 (参考 watch.ino set_mapped_brightness) */
void set_mapped_brightness(uint8_t input)
{
    if (input == 0) {
        lcd_set_backlight(0);
        return;
    }
    const float k = 0.0175f;
    uint8_t output = (uint8_t)((expf(input * k) - 1.0f) / (expf(255 * k) - 1.0f) * 255.0f);
    if (output == 0 && input > 0) output = 1;
    lcd_set_backlight(output);
}

/* 深睡 (逐字移植参考 watch.ino sleep())
 *
 * 顺序与参考工程一致:
 *   1. 关背光 (display.setBrightness(0))
 *   2. 使能深睡保持 → GPIO5 拉高(关断外设电源: 屏幕+SD) → hold 住该电平
 *   3. BUTTON_2 交给 RTC IO, 输入+内部下拉, 高电平(按下)唤醒
 *   4. 进入深睡 (不返回; 唤醒 = 完整重启, app_main 会重新执行)
 *
 * 注意: 唤醒后 main.c 的 periph_power_enable() 必须先 gpio_hold_dis() 释放这里的
 *       电平锁存, 否则 GPIO5 会一直保持高电平, 屏幕/SD 永远不上电。
 */
void board_sleep(void)
{
    lcd_set_backlight(0);

    gpio_deep_sleep_hold_en();
    gpio_set_level((gpio_num_t)PERIPH_PWR_PIN, 1);   /* 关断外设电源 (低有效) */
    gpio_deep_sleep_hold_en();
    gpio_hold_en((gpio_num_t)PERIPH_PWR_PIN);

    rtc_gpio_deinit((gpio_num_t)BUTTON_2);
    rtc_gpio_set_direction((gpio_num_t)BUTTON_2, RTC_GPIO_MODE_INPUT_ONLY);
    rtc_gpio_pulldown_en((gpio_num_t)BUTTON_2);
    rtc_gpio_pullup_dis((gpio_num_t)BUTTON_2);
    esp_sleep_enable_ext0_wakeup((gpio_num_t)BUTTON_2, 1);   /* 按下 BUTTON_2 = 高电平 */

    fflush(stdout);          /* 对应参考工程的 Serial.flush(): 把日志吐出去 */
    esp_deep_sleep_start();
}

/* 板级初始化 */
void board_init(void)
{
    /* NVS (Preferences 使用) */
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }

    sd_state_mutex = xSemaphoreCreateMutex();
    scan_queue = xQueueCreate(10, sizeof(ScanData));

    /* SPI 总线互斥 (参考 watch.ino: 创建二值信号量并先 Give 一次) */
    spi_bus_sem = xSemaphoreCreateBinary();
    if (spi_bus_sem) xSemaphoreGive(spi_bus_sem);

    /* 按键/检测脚: 输入, 内部无上下拉 (参考 watch.ino)
     * 先从 RTC IO 交还 BUTTON_2 —— 深睡唤醒后它可能还挂在 RTC IO 上
     * (参考工程没做这步, 依赖 gpio_config 自动复位; 这里显式做更稳) */
    rtc_gpio_deinit((gpio_num_t)BUTTON_2);

    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << BUTTON_1) | (1ULL << BUTTON_2) | (1ULL << BUTTON_3),
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&io_conf);

    /* 电池电量 + 充电检测任务 (对应 watch.ino setup()) */
    battery_monitor_start();

    /* 外部 RTC (RX8130CE, I2C1 SDA41/SCL42) 初始化 + 同步系统时间
     * (对应 watch.ino setup() 里 rtcManager.begin()/syncToSystem(),
     *  必须排在 LVGL/UI 之前, 否则开机时钟界面读不到正确时间) */
    rtc_manager_init();
}
