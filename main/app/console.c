/*
 * 串口控制台 (移植自参考工程 music_nano/main/app/console.c)
 *
 * 通过 USB-Serial-JTAG 非阻塞读 STDIN, 逐字符拼行, 回车执行系统命令:
 *   stats | ram | psram | nvs | vbat | temp | bt [任务名] | taskmem [任务名]
 *
 * 移植说明:
 *   - 去掉参考工程的 app 命令队列与音乐命令 (scan/conn/play...), 手表无对应队列;
 *   - vbat 直接读手表的 battery_voltage / battery_percentage / is_charging;
 *   - temp 改用 ESP-IDF temperature_sensor 驱动 (ESP32-S3 无 temprature_sens_read);
 *   - stats 不再后台定时采样: 收到命令后立刻回串口 → 采一次 → 等 1s → 再采一次
 *     → 算 1s 窗口内各任务 CPU 占比并返回。
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include <stdbool.h>
#include <fcntl.h>
#include <unistd.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_heap_caps.h"
#include "esp_psram.h"
#include "esp_debug_helpers.h"
#include "esp_private/freertos_debug.h"
#include "xtensa_context.h"
#include "nvs.h"
#include "driver/temperature_sensor.h"
#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"

#include "board.h"

#define LINE_BUF_SIZE 64   /* 串口命令行缓冲长度 */

/* ─────────────── stats 命令: 收到请求后同步采样两次 (间隔 1s) ─────────────── */
static void cmd_stats(void)
{
    printf("采样中 (1s)...\n");

    UBaseType_t prev_count = uxTaskGetNumberOfTasks();
    TaskStatus_t *prev = malloc(prev_count * sizeof(TaskStatus_t));
    if (!prev) {
        printf("[stats] 内存不足\n");
        return;
    }
    uint32_t prev_total = 0;
    prev_count = uxTaskGetSystemState(prev, prev_count, &prev_total);
    TickType_t prev_tick = xTaskGetTickCount();

    vTaskDelay(pdMS_TO_TICKS(1000));   /* 采样窗口 1s */

    UBaseType_t n = uxTaskGetNumberOfTasks();
    TaskStatus_t *cur = malloc(n * sizeof(TaskStatus_t));
    if (!cur) {
        printf("[stats] 内存不足\n");
        free(prev);
        return;
    }
    uint32_t cur_total = 0;
    n = uxTaskGetSystemState(cur, n, &cur_total);
    TickType_t now = xTaskGetTickCount();

    uint32_t delta_ms = (now - prev_tick) * portTICK_PERIOD_MS;   /* 两次采样间隔 (ms) */
    uint32_t delta_total = cur_total - prev_total;                /* 窗口内总运行计数增量 */

    if (delta_total == 0) {
        printf("[stats] 测不到有效差值 (需 CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS)\n");
        free(cur);
        free(prev);
        return;
    }

    printf("\n===== CPU 占用 (~%"PRIu32"ms 窗口, 每核合计 100%%) =====\n", delta_ms);
    printf("%-20s %6s  %4s  %s\n", "任务名", "CPU%", "Prio", "Core");

    /* 先按核累计各任务运行增量, 用于按核归一化 (避免当前运行任务 >100%) */
    uint32_t core_sum[2] = { 0, 0 };
    for (UBaseType_t i = 0; i < n; i++) {
        uint32_t delta_task = 0;
        for (UBaseType_t j = 0; j < prev_count; j++) {
            if (cur[i].xHandle == prev[j].xHandle) {
                if (cur[i].ulRunTimeCounter > prev[j].ulRunTimeCounter) {
                    delta_task = cur[i].ulRunTimeCounter - prev[j].ulRunTimeCounter;
                }
                break;
            }
        }
        if (cur[i].xCoreID == 0 || cur[i].xCoreID == 1) {
            core_sum[cur[i].xCoreID] += delta_task;
        }
    }

    /* 对每个当前任务, 用句柄在两次快照间配对, 计算其运行计数增量占比 */
    for (UBaseType_t i = 0; i < n; i++) {
        uint32_t delta_task = 0;
        for (UBaseType_t j = 0; j < prev_count; j++) {
            if (cur[i].xHandle == prev[j].xHandle) {   /* 同一任务 (按句柄匹配) */
                if (cur[i].ulRunTimeCounter > prev[j].ulRunTimeCounter) {
                    delta_task = cur[i].ulRunTimeCounter - prev[j].ulRunTimeCounter;
                }
                break;
            }
        }
        /* 固定在某核的任务用该核合计做分母 (核内合计恰为 100%);
         * 无亲和性任务 (tskNO_AFFINITY) 退回全局增量做分母 */
        uint32_t denom = delta_total;
        if ((cur[i].xCoreID == 0 || cur[i].xCoreID == 1) && core_sum[cur[i].xCoreID] > 0) {
            denom = core_sum[cur[i].xCoreID];
        }
        float pct = denom ? (float)delta_task / (float)denom * 100.0f : 0.0f;
        if (pct > 100.0f) pct = 100.0f;
        printf("%-20s %5.1f%%  %4u  %d\n",
               cur[i].pcTaskName, pct, (unsigned)cur[i].uxCurrentPriority, cur[i].xCoreID);
    }
    printf("===============================\n");

    free(cur);
    free(prev);
}

/* ─────────────── ram 命令: 内部 RAM 空闲情况 ─────────────── */
static void cmd_free(void)
{
    uint32_t total = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);            /* 内部 RAM 总空闲 */
    uint32_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL); /* 最大连续块 */
    printf("[RAM] 总空闲: %"PRIu32"KB  最大连续块: %"PRIu32"KB", total / 1024, largest / 1024);
    uint32_t dma_total = heap_caps_get_free_size(MALLOC_CAP_DMA);             /* 可 DMA 内存空闲 */
    uint32_t dma_largest = heap_caps_get_largest_free_block(MALLOC_CAP_DMA);  /* 可 DMA 最大连续块 */
    printf(" DMA 空闲: %"PRIu32"KB  最大连续块: %"PRIu32"KB\n", dma_total / 1024, dma_largest / 1024);
}

/* ─────────────── psram 命令: 外部 PSRAM 空闲情况 ─────────────── */
static void cmd_psram(void)
{
    if (!esp_psram_is_initialized()) {
        printf("[PSRAM] 未启用\n");
        return;
    }
    uint32_t total = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);            /* PSRAM 总空闲 */
    uint32_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM); /* PSRAM 最大连续块 */
    printf("[PSRAM] 总空闲: %"PRIu32"KB  最大连续块: %"PRIu32"KB\n", total / 1024, largest / 1024);
}

/* ─────────────── nvs 命令: NVS 分区条目统计 ─────────────── */
static void cmd_nvs(void)
{
    nvs_stats_t st;
    esp_err_t err = nvs_get_stats(NULL, &st);   /* NULL = 默认 "nvs" 分区 */
    if (err != ESP_OK) {
        printf("[NVS] 查询失败: %s\n", esp_err_to_name(err));
        return;
    }
    uint32_t pct = st.total_entries
                 ? (uint32_t)((uint64_t)st.free_entries * 100 / st.total_entries) : 0;
    printf("[NVS] 命名空间:%u  已用条目:%u  空闲条目:%u  可用条目:%u  总条目:%u  空闲:%"PRIu32"%%\n",
           (unsigned)st.namespace_count, (unsigned)st.used_entries,
           (unsigned)st.free_entries, (unsigned)st.available_entries,
           (unsigned)st.total_entries, pct);
}

/* ─────────────── vbat 命令: 电池电压/电量/充电状态 ─────────────── */
static void cmd_vbat(void)
{
    /* 调试用, 直接读全局量 (battery 任务每 20s 更新一次) */
    printf("[电池] %.2f V  电量 %d%%  %s\n",
           battery_voltage, battery_percentage, is_charging ? "充电中" : "未充电");
}

/* ─────────────── temp 命令: ESP32-S3 内部温度 (惰性初始化) ─────────────── */
static temperature_sensor_handle_t s_tsens = NULL;

static void cmd_temp(void)
{
    if (!s_tsens) {
        temperature_sensor_config_t cfg = TEMPERATURE_SENSOR_CONFIG_DEFAULT(-10, 80);
        if (temperature_sensor_install(&cfg, &s_tsens) != ESP_OK) {
            printf("[CPU温度] 传感器初始化失败\n");
            s_tsens = NULL;
            return;
        }
        temperature_sensor_enable(s_tsens);
    }
    float c = 0.0f;
    if (temperature_sensor_get_celsius(s_tsens, &c) != ESP_OK) {
        printf("[CPU温度] 读取失败\n");
        return;
    }
    printf("[CPU温度] %.1f C\n", c);
}

/* ─────────────── bt 命令: 打印任务 backtrace ───────────────
 * 阻塞/挂起任务: 用 TCB 保存的栈顶 pxTopOfStack (pc/a1/a0) 回溯其自身栈, 精确.
 * 正在运行的任务: 快照为最近一次被切出时的上下文 (略旧, 但栈内容仍在).
 * 本任务(自身): 用实时上下文. */
static void cmd_backtrace(const char *name)
{
    TaskHandle_t h = xTaskGetHandle(name);              /* 按名字查任务句柄 */
    if (!h) {
        printf("[bt] 未找到任务: %s\n", name);
        return;
    }

    esp_backtrace_frame_t fr = {0};                     /* 回溯起点帧 (pc/sp/next_pc) */

    if (h == xTaskGetCurrentTaskHandle()) {             /* 要打印的就是控制台任务自己 */
        esp_backtrace_get_start(&fr.pc, &fr.sp, &fr.next_pc);   /* 取实时寄存器上下文 */
        printf("[bt] %s (current task)\n", name);
        esp_backtrace_print_from_frame(50, &fr, false);         /* 打印 50 帧 */
        return;
    }

    /* 其他任务: 从任务自身栈回溯, 必须冻结调度保证 TCB 一致 */
    TaskSnapshot_t snap;
    vTaskSuspendAll();                       /* 冻结调度, 与 esp_backtrace_print_all_tasks 一致 */
    BaseType_t ok = vTaskGetSnapshot(h, &snap);
    xTaskResumeAll();

    if (ok != pdTRUE) {
        printf("[bt] 任务快照失败: %s\n", name);
        return;
    }

    /* Xtensa 异常帧就在任务栈顶: 取 pc(返回地址)/a1(栈指针)/a0(链接寄存器) 重建回溯 */
    XtExcFrame *f = (XtExcFrame *)snap.pxTopOfStack;
    fr.pc = f->pc;
    fr.sp = f->a1;
    fr.next_pc = f->a0;
    printf("[bt] %s (saved stack)\n", name);
    esp_backtrace_print_from_frame(50, &fr, false);
}

/* bt 命令 (无参数): 打印所有任务 backtrace */
static void cmd_backtrace_all(void)
{
    printf("[bt] 所有任务 backtrace:\n");
    esp_backtrace_print_all_tasks(50);
}

/* ─────────────── taskmem 命令: 各任务栈峰值余量 ─────────────── */
static const char *task_state_name(eTaskState st)
{
    switch (st) {
        case eRunning:   return "Run";
        case eReady:     return "Rdy";
        case eBlocked:   return "Blk";
        case eSuspended: return "Sus";
        case eDeleted:   return "Del";
        default:         return "?";
    }
}

static void task_core_str(const TaskStatus_t *t, char *buf, size_t n)
{
    if (t->xCoreID == tskNO_AFFINITY) snprintf(buf, n, "any");
    else                              snprintf(buf, n, "%d", (int)t->xCoreID);
}

/* name==NULL 打印全部任务总览; 否则只打印该任务.
 * 只读 FreeRTOS 任务快照的栈水位, 不挂起调度器, 零额外开销. */
static void cmd_taskmem(const char *name)
{
    TaskHandle_t want = NULL;
    if (name && name[0]) {
        want = xTaskGetHandle(name);            /* 按名字查任务句柄 */
        if (!want) {
            printf("[taskmem] 未找到任务: %s\n", name);
            return;
        }
    }

    UBaseType_t nt = uxTaskGetNumberOfTasks() + 2;
    TaskStatus_t *snap = malloc(nt * sizeof(TaskStatus_t));
    if (!snap) {
        printf("[taskmem] 内存不足\n");
        return;
    }
    uint32_t total = 0;
    nt = uxTaskGetSystemState(snap, nt, &total);

    if (!want) {
        printf("\n[taskmem] 名称                 状态  优先   核  栈峰值余量\n");
    }

    for (UBaseType_t i = 0; i < nt; i++) {
        if (want && snap[i].xHandle != want) continue;   /* 只要单个任务时跳过其它 */

        const char *st = task_state_name(snap[i].eCurrentState);
        char corebuf[16];
        task_core_str(&snap[i], corebuf, sizeof(corebuf));

        if (want) {
            printf("\n[taskmem] %s: 状态=%s 优先=%u 核=%s 栈峰值余量=%u B\n",
                   snap[i].pcTaskName, st, (unsigned)snap[i].uxCurrentPriority,
                   corebuf, (unsigned)snap[i].usStackHighWaterMark);
        } else {
            printf("[taskmem] %-20s %-5s %4u %3s  %8u B\n",
                   snap[i].pcTaskName, st, (unsigned)snap[i].uxCurrentPriority,
                   corebuf, (unsigned)snap[i].usStackHighWaterMark);
        }
    }
    printf("\n");

    free(snap);
}

/* ─────────────── 控制台任务: 非阻塞读串口, 逐字符拼行, 回车执行 ─────────────── */
static void console_task(void *arg)
{
    (void)arg;
    fcntl(STDIN_FILENO, F_SETFL, O_NONBLOCK);   /* 设非阻塞读, 任务循环里轮询 */

    printf("\n=== 手表串口控制台 ===\n");
    printf("系统命令: stats | ram | psram | nvs | vbat | temp | bt [任务名] | taskmem [任务名]\n");

    char line[LINE_BUF_SIZE];   /* 当前行缓冲 */
    int  line_pos = 0;          /* 已输入字符数 */

    while (1) {
        char ch;
        while (read(STDIN_FILENO, &ch, 1) > 0) {   /* 循环读直到没有新字符 */
            if (ch == '\n' || ch == '\r') {        /* 回车: 整行命令就绪 */
                if (line_pos > 0) {
                    line[line_pos] = '\0';

                    if (strcmp(line, "stats") == 0) {
                        cmd_stats();
                    } else if (strcmp(line, "ram") == 0) {
                        cmd_free();
                    } else if (strcmp(line, "psram") == 0) {
                        cmd_psram();
                    } else if (strcmp(line, "nvs") == 0) {
                        cmd_nvs();
                    } else if (strcmp(line, "vbat") == 0) {
                        cmd_vbat();
                    } else if (strcmp(line, "temp") == 0) {
                        cmd_temp();
                    } else if (strcmp(line, "bt") == 0) {
                        cmd_backtrace_all();
                    } else if (strncmp(line, "bt ", 3) == 0) {   /* "bt <任务名>" */
                        cmd_backtrace(line + 3);
                    } else if (strcmp(line, "taskmem") == 0) {
                        cmd_taskmem(NULL);
                    } else if (strncmp(line, "taskmem ", 8) == 0) {   /* "taskmem <任务名>" */
                        cmd_taskmem(line + 8);
                    } else {
                        printf("未知命令: %s\n", line);
                    }

                    line_pos = 0;
                }
            } else if (ch == '\b' || ch == 127) {   /* 退格: 回退一个字符 */
                if (line_pos > 0) line_pos--;
            } else if (line_pos < LINE_BUF_SIZE - 1) {   /* 普通字符: 追加到行缓冲 */
                line[line_pos++] = ch;
            }
        }

        vTaskDelay(pdMS_TO_TICKS(50));   /* 轮询间隔 50ms, 避免空转占满 CPU */
    }
}

/* 控制台初始化: 创建控制台任务 (固定 core 1 / 优先级 1) */
void console_init(void)
{
    /* 安装 USB-Serial-JTAG 驱动并把 VFS 切到驱动模式。
     * 默认(无驱动)模式 RX 只有 64B 硬件 FIFO, 靠应用 read() 排空; 主机连续写入
     * 填满 FIFO 后会对主机 NAK, 导致 idf.py monitor 写超时并告警
     * "Writing to serial is timing out / 应用是否支持交互式 console"。
     * 驱动模式由 ISR 把数据搬进环形缓冲, 主机写入立即被接收, 不再超时。 */
    usb_serial_jtag_driver_config_t usj_cfg = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT(); /* 256/256 */
    if (usb_serial_jtag_driver_install(&usj_cfg) == ESP_OK) {
        usb_serial_jtag_vfs_use_driver();
    }

    xTaskCreatePinnedToCore(console_task, "sys_serial", 4096, NULL, 1, NULL, 1);
}
