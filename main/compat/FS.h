/*
 * 嫁接头: 让参考工程里的 "#include <FS.h>" 原样可用 (提供 Arduino 的 File 类)
 *
 * 注意: 本文件刻意不定义 Arduino 的 "FS_H" 宏。
 *       JPEGDEC.h 里有:
 *           #if defined(__has_include) && __has_include(<FS.h>)  → #include "FS.h"
 *           #ifdef FS_H  →  int open(File &file, JPEG_DRAW_CALLBACK *pfnDraw);
 *       不定义 FS_H 就不会声明那个基于 File 的 open(), 避免引入无用依赖。
 */

#pragma once

#include "fs_shim.h"
