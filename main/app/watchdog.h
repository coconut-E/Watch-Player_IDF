/*
 * 硬件看门狗 (移植自参考工程 watch.ino 的 hw_timer 看门狗)
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/* 启动看门狗: 10s 内未被 feed() 刷新则 esp_restart() (对应 watch.ino setup()) */
void watchdog_init(void);

/* 喂狗: 重新计满 10s (对应 watch.ino loop() 的 timerWrite(watchdog_timer, 0)) */
void watchdog_feed(void);

#ifdef __cplusplus
}
#endif
