#include <JPEGDEC.h>
#include "fullscreen_interfaces.h"

extern SdFs sd;
JPEGDEC *jpeg = NULL;
extern lv_obj_t* fullscreen_container;          // 全屏容器
extern SemaphoreHandle_t sd_state_mutex;        // SD卡状态互斥锁
extern bool sd_card_inserted;                      // SD卡插入状态
extern FileSelectionInstance* g_file_selection_instance; // 文件选择器实例

#define MJPEG_DIR "/视频/"                     // 视频文件存放目录
#define VIDEO_WIDTH  240
#define VIDEO_HEIGHT 280
#define FRAME_BUF_SIZE (VIDEO_WIDTH * VIDEO_HEIGHT * 2)
#define READ_BUF_SIZE 102400
#define FPS_PRINT_INTERVAL 1

// BUTTON 检测相关
#define DEBOUNCE_THRESHOLD      50
#define DEBOUNCE_TIME           30
#define STATE_CHANGE_COOLDOWN   500
#define TIMER_PERIOD            10


static uint8_t* frame_buffers[3] = {NULL, NULL, NULL};
static int write_idx = 0;
static volatile int show_idx = -1;
static QueueHandle_t frame_queue = NULL;
static TaskHandle_t decode_task_hdl = NULL;
static lv_timer_t* video_refresh_timer = NULL;
static volatile bool is_playing = false;
static lv_obj_t* video_img_obj = NULL;
static lv_img_dsc_t video_img_dsc;


static lv_obj_t* g_container = NULL;               // 父容器
static lv_timer_t* BUTTON_timer = NULL;            // BUTTON检测定时器
static bool video_state_machine = false;           // 当前是否在视频播放界面
static bool need_switch_to_video = false;          // 需要切换到视频播放
static uint32_t last_state_change_ms = 0;          // 上次状态切换时间
static String current_video_path = "";             // 当前选中的视频文件完整路径

// 视频信息条相关
static lv_obj_t* video_info_container = nullptr;
static lv_obj_t* video_time_label = nullptr;
static lv_obj_t* video_percent_label = nullptr;
static lv_obj_t* video_battery_area = nullptr;
static lv_obj_t* video_battery_bar = nullptr;
static lv_obj_t* video_battery_tip = nullptr;
static lv_timer_t* video_info_timer = nullptr;

// 视频进度滑块相关
static lv_obj_t* video_progress_slider = NULL;     // 滑块对象
static int64_t target_abs_offset = -1;             // 跳转目标绝对偏移量（-1表示无效）
static bool jump_pending = false;                  // 是否有待处理的跳转

static lv_obj_t* s_play_pause_btn = NULL;  
// BUTTON2 状态
static bool last_BUTTON2_level = false;
static uint32_t stable_start_BUTTON2 = 0;

static volatile bool video_paused = false;          // 暂停标志

// SD卡状态
static bool last_sd_card_state = true;

// ================= 绝对位置相关 =================
static uint64_t current_video_file_size = 0;           // 当前视频文件总大小
static uint64_t current_video_pos = 0;                 // 当前文件偏移量（由解码任务更新）
static SemaphoreHandle_t video_pos_mutex = NULL;       // 保护current_video_pos的互斥锁

// ================= 性能统计变量 =================
static uint32_t frame_count = 0;
static uint32_t total_frame_time = 0;
static uint32_t total_sd_read_time = 0;
static uint32_t total_decode_time = 0;
static uint32_t wait_buffer_time = 0;

// BUTTON 引脚状态管理结构
typedef struct {
    bool level;           // 当前稳定电平
    uint32_t stable_start;// 稳定开始时间
    bool last_stable;     // 上一次稳定电平
    uint32_t press_start; // 按下开始时间戳
    bool long_triggered;  // 是否已触发长按
} BUTTON_pin_state_t;


static BUTTON_pin_state_t BUTTON1_state;
static BUTTON_pin_state_t BUTTON2_state;
static BUTTON_pin_state_t BUTTON3_state;

// =================跳转命令结构体（绝对偏移） =================
typedef struct {
    int64_t abs_offset;   // 绝对偏移量，>=0有效
} jump_cmd_t;
static QueueHandle_t jump_cmd_queue = NULL;

// ================= 函数声明 =================
static void BUTTON_check_cb(lv_timer_t* timer);
static void video_file_selected_cb(const char* filename, void* user_data);
static void video_player_ui(lv_obj_t* parent, const char* filepath);
static void stop_video_playback(void);
static void video_refresh_timer_cb(lv_timer_t* timer);
static void video_decode_task(void *pvParameters);
static int jpegDrawCallback(JPEGDRAW *pDraw);
static void print_performance_info(void);
void fs_create_video(lv_obj_t* container);
static void show_video_file_selector(lv_obj_t* parent);

// 定义两个周期常量 (单位: ms)
#define LV_REFR_NORMAL_PERIOD  20    // 50fps -> 20ms
#define LV_REFR_VIDEO_PERIOD   100000 // 100秒刷新一次，等同于禁用

// 切换到视频模式（手动触发模式）
void set_lvgl_manual_refresh(bool manual) {
    lv_disp_t* disp = lv_disp_get_default();
    if (disp && disp->refr_timer) {
        if (manual) {
            lv_timer_set_period(disp->refr_timer, LV_REFR_VIDEO_PERIOD);
            Serial.println("LVGL 自动刷新已禁用 (周期设为 100s)");
        } else {
            lv_timer_set_period(disp->refr_timer, LV_REFR_NORMAL_PERIOD);
            Serial.println("LVGL 自动刷新已恢复 (50fps)");
        }
    }
}

static void create_video_info_ui(lv_obj_t* parent) {
    video_info_container = lv_obj_create(parent);
    lv_obj_set_size(video_info_container, 280, 25);
    
    // 旋转中心和角度
    lv_obj_set_style_transform_pivot_x(video_info_container, 0, 0);
    lv_obj_set_style_transform_pivot_y(video_info_container, 0, 0);
    lv_obj_set_style_transform_angle(video_info_container, 900, 0);

    lv_obj_set_pos(video_info_container, 230, 0); 

    lv_obj_set_style_pad_left(video_info_container, 70, 0); 
    lv_obj_set_style_pad_top(video_info_container, 15, 0);  
    
    lv_obj_set_flex_flow(video_info_container, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(video_info_container, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(video_info_container, 15, 0);

    // 样式美化
    lv_obj_set_style_bg_color(video_info_container, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(video_info_container, LV_OPA_40, 0); 
    lv_obj_set_style_border_width(video_info_container, 0, 0);
    lv_obj_add_flag(video_info_container, LV_OBJ_FLAG_HIDDEN);

    // 时间和百分比
    video_time_label = lv_label_create(video_info_container);
    lv_obj_set_style_text_font(video_time_label, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(video_time_label, lv_color_white(), 0);

    video_percent_label = lv_label_create(video_info_container);
    lv_obj_set_style_text_font(video_percent_label, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(video_percent_label, lv_color_white(), 0);

    // 电池根容器
    lv_obj_t* bat_root = lv_obj_create(video_info_container);
    lv_obj_set_size(bat_root, 30, 15);
    lv_obj_set_style_bg_opa(bat_root, 0, 0);
    lv_obj_set_style_border_width(bat_root, 0, 0);
    lv_obj_set_style_pad_all(bat_root, 0, 0);

    // 电池外框
    video_battery_area = lv_obj_create(bat_root);
    lv_obj_set_size(video_battery_area, 22, 12); 
    lv_obj_align(video_battery_area, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_set_style_border_width(video_battery_area, 1, 0);
    lv_obj_set_style_border_color(video_battery_area, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(video_battery_area, 0, 0);
    lv_obj_set_style_pad_all(video_battery_area, 1, 0); 

    // 电池内部电量条
    video_battery_bar = lv_obj_create(video_battery_area);
    lv_obj_set_size(video_battery_bar, 0, lv_pct(100)); 
    lv_obj_align(video_battery_bar, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_set_style_bg_color(video_battery_bar, lv_palette_main(LV_PALETTE_GREEN), 0);
    lv_obj_set_style_bg_opa(video_battery_bar, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(video_battery_bar, 0, 0);
    lv_obj_set_style_radius(video_battery_bar, 1, 0);

    // 电池头
    video_battery_tip = lv_obj_create(bat_root);
    lv_obj_set_size(video_battery_tip, 3, 6);
    lv_obj_align_to(video_battery_tip, video_battery_area, LV_ALIGN_OUT_RIGHT_MID, 0, 0);
    lv_obj_set_style_bg_color(video_battery_tip, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(video_battery_tip, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(video_battery_tip, 0, 0);
}

// 更新信息条内容
static void video_info_timer_cb(lv_timer_t* t) {
    // 更新时间
    time_t now = time(nullptr);
    struct tm* ptm = gmtime(&now);
    if (ptm && video_time_label) {
        char buf[6];
        snprintf(buf, sizeof(buf), "%02d:%02d", ptm->tm_hour, ptm->tm_min);
        lv_label_set_text(video_time_label, buf);
    }

    // 更新电池
    if (video_percent_label && video_battery_bar) {
        int pct = atomic_load_int(&battery_percentage); 
        pct = constrain(pct, 0, 100);
        
        lv_label_set_text_fmt(video_percent_label, "%d%%", pct);

        int bar_width = (pct * 18) / 100;
        if (bar_width < 1 && pct > 0) bar_width = 1;
        
        lv_obj_set_width(video_battery_bar, bar_width);

        // 颜色切换
        lv_color_t bat_color;
        if (pct <= 20)      bat_color = lv_palette_main(LV_PALETTE_RED);
        else if (pct <= 30) bat_color = lv_palette_main(LV_PALETTE_AMBER);
        else                bat_color = lv_palette_main(LV_PALETTE_GREEN);
        if (atomic_load_bool(&is_charging)) bat_color = lv_palette_main(LV_PALETTE_BLUE);
        lv_obj_set_style_bg_color(video_battery_bar, bat_color, 0);
    }
}
// ================= JPEG 解码回调 =================
static int jpegDrawCallback(JPEGDRAW *pDraw) {
    uint16_t *pTarget = (uint16_t *)frame_buffers[write_idx];
    uint16_t *pSrc = pDraw->pPixels;
    int x = pDraw->x, y = pDraw->y, w = pDraw->iWidth, h = pDraw->iHeight;

    for (int i = 0; i < h; i++) {
        uint32_t target_offset = (y + i) * VIDEO_WIDTH + x;
        memcpy(&pTarget[target_offset], &pSrc[i * w], w * 2);
    }
    return 1;
}

// ================= 打印性能信息 =================
static void print_performance_info(void) {
    if (frame_count == 0) return;
    uint32_t avg_frame_time = total_frame_time / frame_count;
    uint32_t avg_sd_read = total_sd_read_time / frame_count;
    uint32_t avg_decode = total_decode_time / frame_count;
    uint32_t avg_wait_buf = wait_buffer_time / frame_count;
    uint32_t fps = 1000 / avg_frame_time;
    Serial.printf("%d,%d,%d,%d,%d\n",
                  fps, avg_frame_time, avg_sd_read, avg_decode, avg_wait_buf);
    frame_count = 0;    
    total_frame_time = 0;
    total_sd_read_time = 0;
    total_decode_time = 0;
    wait_buffer_time = 0;
}
// 进度滑块事件回调
static void progress_slider_event_cb(lv_event_t* e) {
    lv_obj_t* slider = lv_event_get_target(e);
    int32_t val = lv_slider_get_value(slider);
    val = 1000 - val;

    uint64_t file_size = 0;
    if (video_pos_mutex) xSemaphoreTake(video_pos_mutex, portMAX_DELAY);
    file_size = current_video_file_size;
    xSemaphoreGive(video_pos_mutex);

    if (file_size == 0) return;

    // 计算目标绝对偏移量（使用 64 位避免溢出）
    uint64_t new_offset = (uint64_t)val * file_size / 1000;  // SLIDER_RANGE = 1000

    if (video_pos_mutex) xSemaphoreTake(video_pos_mutex, portMAX_DELAY);
    target_abs_offset = new_offset;
    jump_pending = true;
    xSemaphoreGive(video_pos_mutex);

    Serial.printf("滑块拖动：目标偏移 %llu / %llu (%.1f%%)\n", new_offset, file_size, (float)val/10);
}
// ================= SD 读取缓存 / 原始帧槽 (生产者 core1, 消费者 core0) =================
// 设计: 生产者把视频文件按小块 (SD_READ_CHUNK, =1 个 FATFS 扇区) DMA 读入 PSRAM 缓存, 抽出
// 一个完整 JPEG 帧后拷贝到空闲帧槽, 再通过队列交给消费者解码; 消费者解码与生产者读卡并行,
// 从而把 SD 读取时间藏到解码时间之下。小块读保证单次 DMA 占 PSRAM 的时间短、可被调度打断
// (DMA 无法中途取消, 所以不能一次搬太大)。
#define SD_READ_CHUNK   (4 * 1024)          // 每次读取块大小 (=1 个 FATFS 扇区)
#define READ_CACHE_SIZE READ_BUF_SIZE       // PSRAM 流缓存 (开始时读满, 之后阈值补满)
#define RAW_SLOT_NUM    3                   // 原始帧槽数量
#define RAW_SLOT_SIZE   READ_BUF_SIZE       // 单个原始帧槽

typedef struct {
    int      slot;      // 槽索引
    uint32_t len;       // 帧字节数
} raw_frame_t;

static uint8_t*      s_raw_slots[RAW_SLOT_NUM] = {NULL, NULL, NULL}; // 原始帧槽 (PSRAM)
static QueueHandle_t s_raw_q  = NULL;                 // raw_frame_t 队列 (生产者 -> 消费者)
static QueueHandle_t s_free_q = NULL;                 // 空闲槽索引队列
static TaskHandle_t  read_task_hdl = NULL;
static volatile bool s_read_done = false;             // 生产者是否已读完并退出

// CPU1: 视频读取任务 (生产者) —— 只做读卡/找帧界/入队, 与解码并行
static void video_read_task(void *pvParameters) {
    const char* filepath = (const char*)pvParameters;

    FsFile videoFile = sd.open(filepath, O_RDONLY);
    if (videoFile) {
        if (video_pos_mutex) xSemaphoreTake(video_pos_mutex, portMAX_DELAY);
        current_video_file_size = videoFile.size();
        xSemaphoreGive(video_pos_mutex);
        Serial.printf("视频文件大小: %llu 字节\n", current_video_file_size);
    } else {
        Serial.println("无法打开文件获取大小");
        current_video_file_size = 0;
    }
    Serial.printf("开始播放视频: %s\n", filepath);

    if (video_pos_mutex) xSemaphoreTake(video_pos_mutex, portMAX_DELAY);
    current_video_pos = videoFile.position();
    xSemaphoreGive(video_pos_mutex);

    uint8_t* cache = (uint8_t*)heap_caps_malloc(READ_CACHE_SIZE, MALLOC_CAP_SPIRAM);
    if (cache == NULL) {
        Serial.println("申请读取缓存失败！");
        videoFile.close();
        s_read_done = true;
        read_task_hdl = NULL;
        vTaskDelete(NULL);
        return;
    }

    uint32_t bytesBuffered = 0;
    bool eof = false;

    // 开始时先把缓存读满
    while (bytesBuffered + SD_READ_CHUNK <= READ_CACHE_SIZE && videoFile.available()) {
        int bytesRead = videoFile.read(cache + bytesBuffered, SD_READ_CHUNK);
        if (bytesRead > 0) bytesBuffered += bytesRead;
        else break;
    }

    while (is_playing) {
        // 处理跳转命令
        jump_cmd_t jump_cmd;
        if (xQueueReceive(jump_cmd_queue, &jump_cmd, 0) == pdTRUE) {
            int64_t target = jump_cmd.abs_offset;
            if (target >= 0) {
                uint64_t file_size = videoFile.size();
                if ((uint64_t)target > file_size) target = file_size;
                videoFile.seekSet(target);
                bytesBuffered = 0;
                eof = false;
                // 丢弃跳转前排队的原始帧, 归还槽
                raw_frame_t stale;
                while (xQueueReceive(s_raw_q, &stale, 0) == pdTRUE) {
                    xQueueSend(s_free_q, &stale.slot, 0);
                }
                if (video_pos_mutex) xSemaphoreTake(video_pos_mutex, portMAX_DELAY);
                current_video_pos = videoFile.position();
                xSemaphoreGive(video_pos_mutex);
                Serial.printf("绝对跳转: %lld\n", target);
            }
        }

        if (video_paused) { vTaskDelay(pdMS_TO_TICKS(10)); continue; }

        // 阈值补满: 剩余空间 >= 一个读块则读一小块 (单次 DMA 小, 可被打断)
        uint32_t sd_read_start = millis();
        if (!eof && bytesBuffered + SD_READ_CHUNK <= READ_CACHE_SIZE && videoFile.available()) {
            int bytesRead = videoFile.read(cache + bytesBuffered, SD_READ_CHUNK);
            if (bytesRead > 0) bytesBuffered += bytesRead;
            else if (!videoFile.available()) eof = true;
        }
        total_sd_read_time += (millis() - sd_read_start);

        if (bytesBuffered == 0 && eof) break;

        // 找帧头 SOI
        uint32_t frameStart = 0;
        bool foundStart = false;
        while (frameStart + 1 < bytesBuffered) {
            if (cache[frameStart] == 0xFF && cache[frameStart + 1] == 0xD8) { foundStart = true; break; }
            frameStart++;
        }
        if (!foundStart) {
            if (eof) break;
            if (bytesBuffered >= READ_CACHE_SIZE - 1) bytesBuffered = 0;  // 缓存满仍无帧头, 判为无效数据
            vTaskDelay(1);
            continue;
        }
        // 丢弃帧头前的无效字节
        if (frameStart > 0) {
            memmove(cache, cache + frameStart, bytesBuffered - frameStart);
            bytesBuffered -= frameStart;
        }

        // 找帧尾 EOI
        uint32_t frameEnd = 2;
        bool foundEnd = false;
        while (frameEnd + 1 < bytesBuffered) {
            if (cache[frameEnd] == 0xFF && cache[frameEnd + 1] == 0xD9) { frameEnd += 2; foundEnd = true; break; }
            frameEnd++;
        }
        if (!foundEnd) {
            if (eof) break;                                   // 末尾残帧, 丢弃
            if (bytesBuffered >= READ_CACHE_SIZE - 1) bytesBuffered = 0;  // 单帧超过缓存, 丢弃避免死锁
            vTaskDelay(1);
            continue;
        }

        // 取空闲帧槽, 拷贝整帧并入队
        int slot = -1;
        if (xQueueReceive(s_free_q, &slot, pdMS_TO_TICKS(200)) != pdTRUE) {
            continue;               // 消费者忙, 稍后再试
        }
        uint32_t frameLen = frameEnd;
        if (frameLen > RAW_SLOT_SIZE) frameLen = RAW_SLOT_SIZE;
        memcpy(s_raw_slots[slot], cache, frameLen);
        raw_frame_t item;
        item.slot = slot;
        item.len  = frameLen;
        if (xQueueSend(s_raw_q, &item, pdMS_TO_TICKS(200)) != pdTRUE) {
            xQueueSend(s_free_q, &slot, 0);
            vTaskDelay(1);
            continue;
        }

        // 从缓存中移除该帧
        uint32_t remaining = bytesBuffered - frameEnd;
        if (remaining > 0) memmove(cache, cache + frameEnd, remaining);
        bytesBuffered = remaining;

        if (video_pos_mutex) xSemaphoreTake(video_pos_mutex, portMAX_DELAY);
        current_video_pos = videoFile.position();
        xSemaphoreGive(video_pos_mutex);
    }

    heap_caps_free(cache);
    videoFile.close();
    s_read_done = true;
    read_task_hdl = NULL;
    Serial.println("视频读取任务结束");
    vTaskDelete(NULL);
}

//CPU0: 视频解码任务 (消费者) —— 只做解码, 读卡已由生产者并行完成
void video_decode_task(void *pvParameters) {
    (void)pvParameters;

    //动态申请
    jpeg = (JPEGDEC *)malloc(sizeof(JPEGDEC));
    if (jpeg == NULL) {
        Serial.println("申请 JPEGDEC 内存失败！");
        vTaskDelete(NULL);
        return;
    }

    int ready_to_show_idx = -1;
    write_idx = 0;

    while (is_playing) {
        if (video_paused) { vTaskDelay(pdMS_TO_TICKS(10)); continue; }

        uint32_t frame_start_time = millis();

        // 取一个原始帧
        raw_frame_t item;
        if (xQueueReceive(s_raw_q, &item, pdMS_TO_TICKS(100)) != pdTRUE) {
            if (s_read_done && uxQueueMessagesWaiting(s_raw_q) == 0) break;
            continue;
        }

        // 等待可写显示缓冲 (队列有空位 且 不覆盖正在显示的缓冲)
        uint32_t wait_buf_start = millis();
        int next_candidate = (write_idx + 1) % 3;
        while (uxQueueSpacesAvailable(frame_queue) == 0 || next_candidate == show_idx) {
            if (!is_playing) break;
            vTaskDelay(1);
        }
        if (!is_playing) { xQueueSend(s_free_q, &item.slot, 0); break; }
        write_idx = next_candidate;
        wait_buffer_time += (millis() - wait_buf_start);

        // 解码
        uint32_t decode_start = millis();
        bool decode_success = false;
        if (jpeg->openRAM(s_raw_slots[item.slot], item.len, jpegDrawCallback)) {
            jpeg->setPixelType(RGB565_LITTLE_ENDIAN);
            jpeg->decode(0, 0, 0);
            jpeg->close();
            decode_success = true;
            ready_to_show_idx = write_idx;
        }
        total_decode_time += (millis() - decode_start);

        // 归还原始帧槽
        xQueueSend(s_free_q, &item.slot, 0);

        if (decode_success) {
            xQueueSend(frame_queue, &ready_to_show_idx, 0);
        } else {
            Serial.println("[MJPEG] 帧解码失败！");
        }

        // 统计
        total_frame_time += (millis() - frame_start_time);
        frame_count++;
        if (frame_count >= FPS_PRINT_INTERVAL) {
            print_performance_info();
        }

        // 主动让出 1 tick: 否则解码任务持续占满 core0 会把 IDLE0 饿死, 触发任务看门狗
        vTaskDelay(1);
    }

    // 清理任务资源
    if (jpeg) {
        free(jpeg);
        jpeg = NULL;
    }
    is_playing = false;
    vTaskDelete(NULL);
}

// ================= LVGL 刷新回调 =================
void video_refresh_timer_cb(lv_timer_t* timer) {
    if (!is_playing || video_paused) return;

    // 记录上一次刷新的时间
    static uint32_t last_refr_time = 0;
    
    // 检查时间间隔是否满足 42ms
    if (millis() - last_refr_time < 45) {
        return; 
    }

    int ready_idx;
    // 使用 xQueuePeek 检查队列，不直接出队
    if (xQueuePeek(frame_queue, &ready_idx, 0) == pdTRUE) {
        // 正式出队并处理帧 (SD=SDMMC, 屏=SPI2, 不共总线, 无需互斥)
        xQueueReceive(frame_queue, &ready_idx, 0);
        show_idx = ready_idx;
        video_img_dsc.data = (const uint8_t*)frame_buffers[show_idx];
        lv_img_set_src(video_img_obj, &video_img_dsc);
        lv_obj_invalidate(video_img_obj);

        // 更新时间戳（在刷新开始前记录，确保间隔是针对帧起始时间的）
        last_refr_time = millis();

        // 手动触发屏幕刷新
        lv_refr_now(NULL);
    }
}

// ================= 停止视频播放，释放资源 =================
static void stop_video_playback(void) {
    if (!is_playing && decode_task_hdl == NULL && read_task_hdl == NULL) return;

    Serial.println("停止视频播放，释放资源");
    is_playing = false;

    // 等待读/解码任务结束
    if (decode_task_hdl != NULL || read_task_hdl != NULL) {
        vTaskDelay(150); // 给任务一点时间退出
        decode_task_hdl = NULL;
        read_task_hdl = NULL;
    }

    // 释放原始帧槽与队列
    for (int i = 0; i < RAW_SLOT_NUM; i++) {
        if (s_raw_slots[i]) {
            heap_caps_free(s_raw_slots[i]);
            s_raw_slots[i] = NULL;
        }
    }
    if (s_raw_q)  { vQueueDelete(s_raw_q);  s_raw_q = NULL; }
    if (s_free_q) { vQueueDelete(s_free_q); s_free_q = NULL; }
    s_read_done = false;

    // 删除刷新定时器
    if (video_refresh_timer) {
        lv_timer_del(video_refresh_timer);
        video_refresh_timer = NULL;
    }

    // 清空队列
    if (frame_queue) {
        int dummy;
        while (xQueueReceive(frame_queue, &dummy, 0) == pdTRUE);
        vQueueDelete(frame_queue);
        frame_queue = NULL;
    }

    // 释放帧缓冲区
    for (int i = 0; i < 3; i++) {
        if (frame_buffers[i]) {
            heap_caps_free(frame_buffers[i]);
            frame_buffers[i] = NULL;
        }
    }

    // 删除图像对象
    if (video_img_obj) {
        lv_obj_del(video_img_obj);
        video_img_obj = NULL;
    }
    // 删除滑块对象
    if (video_progress_slider) {
        lv_obj_del(video_progress_slider);
        video_progress_slider = NULL;
    }
    // 删除播放/暂停按钮
    extern lv_obj_t* s_play_pause_btn;  
    if (s_play_pause_btn) {
        lv_obj_del(s_play_pause_btn);
        s_play_pause_btn = NULL;
    }
        // 删除信息条定时器
    if (video_info_timer) {
        lv_timer_del(video_info_timer);
        video_info_timer = nullptr;
    }
    if (video_info_container) {
        lv_obj_del(video_info_container);
        video_info_container = nullptr;
        // 将所有子指针置空
        video_time_label = nullptr;
        video_percent_label = nullptr;
        video_battery_area = nullptr;
        video_battery_bar = nullptr;
        video_battery_tip = nullptr; 
    }
    // 凸起是独立对象
    if (video_battery_tip) {
        lv_obj_del(video_battery_tip);
        video_battery_tip = nullptr;
    }
    // 删除跳转命令队列
    if (jump_cmd_queue) {
        vQueueDelete(jump_cmd_queue);
        jump_cmd_queue = NULL;
    }
    // 重置文件大小
    if (video_pos_mutex) xSemaphoreTake(video_pos_mutex, portMAX_DELAY);
        current_video_file_size = 0;
    xSemaphoreGive(video_pos_mutex);

    show_idx = -1;
    write_idx = 0;
    video_paused = false;
    video_state_machine = false;

    target_abs_offset = -1;
    jump_pending = false;

    // 重置性能统计
    frame_count = 0;
    total_frame_time = 0;
    total_sd_read_time = 0;
    total_decode_time = 0;
    wait_buffer_time = 0;
    frame_count = 0;
    wait_buffer_time = 0;

    // 恢复 LVGL 默认的自动刷新
    set_lvgl_manual_refresh(false);
}
// 按钮点击回调函数
static void video_play_pause_btn_cb(lv_event_t* e) {
    if (!video_state_machine) return; // 不在视频播放界面则忽略

    if (video_paused) {
        // 恢复播放时保存当前亮度
        uint8_t current = atomic_load_int(&current_brightness);
        preferences.begin("watch", false);
        preferences.putUChar("brightness", current);
        preferences.end();
        
        // 如有跳转请求则发送命令
        if (jump_pending) {
            if (jump_cmd_queue) {
                jump_cmd_t cmd = { .abs_offset = target_abs_offset };
                xQueueSend(jump_cmd_queue, &cmd, 0);
            }
            jump_pending = false;
        }
        video_paused = false;
        set_lvgl_manual_refresh(true);
        // 隐藏滑块
        if (video_progress_slider) {
            lv_obj_add_flag(video_progress_slider, LV_OBJ_FLAG_HIDDEN);
        }
        // 隐藏信息条
        if (video_info_container) {
            lv_obj_add_flag(video_info_container, LV_OBJ_FLAG_HIDDEN);
        }
        Serial.println("视频恢复（按钮）");
    } else {
        // 暂停：记录当前播放位置，并显示滑块
        if (video_pos_mutex) xSemaphoreTake(video_pos_mutex, portMAX_DELAY);
        target_abs_offset = current_video_pos;
        xSemaphoreGive(video_pos_mutex);
        jump_pending = false;   // 初始无跳转
        video_paused = true;
        set_lvgl_manual_refresh(false);
        // 更新滑块位置并显示
        if (video_progress_slider) {
            uint64_t file_size = 0;
            if (video_pos_mutex) xSemaphoreTake(video_pos_mutex, portMAX_DELAY);
            file_size = current_video_file_size;
            xSemaphoreGive(video_pos_mutex);
            if (file_size > 0) {
                int32_t slider_val = (target_abs_offset * 1000) / file_size;
                slider_val = 1000 - slider_val;   // 反转显示值
                lv_slider_set_value(video_progress_slider, slider_val, LV_ANIM_OFF);
            }
            lv_obj_clear_flag(video_progress_slider, LV_OBJ_FLAG_HIDDEN);
        }
         // 显示信息条并立即更新内容
        if (video_info_container) {
            video_info_timer_cb(NULL);   // 立即刷新一次
            lv_obj_clear_flag(video_info_container, LV_OBJ_FLAG_HIDDEN);
        }
        Serial.printf("视频暂停（按钮），基准位置: %lld\n", target_abs_offset);
    }
}
// ================= 创建视频播放界面 =================
static void video_player_ui(lv_obj_t* parent, const char* filepath) {
    // 申请三个 SPIRAM 缓冲
    for (int i = 0; i < 3; i++) {
        frame_buffers[i] = (uint8_t*)heap_caps_malloc(FRAME_BUF_SIZE, MALLOC_CAP_SPIRAM);
        if (frame_buffers[i] == NULL) {
            Serial.printf("申请帧缓冲区 %d 失败！\n", i);
            stop_video_playback();
            return;
        }
    }

    // 创建队列（长度2）
    frame_queue = xQueueCreate(2, sizeof(int));
    if (frame_queue == NULL) {
        Serial.println("创建队列失败！");
        stop_video_playback();
        return;
    }

    // 创建原始帧槽与生产者/消费者队列
    for (int i = 0; i < RAW_SLOT_NUM; i++) {
        s_raw_slots[i] = (uint8_t*)heap_caps_malloc(RAW_SLOT_SIZE, MALLOC_CAP_SPIRAM);
        if (s_raw_slots[i] == NULL) {
            Serial.printf("申请原始帧槽 %d 失败！\n", i);
            stop_video_playback();
            return;
        }
    }
    s_raw_q  = xQueueCreate(RAW_SLOT_NUM, sizeof(raw_frame_t));
    s_free_q = xQueueCreate(RAW_SLOT_NUM, sizeof(int));
    if (s_raw_q == NULL || s_free_q == NULL) {
        Serial.println("创建原始帧队列失败！");
        stop_video_playback();
        return;
    }
    for (int i = 0; i < RAW_SLOT_NUM; i++) {
        xQueueSend(s_free_q, &i, 0);
    }
    s_read_done = false;

    // 初始化图像描述符
    video_img_dsc.header.always_zero = 0;
    video_img_dsc.header.w = VIDEO_WIDTH;
    video_img_dsc.header.h = VIDEO_HEIGHT;
    video_img_dsc.header.cf = LV_IMG_CF_TRUE_COLOR;
    video_img_dsc.data_size = FRAME_BUF_SIZE;
    video_img_dsc.data = (const uint8_t*)frame_buffers[0];

    // 创建图像对象
    video_img_obj = lv_img_create(parent);
    lv_img_set_src(video_img_obj, &video_img_dsc);
    lv_obj_align(video_img_obj, LV_ALIGN_CENTER, 0, 0);

    // 创建进度滑块（宽10，高260，位置5,0），初始隐藏
    video_progress_slider = lv_slider_create(parent);
    lv_obj_set_size(video_progress_slider, 10, 245);
    lv_obj_set_pos(video_progress_slider, 5, 5);
    lv_slider_set_range(video_progress_slider, 0, 1000);  // 千分比精度
    lv_slider_set_value(video_progress_slider, 0, LV_ANIM_OFF);
    lv_obj_add_flag(video_progress_slider, LV_OBJ_FLAG_HIDDEN);  // 默认隐藏

    // 添加事件回调
    lv_obj_add_event_cb(video_progress_slider, progress_slider_event_cb, LV_EVENT_VALUE_CHANGED, NULL);

    // 设置滑块样式
    lv_obj_set_style_bg_color(video_progress_slider, lv_color_make(0,122,255), LV_PART_MAIN);      // 背景（轨道）蓝色
    lv_obj_set_style_bg_opa(video_progress_slider, LV_OPA_COVER, LV_PART_MAIN);                 
    lv_obj_set_style_bg_color(video_progress_slider, lv_color_make(200,200,200), LV_PART_INDICATOR); // 指示器淡灰色
    lv_obj_set_style_bg_opa(video_progress_slider, LV_OPA_COVER, LV_PART_INDICATOR);              
    lv_obj_set_style_bg_color(video_progress_slider, lv_color_make(0,80,180), LV_PART_KNOB);       // 旋钮深蓝色
    lv_obj_set_style_bg_opa(video_progress_slider, LV_OPA_COVER, LV_PART_KNOB);                  
    lv_obj_set_style_radius(video_progress_slider, 5, LV_PART_MAIN);

    // 创建播放/暂停按钮
    lv_obj_t* play_pause_btn = lv_btn_create(parent);
    lv_obj_set_pos(play_pause_btn, 40, 30);      // x=40, y=30
    lv_obj_set_size(play_pause_btn, 170, 220);   // 宽170，高220
    // 设置为完全透明
    lv_obj_set_style_bg_opa(play_pause_btn, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(play_pause_btn, 0, 0);
    lv_obj_set_style_shadow_width(play_pause_btn, 0, 0);
    lv_obj_set_style_outline_width(play_pause_btn, 0, 0);
    lv_obj_clear_flag(play_pause_btn, LV_OBJ_FLAG_CLICK_FOCUSABLE); // 禁止聚焦
    lv_obj_clear_flag(play_pause_btn, LV_OBJ_FLAG_GESTURE_BUBBLE);  // 禁止手势冒泡
    lv_obj_add_event_cb(play_pause_btn, video_play_pause_btn_cb, LV_EVENT_CLICKED, NULL);

    // 保存按钮对象指针以便后续删除
    s_play_pause_btn = play_pause_btn;

    // 创建视频信息条
    create_video_info_ui(parent);
    // 创建信息更新定时器（每秒）
    video_info_timer = lv_timer_create(video_info_timer_cb, 1000, NULL);
    // 立即更新一次
    video_info_timer_cb(NULL);

    // 创建跳转命令队列
    jump_cmd_queue = xQueueCreate(2, sizeof(jump_cmd_t));
    if (jump_cmd_queue == NULL) {
        Serial.println("创建跳转队列失败！");
        stop_video_playback();
        return;
    }

    // 初始化暂停标志
    video_paused = false;

    // 启动读卡任务(生产者, core1, prio2 = 与 lvgl 同级, 时间片轮转避免互相饿死)
    // 与 解码任务(消费者, core0, prio5)
    is_playing = true;
    show_idx = -1;
    write_idx = 0;
    xTaskCreatePinnedToCore(video_read_task, "VideoRead", 8192, (void*)filepath, 2, &read_task_hdl, 1);
    xTaskCreatePinnedToCore(video_decode_task, "VideoDecode", 8192, NULL, 5, &decode_task_hdl, 0);

    // 创建 LVGL 刷新定时器
    video_refresh_timer = lv_timer_create(video_refresh_timer_cb, 2, NULL); 
    video_state_machine = true;
    
    // 暂停 LVGL 默认的自动刷新，完全交由手动控制
    set_lvgl_manual_refresh(true);
    Serial.println("视频播放界面已创建");
}

//文件选择回调
static void video_file_selected_cb(const char* filename, void* user_data) {
    // 检查是否为重新扫描标记
    if (strcmp(filename, SCAN_SPECIAL_FILENAME) == 0) {
        Serial.println("用户选择重新扫描视频文件");
        // 返回主界面
        fs_do_exit();
        // 停止视频播放
        stop_video_playback();
        // 销毁文件选择器
        destroy_file_selection_list(); 
        // 清理 BUTTON 定时器
        if (BUTTON_timer) {
            lv_timer_del(BUTTON_timer);
            BUTTON_timer = NULL;
        }
        // 设置 SD 卡重新扫描标志
        if (xSemaphoreTake(sd_state_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
            sd_force_scan = true;
            xSemaphoreGive(sd_state_mutex);
        }
        
        return;
    }
    
    // 正常文件选择
    Serial.printf("选中视频文件：%s\n", filename);
    current_video_path = String(MJPEG_DIR) + filename;
    need_switch_to_video = true;
}
// ================= BUTTON 检测定时器回调 =================
static void BUTTON_check_cb(lv_timer_t* timer) {
    uint32_t now = lv_tick_get();

    // ---------- SD卡拔出检测 ----------
    static bool current_sd_state = false;
    if (xSemaphoreTake(sd_state_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        current_sd_state = sd_card_inserted;
        xSemaphoreGive(sd_state_mutex);
    }
    if (last_sd_card_state == true && current_sd_state == false) {
        Serial.println("检测到SD卡拔出，清理视频资源并返回");
        fs_do_exit();
        stop_video_playback();
        destroy_file_selection_list();
        
        if (BUTTON_timer) {
            lv_timer_del(BUTTON_timer);
            BUTTON_timer = NULL;
        }
        video_state_machine = false;
        need_switch_to_video = false;
        last_sd_card_state = current_sd_state;
        return;
    }
    last_sd_card_state = current_sd_state;

    // 读取当前BUTTON电平
    bool BUTTON1 = digitalRead(BUTTON_1);
    bool BUTTON2 = digitalRead(BUTTON_2);
    bool BUTTON3 = digitalRead(BUTTON_3);

    // BUTTON2 处理
    if (BUTTON2 != BUTTON2_state.level) {
        BUTTON2_state.stable_start = now;
        BUTTON2_state.level = BUTTON2;
    } else if (now - BUTTON2_state.stable_start >= DEBOUNCE_THRESHOLD) {
        // 上升沿（按下）
        if (BUTTON2_state.level == true && BUTTON2_state.last_stable == false) {
            BUTTON2_state.press_start = now;   // 记录按下时刻
        }
        // 下降沿（释放）
        else if (BUTTON2_state.level == false && BUTTON2_state.last_stable == true) {
            uint32_t press_duration = now - BUTTON2_state.press_start;
            if (now - last_state_change_ms >= STATE_CHANGE_COOLDOWN) {
                last_state_change_ms = now; // 更新最后一次状态切换的时间
                if (video_state_machine) {
                    // 视频播放界面：退出视频，返回文件选择器
                    Serial.println("BUTTON2短按，退出视频播放");
                    if (fullscreen_container) lv_obj_move_foreground(fullscreen_container);
                    stop_video_playback();
                    show_video_file_selector(g_container);
                } else {
                    // 文件选择器界面：退出整个视频功能，返回主界面
                    Serial.println("BUTTON2短按，退出文件选择器");
                    fs_do_exit();
                    destroy_file_selection_list();
                    
                    if (BUTTON_timer) {
                        lv_timer_del(BUTTON_timer);
                        BUTTON_timer = NULL;
                    }
                    return;  // 定时器已删除，直接返回
                }
            } else {
                Serial.println("BUTTON2 触发太快，处于冷却时间，忽略操作");
            }
        }
        BUTTON2_state.last_stable = BUTTON2_state.level;
    }

    // BUTTON1 亮度减（播放状态）
    if (video_state_machine && !video_paused) {
        if (BUTTON1 == true) {
            // 按键按下：亮度减1
            uint8_t current = atomic_load_int(&current_brightness);
            if (current > 1) {
                current--;
            } else {
                current = 1;  // 最低亮度设为1
            }
            atomic_store_int(&current_brightness, current);
            
            // 立即设置亮度
            set_mapped_brightness(current);
            if (!video_paused) {
                preferences.begin("watch", false);
                preferences.putUChar("brightness", current);
                preferences.end();
            }
            
            Serial.printf("亮度减1: %d\n", current);
        }
        if (BUTTON3 == true) {
            // 按键按下：亮度加1
            uint8_t current = atomic_load_int(&current_brightness);
            if (current < 255) {
                current++;
            }
            atomic_store_int(&current_brightness, current);
            
            set_mapped_brightness(current);
            
            // 保存到Preferences（仅在非暂停状态下保存，避免高频写入）
            if (!video_paused) {
                preferences.begin("watch", false);
                preferences.putUChar("brightness", current);
                preferences.end();
            }
            
            Serial.printf("亮度加1: %d\n", current);
        }
    }

    // BUTTON1和BUTTON3跳转功能，只在暂停状态下使用
    if (video_state_machine && video_paused) {
        // BUTTON1 向前跳转
        if (BUTTON1 != BUTTON1_state.level) {
            BUTTON1_state.stable_start = now;
            BUTTON1_state.level = BUTTON1;
            // 重置长按相关变量
            if (BUTTON1_state.level == true) {
                BUTTON1_state.press_start = now;
                BUTTON1_state.long_triggered = false;
            }
        } else if (now - BUTTON1_state.stable_start >= DEBOUNCE_TIME) {
            // 按键持续按下时的处理
            if (BUTTON1_state.level == true) {
                uint32_t press_duration = now - BUTTON1_state.press_start;
                
                if (press_duration > 100) {
                    // 执行跳转
                    target_abs_offset -= 1024 * 1024;
                    if (target_abs_offset < 0) target_abs_offset = 0;
                    jump_pending = true;
                    
                    // 更新滑块
                    if (video_progress_slider && current_video_file_size > 0) {
                        int32_t slider_val = (target_abs_offset * 1000) / current_video_file_size;
                        slider_val = 1000 - slider_val;
                        lv_slider_set_value(video_progress_slider, slider_val, LV_ANIM_OFF);
                    }
                    
                    Serial.printf("向前跳转到: %lld\n", target_abs_offset);
                    
                    // 重置按压时间
                    BUTTON1_state.press_start = now;
                }
            }
            // 下降沿（释放）处理
            else if (BUTTON1_state.level == false && BUTTON1_state.last_stable == true) {
                // 按键释放，清除长按标志
                BUTTON1_state.long_triggered = false;
            }
            BUTTON1_state.last_stable = BUTTON1_state.level;
        }

        // BUTTON3 向后跳转
        if (BUTTON3 != BUTTON3_state.level) {
            BUTTON3_state.stable_start = now;
            BUTTON3_state.level = BUTTON3;
            // 重置长按相关变量
            if (BUTTON3_state.level == true) {
                BUTTON3_state.press_start = now;
                BUTTON3_state.long_triggered = false;
            }
        } else if (now - BUTTON3_state.stable_start >= DEBOUNCE_TIME) {
            // 按键持续按下时的处理
            if (BUTTON3_state.level == true) {
                uint32_t press_duration = now - BUTTON3_state.press_start;
                
                if (press_duration > 100) {
                    target_abs_offset += 1024 * 1024;
                    
                    // 读取文件大小并限制
                    uint64_t file_size = 0;
                    if (video_pos_mutex) xSemaphoreTake(video_pos_mutex, portMAX_DELAY);
                    file_size = current_video_file_size;
                    xSemaphoreGive(video_pos_mutex);
                    
                    if ((uint64_t)target_abs_offset > file_size) {
                        target_abs_offset = file_size;
                    }
                    
                    jump_pending = true;
                    
                    if (video_progress_slider && file_size > 0) {
                        int32_t slider_val = (target_abs_offset * 1000) / file_size;
                        slider_val = 1000 - slider_val;
                        lv_slider_set_value(video_progress_slider, slider_val, LV_ANIM_OFF);
                    }
                    
                    Serial.printf("向后跳转到: %lld\n", target_abs_offset);
                    
                    // 重置按压时间
                    BUTTON3_state.press_start = now;
                }
            }
            // 下降沿（释放）处理
            else if (BUTTON3_state.level == false && BUTTON3_state.last_stable == true) {
                // 按键释放，清除长按标志
                BUTTON3_state.long_triggered = false;
            }
            BUTTON3_state.last_stable = BUTTON3_state.level;
        }
    }

    //处理从文件选择器切换到视频播放
    if (need_switch_to_video) {
        destroy_file_selection_list();
        video_player_ui(g_container, current_video_path.c_str());
        need_switch_to_video = false;
    }
}

// ================= 创建视频文件选择界面 =================
void fs_create_video(lv_obj_t* container) {
    g_container = container;

    // 重置跳转相关变量
    target_abs_offset = -1;
    jump_pending = false;

    // 确保之前没有正在播放的视频
    stop_video_playback();

    // 初始化 BUTTON 引脚状态
    BUTTON2_state.level = digitalRead(BUTTON_2);
    BUTTON2_state.stable_start = lv_tick_get();
    BUTTON2_state.last_stable = BUTTON2_state.level;
    BUTTON2_state.press_start = 0;
    BUTTON2_state.long_triggered = false;

    BUTTON1_state.level = digitalRead(BUTTON_1);
    BUTTON1_state.stable_start = lv_tick_get();
    BUTTON1_state.last_stable = BUTTON1_state.level;
    BUTTON1_state.press_start = 0;
    BUTTON1_state.long_triggered = false;

    BUTTON3_state.level = digitalRead(BUTTON_3);
    BUTTON3_state.stable_start = lv_tick_get();
    BUTTON3_state.last_stable = BUTTON3_state.level;
    BUTTON3_state.press_start = 0;
    BUTTON3_state.long_triggered = false;

    // 创建 BUTTON 检测定时器
    if (BUTTON_timer == NULL) {
        BUTTON_timer = lv_timer_create(BUTTON_check_cb, TIMER_PERIOD, NULL);
        if (!BUTTON_timer) {
            LV_LOG_WARN("视频BUTTON定时器创建失败");
        } else {
            LV_LOG_USER("视频BUTTON定时器已启动");
        }
    }

    // 创建位置互斥锁
    if (video_pos_mutex == NULL) {
        video_pos_mutex = xSemaphoreCreateMutex();
        if (video_pos_mutex == NULL) {
            Serial.println("创建video_pos_mutex失败");
        }
    }

    // 显示文件选择器
    show_video_file_selector(container);
}

static void show_video_file_selector(lv_obj_t* parent) {
    g_file_selection_instance = file_selection_create(
        parent,
        FILE_TYPE_VIDEO,
        video_file_selected_cb,
        NULL
    );
    LV_LOG_USER("视频文件选择界面已显示");
}