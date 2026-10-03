/*
 * 串口控制台 (移植自参考工程 music_nano/main/app/console.c)
 *
 * 通过 USB-Serial-JTAG 非阻塞读 STDIN, 逐字符拼行, 回车执行系统命令:
 *   stats | ram | psram | nvs | vbat | temp | bt [任务名] | taskmem [任务名]
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/* 启动串口控制台: 创建 1s 统计采样定时器 + 控制台任务 */
void console_init(void);

#ifdef __cplusplus
}
#endif
