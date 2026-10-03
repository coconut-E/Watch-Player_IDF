/*
 * 板级全局/函数 (对应参考工程 watch.ino 的全局变量与板级函数)
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ─────────────── 引脚 (与参考工程 fullscreen_interfaces.h 一致) ─────────────── */
#define BUTTON_1        6
#define BUTTON_2        7
#define BUTTON_3        8
#define SD_DETECT_PIN   15
#define BATTERY_ADC_PIN 1
#define CHARGE_DETECT_PIN 39

/* 外设电源使能 (低有效): 屏幕 + SD 共用。
 * 参考工程里是散落的字面量 5 (setup() 拉低开启, sleep() 拉高关断并 hold) */
#define PERIPH_PWR_PIN  5

#define ADC_MAX_VALUE        4096.0f
#define ADC_REF_VOLTAGE      3.0f
#define VOLTAGE_DIVIDER_RATIO 2.0f
#define BATTERY_FULL_V       4.15f
#define BATTERY_EMPTY_V      3.3f
#define FILTER_ALPHA         0.3f

/* I2S 引脚 (音频, 暂未使用) */
#define I2S_BCLK  47
#define I2S_LRCLK 40
#define I2S_DIN   48

/* ─────────────── SD 卡状态 (参考 watch.ino) ─────────────── */
extern bool sd_card_inserted;
extern bool sd_card_initialized;
extern int  sd_card_insertion_event;
extern bool sd_card_init_event;   /* SD 初始化失败事件 */
extern bool sd_card_scan_event;
extern bool sd_force_scan;
extern bool sd_card_warming;      /* SD 卡预热中 (预写入) */
extern SemaphoreHandle_t sd_state_mutex;
extern QueueHandle_t scan_queue;

/* ─────────────── SPI 总线互斥 (参考 watch.ino) ───────────────
 * 参考工程里屏幕与 SD 共用 SPI, 用该信号量互斥。
 * 本移植 SD 走 SDMMC、屏幕走 SPI2, 实际无总线竞争,
 * 仅为保持 fs_video.cpp 等源码原样而保留。 */
extern SemaphoreHandle_t spi_bus_sem;

/* ─────────────── 电池监测 (参考 watch.ino) ─────────────── */
extern float battery_voltage;
extern int   battery_percentage;
extern volatile float Calibration;
extern volatile bool is_charging;

/* ─────────────── 启动动画 (参考 watch.ino) ─────────────── */
extern volatile int slow_start_exit_flag;
extern volatile int slow_start_completed;
extern volatile int current_brightness;

/* ─────────────── 倒计时 (参考 fullscreen_interfaces.h) ─────────────── */
extern volatile bool g_countdown_active;
extern volatile int  g_countdown_remaining;
extern volatile int  g_countdown_total;

/* ─────────────── 队列数据结构 (参考 ui_manager.h) ─────────────── */
typedef struct {
    int category_index;      /* 类别索引 */
    int current_count;       /* 当前计数 */
    char category_name[32];  /* 类别名称 */
} ScanData;

/* 扫描进度回调 (参考 watch.ino) */
void onScanProgress(int categoryIndex, int currentCount, const char* categoryName);

/* 亮度映射 (参考 watch.ino) */
void set_mapped_brightness(uint8_t input);

/* 深睡 (参考 watch.ino sleep(): 关背光/关外设电源/配置 BUTTON_2 唤醒/进入深睡, 不返回) */
void board_sleep(void);

/* 板级初始化: NVS / 电源 / 全局互斥量 */
void board_init(void);

#ifdef __cplusplus
}
#endif
