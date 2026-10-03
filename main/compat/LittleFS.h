/*
 * 嫁接头: 让参考工程里的 "#include <LittleFS.h>" 原样可用
 * (LittleFS/File → SD 卡 VFS, 见 fs_shim.h)
 */

#pragma once

#include "fs_shim.h"
