/*
 * 移植自参考工程 fullscreen_interfaces.h (仅保留主界面需要的部分)
 */

#ifndef FULLSCREEN_INTERFACES_H
#define FULLSCREEN_INTERFACES_H

#include <math.h>
#include <time.h>
#include <stdio.h>
#include <vector>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "driver/gpio.h"

#include "lvgl.h"
#include "board.h"
#include "atomic_utils.h"
#include "arduino_compat.h"
#include "LittleFS.h"
#include "File_Selection.h"
#include "RTCManager.h"

LV_FONT_DECLARE(chinese_24);
LV_FONT_DECLARE(time_70);
LV_FONT_DECLARE(tip_display_35);

/* 退出回调函数声明 (参考工程) */
void fs_do_exit(void);
void board_sleep(void);

/* 全屏容器 / 亮度抽屉 (定义在 ui_manager.cpp) */
extern lv_obj_t* sliding_container;
extern lv_obj_t* fullscreen_container;

#endif
