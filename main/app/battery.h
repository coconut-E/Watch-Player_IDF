/*
 * 电池电量 + 充电检测 (移植自参考工程 watch.ino 的 battery_monitor_task)
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/* 启动电池监测任务 (对应 watch.ino setup() 里的 xTaskCreatePinnedToCore) */
void battery_monitor_start(void);

#ifdef __cplusplus
}
#endif
