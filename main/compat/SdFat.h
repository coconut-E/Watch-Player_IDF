/*
 * 嫁接头: 让参考工程里的 "#include <SdFat.h>" 原样可用
 * (SdFat/FsFile/SdFile/SdFs → ESP-IDF VFS, 见 sd_fat_graft.h)
 *
 * 注意: 实现文件名用 sd_fat_graft.h, 避免与 SdFat.h 在 Windows
 *       大小写不敏感的文件系统上互相覆盖。
 */

#pragma once

#include "sd_fat_graft.h"
