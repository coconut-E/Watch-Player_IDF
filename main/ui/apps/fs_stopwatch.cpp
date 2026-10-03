#include "fullscreen_interfaces.h"

// ==================== 类型定义 ====================

// 秒表状态
typedef enum {
    STOPWATCH_STOPPED,
    STOPWATCH_RUNNING,
    STOPWATCH_PAUSED
} stopwatch_state_t;

// 倒计时状态
typedef enum {
    TIMER_STOPPED,
    TIMER_RUNNING,
    TIMER_PAUSED,
    TIMER_FINISHED
} timer_state_t;

// 秒表数据结构
typedef struct {
    lv_obj_t* time_label;
    lv_obj_t* start_btn;
    lv_obj_t* reset_btn;
    lv_obj_t* pause_btn;
    
    uint32_t start_time;
    uint32_t paused_time;
    uint32_t elapsed_time;
    stopwatch_state_t state;
    
    lv_timer_t* timer;
} stopwatch_t;

// 倒计时数据结构
typedef struct {
    lv_obj_t* title_label;   
    lv_obj_t* min_roller;
    lv_obj_t* sec_roller;
    lv_obj_t* colon_label;
    lv_obj_t* time_label;    
    lv_obj_t* arc;           
    
    lv_obj_t* start_btn;
    lv_obj_t* pause_btn;
    lv_obj_t* reset_btn;

    uint32_t total_seconds;   // 总设定的秒数（仅用于UI进度计算）
    uint32_t remain_seconds;  // 保留未使用，实际从原子变量读取
    timer_state_t state;
    lv_timer_t* timer;
} countdown_t;

// 倒计时命令枚举
typedef enum {
    COUNTDOWN_CMD_PAUSE,
    COUNTDOWN_CMD_RESUME,
    COUNTDOWN_CMD_RESET
} countdown_cmd_t;

// ==================== 全局变量 ====================

static stopwatch_t stopwatch;
static countdown_t countdown;

// 倒计时 RTOS 任务相关
static TaskHandle_t countdown_task = NULL;
static QueueHandle_t countdown_cmd_queue = NULL;

// 原子变量（供外部使用）
volatile bool g_countdown_active = false;
volatile int  g_countdown_remaining = 0;
volatile int  g_countdown_total = 0;       // 总毫秒数
volatile bool g_countdown_paused = false;  // 暂停状态

// GPIO6检测相关
static bool last_BUTTON2_level = false;
static uint32_t stable_start_ms = 0;
static lv_timer_t* BUTTON2_timer = NULL;

static const char* roller_options = 
    "00\n01\n02\n03\n04\n05\n06\n07\n08\n09\n"
    "10\n11\n12\n13\n14\n15\n16\n17\n18\n19\n"
    "20\n21\n22\n23\n24\n25\n26\n27\n28\n29\n"
    "30\n31\n32\n33\n34\n35\n36\n37\n38\n39\n"
    "40\n41\n42\n43\n44\n45\n46\n47\n48\n49\n"
    "50\n51\n52\n53\n54\n55\n56\n57\n58\n59\n60";

#define DEBOUNCE_THRESHOLD 15   // 防抖阈值（ms）

// ==================== 函数前置声明 ====================

static void countdown_task_func(void *pvParameters);
static void countdown_cleanup_cb(lv_event_t* e);

// ==================== 秒表功能 ====================

static void update_time_display() {
    uint32_t hours, minutes, seconds, milliseconds;
    uint32_t total_ms;
    
    if (stopwatch.state == STOPWATCH_RUNNING) {
        total_ms = lv_tick_get() - stopwatch.start_time + stopwatch.elapsed_time;
    } else {
        total_ms = stopwatch.elapsed_time;
    }
    
    hours = total_ms / 3600000;
    minutes = (total_ms % 3600000) / 60000;
    seconds = (total_ms % 60000) / 1000;
    milliseconds = (total_ms % 1000) / 10;
    
    char time_str[32];
    if (hours > 0) {
        lv_snprintf(time_str, sizeof(time_str), "%02lu:%02lu:%02lu.%02lu", 
                   hours, minutes, seconds, milliseconds);
    } else {
        lv_snprintf(time_str, sizeof(time_str), "%02lu:%02lu.%02lu", 
                   minutes, seconds, milliseconds);
    }
    
    lv_label_set_text(stopwatch.time_label, time_str);
}

static void stopwatch_timer_cb(lv_timer_t* timer) {
    if (stopwatch.state == STOPWATCH_RUNNING) {
        update_time_display();
    }
}

static void stopwatch_start() {
    stopwatch.state = STOPWATCH_RUNNING;
    stopwatch.start_time = lv_tick_get();
    
    lv_obj_add_flag(stopwatch.start_btn, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(stopwatch.pause_btn, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(stopwatch.reset_btn, LV_OBJ_FLAG_HIDDEN);
    
    lv_label_set_text(lv_obj_get_child(stopwatch.pause_btn, 0), LV_SYMBOL_PAUSE);
    
    if (stopwatch.timer == NULL) {
        stopwatch.timer = lv_timer_create(stopwatch_timer_cb, 10, NULL);
    }
}

static void stopwatch_pause() {
    if (stopwatch.state == STOPWATCH_RUNNING) {
        stopwatch.state = STOPWATCH_PAUSED;
        stopwatch.elapsed_time += lv_tick_get() - stopwatch.start_time;
        lv_label_set_text(lv_obj_get_child(stopwatch.pause_btn, 0), LV_SYMBOL_PLAY);
    } else if (stopwatch.state == STOPWATCH_PAUSED) {
        stopwatch.state = STOPWATCH_RUNNING;
        stopwatch.start_time = lv_tick_get();
        lv_label_set_text(lv_obj_get_child(stopwatch.pause_btn, 0), LV_SYMBOL_PAUSE);
    }
}

static void stopwatch_reset() {
    if (stopwatch.timer) {
        lv_timer_del(stopwatch.timer);
        stopwatch.timer = NULL;
    }
    
    stopwatch.state = STOPWATCH_STOPPED;
    stopwatch.elapsed_time = 0;
    stopwatch.start_time = 0;
    stopwatch.paused_time = 0;
    
    lv_label_set_text(stopwatch.time_label, "00:00.00");
    
    lv_obj_clear_flag(stopwatch.start_btn, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(stopwatch.pause_btn, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(stopwatch.reset_btn, LV_OBJ_FLAG_HIDDEN);
}

static void stopwatch_start_btn_cb(lv_event_t* e) { stopwatch_start(); }
static void stopwatch_pause_btn_cb(lv_event_t* e) { stopwatch_pause(); }
static void stopwatch_reset_btn_cb(lv_event_t* e) { stopwatch_reset(); }

// ==================== 倒计时功能 ====================

// 更新倒计时显示和进度条
static void countdown_timer_cb(lv_timer_t* t) {
    int remain_ms = atomic_load_int(&g_countdown_remaining);
    int total_ms = atomic_load_int(&g_countdown_total);

    int total_sec = remain_ms / 1000;
    int m = total_sec / 60;
    int s = total_sec % 60;

    lv_label_set_text_fmt(countdown.time_label, "%02u:%02u", m, s);

    if (total_ms > 0) {
        int32_t value = (remain_ms * 100) / total_ms;
        lv_arc_set_value(countdown.arc, value);
    }

    if (remain_ms <= 10000) {
        lv_obj_set_style_arc_color(countdown.arc, lv_palette_main(LV_PALETTE_RED), LV_PART_INDICATOR);
    } else if (remain_ms <= 30000) {
        lv_obj_set_style_arc_color(countdown.arc, lv_palette_main(LV_PALETTE_ORANGE), LV_PART_INDICATOR);
    } else {
        lv_obj_set_style_arc_color(countdown.arc, lv_palette_main(LV_PALETTE_GREEN), LV_PART_INDICATOR);
    }
}

// 开始按钮回调
static void countdown_start_btn_cb(lv_event_t* e) {
    uint16_t m = lv_roller_get_selected(countdown.min_roller);
    uint16_t s = lv_roller_get_selected(countdown.sec_roller);
    countdown.total_seconds = m * 60 + s;
    int total_ms = countdown.total_seconds * 1000;

    if (countdown.total_seconds == 0) {
        lv_label_set_text(countdown.time_label, "请设置时间");
        return;
    }

    // 保存总时间
    atomic_store_int(&g_countdown_total, total_ms);

    // 创建命令队列
    if (countdown_cmd_queue == NULL) {
        countdown_cmd_queue = xQueueCreate(4, sizeof(countdown_cmd_t));
    }

    // 创建新任务前，重置队列，清除可能的残留命令
    if (countdown_cmd_queue) {
        xQueueReset(countdown_cmd_queue);
    }

    // 创建倒计时任务
    BaseType_t ret = xTaskCreatePinnedToCore(
        countdown_task_func,
        "CountdownTask",
        4096,
        (void*)total_ms,
        1,
        &countdown_task,
        1
    );

    if (ret != pdPASS) {
        Serial.println("Failed to create countdown task");
        return;
    }

    // 切换 UI 到运行界面
    lv_obj_add_flag(countdown.min_roller, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(countdown.sec_roller, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(countdown.colon_label, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(countdown.start_btn, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(countdown.title_label, LV_OBJ_FLAG_HIDDEN);

    lv_obj_clear_flag(countdown.time_label, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(countdown.arc, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(countdown.pause_btn, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(countdown.reset_btn, LV_OBJ_FLAG_HIDDEN);

    lv_obj_set_style_arc_color(countdown.arc, lv_palette_main(LV_PALETTE_GREEN), LV_PART_INDICATOR);

    // 创建或恢复 LVGL 定时器
    if (countdown.timer == NULL) {
        countdown.timer = lv_timer_create(countdown_timer_cb, 100, NULL);
    } else {
        lv_timer_resume(countdown.timer);
    }

    countdown.state = TIMER_RUNNING;
}

// 暂停/继续按钮回调
static void countdown_pause_btn_cb(lv_event_t* e) {
    if (!countdown_cmd_queue) return;

    countdown_cmd_t cmd;
    if (countdown.state == TIMER_RUNNING) {
        cmd = COUNTDOWN_CMD_PAUSE;
        countdown.state = TIMER_PAUSED;
        lv_label_set_text(lv_obj_get_child(countdown.pause_btn, 0), LV_SYMBOL_PLAY);
    } else if (countdown.state == TIMER_PAUSED) {
        cmd = COUNTDOWN_CMD_RESUME;
        countdown.state = TIMER_RUNNING;
        lv_label_set_text(lv_obj_get_child(countdown.pause_btn, 0), LV_SYMBOL_PAUSE);
    } else {
        return;
    }
    xQueueSend(countdown_cmd_queue, &cmd, 0);
}

// 重置UI
static void countdown_reset_ui() {
    lv_obj_clear_flag(countdown.min_roller, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(countdown.sec_roller, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(countdown.colon_label, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(countdown.start_btn, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(countdown.title_label, LV_OBJ_FLAG_HIDDEN);

    lv_obj_add_flag(countdown.time_label, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(countdown.arc, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(countdown.pause_btn, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(countdown.reset_btn, LV_OBJ_FLAG_HIDDEN);

    lv_label_set_text(lv_obj_get_child(countdown.pause_btn, 0), LV_SYMBOL_PAUSE);

    lv_roller_set_selected(countdown.min_roller, 0, LV_ANIM_OFF);
    lv_roller_set_selected(countdown.sec_roller, 0, LV_ANIM_OFF);

    lv_obj_set_style_arc_color(countdown.arc, lv_palette_main(LV_PALETTE_GREEN), LV_PART_INDICATOR);

    countdown.state = TIMER_STOPPED;
}

// 重置按钮回调
static void countdown_reset_btn_cb(lv_event_t* e) {
    if (countdown_cmd_queue) {
        countdown_cmd_t cmd = COUNTDOWN_CMD_RESET;
        xQueueSend(countdown_cmd_queue, &cmd, 0);
    }
    countdown_reset_ui();
}

// ==================== 倒计时 RTOS 任务 ====================
static void countdown_task_func(void *pvParameters) {
    int total_ms = (int)pvParameters;
    Serial.printf("Countdown task started, total=%d ms\n", total_ms);

    atomic_store_int(&g_countdown_remaining, total_ms);
    atomic_store_bool(&g_countdown_active, true);
    atomic_store_bool(&g_countdown_paused, false);

    bool paused = false;
    TickType_t last_tick = xTaskGetTickCount();
    int elapsed_ms = 0;

    while (1) {
        countdown_cmd_t cmd;
        if (xQueueReceive(countdown_cmd_queue, &cmd, 0) == pdTRUE) {
            switch (cmd) {
                case COUNTDOWN_CMD_PAUSE:
                    paused = true;
                    atomic_store_bool(&g_countdown_paused, true);
                    Serial.println("Countdown paused");
                    break;
                case COUNTDOWN_CMD_RESUME:
                    paused = false;
                    atomic_store_bool(&g_countdown_paused, false);
                    last_tick = xTaskGetTickCount();
                    Serial.println("Countdown resumed");
                    break;
                case COUNTDOWN_CMD_RESET:
                    atomic_store_bool(&g_countdown_active, false);
                    atomic_store_int(&g_countdown_remaining, 0);
                    atomic_store_bool(&g_countdown_paused, false);
                    countdown_task = NULL;
                    Serial.println("Countdown reset, task exiting");
                    vTaskDelete(NULL);
                    return;
            }
        }

        if (!paused) {
            TickType_t now = xTaskGetTickCount();
            TickType_t diff_ticks = now - last_tick;
            int diff_ms = diff_ticks * portTICK_PERIOD_MS;
            
            if (diff_ms >= 200) { 
                int remain = atomic_load_int(&g_countdown_remaining);
                remain -= diff_ms;
                
                if (remain <= 0) {
                    // 倒计时结束，先将剩余时间设为0
                    atomic_store_int(&g_countdown_remaining, 0);
                    
                    // 等待200ms让UI线程检测到倒计时结束
                    Serial.println("Countdown finished, waiting 200ms for UI detection");
                    vTaskDelay(pdMS_TO_TICKS(200));
                    
                    atomic_store_bool(&g_countdown_paused, false);
                    atomic_store_bool(&g_countdown_active, false);
                    countdown_task = NULL;
                    Serial.println("Countdown task exiting");
                    vTaskDelete(NULL);
                    return;
                } else {
                    atomic_store_int(&g_countdown_remaining, remain);
                }
                
                last_tick = now;
            }
        }

        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

// ==================== BUTTON检测 ====================
static void BUTTON2_check_cb(lv_timer_t* timer) {
    bool current = digitalRead(BUTTON_2);
    uint32_t now = lv_tick_get();
    
    if (current != last_BUTTON2_level) {
        stable_start_ms = now;
        last_BUTTON2_level = current;
    } else {
        if (current == 1 && (now - stable_start_ms) >= DEBOUNCE_THRESHOLD) {
            if (stopwatch.timer) {
                lv_timer_del(stopwatch.timer);
                stopwatch.timer = NULL;
            }
            if (countdown.timer) {
                lv_timer_del(countdown.timer);
                countdown.timer = NULL;
            }
            
            fs_do_exit();
            
            if (BUTTON2_timer) {
                lv_timer_del(BUTTON2_timer);
                BUTTON2_timer = NULL;
            }
            return;
        }
    }
}

// ==================== 双页滚动回调 ====================
static void dual_page_scroll_event_cb(lv_event_t* e) {
}


// ==================== 界面创建函数 ====================
void fs_create_stopwatch(lv_obj_t* parent) {
    // 清理旧GPIO定时器
    if (BUTTON2_timer) {
        lv_timer_del(BUTTON2_timer);
        BUTTON2_timer = NULL;
    }
    
    // 初始化GPIO6检测
    last_BUTTON2_level = digitalRead(BUTTON_2);
    stable_start_ms = lv_tick_get();
    BUTTON2_timer = lv_timer_create(BUTTON2_check_cb, 5, NULL);

    // 创建命令队列
    if (countdown_cmd_queue == NULL) {
        countdown_cmd_queue = xQueueCreate(4, sizeof(countdown_cmd_t));
    }

    // 创建滚动容器
    lv_obj_t* scroll_cont = lv_obj_create(parent);
    lv_obj_set_size(scroll_cont, 240, 280);
    lv_obj_align(scroll_cont, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_pad_all(scroll_cont, 0, 0);
    lv_obj_set_style_border_width(scroll_cont, 0, 0);
    lv_obj_set_scroll_snap_x(scroll_cont, LV_SCROLL_SNAP_CENTER);
    lv_obj_set_scrollbar_mode(scroll_cont, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_scroll_dir(scroll_cont, LV_DIR_HOR);
    lv_obj_add_flag(scroll_cont, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(scroll_cont, LV_OBJ_FLAG_SCROLL_ELASTIC);
    lv_obj_set_style_bg_opa(scroll_cont, LV_OPA_TRANSP, 0);
    lv_obj_add_event_cb(scroll_cont, dual_page_scroll_event_cb, LV_EVENT_ALL, NULL);

    // ========== 左侧页面：秒表 ==========
    lv_obj_t* left_page = lv_obj_create(scroll_cont);
    lv_obj_set_size(left_page, 240, 280);
    lv_obj_set_pos(left_page, 0, 0);
    lv_obj_set_style_bg_color(left_page, lv_color_hex(0x000000), 0);
    lv_obj_set_style_border_width(left_page, 0, 0);
    lv_obj_set_style_pad_all(left_page, 0, 0);

    stopwatch.state = STOPWATCH_STOPPED;
    stopwatch.start_time = 0;
    stopwatch.elapsed_time = 0;
    stopwatch.paused_time = 0;
    stopwatch.timer = NULL;

    lv_obj_t* title = lv_label_create(left_page);
    lv_label_set_text(title, "秒表");
    lv_obj_set_style_text_font(title, &chinese_24, LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(title, lv_color_white(), LV_STATE_DEFAULT);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 20);

    stopwatch.time_label = lv_label_create(left_page);
    lv_label_set_text(stopwatch.time_label, "00:00.00");
    lv_obj_set_style_text_font(stopwatch.time_label, &lv_font_montserrat_48, LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(stopwatch.time_label, lv_color_white(), LV_STATE_DEFAULT);
    lv_obj_align(stopwatch.time_label, LV_ALIGN_CENTER, 0, -20);

    lv_obj_t* btn_container = lv_obj_create(left_page);
    lv_obj_remove_style_all(btn_container);
    lv_obj_set_size(btn_container, 240, 80);
    lv_obj_align(btn_container, LV_ALIGN_BOTTOM_MID, 0, -20);
    lv_obj_set_flex_flow(btn_container, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(btn_container, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(btn_container, 30, LV_STATE_DEFAULT);

    // 开始按钮
    stopwatch.start_btn = lv_btn_create(btn_container);
    lv_obj_set_size(stopwatch.start_btn, 60, 60);
    lv_obj_set_style_radius(stopwatch.start_btn, 30, LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(stopwatch.start_btn, lv_palette_main(LV_PALETTE_GREEN), LV_STATE_DEFAULT);
    lv_obj_t* start_label = lv_label_create(stopwatch.start_btn);
    lv_label_set_text(start_label, LV_SYMBOL_PLAY);
    lv_obj_set_style_text_font(start_label, &lv_font_montserrat_24, 0);
    lv_obj_center(start_label);

    // 重置按钮（初始隐藏）
    stopwatch.reset_btn = lv_btn_create(btn_container);
    lv_obj_set_size(stopwatch.reset_btn, 60, 60);
    lv_obj_set_style_radius(stopwatch.reset_btn, 10, LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(stopwatch.reset_btn, lv_palette_main(LV_PALETTE_RED), LV_STATE_DEFAULT);
    lv_obj_t* reset_label = lv_label_create(stopwatch.reset_btn);
    lv_label_set_text(reset_label, LV_SYMBOL_REFRESH);
    lv_obj_set_style_text_font(reset_label, &lv_font_montserrat_24, 0);
    lv_obj_center(reset_label);
    lv_obj_add_flag(stopwatch.reset_btn, LV_OBJ_FLAG_HIDDEN);

    // 暂停按钮（初始隐藏）
    stopwatch.pause_btn = lv_btn_create(btn_container);
    lv_obj_set_size(stopwatch.pause_btn, 60, 60);
    lv_obj_set_style_radius(stopwatch.pause_btn, 30, LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(stopwatch.pause_btn, lv_palette_main(LV_PALETTE_BLUE), LV_STATE_DEFAULT);
    lv_obj_t* pause_label = lv_label_create(stopwatch.pause_btn);
    lv_label_set_text(pause_label, LV_SYMBOL_PAUSE);
    lv_obj_set_style_text_font(pause_label, &lv_font_montserrat_24, 0);
    lv_obj_center(pause_label);
    lv_obj_add_flag(stopwatch.pause_btn, LV_OBJ_FLAG_HIDDEN);

    lv_obj_add_event_cb(stopwatch.start_btn, stopwatch_start_btn_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(stopwatch.pause_btn, stopwatch_pause_btn_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(stopwatch.reset_btn, stopwatch_reset_btn_cb, LV_EVENT_CLICKED, NULL);

    // ========== 右侧页面：倒计时 ==========
    lv_obj_t* right_page = lv_obj_create(scroll_cont);
    lv_obj_set_size(right_page, 240, 280);
    lv_obj_set_pos(right_page, 240, 0);
    lv_obj_set_style_bg_color(right_page, lv_color_hex(0x000000), 0);
    lv_obj_set_style_border_width(right_page, 0, 0);
    lv_obj_set_style_pad_all(right_page, 0, 0);

    countdown.state = TIMER_STOPPED;
    countdown.total_seconds = 0;
    countdown.remain_seconds = 0;
    countdown.timer = NULL;

    // 标题
    countdown.title_label = lv_label_create(right_page);
    lv_label_set_text(countdown.title_label, "倒计时");
    lv_obj_set_style_text_font(countdown.title_label, &chinese_24, LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(countdown.title_label, lv_color_white(), LV_STATE_DEFAULT);
    lv_obj_align(countdown.title_label, LV_ALIGN_TOP_MID, 0, 20);

    // 分钟滚轮
    countdown.min_roller = lv_roller_create(right_page);
    lv_obj_set_width(countdown.min_roller, 70);
    lv_roller_set_options(countdown.min_roller, roller_options, LV_ROLLER_MODE_NORMAL);
    lv_roller_set_visible_row_count(countdown.min_roller, 4);
    lv_obj_align(countdown.min_roller, LV_ALIGN_CENTER, -65, -30);

    // 秒钟滚轮
    countdown.sec_roller = lv_roller_create(right_page);
    lv_obj_set_width(countdown.sec_roller, 70);
    lv_roller_set_options(countdown.sec_roller, roller_options, LV_ROLLER_MODE_NORMAL);
    lv_roller_set_visible_row_count(countdown.sec_roller, 4);
    lv_obj_align(countdown.sec_roller, LV_ALIGN_CENTER, 65, -30);

    // 冒号
    countdown.colon_label = lv_label_create(right_page);
    lv_label_set_text(countdown.colon_label, ":");
    lv_obj_set_style_text_font(countdown.colon_label, &lv_font_montserrat_40, 0);
    lv_obj_set_style_text_color(countdown.colon_label, lv_color_white(), 0);
    lv_obj_align(countdown.colon_label, LV_ALIGN_CENTER, 0, -35);

    // 设置滚轮样式
    lv_obj_t* rollers[] = {countdown.min_roller, countdown.sec_roller};
    for(int i = 0; i < 2; i++) {
        lv_obj_set_style_text_color(rollers[i], lv_color_white(), LV_PART_MAIN);
        lv_obj_set_style_text_font(rollers[i], &lv_font_montserrat_24, LV_PART_MAIN);
        lv_obj_set_style_bg_opa(rollers[i], LV_OPA_TRANSP, LV_PART_MAIN);
        lv_obj_set_style_border_width(rollers[i], 0, LV_PART_MAIN);
        lv_obj_set_style_text_color(rollers[i], lv_palette_main(LV_PALETTE_YELLOW), LV_PART_SELECTED);
        lv_obj_set_style_text_font(rollers[i], &lv_font_montserrat_40, LV_PART_SELECTED);
        lv_obj_set_style_bg_opa(rollers[i], LV_OPA_TRANSP, LV_PART_SELECTED);
        lv_obj_set_scrollbar_mode(rollers[i], LV_SCROLLBAR_MODE_OFF);
    }

    // 环形进度条（初始隐藏）
    countdown.arc = lv_arc_create(right_page);
    lv_obj_set_size(countdown.arc, 180, 180);
    lv_arc_set_rotation(countdown.arc, 270);
    lv_arc_set_bg_angles(countdown.arc, 0, 360);
    lv_arc_set_value(countdown.arc, 100);
    lv_obj_remove_style(countdown.arc, NULL, LV_PART_KNOB);
    lv_obj_clear_flag(countdown.arc, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_align(countdown.arc, LV_ALIGN_CENTER, 0, -30);
    lv_obj_set_style_arc_width(countdown.arc, 8, LV_PART_MAIN);
    lv_obj_set_style_arc_width(countdown.arc, 8, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(countdown.arc, lv_palette_main(LV_PALETTE_GREEN), LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(countdown.arc, lv_color_hex(0x333333), LV_PART_MAIN);
    lv_obj_add_flag(countdown.arc, LV_OBJ_FLAG_HIDDEN);

    // 倒计时数字显示（初始隐藏）
    countdown.time_label = lv_label_create(right_page);
    lv_obj_set_style_text_font(countdown.time_label, &lv_font_montserrat_48, 0);
    lv_obj_set_style_text_color(countdown.time_label, lv_color_white(), 0);
    lv_obj_align(countdown.time_label, LV_ALIGN_CENTER, 0, -30);
    lv_obj_add_flag(countdown.time_label, LV_OBJ_FLAG_HIDDEN);

    // 按钮容器
    lv_obj_t* timer_btn_cont = lv_obj_create(right_page);
    lv_obj_remove_style_all(timer_btn_cont);
    lv_obj_set_size(timer_btn_cont, 240, 80);
    lv_obj_align(timer_btn_cont, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_flex_flow(timer_btn_cont, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(timer_btn_cont, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(timer_btn_cont, 20, 0);

    // 开始按钮
    countdown.start_btn = lv_btn_create(timer_btn_cont);
    lv_obj_set_size(countdown.start_btn, 60, 60);
    lv_obj_set_style_radius(countdown.start_btn, 30, 0);
    lv_obj_set_style_bg_color(countdown.start_btn, lv_palette_main(LV_PALETTE_GREEN), 0);
    lv_obj_t* l_start = lv_label_create(countdown.start_btn);
    lv_label_set_text(l_start, LV_SYMBOL_PLAY);
    lv_obj_set_style_text_font(l_start, &lv_font_montserrat_24, 0);
    lv_obj_center(l_start);
    lv_obj_add_event_cb(countdown.start_btn, countdown_start_btn_cb, LV_EVENT_CLICKED, NULL);

    // 暂停按钮（初始隐藏）
    countdown.pause_btn = lv_btn_create(timer_btn_cont);
    lv_obj_set_size(countdown.pause_btn, 60, 60);
    lv_obj_set_style_radius(countdown.pause_btn, 30, 0);
    lv_obj_set_style_bg_color(countdown.pause_btn, lv_palette_main(LV_PALETTE_BLUE), 0);
    lv_obj_t* l_pause = lv_label_create(countdown.pause_btn);
    lv_label_set_text(l_pause, LV_SYMBOL_PAUSE);
    lv_obj_set_style_text_font(l_pause, &lv_font_montserrat_24, 0);
    lv_obj_center(l_pause);
    lv_obj_add_flag(countdown.pause_btn, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(countdown.pause_btn, countdown_pause_btn_cb, LV_EVENT_CLICKED, NULL);

    // 重置按钮（初始隐藏）
    countdown.reset_btn = lv_btn_create(timer_btn_cont);
    lv_obj_set_size(countdown.reset_btn, 60, 60);
    lv_obj_set_style_radius(countdown.reset_btn, 10, 0);
    lv_obj_set_style_bg_color(countdown.reset_btn, lv_palette_main(LV_PALETTE_RED), 0);
    lv_obj_t* l_reset = lv_label_create(countdown.reset_btn);
    lv_label_set_text(l_reset, LV_SYMBOL_REFRESH);
    lv_obj_set_style_text_font(l_reset, &lv_font_montserrat_24, 0);
    lv_obj_center(l_reset);
    lv_obj_add_flag(countdown.reset_btn, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(countdown.reset_btn, countdown_reset_btn_cb, LV_EVENT_CLICKED, NULL);

    // ========== 检查倒计时任务是否已在运行 ==========
    if (atomic_load_bool(&g_countdown_active)) {
        int total_ms = atomic_load_int(&g_countdown_total);
        countdown.total_seconds = total_ms / 1000;

        lv_obj_add_flag(countdown.min_roller, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(countdown.sec_roller, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(countdown.colon_label, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(countdown.start_btn, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(countdown.title_label, LV_OBJ_FLAG_HIDDEN);

        lv_obj_clear_flag(countdown.time_label, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(countdown.arc, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(countdown.pause_btn, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(countdown.reset_btn, LV_OBJ_FLAG_HIDDEN);

        // 根据原子暂停状态设置按钮图标
        if (atomic_load_bool(&g_countdown_paused)) {
            lv_label_set_text(lv_obj_get_child(countdown.pause_btn, 0), LV_SYMBOL_PLAY);
            countdown.state = TIMER_PAUSED;
        } else {
            lv_label_set_text(lv_obj_get_child(countdown.pause_btn, 0), LV_SYMBOL_PAUSE);
            countdown.state = TIMER_RUNNING;
        }

        if (countdown.timer == NULL) {
            countdown.timer = lv_timer_create(countdown_timer_cb, 100, NULL);
        }
        countdown_timer_cb(NULL);
    } else {
        if (countdown.timer) {
            lv_timer_del(countdown.timer);
            countdown.timer = NULL;
        }
    }

    // 添加容器删除事件回调，清理 LVGL 定时器
    lv_obj_add_event_cb(parent, countdown_cleanup_cb, LV_EVENT_DELETE, NULL);
}

// ==================== 清理回调 ====================
static void countdown_cleanup_cb(lv_event_t* e) {
    if (countdown.timer) {
        lv_timer_del(countdown.timer);
        countdown.timer = NULL;
    }
}

// ==================== 界面清理函数 ====================
void fs_cleanup_stopwatch(void) {
    if (stopwatch.timer) {
        lv_timer_del(stopwatch.timer);
        stopwatch.timer = NULL;
    }
    // 倒计时 LVGL 定时器已在容器删除时清理
    if (BUTTON2_timer) {
        lv_timer_del(BUTTON2_timer);
        BUTTON2_timer = NULL;
    }
}