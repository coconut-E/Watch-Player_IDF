/*
 * SPDX-License-Identifier: CC0-1.0
 */

#include "sd_card.h"

#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "driver/sdmmc_host.h"

#define TAG "SD_CARD"

/* SDMMC 引脚映射 (GPIO Matrix) */
#define PIN_SD_D0       16   /* DAT0 */
#define PIN_SD_CLK      17   /* CLK  */
#define PIN_SD_CMD      18   /* CMD  */

static sdmmc_card_t *s_card    = NULL;   /* SD 卡信息 */
static bool          s_mounted = false;  /* 挂载状态 */

esp_err_t sd_card_init(void)
{
    if (s_mounted) {
        ESP_LOGW(TAG, "SD 卡已挂载, 跳过初始化");
        return ESP_OK;
    }

    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = false,
        .max_files = 5,
        .allocation_unit_size = 16 * 1024,
    };

    sdmmc_host_t host = SDMMC_HOST_DEFAULT();

    sdmmc_slot_config_t slot_config = SDMMC_SLOT_CONFIG_DEFAULT();
    slot_config.width = 1;                                  /* 1bit 模式 */
    slot_config.clk   = PIN_SD_CLK;
    slot_config.cmd   = PIN_SD_CMD;
    slot_config.d0    = PIN_SD_D0;
    slot_config.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

    esp_err_t ret = esp_vfs_fat_sdmmc_mount(SD_CARD_MOUNT_POINT, &host,
                                            &slot_config, &mount_config, &s_card);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "挂载失败 (%s)", esp_err_to_name(ret));
        s_card    = NULL;
        s_mounted = false;
        return ret;
    }

    s_mounted = true;
    ESP_LOGI(TAG, "SD 卡已挂载");
    sdmmc_card_print_info(stdout, s_card);
    return ESP_OK;
}

void sd_card_deinit(void)
{
    if (!s_mounted) {
        return;
    }

    esp_err_t ret = esp_vfs_fat_sdcard_unmount(SD_CARD_MOUNT_POINT, s_card);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "卸载失败 (%s)", esp_err_to_name(ret));
    }

    s_card    = NULL;
    s_mounted = false;
    ESP_LOGI(TAG, "SD 卡已卸载");
}

bool sd_card_is_mounted(void)
{
    return s_mounted;
}
