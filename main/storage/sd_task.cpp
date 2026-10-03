/*
 * SD 卡任务
 * 移植自参考工程 watch.ino 的 pre_write_warm_sd_card() / initialize_sd_card() / sd_init_task()
 *
 * 移植说明:
 *   - 底层由 SdFat(SPI) 改为 SDMMC + FATFS (见 sd_card.c / sd_scan.cpp);
 *   - 参考工程用 spi_busy_flag 互斥 SPI (屏幕与 SD 共用总线), 我们用 SDMMC 不共用, 故去掉;
 *   - sd_init_task 的 "外层 while + 内层 while" 结构合并为单层循环 (语义等价)。
 */

#include "sd_task.h"

#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

#include "arduino_compat.h"
#include "atomic_utils.h"
#include "board.h"
#include "sd_card.h"
#include "sd_scan.h"
#include "ui_manager.h"

#define TAG "SD_TASK"

#define WARM_FILE  SD_CARD_MOUNT_POINT "/warm_sd.tmp"

// 预写入函数
void pre_write_warm_sd_card() {
    if (xSemaphoreTake(sd_state_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        sd_card_warming = true;
        xSemaphoreGive(sd_state_mutex);
    }
    // 预写文件
    const char* warm_file = WARM_FILE;
    FILE* warmFile = fopen(warm_file, "w");
    Serial.printf("%lu\n", millis());
    if (warmFile) {
        fputc('0', warmFile);
        fflush(warmFile);
        fclose(warmFile);
        //删除临时文件
        remove(warm_file);
        Serial.printf("%lu\n", millis());
    } else {
        Serial.println("SD卡预写入失败");
    }
    if (xSemaphoreTake(sd_state_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        sd_card_warming = false;
        xSemaphoreGive(sd_state_mutex);
    }
}

// ==================== SD卡初始化 ====================
bool initialize_sd_card() {
    bool init_result = false;
    setScanProgressCallback(onScanProgress);

    vTaskDelay(pdMS_TO_TICKS(10));
    bool sd_init_ok = initializeSDCard();

    if (!sd_init_ok) {
        Serial.println("SD卡初始化失败");
        if (xSemaphoreTake(sd_state_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
            sd_card_initialized = false;
            sd_card_init_event = true;
            xSemaphoreGive(sd_state_mutex);
        }
        init_result = false;
    } else {
        Serial.println("SD卡初始化成功");

        // 预写入
        //pre_write_warm_sd_card();

        init_result = true;
        ensureDirectoriesAndFiles();
        if (checkNeedRescan()) {
            if (xSemaphoreTake(sd_state_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
                sd_card_scan_event = true;
                xSemaphoreGive(sd_state_mutex);
            }
            if (scan_queue != NULL) xQueueReset(scan_queue);
            performScan();
            if (xSemaphoreTake(sd_state_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
                sd_card_scan_event = false;
                xSemaphoreGive(sd_state_mutex);
            }
        }
        // 设置SD卡初始化成功状态
        if (xSemaphoreTake(sd_state_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
            sd_card_initialized = true;
            xSemaphoreGive(sd_state_mutex);
        }
    }
    return init_result;
}

// SD 卡检测任务 (GPIO15: 低=已插入)
void sd_init_task(void* parameter) {
    /* SD 检测脚: 内部上拉 (对应 watch.ino setup() 的 pinMode(SD_DETECT_PIN, INPUT_PULLUP))
     * 无卡=高, 插入(接地)=低; 不配置会悬空导致误判 */
    pinMode(SD_DETECT_PIN, INPUT_PULLUP);

    uint8_t last_sd_pin_state = HIGH;

    vTaskDelay(pdMS_TO_TICKS(200));

    while (atomic_load_bool(&is_fullscreen_container_active)) {
        vTaskDelay(pdMS_TO_TICKS(200));
    }

    if (digitalRead(SD_DETECT_PIN) == LOW) {
        vTaskDelay(pdMS_TO_TICKS(200));
        Serial.println("SD卡插入");
        if (xSemaphoreTake(sd_state_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
            sd_card_inserted = true;
            xSemaphoreGive(sd_state_mutex);
        }
        initialize_sd_card();
    } else {
        Serial.println("未检测到SD卡");
        if (xSemaphoreTake(sd_state_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
            sd_card_inserted = false;
            sd_card_initialized = false;
            xSemaphoreGive(sd_state_mutex);
        }
    }

    last_sd_pin_state = digitalRead(SD_DETECT_PIN);

    while (1) {
        // 强制重新扫描
        bool need_force_scan = false;
        if (xSemaphoreTake(sd_state_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
            need_force_scan = sd_force_scan;
            if (need_force_scan) sd_force_scan = false;
            xSemaphoreGive(sd_state_mutex);
        }
        if (need_force_scan) {
            Serial.println("触发SD卡重新扫描");
            if (xSemaphoreTake(sd_state_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
                sd_card_scan_event = true;
                xSemaphoreGive(sd_state_mutex);
            }
            if (scan_queue) xQueueReset(scan_queue);
            performScan();
            if (xSemaphoreTake(sd_state_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
                sd_card_scan_event = false;
                xSemaphoreGive(sd_state_mutex);
            }
        }

        uint8_t curr_sd_pin_state = digitalRead(SD_DETECT_PIN);
        vTaskDelay(pdMS_TO_TICKS(200));
        if (last_sd_pin_state == HIGH && curr_sd_pin_state == LOW) {
            Serial.println("SD卡插入");
            if (xSemaphoreTake(sd_state_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
                sd_card_insertion_event = 1;
                sd_card_inserted = true;
                xSemaphoreGive(sd_state_mutex);
            }
            vTaskDelay(pdMS_TO_TICKS(200));   // 等卡上电稳定再初始化
            initialize_sd_card();
        } else if (last_sd_pin_state == LOW && curr_sd_pin_state == HIGH) {
            Serial.println("[SD检测] 上升沿触发,SD卡拔出");
            if (xSemaphoreTake(sd_state_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
                sd_card_insertion_event = -1;
                sd_card_inserted = false;
                sd_card_initialized = false;
                xSemaphoreGive(sd_state_mutex);
            }
            sd_card_deinit();   // 拔卡卸载, 避免下次插卡复用旧卡对象
        }
        last_sd_pin_state = curr_sd_pin_state;
    }
}

void sd_task_start(void)
{
    xTaskCreatePinnedToCore(sd_init_task, "SD Init", 4096, NULL, 1, NULL, 0);
}
