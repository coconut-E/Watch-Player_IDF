/*
 * LVGL 底层移植 (对应参考工程 watch.ino 的 setup()/loop() 中显示与触摸部分)
 */

#pragma once

#include "esp_err.h"
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 屏幕尺寸 (与参考工程一致) */
#define TFT_HOR_RES   240
#define TFT_VER_RES   280

/* 初始化 LVGL: 显示驱动 + 触摸驱动 (对应 watch.ino setup() 的 lvgl 部分) */
void ui_core_display_init(void);

/* 启动 LVGL 主任务 (对应 watch.ino loop()) */
void ui_core_start(void);

/* 获取 LVGL 显示对象 */
lv_disp_t *ui_core_get_disp(void);

/* C 可调用入口: 初始化 LVGL + 主界面 + 启动 LVGL 任务 (定义于 ui_manager.cpp) */
void ui_start_all(void);

#ifdef __cplusplus
}
#endif
