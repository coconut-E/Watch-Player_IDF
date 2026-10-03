/*
 * 开机背光淡入动画 + 恢复亮度
 *
 * 逐字移植参考工程 watch.ino 的 slow_start_task():
 *     preferences.begin("watch", true);
 *     int target_linear = preferences.getUChar("brightness", 128);
 *     preferences.end();
 *     int steps = 80;
 *     display.setBrightness(0);
 *     for (int i = 1; i <= steps; i++) {
 *         if (atomic_load_int(&slow_start_exit_flag)) { vTaskDelete(NULL); return; }
 *         int current_linear = (target_linear * i) / steps;
 *         set_mapped_brightness(current_linear);
 *         atomic_store_int(&current_brightness, current_linear);
 *         vTaskDelay(pdMS_TO_TICKS(5));
 *     }
 *     set_mapped_brightness(target_linear);
 *     atomic_store_int(&current_brightness, target_linear);
 *     atomic_store_int(&slow_start_completed, 1);
 *     vTaskDelete(NULL);
 *
 * 放在 C++ 文件里是因为要读 preferences (NVS 封装)。
 * 任务创建方式与参考一致: core 1 / 优先级 1 / 2048 字节栈。
 * slow_start_exit_flag 由 fs_time.cpp 的 detect_timer_cb 在 BUTTON_2 松开时置 1,
 * 用于打断这段淡入(用户已经开始操作了)。
 */

#include "board.h"
#include "lcd.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "arduino_compat.h"   /* preferences / atomic_* */
#include "atomic_utils.h"

#define BOOT_ANIM_STEPS      80   /* 80 x 5ms = 400ms 淡入 */
#define BOOT_ANIM_STEP_MS    5

static void boot_anim_task(void *parameter)
{
    (void)parameter;

    preferences.begin("watch", true);
    int target_linear = preferences.getUChar("brightness", 128);
    preferences.end();

    lcd_set_backlight(0);
    atomic_store_int(&current_brightness, 0);

    for (int i = 1; i <= BOOT_ANIM_STEPS; i++) {
        if (atomic_load_int(&slow_start_exit_flag)) {
            vTaskDelete(NULL);
            return;
        }
        int current_linear = (target_linear * i) / BOOT_ANIM_STEPS;
        set_mapped_brightness(current_linear);
        atomic_store_int(&current_brightness, current_linear);
        vTaskDelay(pdMS_TO_TICKS(BOOT_ANIM_STEP_MS));
    }

    set_mapped_brightness(target_linear);
    atomic_store_int(&current_brightness, target_linear);
    atomic_store_int(&slow_start_completed, 1);

    vTaskDelete(NULL);
}

extern "C" void boot_anim_start(void)
{
    xTaskCreatePinnedToCore(boot_anim_task, "SlowStart", 2048, NULL, 1, NULL, 1);
}
