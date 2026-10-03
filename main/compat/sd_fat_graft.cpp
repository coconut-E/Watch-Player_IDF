/*
 * Arduino SdFat → ESP-IDF VFS(FATFS on SDMMC) 嫁接层: 全局卷对象
 * (参考工程 watch.ino / fullscreen_interfaces.h 里的 "SdFs sd;" / "SdFat sd;")
 */

#include "sd_fat_graft.h"

SdFs sd;
