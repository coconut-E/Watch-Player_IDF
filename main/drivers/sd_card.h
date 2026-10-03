/*
 * SPDX-License-Identifier: CC0-1.0
 */

#pragma once

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SD_CARD_MOUNT_POINT "/sdcard"

/* 初始化并挂载 SD 卡 (SDMMC, 1bit) */
esp_err_t sd_card_init(void);

/* 卸载 SD 卡 */
void sd_card_deinit(void);

/* 查询 SD 卡是否已挂载 */
bool sd_card_is_mounted(void);

#ifdef __cplusplus
}
#endif
