/*
 * 硬件看门狗
 *
 * 逐字移植参考工程 watch.ino:
 *   setup():
 *       watchdog_timer = timerBegin(1000000);                 // 1MHz
 *       timerAttachInterrupt(watchdog_timer, &onWatchdogTimeout);
 *       timerAlarm(watchdog_timer, 10 * 1000000, false, 0);   // 10s 单次
 *   loop():
 *       if (watchdog_timer) { timerWrite(watchdog_timer, 0); } // 喂狗
 *   onWatchdogTimeout():
 *       esp_restart();
 *
 * 移植说明:
 *   - 用 esp_timer 单次定时器替代通用定时器;
 *   - 回调运行在 esp_timer 任务 (优先级高于普通任务), 即使 LVGL 循环卡死也能触发复位;
 *   - feed() 对应 timerWrite(): esp_timer_restart() 重新起算 10s。
 */

#include "watchdog.h"

#include "esp_timer.h"
#include "esp_system.h"
#include "esp_log.h"

#define TAG "WATCHDOG"

#define WATCHDOG_TIMEOUT_US  (10ULL * 1000 * 1000)   /* 10s, 与参考工程一致 */

static esp_timer_handle_t s_wdt = NULL;

/* 对应参考工程 onWatchdogTimeout(): esp_restart() */
static void watchdog_timeout_cb(void *arg)
{
    (void)arg;
    ESP_LOGE(TAG, "看门狗超时 (%llus), 重启", (unsigned long long)(WATCHDOG_TIMEOUT_US / 1000000));
    esp_restart();
}

void watchdog_init(void)
{
    if (s_wdt) return;

    const esp_timer_create_args_t args = {
        .callback = &watchdog_timeout_cb,
        .arg      = NULL,
        .name     = "wdt",
    };
    if (esp_timer_create(&args, &s_wdt) != ESP_OK) {
        ESP_LOGE(TAG, "看门狗定时器创建失败");
        s_wdt = NULL;
        return;
    }
    esp_timer_start_once(s_wdt, WATCHDOG_TIMEOUT_US);
    ESP_LOGI(TAG, "看门狗已启动 (%llus)", (unsigned long long)WATCHDOG_TIMEOUT_US);
}

void watchdog_feed(void)
{
    if (!s_wdt) return;
    esp_timer_restart(s_wdt, WATCHDOG_TIMEOUT_US);
}
