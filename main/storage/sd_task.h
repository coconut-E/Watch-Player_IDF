/*
 * SD 卡任务 (移植自参考工程 watch.ino 的 SD 部分)
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/* 启动 SD 卡检测/初始化任务 (对应 watch.ino setup() 里的 xTaskCreatePinnedToCore(sd_init_task,...)) */
void sd_task_start(void);

#ifdef __cplusplus
}
#endif
