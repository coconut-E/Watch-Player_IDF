#include "fullscreen_interfaces.h"
#include <stdio.h>
#include <stdlib.h>
#include <FS.h>
#include <JPEGDEC.h>
#include <Arduino.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

extern void fs_do_exit();
extern FileSelectionInstance* g_file_selection_instance;
extern SdFs sd;

// --- 常量与配置 ---
#define SRC_W 480
#define SRC_BLOCK_H 200
#define POOL_SIZE 7
#define CANVAS_W 240
#define CANVAS_H 280

// --- 按键与状态机配置 ---
#define DEBOUNCE_THRESHOLD 20
#define STATE_CHANGE_COOLDOWN 500
#define TIMER_PERIOD 10
#define ZOOM_COOLDOWN_MS 50


#define COMIC_HISTORY_FILE_PATH "/comic_hist.txt" // 漫画历史记录文件路径
#define COMIC_OFFSET_FIELD_WIDTH 10                // 偏移量字段宽度


// --- 队列数据结构 ---
typedef struct {
    int32_t block_id;
    int pool_idx;
} decode_msg_t;

// --- 状态变量 ---
static uint16_t* psram_pool[POOL_SIZE];
static int32_t pool_content_ids[POOL_SIZE];
static bool pool_is_pending[POOL_SIZE];

static lv_obj_t* canvas_obj = NULL;
static uint16_t* canvas_buf = NULL;
static lv_timer_t* monitor_timer = NULL;

static float current_zoom = 0.5f;
static int32_t scroll_x = 0;
static int32_t scroll_y = 0;

// --- 惯性滑动相关变量 ---
static float velocity_x = 0.0f;
static float velocity_y = 0.0f;
static bool is_drifting = false;
const float friction = 0.92f;
const float stop_threshold = 0.5f;
const float MAX_VELOCITY = 120.0f;

// --- CMJ 与解码相关 ---
static FsFile cmj_file;
static int64_t* cmj_offsets = NULL;
static uint32_t cmj_total_frames = 0;
static bool cmj_loaded = false;

// --- 多线程同步组件 ---
static TaskHandle_t decode_task_handle = NULL;
static QueueHandle_t xDecodeReqQueue = NULL;
static QueueHandle_t xDecodeDoneQueue = NULL;
static uint16_t* intermediate_psram_buf = NULL;

// --- 文件选择与状态机变量 ---
static lv_obj_t* g_container = NULL;
static bool comic_state_machine = false;
static bool need_switch_to_comic = false;
static uint32_t last_state_change_ms = 0;
static String current_book_path = "";
static String current_file_name = "";

// 按键防抖变量
static bool last_BUTTON2_level = false;
static uint32_t stable_start_BUTTON2 = 0;

static bool last_BUTTON1_level = false;
static uint32_t stable_start_BUTTON1 = 0;
static uint32_t last_zoom_BUTTON1 = 0;

static bool last_BUTTON3_level = false;
static uint32_t stable_start_BUTTON3 = 0;
static uint32_t last_zoom_BUTTON3 = 0;

// SD卡状态检测
static bool last_sd_card_state = true;
static lv_timer_t* gpio_timer = NULL;

// --- 函数声明 ---
static int cmj_jpeg_draw_callback(JPEGDRAW *pDraw);
static void decode_rtos_task(void* pvParameters);
static bool load_cmj_index(const char* path);
static void render_sampling_view(void);
static void drag_event_cb(lv_event_t * e);
static void monitor_timer_cb(lv_timer_t * t);
static void comic_reader_ui(lv_obj_t* parent);
static void clean_comic_interface(void);
static void comic_file_selected_cb(const char* filename, void* user_data);
static void comic_gpio_check_cb(lv_timer_t* timer);
static void fs_cleanup_comic_gpio_timer(void);
static void save_comic_progress(void);
static int32_t load_comic_progress(const String& filename);

//解码回调
static int cmj_jpeg_draw_callback(JPEGDRAW *pDraw) {
    uint16_t* dest = intermediate_psram_buf + pDraw->y * SRC_W + pDraw->x;
    uint16_t* src = pDraw->pPixels;
    for (int y = 0; y < pDraw->iHeight; y++) {
        memcpy(dest, src, pDraw->iWidth * 2);
        dest += SRC_W;
        src += pDraw->iWidth;
    }
    return 1;
}

//解码任务 (运行在 CPU 0)
static void decode_rtos_task(void* pvParameters) {
    printf("[Task] Decoder Task Started on Core %d\n", xPortGetCoreID());
    JPEGDEC *local_decoder = new JPEGDEC();
    if (!local_decoder) {
        printf("[Task] Failed to allocate decoder!\n");
        vTaskDelete(NULL);
        return;
    }
    while (1) {
        decode_msg_t msg;
        if (xQueueReceive(xDecodeReqQueue, &msg, portMAX_DELAY) == pdTRUE) {
            if (msg.block_id == -1) break;
            while (uxQueueMessagesWaiting(xDecodeDoneQueue) != 0) {
                vTaskDelay(pdMS_TO_TICKS(1));
            }
            if (cmj_loaded && msg.block_id < (int32_t)cmj_total_frames) {
                int64_t start_offset = cmj_offsets[msg.block_id];
                uint32_t frame_size = cmj_offsets[msg.block_id + 1] - start_offset;
                uint8_t* jpeg_buf = (uint8_t*)malloc(frame_size);
                if (jpeg_buf) {
                    cmj_file.seek(start_offset);
                    cmj_file.read(jpeg_buf, frame_size);
                    local_decoder->setPixelType(RGB565_LITTLE_ENDIAN);
                    if (local_decoder->openRAM(jpeg_buf, frame_size, cmj_jpeg_draw_callback)) {
                        local_decoder->decode(0, 0, 0);
                        local_decoder->close();
                    }
                    free(jpeg_buf);
                }
            }
            xQueueSend(xDecodeDoneQueue, &msg, portMAX_DELAY);
        }
    }
    if (local_decoder) {
        delete local_decoder;
        printf("[Task] Decoder Instance Deleted.\n");
    }
    if (intermediate_psram_buf) {
        free(intermediate_psram_buf);
        intermediate_psram_buf = NULL;
    }
    printf("[Task] Decoder Task Thread Exiting...\n");
    decode_task_handle = NULL;
    vTaskDelete(NULL);
}

static bool load_cmj_index(const char* path) {
    if (!cmj_file.open(path, O_RDONLY)) return false;
    char magic[4];
    cmj_file.read(magic, 4);
    if (memcmp(magic, "CMJB", 4) != 0) {
        cmj_file.close();
        return false;
    }
    cmj_file.read(&cmj_total_frames, 4);
    cmj_offsets = (int64_t*)malloc((cmj_total_frames + 1) * sizeof(int64_t));
    for (uint32_t i = 0; i <= cmj_total_frames; i++) {
        cmj_file.read(&cmj_offsets[i], sizeof(int64_t));
    }
    cmj_loaded = true;
    return true;
}
//保存漫画阅读进度
static void save_comic_progress(void) {
    if (!cmj_loaded || current_file_name.length() == 0) {
        return;
    }

    int32_t offset_to_save = scroll_y;

    // 挂载 LittleFS
    if (!LittleFS.begin(true, "/littlefs", 10, "littlefs")) {
        Serial.println("❌ Comic LittleFS 挂载失败");
        return;
    }

    String target_prefix = current_file_name + ":";
    size_t prefix_len = target_prefix.length();

    bool found = false;
    uint32_t offset_field_pos = 0;

    // 尝试以 "r+" 模式打开
    File history_file = LittleFS.open(COMIC_HISTORY_FILE_PATH, "r+");
    if (history_file) {
        history_file.seek(0);
        while (history_file.available()) {
            uint32_t line_start_pos = history_file.position();
            String line = history_file.readStringUntil('\n');
            if (line.length() == 0) continue;
            if (line.startsWith(target_prefix)) {
                found = true;
                offset_field_pos = line_start_pos + prefix_len;
                break;
            }
        }
    }

    // 准备偏移量字符串
    char offset_str[COMIC_OFFSET_FIELD_WIDTH + 1];
    snprintf(offset_str, sizeof(offset_str), "%-10ld", (long)offset_to_save);

    if (found && history_file) {
        // 找到了记录，覆盖写入
        history_file.seek(offset_field_pos);
        history_file.write((uint8_t*)offset_str, COMIC_OFFSET_FIELD_WIDTH);
        history_file.close();
        Serial.printf("✅ 更新漫画进度：%s 偏移 %ld\n", current_file_name.c_str(), (long)offset_to_save);
    } else {
        // 没找到记录，追加写入
        if (history_file) history_file.close();
        history_file = LittleFS.open(COMIC_HISTORY_FILE_PATH, "a");
        if (history_file) {
            String new_line = target_prefix + offset_str + "\n";
            history_file.write((uint8_t*)new_line.c_str(), new_line.length());
            history_file.close();
            Serial.printf("✅ 新增漫画进度：%s 偏移 %ld\n", current_file_name.c_str(), (long)offset_to_save);
        } else {
            Serial.println("❌ 无法创建/打开漫画历史记录文件");
        }
    }

    LittleFS.end();
}

//加载漫画阅读进度
static int32_t load_comic_progress(const String& filename) {
    if (filename.length() == 0) return 0;

    if (!LittleFS.begin(true, "/littlefs", 10, "littlefs")) {
        Serial.println("ℹ️ Comic LittleFS 挂载失败，从0开始");
        return 0;
    }

    File history_file = LittleFS.open(COMIC_HISTORY_FILE_PATH, "r");
    if (!history_file) {
        Serial.println("ℹ️ 漫画历史记录文件不存在，从0开始");
        LittleFS.end();
        return 0;
    }

    String target_prefix = filename + ":";
    size_t prefix_len = target_prefix.length();

    history_file.seek(0);
    while (history_file.available()) {
        String line = history_file.readStringUntil('\n');
        if (line.startsWith(target_prefix)) {
            if (line.length() > prefix_len) {
                String offset_part = line.substring(prefix_len);
                offset_part.trim();
                int32_t offset = offset_part.toInt();
                history_file.close();
                LittleFS.end();
                Serial.printf("📖 加载漫画历史进度：%s 偏移 %ld\n", filename.c_str(), (long)offset);
                return offset;
            }
        }
    }

    history_file.close();
    LittleFS.end();
    Serial.printf("ℹ️ 无漫画历史记录，%s 从0开始\n", filename.c_str());
    return 0;
}

//GPIO 定时器清理
static void fs_cleanup_comic_gpio_timer(void) {
    if (gpio_timer) {
        lv_timer_del(gpio_timer);
        gpio_timer = NULL;
    }
}

// 漫画文件选择回调
static void comic_file_selected_cb(const char* filename, void* user_data) {
    if (strcmp(filename, SCAN_SPECIAL_FILENAME) == 0) {
        Serial.println("用户选择重新扫描漫画文件");
        fs_do_exit();
        clean_comic_interface();
        destroy_file_selection_list();
        fs_cleanup_comic_gpio_timer();
        if (xSemaphoreTake(sd_state_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
            sd_force_scan = true;
            xSemaphoreGive(sd_state_mutex);
        }
        return;
    }
    Serial.printf("选中漫画文件：%s\n", filename);
    current_book_path = String("/漫画/") + filename;
    current_file_name = String(filename);
    need_switch_to_comic = true;
}

//  GPIO 检测定时器回调 
static void comic_gpio_check_cb(lv_timer_t* timer) {
    uint32_t now = lv_tick_get();

    // SD卡拔出检测
    bool current_sd_state = false;
    if (xSemaphoreTake(sd_state_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        current_sd_state = sd_card_inserted;
        xSemaphoreGive(sd_state_mutex);
    }
    if (last_sd_card_state == true && current_sd_state == false) {
        Serial.println("⚠️ 检测到SD卡拔出，清理漫画资源并返回");
        fs_do_exit();
        clean_comic_interface();
        destroy_file_selection_list();
        fs_cleanup_comic_gpio_timer();
        comic_state_machine = false;
        need_switch_to_comic = false;
        last_sd_card_state = current_sd_state;
        return;
    }
    last_sd_card_state = current_sd_state;

    // 检测 BUTTON2 (返回/退出)
    bool BUTTON2 = digitalRead(BUTTON_2);
    if (BUTTON2 != last_BUTTON2_level) {
        stable_start_BUTTON2 = now;
        last_BUTTON2_level = BUTTON2;
    } else if (BUTTON2 == 1 && (now - stable_start_BUTTON2) >= DEBOUNCE_THRESHOLD) {
        if (now - last_state_change_ms > STATE_CHANGE_COOLDOWN) {
            last_state_change_ms = now;
            if (comic_state_machine) {
                // 退出漫画阅读界面，回到文件选择器
                if (fullscreen_container) lv_obj_move_foreground(fullscreen_container);
                clean_comic_interface();
                file_selection_create(g_container, FILE_TYPE_COMIC, comic_file_selected_cb, NULL);
                comic_state_machine = false;
            } else {
                // 退出整个功能
                fs_do_exit();
                destroy_file_selection_list();
                fs_cleanup_comic_gpio_timer();
                return;
            }
        }
    }

    // 检测 BUTTON1 (放大) 和 BUTTON3 (缩小)
    if (comic_state_machine) {
        bool BUTTON1 = digitalRead(BUTTON_1);
        if (BUTTON1 != last_BUTTON1_level) {
            stable_start_BUTTON1 = now;
            last_BUTTON1_level = BUTTON1;
        } else if (BUTTON1 == 1 && (now - stable_start_BUTTON1) >= DEBOUNCE_THRESHOLD) {
            if (now - last_zoom_BUTTON1 > ZOOM_COOLDOWN_MS) {
                last_zoom_BUTTON1 = now;
                float old_zoom = current_zoom;
                current_zoom += 0.05f;
                if (current_zoom > 2.0f) current_zoom = 2.0f;
                scroll_x += (int32_t)((CANVAS_W / 2.0f) * (1.0f / old_zoom - 1.0f / current_zoom));
                scroll_y += (int32_t)((CANVAS_H / 2.0f) * (1.0f / old_zoom - 1.0f / current_zoom));
                is_drifting = false;
                render_sampling_view();
            }
        }

        bool BUTTON3 = digitalRead(BUTTON_3);
        if (BUTTON3 != last_BUTTON3_level) {
            stable_start_BUTTON3 = now;
            last_BUTTON3_level = BUTTON3;
        } else if (BUTTON3 == 1 && (now - stable_start_BUTTON3) >= DEBOUNCE_THRESHOLD) {
            if (now - last_zoom_BUTTON3 > ZOOM_COOLDOWN_MS) {
                last_zoom_BUTTON3 = now;
                float old_zoom = current_zoom;
                current_zoom -= 0.05f;
                if (current_zoom < 0.5f) current_zoom = 0.5f;
                scroll_x += (int32_t)((CANVAS_W / 2.0f) * (1.0f / old_zoom - 1.0f / current_zoom));
                scroll_y += (int32_t)((CANVAS_H / 2.0f) * (1.0f / old_zoom - 1.0f / current_zoom));
                is_drifting = false;
                render_sampling_view();
            }
        }
    }

    // 处理文件选择完成后的切换请求
    if (need_switch_to_comic) {
        destroy_file_selection_list();
        comic_reader_ui(g_container);
        comic_state_machine = true;
        need_switch_to_comic = false;
    }
}

// 创建漫画文件选择界面(入口)
void fs_create_comic(lv_obj_t* container) {
    g_container = container;
    fs_cleanup_comic_gpio_timer();

    // 初始化按键状态
    last_BUTTON2_level = digitalRead(BUTTON_2);
    stable_start_BUTTON2 = lv_tick_get();
    last_BUTTON1_level = digitalRead(BUTTON_1);
    stable_start_BUTTON1 = lv_tick_get();
    last_BUTTON3_level = digitalRead(BUTTON_3);
    stable_start_BUTTON3 = lv_tick_get();

    // 初始化SD卡状态
    if (xSemaphoreTake(sd_state_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        last_sd_card_state = sd_card_inserted;
        xSemaphoreGive(sd_state_mutex);
    } else {
        last_sd_card_state = true;
    }

    // 创建 GPIO 检测定时器
    gpio_timer = lv_timer_create(comic_gpio_check_cb, TIMER_PERIOD, NULL);

    // 创建文件选择器
    g_file_selection_instance = file_selection_create(container, FILE_TYPE_COMIC, comic_file_selected_cb, NULL);
}

//清理漫画阅读界面资源
static void clean_comic_interface(void) {
    save_comic_progress();
    if (monitor_timer) {
        lv_timer_del(monitor_timer);
        monitor_timer = NULL;
    }
    // 通知解码任务退出
    if (xDecodeReqQueue != NULL) {
        decode_msg_t exit_msg = {.block_id = -1, .pool_idx = -1};
        xQueueSendToFront(xDecodeReqQueue, &exit_msg, portMAX_DELAY);
    }
    // 等待解码任务结束
    int timeout_ms = 500;
    while (decode_task_handle != NULL && timeout_ms > 0) {
        vTaskDelay(pdMS_TO_TICKS(10));
        timeout_ms -= 10;
    }
    // 释放内存池
    for (int i = 0; i < POOL_SIZE; i++) {
        if (psram_pool[i]) {
            free(psram_pool[i]);
            psram_pool[i] = NULL;
        }
    }
    if (canvas_buf) {
        free(canvas_buf);
        canvas_buf = NULL;
    }
    if (canvas_obj) {
        lv_obj_del(canvas_obj);
        canvas_obj = NULL;
    }
    if (cmj_offsets) {
        free(cmj_offsets);
        cmj_offsets = NULL;
    }
    if (cmj_file) cmj_file.close();
    // 删除队列
    if (xDecodeReqQueue) {
        vQueueDelete(xDecodeReqQueue);
        xDecodeReqQueue = NULL;
    }
    if (xDecodeDoneQueue) {
        vQueueDelete(xDecodeDoneQueue);
        xDecodeDoneQueue = NULL;
    }
    cmj_loaded = false;
    is_drifting = false;
    current_zoom = 0.5f;
    scroll_x = 0;
    scroll_y = 0;
}

//核心渲染器
void render_sampling_view() {
    if (!cmj_loaded) return;

    //尽可能排空 Done 队列
    decode_msg_t done_msg;
    while (xQueueReceive(xDecodeDoneQueue, &done_msg, 0) == pdTRUE) {
        memcpy(psram_pool[done_msg.pool_idx], intermediate_psram_buf, SRC_W * SRC_BLOCK_H * 2);
        pool_content_ids[done_msg.pool_idx] = done_msg.block_id;
        pool_is_pending[done_msg.pool_idx] = false;
    }

    //计算视野与预取
    int32_t roi_w = (int32_t)(CANVAS_W / current_zoom);
    int32_t roi_h = (int32_t)(CANVAS_H / current_zoom);
    
    int32_t max_scroll_y = (cmj_total_frames * SRC_BLOCK_H) - roi_h;
    if (scroll_y < 0) scroll_y = 0;
    if (scroll_y > max_scroll_y) scroll_y = max_scroll_y;
    
    int32_t max_scroll_x = SRC_W - roi_w;
    if (scroll_x < 0) scroll_x = 0;
    if (scroll_x > max_scroll_x) scroll_x = max_scroll_x;
    if (roi_w >= SRC_W) scroll_x = (SRC_W - roi_w) / 2;

    int32_t center_y = scroll_y + (roi_h / 2);
    int32_t center_block_id = center_y / SRC_BLOCK_H;
    int32_t start_id = center_block_id - (POOL_SIZE / 2);
    int32_t end_id = start_id + POOL_SIZE - 1;

    if (start_id < 0) start_id = 0;
    if (end_id >= (int32_t)cmj_total_frames) end_id = (int32_t)cmj_total_frames - 1;

    for (int32_t id = start_id; id <= end_id; id++) {
        int pool_idx = id % POOL_SIZE;
        if (pool_content_ids[pool_idx] != id && !pool_is_pending[pool_idx]) {
            memset(psram_pool[pool_idx], 0, SRC_W * SRC_BLOCK_H * 2);
            pool_is_pending[pool_idx] = true;
            decode_msg_t req = {.block_id = id, .pool_idx = pool_idx};
            xQueueSend(xDecodeReqQueue, &req, portMAX_DELAY);
        }
    }

    //渲染画布
    for (int dy = 0; dy < CANVAS_H; dy++) {
        uint16_t* line_buf = &canvas_buf[dy * CANVAS_W];
        int32_t sy = scroll_y + (dy * roi_h) / CANVAS_H;
        int target_block_id = sy / SRC_BLOCK_H;
        int local_y = sy % SRC_BLOCK_H;
        int pool_idx = target_block_id % POOL_SIZE;
        if (pool_content_ids[pool_idx] == target_block_id) {
            uint16_t* src_pixel_start = &psram_pool[pool_idx][local_y * SRC_W];
            for (int dx = 0; dx < CANVAS_W; dx++) {
                int32_t sx = scroll_x + (dx * roi_w) / CANVAS_W;
                if (sx >= 0 && sx < SRC_W) {
                    line_buf[dx] = src_pixel_start[sx];
                } else {
                    line_buf[dx] = 0x0000;
                }
            }
        } else {
            memset(line_buf, 0, CANVAS_W * 2);
        }
    }
    lv_obj_invalidate(canvas_obj);
}

// 事件回调
static void drag_event_cb(lv_event_t * e) {
    lv_indev_t * indev = lv_indev_get_act();
    lv_point_t vect;
    lv_indev_get_vect(indev, &vect);
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_PRESSING) {
        is_drifting = false;
        float vx = -((float)vect.x / current_zoom);
        float vy = -((float)vect.y / current_zoom);
        velocity_x = (vx > MAX_VELOCITY) ? MAX_VELOCITY : ((vx < -MAX_VELOCITY) ? -MAX_VELOCITY : vx);
        velocity_y = (vy > MAX_VELOCITY) ? MAX_VELOCITY : ((vy < -MAX_VELOCITY) ? -MAX_VELOCITY : vy);
        scroll_x += (int32_t)velocity_x;
        scroll_y += (int32_t)velocity_y;
        render_sampling_view();
    } else if (code == LV_EVENT_RELEASED) {
        if (abs(velocity_x) > 1.0f || abs(velocity_y) > 1.0f) {
            is_drifting = true;
        }
    }
}

static void monitor_timer_cb(lv_timer_t * t) {
    bool changed = false;
    //只要解码完成就刷新
    if (uxQueueMessagesWaiting(xDecodeDoneQueue) > 0) {
        changed = true;
    }
    // 处理惯性滑动
    if (is_drifting) {
        scroll_x += (int32_t)velocity_x;
        scroll_y += (int32_t)velocity_y;
        velocity_x *= friction;
        velocity_y *= friction;
        if (abs(velocity_x) < stop_threshold && abs(velocity_y) < stop_threshold) {
            velocity_x = 0;
            velocity_y = 0;
            is_drifting = false;
        }
        changed = true;
    }
    if (changed) {
        render_sampling_view();
    }
}

//创建漫画阅读界面
static void comic_reader_ui(lv_obj_t* parent) {
    //初始化内存与队列
    xDecodeReqQueue = xQueueCreate(POOL_SIZE, sizeof(decode_msg_t));
    xDecodeDoneQueue = xQueueCreate(1, sizeof(decode_msg_t));
    intermediate_psram_buf = (uint16_t*)malloc(SRC_W * SRC_BLOCK_H * 2);
    for (int i = 0; i < POOL_SIZE; i++) {
        psram_pool[i] = (uint16_t*)malloc(SRC_W * SRC_BLOCK_H * 2);
        pool_content_ids[i] = -1;
        pool_is_pending[i] = false;
    }
    canvas_buf = (uint16_t*)malloc(CANVAS_W * CANVAS_H * 2);

    // 启动解码任务
    xTaskCreatePinnedToCore(decode_rtos_task, "comic_dec", 8192, NULL, 5, &decode_task_handle, 0);

    // 加载索引
    if (!load_cmj_index(current_book_path.c_str())) {
        Serial.println("❌ 无法加载漫画文件");
        return;
    }

    // 加载历史滚动位置
    scroll_y = load_comic_progress(current_file_name);

    //创建 UI
    canvas_obj = lv_canvas_create(parent);
    lv_canvas_set_buffer(canvas_obj, canvas_buf, CANVAS_W, CANVAS_H, LV_IMG_CF_TRUE_COLOR);
    lv_obj_center(canvas_obj);
    lv_obj_add_flag(canvas_obj, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(canvas_obj, drag_event_cb, LV_EVENT_ALL, NULL);
    
    monitor_timer = lv_timer_create(monitor_timer_cb, 50, NULL);
    
    if (sliding_container) lv_obj_move_foreground(sliding_container);
    
    render_sampling_view();
}

//外部销毁/退出接口
void fs_comic_destory() {
    clean_comic_interface();
    destroy_file_selection_list();
    fs_cleanup_comic_gpio_timer();
    fs_do_exit();
}

// 外部清理接口 
void fs_cleanup_comic(void) {
    clean_comic_interface();
    destroy_file_selection_list();
    fs_cleanup_comic_gpio_timer();
}
