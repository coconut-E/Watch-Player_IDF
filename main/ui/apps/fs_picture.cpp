#include "fullscreen_interfaces.h"
#include "ImageProcessor.h"


extern SdFat sd;
extern bool sd_card_initialized;
extern SemaphoreHandle_t sd_state_mutex;

// 结构体定义 
struct DecodeRequest {
    char path[256];
};

struct DecodeResult {
    uint16_t* buffer;
    size_t size;
    bool success;
    bool needs_removal; 
};

// 全局变量
static lv_obj_t* img_obj = nullptr;
static lv_obj_t* loading_bar = nullptr;
static lv_obj_t* msg_label = nullptr;
static lv_timer_t* monitor_timer = nullptr;

static std::vector<String> file_list; 
static int current_index = 0;
static int last_nav_dir = 1; // 记录导航方向 
static uint16_t* current_display_buffer = nullptr;

static TaskHandle_t decode_task_handle = nullptr;
static QueueHandle_t request_queue = nullptr;
static volatile int loading_progress = 0;
static volatile bool is_decoding = false;
static volatile bool decode_finished = false;
static DecodeResult pending_result;
static volatile bool exit_requested = false;


static bool last_sd_state = false; // 记录上次检测到的SD卡初始化状态
static volatile bool task_quit_flag = false; // 用于通知解码任务自删


// 自动轮播相关变量
static int carousel_interval = 0;          // 读取到的间隔秒数(0表示未设置)
static int carousel_direction = 0;         // 0: 关闭, 1: 下一张, -1: 上一张
static uint32_t carousel_idle_elapsed_ms  = 0;
static lv_obj_t* carousel_tip_label = nullptr;
static lv_timer_t* carousel_tip_timer = nullptr;


#define SCAN_LIST_PATH "/ScanList/picture.txt"
#define IMG_ROOT_PATH "/图片/"
#define STACK_SIZE_PICTURE  (24 * 1024)

static void load_file_list_to_ram();
static void request_new_image(int index);
static void clean_up_resources();
static void save_position_and_exit();
static void picture_gesture_cb(lv_event_t* e);
static void on_progress_worker(int percent);

//解码任务
void picture_decode_task(void* param) {
    ImageProcessor* localImgProc = new ImageProcessor();
    localImgProc->setProgressCallback(on_progress_worker);
    DecodeRequest req;
    while (1) {
        // 非阻塞检查
        if (xQueueReceive(request_queue, &req, pdMS_TO_TICKS(100)) == pdTRUE) {
            is_decoding = true;
            decode_finished = false;
            loading_progress = 0;

            uint16_t* new_buf = nullptr;
            size_t new_size = 0;
            ImageProcessor::ErrorCode res = localImgProc->loadAndProcessImage(req.path, &new_buf, &new_size); 

            pending_result.buffer = new_buf;
            pending_result.size = new_size;
            pending_result.success = (res == ImageProcessor::SUCCESS);

            if (res != ImageProcessor::SUCCESS) {
                if (sd.exists(req.path)) sd.remove(req.path); 
                pending_result.needs_removal = true; 
            } else {
                pending_result.needs_removal = false;
            }

            is_decoding = false;
            decode_finished = true;
        }

        // 检查是否需要自删
        if (task_quit_flag) {
            delete localImgProc;
            decode_task_handle = nullptr;
            vTaskDelete(NULL);
        }
    }
}

static void on_progress_worker(int percent) { 
    loading_progress = percent; 
}

static void load_file_list_to_ram() {
    file_list.clear();
    FsFile file = sd.open(SCAN_LIST_PATH, O_RDONLY);
    if (!file) return;
    while (file.available()) {
        String line = file.readStringUntil('\n');
        line.trim();
        if (line.length() > 0 && !line.startsWith("FileCount:")) {
            file_list.push_back(line);
        }
    }
    file.close();
}
// 显示轮播状态提示 UI
static void show_carousel_tip() {
    const char* text = "";
    if (carousel_direction == 1) text = "自动轮播\n 下一张";
    else if (carousel_direction == -1) text = "自动轮播\n 上一张";
    else text = "已关闭轮播";

    if (!carousel_tip_label) {
        carousel_tip_label = lv_label_create(lv_scr_act());
        lv_obj_set_style_text_font(carousel_tip_label, &tip_display_35, 0);
        lv_obj_set_style_text_color(carousel_tip_label, lv_color_white(), 0);
        lv_obj_set_style_bg_opa(carousel_tip_label, LV_OPA_60, 0);
        lv_obj_set_style_bg_color(carousel_tip_label, lv_color_black(), 0);
        lv_obj_set_style_pad_ver(carousel_tip_label, 10, 0);
        lv_obj_set_style_pad_hor(carousel_tip_label, 15, 0);
        lv_obj_set_style_radius(carousel_tip_label, 5, 0);
        lv_obj_align(carousel_tip_label, LV_ALIGN_TOP_MID, 0, 20);
        lv_obj_add_flag(carousel_tip_label, LV_OBJ_FLAG_HIDDEN);
    }

    lv_label_set_text(carousel_tip_label, text);
    lv_obj_clear_flag(carousel_tip_label, LV_OBJ_FLAG_HIDDEN);

    // 重置或创建1秒后隐藏的定时器
    if (carousel_tip_timer) {
        lv_timer_reset(carousel_tip_timer);
    } else {
        carousel_tip_timer = lv_timer_create([](lv_timer_t* t) {
            if (carousel_tip_label && lv_obj_is_valid(carousel_tip_label)) {
                lv_obj_add_flag(carousel_tip_label, LV_OBJ_FLAG_HIDDEN);
            }
            carousel_tip_timer = nullptr;
        }, 1000, NULL);
        lv_timer_set_repeat_count(carousel_tip_timer, 1);
    }
}

static void request_new_image(int index) {
    if (file_list.empty()) {
        lv_label_set_text(msg_label, "无文件");
        lv_obj_add_flag(loading_bar, LV_OBJ_FLAG_HIDDEN);
        if (current_display_buffer) {
            free(current_display_buffer);
            current_display_buffer = nullptr;
            lv_img_set_src(img_obj, NULL);
        }
        return;
    }
    
    if (is_decoding) return;

    lv_bar_set_value(loading_bar, 0, LV_ANIM_OFF);
    lv_obj_clear_flag(loading_bar, LV_OBJ_FLAG_HIDDEN);
    
    DecodeRequest req;
    snprintf(req.path, sizeof(req.path), "%s%s", IMG_ROOT_PATH, file_list[index].c_str());
    xQueueSend(request_queue, &req, 0);
}

// UI定时器
static void monitor_timer_cb(lv_timer_t* timer) {
    // 按键检测退出
    if (digitalRead(BUTTON_2) == HIGH) { 
        // 如果正在解码，忽略按键退出
        if (is_decoding) {
            return;
        }
        save_position_and_exit(); 
        return; 
    }
    //自动轮播按键检测
    static bool btn1_prev = false;
    static bool btn3_prev = false;
    
    bool btn1_curr = (digitalRead(BUTTON_1) == HIGH);
    bool btn3_curr = (digitalRead(BUTTON_3) == HIGH);
    
    // 按钮1：切换上一张 / 关闭
    if (btn1_curr && !btn1_prev) {
        if (carousel_direction == -1) carousel_direction = 0; // 再按关闭
        else carousel_direction = -1;                        // 开启上一张
        carousel_idle_elapsed_ms = 0; // 按下时重置计时
        show_carousel_tip();
    }
    // 按钮3：切换下一张 / 关闭
    if (btn3_curr && !btn3_prev) {
        if (carousel_direction == 1) carousel_direction = 0; // 再按关闭
        else carousel_direction = 1;                         // 开启下一张
        carousel_idle_elapsed_ms = 0; // 按下时重置计时
        show_carousel_tip();
    }
    
    btn1_prev = btn1_curr;
    btn3_prev = btn3_curr;  

    //SD卡拔出检测 
    bool current_sd_ok = false;
    if (xSemaphoreTake(sd_state_mutex, 0) == pdTRUE) {
        current_sd_ok = sd_card_initialized;
        xSemaphoreGive(sd_state_mutex);
    }

    if (last_sd_state == true && current_sd_ok == false) {
        Serial.println("[图片查看器] 检测到SD卡拔出，退出...");
        save_position_and_exit();
        return;
    }
    last_sd_state = current_sd_ok;

    // 进度条显示逻辑
    if (is_decoding) {
        lv_obj_clear_flag(loading_bar, LV_OBJ_FLAG_HIDDEN);
        lv_bar_set_value(loading_bar, loading_progress, LV_ANIM_ON);
    } else {
        lv_obj_add_flag(loading_bar, LV_OBJ_FLAG_HIDDEN);
    }
    // 正在加载中：强制清零计时，不进行累加
    if (is_decoding) {
        carousel_idle_elapsed_ms = 0;
    } 
    // 空闲状态：进行累加并判断是否达到间隔时间
    else if (carousel_direction != 0 && carousel_interval > 0 && !file_list.empty()) {
        // 每次定时器回调(50ms)累加一次
        carousel_idle_elapsed_ms += 50; 
        
        if (carousel_idle_elapsed_ms >= (uint32_t)carousel_interval * 1000) {
            carousel_idle_elapsed_ms = 0; // 达到间隔，触发后清零重新计时
            
            if (carousel_direction == 1) {
                current_index = (current_index + 1) % file_list.size();
            } else if (carousel_direction == -1) {
                current_index = (current_index - 1 + file_list.size()) % file_list.size();
            }
            request_new_image(current_index);
        }
    }


    // 处理解码完成结果
    if (decode_finished) {
        decode_finished = false;

        // 错误处理时的方向判定
        if (pending_result.needs_removal) {
            if (!file_list.empty()) {
                file_list.erase(file_list.begin() + current_index);
            }

            if (file_list.empty()) {
                lv_label_set_text(msg_label, "无文件");
                lv_obj_add_flag(loading_bar, LV_OBJ_FLAG_HIDDEN);
                if (current_display_buffer) { 
                    free(current_display_buffer); 
                    current_display_buffer = nullptr; 
                }
                lv_img_set_src(img_obj, NULL);
            } else {
                if (last_nav_dir == -1) {
                    current_index--; 
                }
                // 边界修正
                if (current_index >= file_list.size()) current_index = 0;
                if (current_index < 0) current_index = file_list.size() - 1;

                request_new_image(current_index);
            }
            return; 
        }

        if (pending_result.success && pending_result.buffer) {
            lv_label_set_text(msg_label, ""); 
            if (current_display_buffer) free(current_display_buffer);
            current_display_buffer = pending_result.buffer;

            static lv_img_dsc_t img_dsc;
            img_dsc.header.always_zero = 0;
            img_dsc.header.w = 240; 
            img_dsc.header.h = 280; 
            img_dsc.data_size = pending_result.size;
            img_dsc.header.cf = LV_IMG_CF_TRUE_COLOR;
            img_dsc.data = (const uint8_t*)current_display_buffer;
            lv_img_set_src(img_obj, &img_dsc);
        }

        lv_obj_add_flag(loading_bar, LV_OBJ_FLAG_HIDDEN);
        if (exit_requested) { 
            exit_requested = false; 
            save_position_and_exit(); 
        }
    }
}

//手势与退出管理 
static void picture_gesture_cb(lv_event_t* e) {
    if (is_decoding || file_list.empty()) return;
    lv_dir_t dir = lv_indev_get_gesture_dir(lv_indev_get_act());
    if (dir == LV_DIR_TOP) {
        last_nav_dir = 1; // 记录方向：向后
        current_index = (current_index + 1) % file_list.size();
        carousel_idle_elapsed_ms = 0; //用户手动滑动，重置轮播计时
        request_new_image(current_index);
    } else if (dir == LV_DIR_BOTTOM) {
        last_nav_dir = -1; // 记录方向：向前
        current_index = (current_index - 1 < 0) ? file_list.size() - 1 : current_index - 1;
        carousel_idle_elapsed_ms = 0; // 用户手动滑动，重置轮播计时
        request_new_image(current_index);
    }
}


static void save_position_and_exit() {
    FsFile listFile = sd.open(SCAN_LIST_PATH, O_WRONLY | O_CREAT | O_TRUNC);
    if (listFile) {
        for (const auto& name : file_list) listFile.println(name);
        listFile.close();
    }
    preferences.begin("watch", false);
    preferences.putInt("picture", current_index); 
    preferences.end();
    fs_do_exit(); 
    clean_up_resources();
}

//清理与退出 
static void clean_up_resources() {
    //先通知任务自删
    task_quit_flag = true; 
    
    //清理UI组件
    if (lv_scr_act()) lv_obj_remove_event_cb(lv_scr_act(), picture_gesture_cb);
    if (monitor_timer) { 
        lv_timer_del(monitor_timer); 
        monitor_timer = nullptr; 
    }
    // 清理自动轮播相关资源
    if (carousel_tip_timer) {
        lv_timer_del(carousel_tip_timer);
        carousel_tip_timer = nullptr;
    }
    if (carousel_tip_label && lv_obj_is_valid(carousel_tip_label)) {
        lv_obj_del(carousel_tip_label);
        carousel_tip_label = nullptr;
    }
    // 释放内存
    if (current_display_buffer) { 
        free(current_display_buffer); 
        current_display_buffer = nullptr; 
    }
    
    //清空文件列表
    file_list.clear();
    std::vector<String>().swap(file_list);
    
    //重置状态变量
    exit_requested = false;
    is_decoding = false;
    decode_finished = false;
    loading_progress = 0;

    carousel_direction = 0;
    carousel_interval = 0;
    carousel_idle_elapsed_ms = 0;
}
 
// 入口函数 
void fs_create_picture(lv_obj_t* container) {
    // 初始化变量
    task_quit_flag = false;
    exit_requested = false;
    is_decoding = false;
    decode_finished = false;

    // 读取自动轮播间隔配置
    preferences.begin("watch", true);
    carousel_interval = preferences.getInt("interval", 0); // 默认0代表未启用
    preferences.end();
    Serial.println(carousel_interval);
    if (carousel_interval < 1) carousel_interval = 1; // 确保非0s
    
    carousel_direction = 0; // 每次进入默认关闭
    carousel_idle_elapsed_ms = 0; // 初始化计时器
    if (xSemaphoreTake(sd_state_mutex, 10) == pdTRUE) {
        last_sd_state = sd_card_initialized;
        xSemaphoreGive(sd_state_mutex);
    }

    // 创建UI组件
    img_obj = lv_img_create(container);
    lv_obj_center(img_obj);
    
    loading_bar = lv_bar_create(container);
    lv_obj_set_size(loading_bar, 160, 10);
    lv_obj_center(loading_bar);
    lv_obj_add_flag(loading_bar, LV_OBJ_FLAG_HIDDEN); 
    
    msg_label = lv_label_create(container);
    lv_obj_set_style_text_font(msg_label, &tip_display_35, 0);
    lv_obj_center(msg_label);

    // 加载文件列表
    load_file_list_to_ram();
    
    if (file_list.empty()) {
        lv_label_set_text(msg_label, "无文件");
    } else {
        lv_label_set_text(msg_label, "");
        // 读取上次位置
        preferences.begin("watch", false);
        current_index = preferences.getInt("interval", 0); 
        preferences.end();
        if (current_index >= file_list.size()) current_index = 0;
    }

    // 初始化任务与定时器
    request_queue = xQueueCreate(1, sizeof(DecodeRequest));
    
    xTaskCreatePinnedToCore(picture_decode_task, "PicDecode", STACK_SIZE_PICTURE, NULL, 1, &decode_task_handle, 0);
    lv_obj_add_event_cb(lv_scr_act(), picture_gesture_cb, LV_EVENT_GESTURE, NULL);
    monitor_timer = lv_timer_create(monitor_timer_cb, 50, NULL);

    // 有文件才发起首张解码请求
    if (!file_list.empty()) {
        last_nav_dir = 1;
        request_new_image(current_index);
    }
}