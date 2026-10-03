#include "fullscreen_interfaces.h"
#include "ImageProcessor.h"
#include <WiFi.h>
#include <vector>
#include <WiFiUdp.h>
#include <NTPClient.h>
#include "wallpaper_format.h"


extern SdFs sd;

static WiFiUDP ntpUDP;
static NTPClient timeClient(ntpUDP, "ntp.aliyun.com", 28800, 60000);

// 状态枚举与全局变量
enum task_state {
    STATE_IDLE,
    STATE_WAIT_ANIM, // 等待动画结束
    STATE_SCANNING,  // 扫描SD卡
    STATE_DECODING,  // 解码中
    STATE_WRITING,   // 写入Flash中
    STATE_SUCCESS,   // 成功
    STATE_ERROR      // 失败
};

// 时间同步状态
static char time_sync_status[64] = {0};
static volatile bool time_sync_done = false;
static volatile bool time_sync_success = false;
static lv_timer_t* time_sync_timer = nullptr;

// 线程通信变量
static volatile task_state current_state = STATE_IDLE;
static volatile int task_progress = 0;
static char status_message[64] = {0};
static TaskHandle_t wp_task_handle = nullptr;

// UI 对象
static lv_obj_t* mainList = nullptr;
static lv_obj_t* subContainer = nullptr;
static lv_timer_t* ioTimer = nullptr;

// 物理按键检测
static lv_timer_t* uiMonitorTimer = nullptr;

// 壁纸任务UI刷新
static lv_obj_t* progressBar = nullptr;
static lv_obj_t* progressLabel = nullptr;
static lv_obj_t* wp_refresh_btn = nullptr;
// ================= 壁纸槽位与图片选择相关 =================
static lv_obj_t* wp_slot_dropdown = nullptr;      // 槽位下拉框
static lv_obj_t* wp_image_dropdown = nullptr;     // 图片文件下拉框
static lv_obj_t* wp_scan_btn = nullptr;           // 扫描SD卡按钮
static std::vector<String> wallpaper_file_list;   // SD卡中的图片文件列表
static volatile bool wp_scan_done = false;        // 图片扫描完成标志
static volatile bool wp_scanning = false;         // 正在扫描标志
static lv_timer_t* wp_scan_check_timer = nullptr; // 扫描检查定时器
static int selected_wp_slot_num = 1;              // 选中的槽位号
static String selected_wp_image_path = "";        // 选中的图片完整路径


// ================= WiFi 全局变量 =================
static std::vector<String> scanned_ssid_list;
static bool wifi_scan_done = false;
static SemaphoreHandle_t scan_mutex = NULL;
static bool scanning = false;

// WiFi 配置页 UI 控件句柄
static lv_obj_t* ssid_dropdown = nullptr;
static lv_obj_t* slot_dropdown = nullptr;
static lv_obj_t* password_ta = nullptr;
static lv_obj_t* kb = nullptr;
static lv_timer_t* scan_check_timer = nullptr;
static lv_obj_t* wifiList = nullptr;

// ================= 天气设置相关 =================
static lv_obj_t* weather_lat_ta = nullptr;
static lv_obj_t* weather_lon_ta = nullptr;
static lv_obj_t* weather_kb = nullptr;
static lv_obj_t* weather_active_ta = nullptr;



//=================
static lv_obj_t* carousel_interval_dd = nullptr; //轮播间隔下拉框句柄
// ================= 辅助函数声明 =================
static void on_decode_progress(int percent);
static void on_decode_error(ImageProcessor::ErrorCode error, const char* message);
static void set_status_msg(const char* msg);

// UI 动画与交互函数
static void anim_del_sub_obj_cb(lv_anim_t* a);
static void slide_animation(lv_obj_t* obj, int32_t start_y, int32_t end_y, lv_anim_ready_cb_t ready_cb);
static void create_sub_page_base();
static void cleanup_sub_page();
static void close_sub_page();

// 功能函数声明
static void show_time_sync_page();
static void show_wifi_page();
static void init_wallpaper_ui();
static void start_wallpaper_task();
static void show_wallpaper_page();
static void show_battery_calibration_page();
static void show_carousel_interval_page();
static void handle_menu_selection(const char* itemText);
static void wp_scan_sd_task(void* param);
static void wp_scan_check_timer_cb(lv_timer_t* timer);
static void wp_scan_btn_cb(lv_event_t* e);


// WiFi 相关新增函数声明
static void wifi_scan_task(void* param);
static void scan_check_timer_cb(lv_timer_t* timer);
static void kb_event_cb(lv_event_t* e);
static void ta_event_cb(lv_event_t* e);
static void save_wifi_config_cb(lv_event_t* e);
static void trigger_rescan_cb(lv_event_t* e);
static void config_btn_cb(lv_event_t* e);
static ImageProcessor::ErrorCode decode_image(const String& imagePath, uint16_t** outputBuffer, size_t* outputSize);
static bool write_to_littlefs(uint16_t* outputBuffer, size_t outputSize);
void wallpaper_worker_task(void* param);
static void wallpaper_ui_monitor_cb(lv_timer_t* timer);
static void wp_refresh_btn_cb(lv_event_t* e);
static void handle_back_button();
static void poll_timer_cb(lv_timer_t* timer);
static void list_item_cb(lv_event_t* e);

// 时间同步相关函数
static void time_sync_wifi_btn_cb(lv_event_t* e);
static void time_sync_manual_btn_cb(lv_event_t* e);
static void wifi_ntp_sync_task(void* param);
static void time_sync_ui_monitor_cb(lv_timer_t* timer);
static void time_manual_set_btn_cb(lv_event_t* e);

static bool is_valid_coordinate_str(const char* str);
static void weather_kb_event_cb(lv_event_t* e);
static void weather_ta_event_cb(lv_event_t* e);
static void save_weather_settings_cb(lv_event_t* e);
static void show_weather_settings_page(void);

//电池电压校准
static lv_timer_t* cal_timer = nullptr;
static lv_obj_t* voltage_label = nullptr;
static lv_obj_t* battery_percent_label = nullptr; // 显示校准后的电量
static lv_obj_t* cal_slider = nullptr;
static lv_obj_t* cal_value_label = nullptr; // 显示当前校准百分比


static uint16_t* wp_preview_buf = nullptr;

static int wp_preview_jpeg_cb(JPEGDRAW *pDraw) {
    if (!wp_preview_buf) return 0;
    int x = pDraw->x, y = pDraw->y, w = pDraw->iWidth, h = pDraw->iHeight;
    uint16_t *pSrc = pDraw->pPixels;
    for (int i = 0; i < h; i++) {
        uint32_t dst_off = (uint32_t)(y + i) * 240 + x;
        memcpy(&wp_preview_buf[dst_off], &pSrc[i * w], w * 2);
    }
    return 1;
}
// 线程安全的进度回调
static void on_decode_progress(int percent) {
    task_progress = (int)(percent * 0.4);
    snprintf(status_message, sizeof(status_message), "图像解码中...%d%%", percent);
}

static void on_decode_error(ImageProcessor::ErrorCode error, const char* message) {
    snprintf(status_message, sizeof(status_message), "错误: %s", message);
}

static void set_status_msg(const char* msg) {
    strncpy(status_message, msg, sizeof(status_message) - 1);
    status_message[sizeof(status_message) - 1] = '\0';
}

// ================= UI 动画函数 =================
static void anim_del_sub_obj_cb(lv_anim_t* a) {
    if (subContainer) {
        lv_obj_del(subContainer);
        subContainer = nullptr;
    }
    progressBar = nullptr;
    progressLabel = nullptr;
    wp_refresh_btn = nullptr;
    wp_slot_dropdown = nullptr;       
    wp_image_dropdown = nullptr;      
    wp_scan_btn = nullptr;           
    weather_lat_ta = nullptr;
    weather_lon_ta = nullptr;
    weather_kb = nullptr;
    weather_active_ta = nullptr;
    if (uiMonitorTimer) {
        lv_timer_del(uiMonitorTimer);
        uiMonitorTimer = nullptr;
    }
    if (wp_scan_check_timer) {        
        lv_timer_del(wp_scan_check_timer);
        wp_scan_check_timer = nullptr;
    }
}



static void slide_animation(lv_obj_t* obj, int32_t start_y, int32_t end_y, lv_anim_ready_cb_t ready_cb) {
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, obj);
    lv_anim_set_values(&a, start_y, end_y);
    lv_anim_set_time(&a, 500);
    lv_anim_set_exec_cb(&a, (lv_anim_exec_xcb_t)lv_obj_set_y);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    if (ready_cb) lv_anim_set_ready_cb(&a, ready_cb);
    lv_anim_start(&a);
}

static void create_sub_page_base() {
    if (subContainer) return;
    subContainer = lv_obj_create(lv_scr_act());
    lv_obj_set_size(subContainer, 240, 280);
    lv_obj_set_pos(subContainer, 0, 280);
    lv_obj_set_style_bg_color(subContainer, lv_color_hex(0x212121), LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(subContainer, 0, 0);
    lv_obj_set_style_pad_all(subContainer, 0, 0);
    lv_obj_clear_flag(subContainer, LV_OBJ_FLAG_SCROLLABLE);
    slide_animation(subContainer, 280, 0, NULL);
}

static void cleanup_sub_page() {
    if (subContainer) {
        lv_obj_del(subContainer);
        subContainer = nullptr;
    }
    carousel_interval_dd = nullptr;
    progressBar = nullptr;
    progressLabel = nullptr;
    wp_refresh_btn = nullptr;
    wp_slot_dropdown = nullptr;       // 新增
    wp_image_dropdown = nullptr;      // 新增
    wp_scan_btn = nullptr;            // 新增
    weather_lat_ta = nullptr;
    weather_lon_ta = nullptr;
    weather_kb = nullptr;
    weather_active_ta = nullptr;
    if (uiMonitorTimer) {
        lv_timer_del(uiMonitorTimer);
        uiMonitorTimer = nullptr;
    }
    if (wp_scan_check_timer) {        // 新增
        lv_timer_del(wp_scan_check_timer);
        wp_scan_check_timer = nullptr;
    }
}



static void close_sub_page() {
    if (subContainer && lv_obj_get_y(subContainer) == 0) {
        slide_animation(subContainer, 0, 280, anim_del_sub_obj_cb);
    } else {
        cleanup_sub_page();
    }
}

//  UI 监控定时器回调
static void wallpaper_ui_monitor_cb(lv_timer_t* timer) {
    if (progressLabel && lv_obj_is_valid(progressLabel)) {
        lv_label_set_text(progressLabel, status_message);
    }
    if (progressBar && lv_obj_is_valid(progressBar)) {
        lv_bar_set_value(progressBar, task_progress, LV_ANIM_ON);
    }
    if (current_state == STATE_SUCCESS || current_state == STATE_ERROR) {
        // 解除按钮禁用
        if (wp_refresh_btn && lv_obj_is_valid(wp_refresh_btn)) {
            lv_obj_clear_state(wp_refresh_btn, LV_STATE_DISABLED);
        }
        // 将状态重置为空闲，以允许退出
        current_state = STATE_IDLE;
    }
}

static void handle_back_button() {
    bool is_sensitive_operation = false;

    // 检查是否在 WiFi 时间同步过程中
    if (time_sync_timer != nullptr && !time_sync_done) {
        is_sensitive_operation = true;
        Serial.println("时间同步中，禁止退出");
    }

    // 检查是否在 WiFi 扫描过程中
    if (scanning || wifi_scan_done == false) {
        if (scan_check_timer != nullptr && !wifi_scan_done) {
            is_sensitive_operation = true;
            Serial.println("WiFi扫描中，禁止退出");
        }
    }

    // 检查是否在 WiFi 配置页面的扫描过程中
    if (ssid_dropdown && lv_obj_is_valid(ssid_dropdown)) {
        char buf[32];
        lv_dropdown_get_selected_str(ssid_dropdown, buf, sizeof(buf));
        if (strcmp(buf, "扫描中...") == 0) {
            is_sensitive_operation = true;
            Serial.println("WiFi扫描中，禁止退出");
        }
    }

    // ===== 检查是否在壁纸图片扫描过程中 =====
    if (wp_scanning || (wp_scan_check_timer != nullptr && !wp_scan_done)) {
        is_sensitive_operation = true;
        Serial.println("壁纸图片扫描中，禁止退出");
    }

    if (is_sensitive_operation) {
        return;
    }

    if (current_state != STATE_IDLE && current_state != STATE_SUCCESS && current_state != STATE_ERROR) {
        Serial.println("任务处理中，请稍候...");
        return;
    }

    if (subContainer && lv_obj_get_y(subContainer) == 0) {
        slide_animation(subContainer, 0, 280, anim_del_sub_obj_cb);
    } else {
        if (ioTimer) {
            lv_timer_del(ioTimer);
            ioTimer = nullptr;
        }
        if (subContainer) {
            lv_obj_del(subContainer);
            subContainer = nullptr;
        }
        if (uiMonitorTimer) {
            lv_timer_del(uiMonitorTimer);
            uiMonitorTimer = nullptr;
        }
        if (wp_task_handle) {
            vTaskDelete(wp_task_handle);
            wp_task_handle = nullptr;
        }
        fs_do_exit();
    }
}


// ================= BUTTON 按键轮询 =================
static void poll_timer_cb(lv_timer_t* timer) {
    static bool button_pressed = false;
    if (digitalRead(BUTTON_2) == HIGH) {
        if (!button_pressed) button_pressed = true;
    } else {
        if (button_pressed) {
            button_pressed = false;
            handle_back_button();
        }
    }
}

// ================= 主菜单列表项点击回调 =================
static void list_item_cb(lv_event_t* e) {
    lv_obj_t* btn = lv_event_get_target(e);
    const char* itemText = lv_list_get_btn_text(mainList, btn);
    handle_menu_selection(itemText);
}

// ================= 各菜单项实现 =================
// 时间同步
static void show_time_sync_page() {
    // 清除子页面原有内容
    lv_obj_clean(subContainer);

    // 创建两个按钮
    lv_obj_t* btn_wifi = lv_btn_create(subContainer);
    lv_obj_set_size(btn_wifi, 200, 50);
    lv_obj_align(btn_wifi, LV_ALIGN_CENTER, 0, -30);
    lv_obj_set_style_bg_color(btn_wifi, lv_color_hex(0x3498db), 0);
    lv_obj_add_event_cb(btn_wifi, time_sync_wifi_btn_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t* label_wifi = lv_label_create(btn_wifi);
    lv_label_set_text(label_wifi, "NTP 同步");
    lv_obj_set_style_text_font(label_wifi, &chinese_24, 0);
    lv_obj_center(label_wifi);

    lv_obj_t* btn_manual = lv_btn_create(subContainer);
    lv_obj_set_size(btn_manual, 200, 50);
    lv_obj_align(btn_manual, LV_ALIGN_CENTER, 0, 30);
    lv_obj_set_style_bg_color(btn_manual, lv_color_hex(0x2ecc71), 0);
    lv_obj_add_event_cb(btn_manual, time_sync_manual_btn_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t* label_manual = lv_label_create(btn_manual);
    lv_label_set_text(label_manual, "手动同步");
    lv_obj_set_style_text_font(label_manual, &chinese_24, 0);
    lv_obj_center(label_manual);
}

void wifi_ntp_sync_task(void* param) {
    time_sync_done = false;
    time_sync_success = false;
    strcpy(time_sync_status, "正在扫描 WiFi...");

    // 扫描 WiFi 网络
    WiFi.mode(WIFI_STA);
    WiFi.disconnect();
    int n = WiFi.scanNetworks();
    if (n == 0) {
        strcpy(time_sync_status, "未扫描到任何 WiFi");
        time_sync_done = true;
        vTaskDelete(NULL);
        return;
    }
 
    // 读取保存的 5 个槽位
    preferences.begin("watch", true);
    String ssid_list[5];
    String pass_list[5];
    for (int i = 0; i < 5; i++) {
        char key_ssid[16], key_pass[16];
        snprintf(key_ssid, sizeof(key_ssid), "WiFiSSID%d", i+1);
        snprintf(key_pass, sizeof(key_pass), "WiFiPass%d", i+1);
        ssid_list[i] = preferences.getString(key_ssid, "");
        pass_list[i] = preferences.getString(key_pass, "");
    }
    preferences.end();

    // 按槽位顺序尝试连接
    bool connected = false;
    for (int i = 0; i < 5; i++) {
        if (ssid_list[i].isEmpty()) continue;

        // 检查扫描结果中是否包含此 SSID
        bool found = false;
        for (int j = 0; j < n; j++) {
            if (WiFi.SSID(j) == ssid_list[i]) {
                found = true;
                break;
            }
        }
        if (!found) continue;

        snprintf(time_sync_status, sizeof(time_sync_status), "正在连接 %s...", ssid_list[i].c_str());
        WiFi.setTxPower(WIFI_POWER_11dBm);
        WiFi.begin(ssid_list[i].c_str(), pass_list[i].c_str());

        // 等待连接，超时 10 秒
        int timeout = 20; // 20 * 500ms = 10s
        while (--timeout > 0 && WiFi.status() != WL_CONNECTED) {
            vTaskDelay(pdMS_TO_TICKS(500));
        }
        if (WiFi.status() == WL_CONNECTED) {
            connected = true;
            break;
        }
        WiFi.disconnect();
    }

    if (!connected) {
        strcpy(time_sync_status, "连接失败");
        time_sync_done = true;
        vTaskDelete(NULL);
        return;
    }

    strcpy(time_sync_status, "WiFi 已连接，同步时间中...");

    // NTPClient 同步时间
    timeClient.begin();
    int retryCount = 0;
    const int MAX_RETRIES = 3;
    bool timeSynced = false;
    while (retryCount < MAX_RETRIES && !timeSynced) {
        if (timeClient.update()) {
            timeSynced = true;
            // 设置系统时间
            struct timeval tv;
            tv.tv_sec = timeClient.getEpochTime();
            tv.tv_usec = 0;
            settimeofday(&tv, NULL);
            strcpy(time_sync_status, "时间同步成功");
            time_sync_success = true;
        } else {
            retryCount++;
            snprintf(time_sync_status, sizeof(time_sync_status), "同步重试 %d/%d...", retryCount, MAX_RETRIES);
            vTaskDelay(pdMS_TO_TICKS(500)); // 等待0.5秒再重试
        }
    }
    if (!timeSynced) {
        strcpy(time_sync_status, "NTP 同步超时");
    }

    // 同步到外部 RTC
    if (time_sync_success && rtcManager.isAvailable()) {
        strcpy(time_sync_status, "正在同步到 RTC...");
        const int RTC_RETRY_COUNT = 3;
        bool rtc_sync_success = false;
        for (int retry = 1; retry <= RTC_RETRY_COUNT; retry++) {
            if (retry > 1) {
                snprintf(time_sync_status, sizeof(time_sync_status), "RTC 同步重试 %d/%d...", retry, RTC_RETRY_COUNT);
                vTaskDelay(pdMS_TO_TICKS(200)); // 重试前等待200ms
            }
            if (rtcManager.syncFromSystem()) {
                rtc_sync_success = true;
                break;
            }
            // 重试前稍微延迟
            vTaskDelay(pdMS_TO_TICKS(100));
        }
        if (rtc_sync_success) {
            strlcat(time_sync_status, " (RTC 同步成功)", sizeof(time_sync_status));
        } else {
            strlcat(time_sync_status, " (RTC 同步失败)", sizeof(time_sync_status));
            // 可以选择记录错误，但不影响主流程
            Serial.println("RTC sync failed after 3 attempts");
        }
    }else{
        strcpy(time_sync_status, "时间同步成功,无外部RTC");
    }

    time_sync_done = true;

    // 断开 WiFi 关闭射频并释放资源
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
    WiFi.end();
    vTaskDelete(NULL);
}

static void time_sync_ui_monitor_cb(lv_timer_t* timer) {
    // 查找状态标签
    lv_obj_t* label = lv_obj_get_child(subContainer, 0);
    if (label && lv_obj_check_type(label, &lv_label_class)) {
        lv_label_set_text(label, time_sync_status);
    }
    if (time_sync_done) {
        // 任务完成，停止定时器
        lv_timer_del(time_sync_timer);
        time_sync_timer = nullptr;

        // 添加一个返回按钮
        lv_obj_t* btn_back = lv_btn_create(subContainer);
        lv_obj_set_size(btn_back, 100, 40);
        lv_obj_align(btn_back, LV_ALIGN_BOTTOM_MID, 0, -10);
        lv_obj_set_style_bg_color(btn_back, lv_color_hex(0xe74c3c), 0);
        lv_obj_add_event_cb(btn_back, [](lv_event_t* e) {
            lv_obj_clean(subContainer);
            show_time_sync_page(); // 返回上一级
        }, LV_EVENT_CLICKED, NULL);
        lv_obj_t* label_back = lv_label_create(btn_back);
        lv_label_set_text(label_back, "返回");
        lv_obj_set_style_text_font(label_back, &chinese_24, 0);
        lv_obj_center(label_back);
    }
}

static void time_sync_wifi_btn_cb(lv_event_t* e) {
    // 清空子页面
    lv_obj_clean(subContainer);

    // 创建一个状态标签
    lv_obj_t* status_label = lv_label_create(subContainer);
    lv_label_set_text(status_label, "准备中...");
    lv_obj_set_style_text_font(status_label, &chinese_24, LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(status_label, lv_color_white(), LV_STATE_DEFAULT);

    // 设置宽度并允许自动换行
    lv_obj_set_width(status_label, 220);
    lv_label_set_long_mode(status_label, LV_LABEL_LONG_WRAP);
    lv_obj_center(status_label); // 居中后仍会在宽度内换行

    // 初始化状态变量
    time_sync_done = false;
    strcpy(time_sync_status, "初始化...");

    // 创建监控定时器
    if (time_sync_timer) lv_timer_del(time_sync_timer);
    time_sync_timer = lv_timer_create(time_sync_ui_monitor_cb, 100, NULL);

    // 启动任务
    xTaskCreatePinnedToCore(wifi_ntp_sync_task, "WiFiNTPSync", 8192, NULL, 1, NULL, 0);
}

static void time_sync_manual_btn_cb(lv_event_t* e) {
    lv_obj_clean(subContainer);
    
    // 获取当前时间作为默认值
    struct tm now;
    getLocalTime(&now);

    // 创建下拉框：年 (2020-2030)
    lv_obj_t* year_dd = lv_dropdown_create(subContainer);
    lv_dropdown_set_options(year_dd, "2020\n2021\n2022\n2023\n2024\n2025\n2026\n2027\n2028\n2029\n2030");
    lv_obj_set_size(year_dd, 80, 45);
    lv_obj_align(year_dd, LV_ALIGN_TOP_LEFT, 10, 20);
    lv_dropdown_set_selected(year_dd, now.tm_year + 1900 - 2020);
    lv_obj_set_style_text_font(year_dd, &chinese_24, 0);
    lv_obj_t* year_list = lv_dropdown_get_list(year_dd);
    lv_obj_set_style_text_font(year_list, &chinese_24, LV_PART_MAIN);
    lv_dropdown_set_symbol(year_dd, NULL);

    // 月 (1-12)
    lv_obj_t* month_dd = lv_dropdown_create(subContainer);
    lv_dropdown_set_options(month_dd, "1\n2\n3\n4\n5\n6\n7\n8\n9\n10\n11\n12");
    lv_obj_set_size(month_dd, 60, 45);
    lv_obj_align(month_dd, LV_ALIGN_TOP_LEFT, 100, 20);
    lv_dropdown_set_selected(month_dd, now.tm_mon); // tm_mon 0-11
    lv_obj_set_style_text_font(month_dd, &chinese_24, 0);
    lv_obj_t* month_list = lv_dropdown_get_list(month_dd);
    lv_obj_set_style_text_font(month_list, &chinese_24, LV_PART_MAIN);
    lv_dropdown_set_symbol(month_dd, NULL);

    // 日 (1-31)
    lv_obj_t* day_dd = lv_dropdown_create(subContainer);
    lv_dropdown_set_options(day_dd, "1\n2\n3\n4\n5\n6\n7\n8\n9\n10\n11\n12\n13\n14\n15\n16\n17\n18\n19\n20\n21\n22\n23\n24\n25\n26\n27\n28\n29\n30\n31");
    lv_obj_set_size(day_dd, 60, 45);
    lv_obj_align(day_dd, LV_ALIGN_TOP_LEFT, 170, 20);
    lv_dropdown_set_selected(day_dd, now.tm_mday - 1);
    lv_obj_set_style_text_font(day_dd, &chinese_24, 0);
    lv_obj_t* day_list = lv_dropdown_get_list(day_dd);
    lv_obj_set_style_text_font(day_list, &chinese_24, LV_PART_MAIN);
    lv_dropdown_set_symbol(day_dd, NULL);

    // 时 (0-23)
    lv_obj_t* hour_dd = lv_dropdown_create(subContainer);
    lv_dropdown_set_options(hour_dd, "0\n1\n2\n3\n4\n5\n6\n7\n8\n9\n10\n11\n12\n13\n14\n15\n16\n17\n18\n19\n20\n21\n22\n23");
    lv_obj_set_size(hour_dd, 70, 45);
    lv_obj_align(hour_dd, LV_ALIGN_TOP_LEFT, 40, 80);
    lv_dropdown_set_selected(hour_dd, now.tm_hour);
    lv_obj_set_style_text_font(hour_dd, &chinese_24, 0);
    lv_obj_t* hour_list = lv_dropdown_get_list(hour_dd);
    lv_obj_set_style_text_font(hour_list, &chinese_24, LV_PART_MAIN);
    lv_dropdown_set_symbol(hour_dd, NULL);

    // 分 (0-59)
    lv_obj_t* min_dd = lv_dropdown_create(subContainer);
    lv_dropdown_set_options(min_dd, "0\n1\n2\n3\n4\n5\n6\n7\n8\n9\n10\n11\n12\n13\n14\n15\n16\n17\n18\n19\n20\n21\n22\n23\n24\n25\n26\n27\n28\n29\n30\n31\n32\n33\n34\n35\n36\n37\n38\n39\n40\n41\n42\n43\n44\n45\n46\n47\n48\n49\n50\n51\n52\n53\n54\n55\n56\n57\n58\n59");
    lv_obj_set_size(min_dd, 70, 45);
    lv_obj_align(min_dd, LV_ALIGN_TOP_LEFT, 130, 80);
    lv_dropdown_set_selected(min_dd, now.tm_min);
    lv_obj_set_style_text_font(min_dd, &chinese_24, 0);
    lv_obj_t* min_list = lv_dropdown_get_list(min_dd);
    lv_obj_set_style_text_font(min_list, &chinese_24, LV_PART_MAIN);
    lv_dropdown_set_symbol(min_dd, NULL);

    // 设置按钮
    lv_obj_t* set_btn = lv_btn_create(subContainer);
    lv_obj_set_size(set_btn, 120, 40);
    lv_obj_align(set_btn, LV_ALIGN_BOTTOM_MID, 0, -50);
    lv_obj_set_style_bg_color(set_btn, lv_color_hex(0x2ecc71), 0);
    lv_obj_add_event_cb(set_btn, time_manual_set_btn_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t* set_label = lv_label_create(set_btn);
    lv_label_set_text(set_label, "设置时间");
    lv_obj_set_style_text_font(set_label, &chinese_24, 0);
    lv_obj_center(set_label);

    // 返回按钮
    lv_obj_t* back_btn = lv_btn_create(subContainer);
    lv_obj_set_size(back_btn, 100, 40);
    lv_obj_align(back_btn, LV_ALIGN_BOTTOM_MID, 0, -5);
    lv_obj_set_style_bg_color(back_btn, lv_color_hex(0xe74c3c), 0);
    lv_obj_add_event_cb(back_btn, [](lv_event_t* e) {
        lv_obj_clean(subContainer);
        show_time_sync_page();
    }, LV_EVENT_CLICKED, NULL);
    lv_obj_t* back_label = lv_label_create(back_btn);
    lv_label_set_text(back_label, "返回");
    lv_obj_set_style_text_font(back_label, &chinese_24, 0);
    lv_obj_center(back_label);
}

static void time_manual_set_btn_cb(lv_event_t* e) {
    // 获取子页面中所有的下拉框（按创建顺序：年、月、日、时、分）
    lv_obj_t* year_dd = lv_obj_get_child(subContainer, 0);
    lv_obj_t* month_dd = lv_obj_get_child(subContainer, 1);
    lv_obj_t* day_dd = lv_obj_get_child(subContainer, 2);
    lv_obj_t* hour_dd = lv_obj_get_child(subContainer, 3);
    lv_obj_t* min_dd = lv_obj_get_child(subContainer, 4);

    if (!year_dd || !month_dd || !day_dd || !hour_dd || !min_dd) return;

    int year = lv_dropdown_get_selected(year_dd) + 2020;
    int month = lv_dropdown_get_selected(month_dd) + 1; // 下拉索引0对应1月
    int day = lv_dropdown_get_selected(day_dd) + 1;
    int hour = lv_dropdown_get_selected(hour_dd);
    int minute = lv_dropdown_get_selected(min_dd);

    struct tm t = {0};
    t.tm_year = year - 1900;
    t.tm_mon = month - 1;
    t.tm_mday = day;
    t.tm_hour = hour;
    t.tm_min = minute;
    t.tm_sec = 0;

    time_t t_sec = mktime(&t);
    struct timeval now = { .tv_sec = t_sec };
    settimeofday(&now, NULL);

    // 同步到外部 RTC
    if (rtcManager.isAvailable()) {
        rtcManager.syncFromSystem();
    }

    // 显示成功提示
    lv_obj_clean(subContainer);
    lv_obj_t* msg = lv_label_create(subContainer);
    lv_label_set_text(msg, "时间设置成功");
    lv_obj_set_style_text_font(msg, &chinese_24, LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(msg, lv_color_white(), LV_STATE_DEFAULT);
    lv_obj_center(msg);

    // 添加返回按钮
    lv_obj_t* back_btn = lv_btn_create(subContainer);
    lv_obj_set_size(back_btn, 100, 40);
    lv_obj_align(back_btn, LV_ALIGN_BOTTOM_MID, 0, -10);
    lv_obj_set_style_bg_color(back_btn, lv_color_hex(0xe74c3c), 0);
    lv_obj_add_event_cb(back_btn, [](lv_event_t* e) {
        lv_obj_clean(subContainer);
        show_time_sync_page();
    }, LV_EVENT_CLICKED, NULL);
    lv_obj_t* back_label = lv_label_create(back_btn);
    lv_label_set_text(back_label, "返回");
    lv_obj_set_style_text_font(back_label, &chinese_24, 0);
    lv_obj_center(back_label);
}

//WiFi 列表项点击回调
static void wifi_item_cb(lv_event_t* e) {
    lv_obj_t* btn = lv_event_get_target(e);
    const char* wifiName = lv_list_get_btn_text(wifiList, btn);

    // 获取当前点击的是第几个按钮
    int index = lv_obj_get_index(btn);
    String saved_pass = "未设置";
    if (index >= 1 && index <= 5) {
        preferences.begin("watch", true);
        char key_pass[16];
        snprintf(key_pass, sizeof(key_pass), "WiFiPass%d", index);
        saved_pass = preferences.getString(key_pass, "无密码");
        preferences.end();
    }

    static const char* btns[] = {"确定", ""};
    char msg[128];
    // 使用从 Preferences 读取到的 saved_pass
    snprintf(msg, sizeof(msg), "SSID: %s\n密码: %s", wifiName ? wifiName : "Unknown", saved_pass.c_str());
    lv_obj_t* mbox = lv_msgbox_create(subContainer, "WiFi 信息", msg, btns, true);
    lv_obj_set_style_text_font(mbox, &chinese_24, 0);
    lv_obj_center(mbox);
    lv_obj_add_event_cb(mbox, [](lv_event_t* ev) {
        lv_msgbox_close(lv_event_get_current_target(ev));
    }, LV_EVENT_VALUE_CHANGED, NULL);
}

// ================= WiFi 扫描任务 =================
void wifi_scan_task(void* param) {
    WiFi.mode(WIFI_STA);
    WiFi.disconnect();
    int n = WiFi.scanNetworks();

    if (scan_mutex == NULL) scan_mutex = xSemaphoreCreateMutex();
    xSemaphoreTake(scan_mutex, portMAX_DELAY);
    scanned_ssid_list.clear();
    for (int i = 0; i < n; ++i) {
        scanned_ssid_list.push_back(WiFi.SSID(i));
    }
    xSemaphoreGive(scan_mutex);

    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
    WiFi.end();
    wifi_scan_done = true;
    scanning = false;
    vTaskDelete(NULL);
}

// 扫描检查定时器回调
static void scan_check_timer_cb(lv_timer_t* timer) {
    if (wifi_scan_done && ssid_dropdown && lv_obj_is_valid(ssid_dropdown)) {
        String options = "";
        xSemaphoreTake(scan_mutex, portMAX_DELAY);
        if (scanned_ssid_list.empty()) {
            options = "未扫描到网络";
        } else {
            for (const auto& ssid : scanned_ssid_list) {
                options += ssid + "\n";
            }
            if (options.length() > 0) options.remove(options.length() - 1);
        }
        xSemaphoreGive(scan_mutex);

        lv_dropdown_set_options(ssid_dropdown, options.c_str());
        lv_dropdown_set_text(ssid_dropdown, NULL);

        lv_timer_del(timer);
        scan_check_timer = nullptr;
        scanning = false;
        wifi_scan_done = true;
    }
}

// 键盘事件回调
static void kb_event_cb(lv_event_t* e) {
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_READY || code == LV_EVENT_CANCEL) {
        lv_obj_add_flag(kb, LV_OBJ_FLAG_HIDDEN);
        if (code == LV_EVENT_READY) {
            lv_obj_clear_state(password_ta, LV_STATE_FOCUSED);
        }
    }
}

// 文本框聚焦回调
static void ta_event_cb(lv_event_t* e) {
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_FOCUSED) {
        if (kb) {
            lv_obj_clear_flag(kb, LV_OBJ_FLAG_HIDDEN);
            lv_obj_move_foreground(kb);
        }
    }
}

// 保存配置回调
static void save_wifi_config_cb(lv_event_t* e) {
    if (!slot_dropdown || !ssid_dropdown || !password_ta) return;
    char buf[64];
    int slot_index = lv_dropdown_get_selected(slot_dropdown);
    int slot_num = slot_index + 1; // 1~5

    lv_dropdown_get_selected_str(ssid_dropdown, buf, sizeof(buf));
    String target_ssid = String(buf);
    String target_pass = String(lv_textarea_get_text(password_ta));

    if (target_ssid.isEmpty() || target_ssid == "扫描中..." || target_ssid == "未扫描到网络") {
        lv_obj_t* mbox = lv_msgbox_create(subContainer, "错误", "请选择有效的 WiFi", NULL, true);
        lv_obj_set_style_text_font(mbox, &chinese_24, 0);
        lv_obj_center(mbox);
        lv_obj_add_event_cb(mbox, [](lv_event_t* ev){ lv_msgbox_close(lv_event_get_current_target(ev)); }, LV_EVENT_VALUE_CHANGED, NULL);
        return;
    }

    preferences.begin("watch", false);
    char key_ssid[16], key_pass[16];
    snprintf(key_ssid, sizeof(key_ssid), "WiFiSSID%d", slot_num);
    snprintf(key_pass, sizeof(key_pass), "WiFiPass%d", slot_num);
    preferences.putString(key_ssid, target_ssid);
    preferences.putString(key_pass, target_pass);
    preferences.end();

    lv_obj_clean(subContainer);
    show_wifi_page(); // 重新显示列表页
}

// 触发重新扫描
static void trigger_rescan_cb(lv_event_t* e) {
    if (scanning) return;
    scanning = true;
    wifi_scan_done = false; // 重置完成标志

    lv_dropdown_set_options(ssid_dropdown, "扫描中...");
    lv_dropdown_set_text(ssid_dropdown, "扫描中...");

    if (scan_check_timer) lv_timer_del(scan_check_timer);
    scan_check_timer = lv_timer_create(scan_check_timer_cb, 200, NULL);

    xTaskCreatePinnedToCore(wifi_scan_task, "WiFiScan", 4096, NULL, 1, NULL, 0);
}

// 配置 WiFi 按钮回调
static void config_btn_cb(lv_event_t* e) {
    lv_obj_clean(subContainer);
    lv_obj_t* config_page = lv_obj_create(subContainer);
    lv_obj_set_size(config_page, 240, 280);
    lv_obj_set_style_bg_color(config_page, lv_color_hex(0x212121), 0);
    lv_obj_set_style_border_width(config_page, 0, 0);
    lv_obj_set_style_pad_all(config_page, 0, 0);
    lv_obj_center(config_page);

    //槽位下拉框
    slot_dropdown = lv_dropdown_create(config_page);
    lv_dropdown_set_options(slot_dropdown, "WiFi 槽位 1\nWiFi 槽位 2\nWiFi 槽位 3\nWiFi 槽位 4\nWiFi 槽位 5");
    lv_obj_set_width(slot_dropdown, 150);
    lv_obj_align(slot_dropdown, LV_ALIGN_TOP_RIGHT, -20, 10);
    lv_obj_set_style_text_font(slot_dropdown, &chinese_24, 0);
    lv_obj_t* slot_list = lv_dropdown_get_list(slot_dropdown);
    lv_obj_set_style_text_font(slot_list, &chinese_24, LV_PART_MAIN);
    lv_dropdown_set_symbol(slot_dropdown, NULL);

    lv_obj_t* label = lv_label_create(config_page);
    lv_label_set_text(label, "覆盖:");
    lv_obj_set_style_text_font(label, &chinese_24, 0);
    lv_obj_set_style_text_color(label, lv_color_white(), 0);
    lv_obj_align(label, LV_ALIGN_TOP_LEFT, 10, 20);

    ssid_dropdown = lv_dropdown_create(config_page);
    lv_obj_set_size(ssid_dropdown, 220, 50);
    lv_obj_align(ssid_dropdown, LV_ALIGN_TOP_MID, 0, 60);
    lv_dropdown_set_options(ssid_dropdown, "点击扫描按钮获取 WiFi 列表");
    lv_dropdown_set_text(ssid_dropdown, "请先扫描 WiFi");
    lv_obj_set_style_text_font(ssid_dropdown, &chinese_24, 0);
    lv_obj_t* ssid_list = lv_dropdown_get_list(ssid_dropdown);
    lv_obj_set_style_text_font(ssid_list, &chinese_24, LV_PART_MAIN);
    lv_dropdown_set_symbol(ssid_dropdown, NULL);

    // 密码输入框
    password_ta = lv_textarea_create(config_page);
    lv_obj_set_size(password_ta, 220, 50);
    lv_obj_align(password_ta, LV_ALIGN_TOP_MID, 0, 120);
    lv_textarea_set_placeholder_text(password_ta, "请输入密码...");
    lv_textarea_set_one_line(password_ta, true);
    lv_textarea_set_password_mode(password_ta, false);
    lv_obj_set_style_text_font(password_ta, &chinese_24, 0);
    lv_obj_add_event_cb(password_ta, ta_event_cb, LV_EVENT_ALL, NULL);

    // 扫描按钮
    lv_obj_t* scan_btn = lv_btn_create(config_page);
    lv_obj_set_size(scan_btn, 100, 40);
    lv_obj_align(scan_btn, LV_ALIGN_TOP_MID, -60, 180); // 左半边
    lv_obj_set_style_bg_color(scan_btn, lv_color_hex(0x3498db), 0);
    lv_obj_add_event_cb(scan_btn, trigger_rescan_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t* scan_label = lv_label_create(scan_btn);
    lv_label_set_text(scan_label, "扫描");
    lv_obj_set_style_text_font(scan_label, &chinese_24, 0);
    lv_obj_center(scan_label);

    // 保存按钮
    lv_obj_t* save_btn = lv_btn_create(config_page);
    lv_obj_set_size(save_btn, 100, 40);
    lv_obj_align(save_btn, LV_ALIGN_TOP_MID, 60, 180); // 右半边
    lv_obj_set_style_bg_color(save_btn, lv_color_hex(0x2ecc71), 0);
    lv_obj_add_event_cb(save_btn, save_wifi_config_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t* save_label = lv_label_create(save_btn);
    lv_label_set_text(save_label, "保存");
    lv_obj_set_style_text_font(save_label, &chinese_24, 0);
    lv_obj_center(save_label);

    // 取消按钮
    lv_obj_t* cancel_btn = lv_btn_create(config_page);
    lv_obj_set_size(cancel_btn, 220, 40);
    lv_obj_align(cancel_btn, LV_ALIGN_BOTTOM_MID, 0, -10);
    lv_obj_set_style_bg_color(cancel_btn, lv_color_hex(0xe74c3c), 0);
    lv_obj_add_event_cb(cancel_btn, [](lv_event_t* e) {
        lv_obj_clean(subContainer);
        show_wifi_page(); // 返回 WiFi 列表页
    }, LV_EVENT_CLICKED, NULL);
    lv_obj_t* cancel_label = lv_label_create(cancel_btn);
    lv_label_set_text(cancel_label, "取消");
    lv_obj_set_style_text_font(cancel_label, &chinese_24, 0);
    lv_obj_center(cancel_label);

    // 键盘
    kb = lv_keyboard_create(config_page);
    lv_obj_set_size(kb, 240, 120);
    lv_obj_align(kb, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_keyboard_set_textarea(kb, password_ta);
    lv_obj_set_style_pad_all(kb, 2, LV_PART_MAIN);
    lv_obj_set_style_pad_gap(kb, 3, LV_PART_MAIN);
    lv_obj_set_style_radius(kb, 4, LV_PART_ITEMS);
    lv_obj_set_style_text_font(kb, &lv_font_montserrat_14, LV_PART_ITEMS);
    lv_obj_set_style_bg_color(kb, lv_palette_lighten(LV_PALETTE_BLUE, 1), LV_STATE_PRESSED | LV_PART_ITEMS);
    lv_obj_add_flag(kb, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(kb, kb_event_cb, LV_EVENT_ALL, NULL);
}

// ================= 主 WiFi 列表页面 =================
static void show_wifi_page() {
    wifiList = lv_list_create(subContainer);
    lv_obj_set_size(wifiList, 240, 280);
    lv_obj_align(wifiList, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_set_style_border_width(wifiList, 0, 0);
    lv_obj_set_style_pad_all(wifiList, 0, 0);
    lv_obj_set_style_bg_color(wifiList, lv_color_black(), 0);

    // 配置 WiFi 按钮
    lv_obj_t* cfgBtn = lv_list_add_btn(wifiList, NULL, " 配置 WiFi");
    lv_obj_set_height(cfgBtn, 56);
    lv_obj_set_style_text_font(cfgBtn, &chinese_24, 0);
    lv_obj_set_style_bg_color(cfgBtn, lv_color_hex(0x2c3e50), 0);
    lv_obj_set_style_text_color(cfgBtn, lv_color_hex(0x3498db), 0);
    lv_obj_set_style_border_side(cfgBtn, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_width(cfgBtn, 2, 0);
    lv_obj_set_style_border_color(cfgBtn, lv_color_hex(0x3498db), 0);
    lv_obj_add_event_cb(cfgBtn, config_btn_cb, LV_EVENT_CLICKED, NULL);

    // 读取 5 个槽位并显示
    preferences.begin("watch", true);
    for (int i = 1; i <= 5; i++) {
        char key[16];
        snprintf(key, sizeof(key), "WiFiSSID%d", i);
        String ssid = preferences.getString(key, "");

        String labelText;
        if (ssid.isEmpty()) {
            char buf[32];
            snprintf(buf, sizeof(buf), "槽位 %d (空)", i);
            labelText = String(buf);
        } else {
            labelText = ssid;
        }

        lv_obj_t* btn = lv_list_add_btn(wifiList, NULL, labelText.c_str());
        lv_obj_set_height(btn, 56);
        lv_obj_set_style_text_font(btn, ssid.isEmpty() ? &chinese_24 : &chinese_24, 0);
        lv_obj_set_style_radius(btn, 0, 0);
        lv_obj_set_style_border_side(btn, LV_BORDER_SIDE_BOTTOM, 0);
        lv_obj_set_style_border_width(btn, 1, 0);
        lv_obj_set_style_border_color(btn, lv_color_hex(0x444444), 0);
        lv_obj_set_style_bg_color(btn, lv_color_hex(0x212121), 0);
        lv_obj_set_style_text_color(btn, ssid.isEmpty() ? lv_color_hex(0x888888) : lv_color_white(), 0);
        lv_obj_add_event_cb(btn, wifi_item_cb, LV_EVENT_CLICKED, NULL);
    }
    preferences.end();
}
// ================= 天气设置页面 =================
static bool is_valid_coordinate_str(const char* str) {
    if (!str || str[0] == '\0') return false;
    bool has_digit = false;
    for (int i = 0; str[i]; i++) {
        char c = str[i];
        if (c == '\n' || c == '\r') continue;
        if (isdigit(c)) {
            has_digit = true;
        } else if (c == '.') {
            // 小数点：允许，但不检查重复
        } else if (c == '-') {
            if (i != 0) return false; // 负号只能在开头
        } else {
            return false; // 非法字符
        }
    }
    return has_digit;
}

static void weather_kb_event_cb(lv_event_t* e) {
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_READY || code == LV_EVENT_CANCEL) {
        lv_obj_add_flag(weather_kb, LV_OBJ_FLAG_HIDDEN);
        if (code == LV_EVENT_READY) {
            if (weather_lat_ta) lv_obj_clear_state(weather_lat_ta, LV_STATE_FOCUSED);
            if (weather_lon_ta) lv_obj_clear_state(weather_lon_ta, LV_STATE_FOCUSED);
        }
    }
}

static void weather_ta_event_cb(lv_event_t* e) {
    lv_obj_t* ta = lv_event_get_target(e);
    lv_event_code_t code = lv_event_get_code(e);
    weather_active_ta = ta;
    if (code == LV_EVENT_FOCUSED) {
        if (weather_kb) {
            lv_keyboard_set_textarea(weather_kb, ta);
            lv_obj_clear_flag(weather_kb, LV_OBJ_FLAG_HIDDEN);
            lv_obj_move_foreground(weather_kb);
        }
    }
}

static void save_weather_settings_cb(lv_event_t* e) {
    // 先隐藏键盘
    if (weather_kb) {
        lv_obj_add_flag(weather_kb, LV_OBJ_FLAG_HIDDEN);
    }

    const char* lat_str = lv_textarea_get_text(weather_lat_ta);
    const char* lon_str = lv_textarea_get_text(weather_lon_ta);

    // 校验输入格式
    if (!is_valid_coordinate_str(lat_str)) {
        lv_obj_t* mbox = lv_msgbox_create(subContainer, "错误", "纬度格式无效\n请输入数字", NULL, true);
        lv_obj_set_style_text_font(mbox, &chinese_24, 0);
        lv_obj_center(mbox);
        lv_obj_add_event_cb(mbox, [](lv_event_t* ev) {
            lv_msgbox_close(lv_event_get_current_target(ev));
        }, LV_EVENT_VALUE_CHANGED, NULL);
        return;
    }
    if (!is_valid_coordinate_str(lon_str)) {
        lv_obj_t* mbox = lv_msgbox_create(subContainer, "错误", "经度格式无效\n请输入数字", NULL, true);
        lv_obj_set_style_text_font(mbox, &chinese_24, 0);
        lv_obj_center(mbox);
        lv_obj_add_event_cb(mbox, [](lv_event_t* ev) {
            lv_msgbox_close(lv_event_get_current_target(ev));
        }, LV_EVENT_VALUE_CHANGED, NULL);
        return;
    }

    float latitude = atof(lat_str);
    float longitude = atof(lon_str);

    // 校验纬度范围 -90 ~ 90
    if (latitude < -90.0f || latitude > 90.0f) {
        lv_obj_t* mbox = lv_msgbox_create(subContainer, "错误", "纬度范围: -90 ~ 90", NULL, true);
        lv_obj_set_style_text_font(mbox, &chinese_24, 0);
        lv_obj_center(mbox);
        lv_obj_add_event_cb(mbox, [](lv_event_t* ev) {
            lv_msgbox_close(lv_event_get_current_target(ev));
        }, LV_EVENT_VALUE_CHANGED, NULL);
        return;
    }

    // 校验经度范围 -180 ~ 180
    if (longitude < -180.0f || longitude > 180.0f) {
        lv_obj_t* mbox = lv_msgbox_create(subContainer, "错误", "经度范围: -180 ~ 180", NULL, true);
        lv_obj_set_style_text_font(mbox, &chinese_24, 0);
        lv_obj_center(mbox);
        lv_obj_add_event_cb(mbox, [](lv_event_t* ev) {
            lv_msgbox_close(lv_event_get_current_target(ev));
        }, LV_EVENT_VALUE_CHANGED, NULL);
        return;
    }

    preferences.begin("watch", false);
    preferences.putFloat("weather_lat", latitude);
    preferences.putFloat("weather_lon", longitude);
    preferences.end();

    Serial.printf("天气设置已保存: lat=%.2f, lon=%.2f\n", latitude, longitude);

    // 显示成功提示
    lv_obj_t* msg = lv_label_create(subContainer);
    lv_label_set_text(msg, "已保存");
    lv_obj_set_style_text_font(msg, &chinese_24, 0);
    lv_obj_set_style_text_color(msg, lv_color_hex(0x2ecc71), 0);
    lv_obj_align(msg, LV_ALIGN_TOP_MID, 0, 190);
    lv_timer_t* del_timer = lv_timer_create([](lv_timer_t* t) {
        lv_obj_t* obj = (lv_obj_t*)t->user_data;
        if (obj && lv_obj_is_valid(obj)) {
            lv_obj_del(obj);
        }
        lv_timer_del(t);
    }, 1000, msg);
}

static void show_weather_settings_page(void) {
    preferences.begin("watch", true);
    float saved_lat = preferences.getFloat("weather_lat", 31.23f);
    float saved_lon = preferences.getFloat("weather_lon", 121.47f);
    preferences.end();

    char lat_buf[16], lon_buf[16];
    snprintf(lat_buf, sizeof(lat_buf), "%.2f", saved_lat);
    snprintf(lon_buf, sizeof(lon_buf), "%.2f", saved_lon);

    // 纬度标签
    lv_obj_t* lat_label = lv_label_create(subContainer);
    lv_label_set_text(lat_label, "纬度:");
    lv_obj_set_style_text_font(lat_label, &chinese_24, 0);
    lv_obj_set_style_text_color(lat_label, lv_color_white(), 0);
    lv_obj_align(lat_label, LV_ALIGN_TOP_LEFT, 10, 8);

    // 纬度输入框
    weather_lat_ta = lv_textarea_create(subContainer);
    lv_obj_set_size(weather_lat_ta, 155, 35);
    lv_obj_align(weather_lat_ta, LV_ALIGN_TOP_RIGHT, -10, 3);
    lv_textarea_set_one_line(weather_lat_ta, true);
    lv_textarea_set_text(weather_lat_ta, lat_buf);
    lv_obj_set_style_text_font(weather_lat_ta, &chinese_24, 0);
    lv_obj_add_event_cb(weather_lat_ta, weather_ta_event_cb, LV_EVENT_ALL, NULL);

    // 经度标签
    lv_obj_t* lon_label = lv_label_create(subContainer);
    lv_label_set_text(lon_label, "经度:");
    lv_obj_set_style_text_font(lon_label, &chinese_24, 0);
    lv_obj_set_style_text_color(lon_label, lv_color_white(), 0);
    lv_obj_align(lon_label, LV_ALIGN_TOP_LEFT, 10, 48);

    // 经度输入框
    weather_lon_ta = lv_textarea_create(subContainer);
    lv_obj_set_size(weather_lon_ta, 155, 35);
    lv_obj_align(weather_lon_ta, LV_ALIGN_TOP_RIGHT, -10, 43);
    lv_textarea_set_one_line(weather_lon_ta, true);
    lv_textarea_set_text(weather_lon_ta, lon_buf);
    lv_obj_set_style_text_font(weather_lon_ta, &chinese_24, 0);
    lv_obj_add_event_cb(weather_lon_ta, weather_ta_event_cb, LV_EVENT_ALL, NULL);

    // 范围提示
    lv_obj_t* hint_label = lv_label_create(subContainer);
    lv_label_set_text(hint_label, "纬度:-90~90\n经度:-180~180");
    lv_obj_set_style_text_font(hint_label, &chinese_24, 0);
    lv_obj_set_style_text_color(hint_label, lv_color_hex(0x888888), 0);
    lv_obj_align(hint_label, LV_ALIGN_TOP_MID, 0, 95);

    // 保存按钮
    lv_obj_t* save_btn = lv_btn_create(subContainer);
    lv_obj_set_size(save_btn, 100, 35);
    lv_obj_align(save_btn, LV_ALIGN_TOP_MID, -60, 150);
    lv_obj_set_style_bg_color(save_btn, lv_color_hex(0x2ecc71), 0);
    lv_obj_add_event_cb(save_btn, save_weather_settings_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t* save_label = lv_label_create(save_btn);
    lv_label_set_text(save_label, "保存");
    lv_obj_set_style_text_font(save_label, &chinese_24, 0);
    lv_obj_center(save_label);

    // 返回按钮
    lv_obj_t* back_btn = lv_btn_create(subContainer);
    lv_obj_set_size(back_btn, 100, 35);
    lv_obj_align(back_btn, LV_ALIGN_TOP_MID, 60, 150);
    lv_obj_set_style_bg_color(back_btn, lv_color_hex(0xe74c3c), 0);
    lv_obj_add_event_cb(back_btn, [](lv_event_t* e) {
        close_sub_page();
    }, LV_EVENT_CLICKED, NULL);
    lv_obj_t* back_label = lv_label_create(back_btn);
    lv_label_set_text(back_label, "返回");
    lv_obj_set_style_text_font(back_label, &chinese_24, 0);
    lv_obj_center(back_label);

    // 键盘
    weather_kb = lv_keyboard_create(subContainer);
    lv_obj_set_size(weather_kb, 240, 120);
    lv_obj_align(weather_kb, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_keyboard_set_textarea(weather_kb, weather_lat_ta);
    lv_obj_set_style_pad_all(weather_kb, 2, LV_PART_MAIN);
    lv_obj_set_style_pad_gap(weather_kb, 3, LV_PART_MAIN);
    lv_obj_set_style_radius(weather_kb, 4, LV_PART_ITEMS);
    lv_obj_set_style_text_font(weather_kb, &lv_font_montserrat_14, LV_PART_ITEMS);
    lv_obj_set_style_bg_color(weather_kb, lv_palette_lighten(LV_PALETTE_BLUE, 1), LV_STATE_PRESSED | LV_PART_ITEMS);
    lv_obj_add_flag(weather_kb, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(weather_kb, weather_kb_event_cb, LV_EVENT_ALL, NULL);
}

// ================= 功能页面 =================
static void show_wallpaper_page() {
    init_wallpaper_ui();
}

static void cal_timer_cb(lv_timer_t* timer) {
    // 检查所有 UI 对象是否有效
    if (!voltage_label || !lv_obj_is_valid(voltage_label) ||
        !battery_percent_label || !lv_obj_is_valid(battery_percent_label) ||
        !cal_slider || !lv_obj_is_valid(cal_slider)) {
        if (cal_timer) {
            lv_timer_del(cal_timer);
            cal_timer = nullptr;
        }
        return;
    }

    // 采集电压
    const int SAMPLES = 20;
    uint32_t sum = 0;
    for (int i = 0; i < SAMPLES; i++) {
        sum += analogReadMilliVolts(BATTERY_ADC_PIN);
        vTaskDelay(pdMS_TO_TICKS(2));
    }
    float avg_mv = sum / (float)SAMPLES;
    float voltage = (avg_mv / 1000.0f) * VOLTAGE_DIVIDER_RATIO;

    // 获取当前滑动条校准值
    int cal_val = lv_slider_get_value(cal_slider);

    // 计算校准后电压
    float calibrated_voltage = voltage * (cal_val / 100.0f);

    // 更新电压标签
    char buf[16];
    snprintf(buf, sizeof(buf), "%.2fV", calibrated_voltage);
    lv_label_set_text(voltage_label, buf);

    // 根据校准后电压计算百分比
    int cal_percent = (int)((calibrated_voltage - BATTERY_EMPTY_V) / (BATTERY_FULL_V - BATTERY_EMPTY_V) * 100);
    if (cal_percent < 0) cal_percent = 0;
    if (cal_percent > 100) cal_percent = 100;

    // 更新校准后电量标签
    snprintf(buf, sizeof(buf), "%d%%", cal_percent);
    lv_label_set_text(battery_percent_label, buf);
}

// 滑动条事件：更新校准值显示并刷新电量
static void cal_slider_event_cb(lv_event_t* e) {
    int cal_val = lv_slider_get_value(cal_slider);
    lv_label_set_text_fmt(cal_value_label, "校准: %d%%", cal_val);

    // 立即调用定时器回调更新电量预览
    if (cal_timer) {
        cal_timer_cb(cal_timer);
    }
}

// 保存按钮回调
static void save_cal_cb(lv_event_t* e) {
    int cal_val = lv_slider_get_value(cal_slider);
    preferences.begin("watch", false);
    preferences.putFloat("batt_cal", (float)cal_val);
    preferences.end();
    atomic_store_float(&Calibration, (float)cal_val);

    // 显示短暂提示
    lv_obj_t* msg = lv_label_create(subContainer);
    lv_label_set_text(msg, "已保存");
    lv_obj_set_style_text_font(msg, &chinese_24, 0);
    lv_obj_set_style_text_color(msg, lv_color_hex(0x2ecc71), 0);
    lv_obj_align(msg, LV_ALIGN_BOTTOM_MID, 0, -50);
    lv_timer_t* del_timer = lv_timer_create([](lv_timer_t* t) {
        lv_obj_t* obj = (lv_obj_t*)t->user_data;
        if (obj && lv_obj_is_valid(obj)) {
            lv_obj_del(obj);
        }
        lv_timer_del(t);
    }, 1000, msg);
}

// 返回按钮回调
static void back_cal_cb(lv_event_t* e) {
    if (cal_timer) {
        lv_timer_del(cal_timer);
        cal_timer = nullptr;
    }
    close_sub_page(); // 关闭子页面，subContainer 及其子对象将被删除
}

// 构建电池校准页面
static void show_battery_calibration_page() {
    // 确保先创建子容器
    create_sub_page_base();
    lv_obj_clean(subContainer);

    // 电压标签
    voltage_label = lv_label_create(subContainer);
    lv_obj_set_style_text_font(voltage_label, &chinese_24, 0);
    lv_obj_set_style_text_color(voltage_label, lv_color_white(), 0);
    lv_obj_align(voltage_label, LV_ALIGN_TOP_MID, 0, 20);

    // 电量百分比标签（校准后）
    battery_percent_label = lv_label_create(subContainer);
    lv_obj_set_style_text_font(battery_percent_label, &chinese_24, 0);
    lv_obj_set_style_text_color(battery_percent_label, lv_color_white(), 0);
    lv_obj_align(battery_percent_label, LV_ALIGN_TOP_MID, 0, 60);

    // 校准滑动条
    cal_slider = lv_slider_create(subContainer);
    lv_obj_set_width(cal_slider, 200);
    lv_obj_align(cal_slider, LV_ALIGN_CENTER, 0, -20);
    lv_slider_set_range(cal_slider, 80, 120);
    lv_slider_set_value(cal_slider, (int)atomic_load_float(&Calibration), LV_ANIM_OFF);
    lv_obj_add_event_cb(cal_slider, cal_slider_event_cb, LV_EVENT_VALUE_CHANGED, NULL);

    // 当前校准值显示
    cal_value_label = lv_label_create(subContainer);
    lv_label_set_text_fmt(cal_value_label, "校准: %d%%", (int)atomic_load_float(&Calibration));
    lv_obj_set_style_text_font(cal_value_label, &chinese_24, 0);
    lv_obj_set_style_text_color(cal_value_label, lv_color_white(), 0);
    lv_obj_align(cal_value_label, LV_ALIGN_CENTER, 0, 20);

    // 保存按钮
    lv_obj_t* save_btn = lv_btn_create(subContainer);
    lv_obj_set_size(save_btn, 80, 40);
    lv_obj_align(save_btn, LV_ALIGN_BOTTOM_LEFT, 10, -10);
    lv_obj_set_style_bg_color(save_btn, lv_color_hex(0x2ecc71), 0);
    lv_obj_add_event_cb(save_btn, save_cal_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t* save_label = lv_label_create(save_btn);
    lv_label_set_text(save_label, "保存");
    lv_obj_set_style_text_font(save_label, &chinese_24, 0);
    lv_obj_center(save_label);

    // 返回按钮
    lv_obj_t* back_btn = lv_btn_create(subContainer);
    lv_obj_set_size(back_btn, 80, 40);
    lv_obj_align(back_btn, LV_ALIGN_BOTTOM_RIGHT, -10, -10);
    lv_obj_set_style_bg_color(back_btn, lv_color_hex(0xe74c3c), 0);
    lv_obj_add_event_cb(back_btn, back_cal_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t* back_label = lv_label_create(back_btn);
    lv_label_set_text(back_label, "返回");
    lv_obj_set_style_text_font(back_label, &chinese_24, 0);
    lv_obj_center(back_label);

    // 启动定时器（每500ms更新一次）
    cal_timer = lv_timer_create(cal_timer_cb, 500, NULL);
}

// 保存轮播间隔回调
static void save_carousel_interval_cb(lv_event_t* e) {
    if (!carousel_interval_dd) return;
    int interval = lv_dropdown_get_selected(carousel_interval_dd) + 1;
    preferences.begin("watch", false);
    preferences.putInt("interval", interval);
    preferences.end();
    Serial.println(interval);

    // 显示保存成功提示
    lv_obj_t* msg = lv_label_create(subContainer);
    lv_label_set_text(msg, "已保存");
    lv_obj_set_style_text_font(msg, &chinese_24, 0);
    lv_obj_set_style_text_color(msg, lv_color_hex(0x2ecc71), 0);
    lv_obj_align(msg, LV_ALIGN_BOTTOM_MID, 0, -50);
    lv_timer_t* del_timer = lv_timer_create([](lv_timer_t* t) {
        lv_obj_t* obj = (lv_obj_t*)t->user_data;
        if (obj && lv_obj_is_valid(obj)) {
            lv_obj_del(obj);
        }
        lv_timer_del(t);
    }, 1000, msg);
}

static void show_carousel_interval_page() {
    // 标题标签
    lv_obj_t* label = lv_label_create(subContainer);
    lv_label_set_text(label, "轮播间隔:");
    lv_obj_set_style_text_font(label, &chinese_24, 0);
    lv_obj_set_style_text_color(label, lv_color_white(), 0);
    lv_obj_align(label, LV_ALIGN_TOP_MID, 0, 20);

    // ✅ 保存下拉框到静态变量
    carousel_interval_dd = lv_dropdown_create(subContainer);
    lv_dropdown_set_options(carousel_interval_dd, "1秒\n2秒\n3秒\n4秒\n5秒\n6秒\n7秒\n8秒\n9秒\n10秒");
    lv_obj_set_width(carousel_interval_dd, 150);
    lv_obj_align(carousel_interval_dd, LV_ALIGN_TOP_MID, 0, 60);
    lv_obj_set_style_text_font(carousel_interval_dd, &chinese_24, 0);
    lv_obj_t* dd_list = lv_dropdown_get_list(carousel_interval_dd);
    lv_obj_set_style_text_font(dd_list, &chinese_24, LV_PART_MAIN);
    lv_dropdown_set_symbol(carousel_interval_dd, NULL);

    // 读取当前保存的值
    preferences.begin("watch", true);
    int current_val = preferences.getInt("interval", 3);
    preferences.end();
    Serial.println(current_val);
    if (current_val < 1) current_val = 1;
    if (current_val > 10) current_val = 10;
    lv_dropdown_set_selected(carousel_interval_dd, current_val - 1);
 
    // 保存按钮
    lv_obj_t* save_btn = lv_btn_create(subContainer);
    lv_obj_set_size(save_btn, 120, 40);
    lv_obj_align(save_btn, LV_ALIGN_BOTTOM_MID, 0, -10);
    lv_obj_set_style_bg_color(save_btn, lv_color_hex(0x2ecc71), 0);
    lv_obj_add_event_cb(save_btn, save_carousel_interval_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t* save_label = lv_label_create(save_btn);
    lv_label_set_text(save_label, "保存");
    lv_obj_set_style_text_font(save_label, &chinese_24, 0);
    lv_obj_center(save_label);
}

static void show_rescan_page() {
    // 检查SD卡状态
    bool sd_ready = false;
    if (xSemaphoreTake(sd_state_mutex, pdMS_TO_TICKS(20)) == pdTRUE) {
        sd_ready = sd_card_initialized;
        xSemaphoreGive(sd_state_mutex);
    }

    if (!sd_ready) {
        // SD卡未初始化，显示提示
        lv_obj_t* tip = lv_label_create(subContainer);
        lv_label_set_text(tip, "SD卡初始化失败");
        lv_obj_set_style_text_font(tip, &chinese_24, LV_STATE_DEFAULT);
        lv_obj_set_style_text_color(tip, lv_color_white(), LV_STATE_DEFAULT);
        lv_obj_set_style_text_align(tip, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_center(tip);

        lv_obj_t* back_btn = lv_btn_create(subContainer);
        lv_obj_set_size(back_btn, 100, 40);
        lv_obj_align(back_btn, LV_ALIGN_BOTTOM_MID, 0, -10);
        lv_obj_set_style_bg_color(back_btn, lv_color_hex(0xe74c3c), 0);
        lv_obj_add_event_cb(back_btn, [](lv_event_t* e) {
            close_sub_page();
        }, LV_EVENT_CLICKED, NULL);
        lv_obj_t* back_label = lv_label_create(back_btn);
        lv_label_set_text(back_label, "返回");
        lv_obj_set_style_text_font(back_label, &chinese_24, 0);
        lv_obj_center(back_label);
        return;
    }

    // 显示扫描中提示
    lv_obj_t* tip = lv_label_create(subContainer);
    lv_label_set_text(tip, "正在重新扫描SD卡...\n请稍后");
    lv_obj_set_style_text_font(tip, &chinese_24, LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(tip, lv_color_white(), LV_STATE_DEFAULT);
    lv_obj_set_style_text_align(tip, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(tip);

    // 延迟后退出设置页面（给用户看到提示）
    lv_timer_t* exit_timer = lv_timer_create([](lv_timer_t* timer) {
        // 关闭子页面
        if (subContainer && lv_obj_get_y(subContainer) == 0) {
            slide_animation(subContainer, 0, 280, anim_del_sub_obj_cb);
        } else {
            cleanup_sub_page();
        }
        // 设置强制扫描标志
        if (xSemaphoreTake(sd_state_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
            sd_force_scan = true;
            xSemaphoreGive(sd_state_mutex);
        }
        // 退出整个设置界面
        fs_do_exit();
        lv_timer_del(timer);
    }, 500, NULL);
}

// ================= 菜单选择分发 =================
static void handle_menu_selection(const char* itemText) {
    create_sub_page_base();
    if (strcmp(itemText, "时间同步") == 0) {
        show_time_sync_page();
    } else if (strcmp(itemText, "WiFi") == 0) {
        show_wifi_page();
    } else if (strcmp(itemText, "壁纸") == 0) {
        show_wallpaper_page();
    } else if (strcmp(itemText, "电池校准") == 0) {
        show_battery_calibration_page();
    } else if (strcmp(itemText, "自动轮播间隔") == 0) {
        show_carousel_interval_page();
    } else if (strcmp(itemText, "重新扫描SD卡") == 0) {
        show_rescan_page();
    } else if (strcmp(itemText, "天气设置") == 0) {
        show_weather_settings_page();
    }else {
        lv_obj_t* tip = lv_label_create(subContainer);
        char buf[64];
        snprintf(buf, sizeof(buf), "%s\n功能开发中...", itemText);
        lv_label_set_text(tip, buf);
        lv_obj_set_style_text_font(tip, &chinese_24, LV_STATE_DEFAULT);
        lv_obj_set_style_text_color(tip, lv_color_white(), LV_STATE_DEFAULT);
        lv_obj_set_style_text_align(tip, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_center(tip);
    }
}



static ImageProcessor::ErrorCode decode_image(const String& imagePath, uint16_t** outputBuffer, size_t* outputSize) {
    set_status_msg("正在解码图像...");
    ImageProcessor* imgProc = new ImageProcessor();
    imgProc->setProgressCallback(on_decode_progress);
    imgProc->setErrorCallback(on_decode_error);
    ImageProcessor::ErrorCode result = imgProc->loadAndProcessImage(
        imagePath.c_str(), outputBuffer, outputSize, 240, 280
    );
    delete imgProc;
    return result;
}
static bool write_to_littlefs(uint16_t* outputBuffer, size_t outputSize, int slot_num) {
    char filename[32];
    snprintf(filename, sizeof(filename), "/wallpaper%d.bin", slot_num);
    set_status_msg("写入内置存储...");

    if (!LittleFS.begin(true, "/littlefs", 10, "littlefs")) {
        set_status_msg("LittleFS 挂载失败");
        return false;
    }

    // ✅ 检查空间（剩余空间 + 当前槽已有文件大小，因为会覆盖）
    size_t total_needed = WP_HEADER_SIZE + outputSize;
    size_t free_bytes = LittleFS.totalBytes() - LittleFS.usedBytes();
    File oldFile = LittleFS.open(filename, FILE_READ);
    if (oldFile) {
        free_bytes += oldFile.size();
        oldFile.close();
    }
    if (free_bytes < total_needed) {
        set_status_msg("存储空间不足");
        LittleFS.end();
        return false;
    }

    File wallpaperFile = LittleFS.open(filename, FILE_WRITE);
    if (!wallpaperFile) {
        set_status_msg("无法创建文件");
        LittleFS.end();
        return false;
    }

    // ✅ 写入文件头
    wp_header_t header;
    memset(&header, 0, sizeof(header));
    memcpy(header.magic, WP_MAGIC, WP_MAGIC_LEN);
    header.type = WP_TYPE_STATIC;
    header.mjpeg_size = 0;
    wallpaperFile.write((const uint8_t*)&header, sizeof(header));

    // 写入 RGB565 数据
    const size_t CHUNK_SIZE = 2048;
    size_t written_total = 0;
    uint8_t* data_ptr = (uint8_t*)outputBuffer;
    bool write_failed = false;
    while (written_total < outputSize) {
        size_t remaining = outputSize - written_total;
        size_t to_write = (remaining > CHUNK_SIZE) ? CHUNK_SIZE : remaining;
        size_t res = wallpaperFile.write(data_ptr + written_total, to_write);
        if (res != to_write) { write_failed = true; break; }
        written_total += res;
        task_progress = 40 + (int)((float)written_total / outputSize * 20.0f);
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    wallpaperFile.close();
    LittleFS.end();
    return !write_failed;
}
// 写入 MJPEG 壁纸（头部 + 预览 + MJPEG 原始流）
static bool write_mjpeg_to_littlefs(uint16_t* previewBuffer, size_t previewSize,
                                     const char* mjpegSdPath, size_t mjpegFileSize, int slot_num) {
    char filename[32];
    snprintf(filename, sizeof(filename), "/wallpaper%d.bin", slot_num);
    char msg[64];
    snprintf(msg, sizeof(msg), "写入MJPEG壁纸...");
    set_status_msg(msg);

    if (!LittleFS.begin(true, "/littlefs", 10, "littlefs")) {
        set_status_msg("LittleFS 挂载失败");
        return false;
    }

    // 检查空间：头部 + 预览 + MJPEG 数据（剩余空间 + 当前槽已有文件大小，因为会覆盖）
    size_t total_needed = WP_HEADER_SIZE + previewSize + mjpegFileSize;
    size_t free_bytes = LittleFS.totalBytes() - LittleFS.usedBytes();
    File oldFile = LittleFS.open(filename, FILE_READ);
    if (oldFile) {
        free_bytes += oldFile.size();
        oldFile.close();
    }
    if (free_bytes < total_needed + 4096) {  // 预留 4KB 余量
        snprintf(msg, sizeof(msg), "空间不足(需%dKB, 剩%dKB)",
                 (int)(total_needed / 1024), (int)(free_bytes / 1024));
        set_status_msg(msg);
        LittleFS.end();
        return false;
    }

    File wallpaperFile = LittleFS.open(filename, FILE_WRITE);
    if (!wallpaperFile) {
        set_status_msg("无法创建文件");
        LittleFS.end();
        return false;
    }

    // 1. 写入文件头
    wp_header_t header;
    memset(&header, 0, sizeof(header));
    memcpy(header.magic, WP_MAGIC, WP_MAGIC_LEN);
    header.type = WP_TYPE_MJPEG;
    header.mjpeg_size = (uint32_t)mjpegFileSize;
    wallpaperFile.write((const uint8_t*)&header, sizeof(header));

    // 2. 写入 RGB565 预览
    size_t preview_written = 0;
    uint8_t* prev_ptr = (uint8_t*)previewBuffer;
    while (preview_written < previewSize) {
        size_t chunk = (previewSize - preview_written > 4096) ? 4096 : (previewSize - preview_written);
        size_t w = wallpaperFile.write(prev_ptr + preview_written, chunk);
        if (w != chunk) {
            wallpaperFile.close();
            LittleFS.end();
            set_status_msg("写入预览失败");
            return false;
        }
        preview_written += w;
        vTaskDelay(pdMS_TO_TICKS(5));
    }

    // 3. 从 SD 卡逐块复制 MJPEG 原始数据到 LittleFS
    SdFile mjpegFile;
    if (!mjpegFile.open(mjpegSdPath, O_RDONLY)) {
        wallpaperFile.close();
        LittleFS.end();
        set_status_msg("无法打开MJPEG源文件");
        return false;
    }

    const size_t COPY_BUF_SIZE = 8192;
    uint8_t* copyBuf = (uint8_t*)malloc(COPY_BUF_SIZE);
    if (!copyBuf) {
        mjpegFile.close();
        wallpaperFile.close();
        LittleFS.end();
        set_status_msg("内存不足");
        return false;
    }

    size_t mj_written = 0;
    bool copy_ok = true;
    while (mj_written < mjpegFileSize) {
        size_t to_read = (mjpegFileSize - mj_written > COPY_BUF_SIZE)
                         ? COPY_BUF_SIZE : (mjpegFileSize - mj_written);
        int rd = mjpegFile.read(copyBuf, to_read);
        if (rd <= 0) { copy_ok = false; break; }
        size_t wr = wallpaperFile.write(copyBuf, rd);
        if (wr != (size_t)rd) { copy_ok = false; break; }
        mj_written += wr;
        task_progress = 40 + (int)((float)mj_written / mjpegFileSize * 20.0f);
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    free(copyBuf);
    mjpegFile.close();
    wallpaperFile.close();
    LittleFS.end();

    //if (!copy_ok) set_status_msg("MJPEG数据写入失败");
    return copy_ok;
}
// 从 MJPEG 文件中提取首帧解码为 RGB565
static bool extract_mjpeg_first_frame(const char* mjpegSdPath, uint16_t** outPreview, size_t* outSize) {
    SdFile mjpegFile;
    if (!mjpegFile.open(mjpegSdPath, O_RDONLY)) return false;

    // 读取前 64KB 足以捕获首帧
    const size_t FIRST_CHUNK = 65536;
    uint8_t* readBuf = (uint8_t*)malloc(FIRST_CHUNK);
    if (!readBuf) { mjpegFile.close(); return false; }

    int bytesRead = mjpegFile.read(readBuf, FIRST_CHUNK);
    mjpegFile.close();
    if (bytesRead <= 4) { free(readBuf); return false; }

    // 寻找首帧起始 0xFF 0xD8
    uint32_t frameStart = 0;
    bool foundStart = false;
    while (frameStart < (uint32_t)bytesRead - 1) {
        if (readBuf[frameStart] == 0xFF && readBuf[frameStart + 1] == 0xD8) {
            foundStart = true;
            break;
        }
        frameStart++;
    }
    if (!foundStart) { free(readBuf); return false; }

    // 寻找首帧结束 0xFF 0xD9
    uint32_t frameEnd = frameStart + 2;
    bool foundEnd = false;
    while (frameEnd < (uint32_t)bytesRead - 1) {
        if (readBuf[frameEnd] == 0xFF && readBuf[frameEnd + 1] == 0xD9) {
            frameEnd += 2; // 包含 0xD9
            foundEnd = true;
            break;
        }
        frameEnd++;
    }
    if (!foundEnd) { free(readBuf); return false; }

    // 分配预览缓冲区
    *outSize = WP_PREVIEW_SIZE;
    *outPreview = (uint16_t*)malloc(*outSize);
    if (!*outPreview) { free(readBuf); return false; }
    memset(*outPreview, 0, *outSize);

    // 使用 JPEGDEC 解码首帧到预览缓冲区
    wp_preview_buf = *outPreview;
    bool success = false;
    JPEGDEC* jpeg = new JPEGDEC();
    if (jpeg->openRAM(readBuf + frameStart, frameEnd - frameStart, wp_preview_jpeg_cb)) {
        jpeg->setPixelType(RGB565_LITTLE_ENDIAN);
        jpeg->decode(0, 0, 0);
        jpeg->close();
        success = true;
    }
    delete jpeg;
    wp_preview_buf = nullptr;
    free(readBuf);
    return success;
}

void wallpaper_worker_task(void* param) {
    current_state = STATE_WAIT_ANIM;
    task_progress = 0;
    set_status_msg("等待界面加载...");
    vTaskDelay(pdMS_TO_TICKS(500));

    current_state = STATE_SCANNING;
    String imagePath = selected_wp_image_path;
    if (imagePath.isEmpty()) {
        current_state = STATE_ERROR;
        set_status_msg("未选择图片文件");
        vTaskDelete(NULL);
        return;
    }

    // 验证文件存在
    SdFile testFile;
    if (!testFile.open(imagePath.c_str(), O_RDONLY)) {
        current_state = STATE_ERROR;
        set_status_msg("图片文件不存在或SD卡异常");
        vTaskDelete(NULL);
        return;
    }
    testFile.close();

    //  判断类型：MJPEG 还是静态图片
    bool isMjpeg = is_mjpeg_extension(imagePath.c_str());

    if (isMjpeg) {
        // ========== MJPEG 处理分支 ==========
        current_state = STATE_DECODING;
        set_status_msg("提取MJPEG首帧预览...");

        // 获取 MJPEG 文件大小
        SdFile sizeFile;
        sizeFile.open(imagePath.c_str(), O_RDONLY);
        size_t mjpegFileSize = sizeFile.fileSize();
        sizeFile.close();

        // 提取首帧作为预览
        uint16_t* previewBuffer = nullptr;
        size_t previewSize = 0;
        if (!extract_mjpeg_first_frame(imagePath.c_str(), &previewBuffer, &previewSize)) {
            current_state = STATE_ERROR;
            set_status_msg("MJPEG首帧提取失败");
            vTaskDelete(NULL);
            return;
        }

        current_state = STATE_WRITING;
        bool writeSuccess = write_mjpeg_to_littlefs(
            previewBuffer, previewSize,
            imagePath.c_str(), mjpegFileSize,
            selected_wp_slot_num
        );
        free(previewBuffer);

        if (!writeSuccess) {
            current_state = STATE_ERROR;
            //set_status_msg("MJPEG写入失败");
        } else {
            task_progress = 100;
            current_state = STATE_SUCCESS;
            char success_msg[64];
            snprintf(success_msg, sizeof(success_msg),
                     "完成 (%dKB)",
                     selected_wp_slot_num, (int)(mjpegFileSize / 1024));
            set_status_msg(success_msg);
        }
    } else {
        // ========== 静态图片处理分支（原有逻辑 + 头部）==========
        current_state = STATE_DECODING;
        uint16_t* outputBuffer = nullptr;
        size_t outputSize = 0;
        ImageProcessor::ErrorCode result = decode_image(imagePath, &outputBuffer, &outputSize);

        if (result != ImageProcessor::SUCCESS || outputBuffer == nullptr) {
            current_state = STATE_ERROR;
            if (outputBuffer) free(outputBuffer);
            vTaskDelete(NULL);
            return;
        }

        current_state = STATE_WRITING;
        bool writeSuccess = write_to_littlefs(outputBuffer, outputSize, selected_wp_slot_num);
        free(outputBuffer);

        if (!writeSuccess) {
            current_state = STATE_ERROR;
            set_status_msg("写入 Flash 失败");
        } else {
            task_progress = 100;
            current_state = STATE_SUCCESS;
            char success_msg[64];
            snprintf(success_msg, sizeof(success_msg), "槽位%d设置成功", selected_wp_slot_num);
            set_status_msg(success_msg);
        }
    }

    wp_task_handle = nullptr;
    vTaskDelete(NULL);
}


// ================= SD卡图片扫描任务 =================
static void wp_scan_sd_task(void* param) {
    wallpaper_file_list.clear();
    if (!sd.card() || sd.card()->errorCode()) {
        wp_scan_done = true;
        wp_scanning = false;
        vTaskDelete(NULL);
        return;
    }
    SdFile dir;
    if (!dir.open("/壁纸", O_RDONLY)) {
        wp_scan_done = true;
        wp_scanning = false;
        vTaskDelete(NULL);
        return;
    }
    const char* extensions[] = {
        ".png", ".jpg", ".jpeg", ".PNG", ".JPG", ".JPEG",
        ".mjpeg", ".MJPEG"
    };
    SdFile file;
    while (file.openNext(&dir, O_RDONLY)) {
        char filename[256];
        file.getName(filename, sizeof(filename));
        for (const char* ext : extensions) {
            if (strstr(filename, ext) != nullptr) {
                wallpaper_file_list.push_back(String(filename));
                break;
            }
        }
        file.close();
    }
    dir.close();
    wp_scan_done = true;
    wp_scanning = false;
    vTaskDelete(NULL);
}


// 图片扫描检查定时器回调
static void wp_scan_check_timer_cb(lv_timer_t* timer) {
    if (wp_scan_done && wp_image_dropdown && lv_obj_is_valid(wp_image_dropdown)) {
        String options = "";
        if (wallpaper_file_list.empty()) {
            options = "未找到图片";
        } else {
            for (const auto& name : wallpaper_file_list) {
                options += name + "\n";
            }
            if (options.length() > 0) options.remove(options.length() - 1);
        }
        lv_dropdown_set_options(wp_image_dropdown, options.c_str());
        lv_dropdown_set_text(wp_image_dropdown, NULL);

        // 恢复扫描按钮
        if (wp_scan_btn && lv_obj_is_valid(wp_scan_btn)) {
            lv_obj_clear_state(wp_scan_btn, LV_STATE_DISABLED);
        }

        lv_timer_del(timer);
        wp_scan_check_timer = nullptr;
    }
}

// 扫描SD卡按钮回调
static void wp_scan_btn_cb(lv_event_t* e) {
    if (wp_scanning) return;
    wp_scanning = true;
    wp_scan_done = false;

    if (wp_image_dropdown && lv_obj_is_valid(wp_image_dropdown)) {
        lv_dropdown_set_options(wp_image_dropdown, "扫描中...");
        lv_dropdown_set_text(wp_image_dropdown, "扫描中...");
    }

    if (wp_scan_btn && lv_obj_is_valid(wp_scan_btn)) {
        lv_obj_add_state(wp_scan_btn, LV_STATE_DISABLED);
    }

    if (wp_scan_check_timer) lv_timer_del(wp_scan_check_timer);
    wp_scan_check_timer = lv_timer_create(wp_scan_check_timer_cb, 200, NULL);

    xTaskCreatePinnedToCore(wp_scan_sd_task, "WpScanSD", 4096, NULL, 1, NULL, 0);
}

static void wp_refresh_btn_cb(lv_event_t* e) {
    // 防止重复触发
    if (current_state != STATE_IDLE && current_state != STATE_SUCCESS && current_state != STATE_ERROR) {
        return;
    }

    // 获取选中的槽位号
    if (wp_slot_dropdown && lv_obj_is_valid(wp_slot_dropdown)) {
        selected_wp_slot_num = lv_dropdown_get_selected(wp_slot_dropdown) + 1; // 1~5
    } else {
        set_status_msg("请选择存储槽位");
        return;
    }

    // 获取选中的图片文件名
    if (wp_image_dropdown && lv_obj_is_valid(wp_image_dropdown)) {
        char buf[128];
        lv_dropdown_get_selected_str(wp_image_dropdown, buf, sizeof(buf));
        String selected_name = String(buf);

        if (selected_name.isEmpty() ||
            selected_name == "请先扫描" ||
            selected_name == "扫描中..." ||
            selected_name == "未找到图片" ||
            selected_name == "点击扫描获取列表") {
            set_status_msg("请先选择图片文件");
            if (progressLabel && lv_obj_is_valid(progressLabel)) {
                lv_label_set_text(progressLabel, "请先选择图片文件");
            }
            return;
        }

        selected_wp_image_path = String("/壁纸/") + selected_name;
    } else {
        set_status_msg("请先选择图片文件");
        return;
    }

    // 触发时立刻禁用按钮
    if (wp_refresh_btn && lv_obj_is_valid(wp_refresh_btn)) {
        lv_obj_add_state(wp_refresh_btn, LV_STATE_DISABLED);
    }

    start_wallpaper_task();
}

static void init_wallpaper_ui() {
    // ===== 槽位选择 =====
    lv_obj_t* slot_label = lv_label_create(subContainer);
    lv_label_set_text(slot_label, "存储");
    lv_obj_set_style_text_font(slot_label, &chinese_24, LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(slot_label, lv_color_white(), LV_STATE_DEFAULT);
    lv_obj_align(slot_label, LV_ALIGN_TOP_LEFT, 10, 8);

    wp_slot_dropdown = lv_dropdown_create(subContainer);
    lv_dropdown_set_options(wp_slot_dropdown,
        "槽位 1\n槽位 2\n槽位 3\n槽位 4\n槽位 5");
    lv_obj_set_size(wp_slot_dropdown, 140, 40);
    lv_obj_align(wp_slot_dropdown, LV_ALIGN_TOP_RIGHT, -10, 3);
    lv_obj_set_style_text_font(wp_slot_dropdown, &chinese_24, 0);
    lv_obj_t* slot_dd_list = lv_dropdown_get_list(wp_slot_dropdown);
    lv_obj_set_style_text_font(slot_dd_list, &chinese_24, LV_PART_MAIN);
    lv_dropdown_set_symbol(wp_slot_dropdown, NULL);

    // ===== 图片文件选择 =====
    lv_obj_t* img_label = lv_label_create(subContainer);
    lv_label_set_text(img_label, "图片文件:");
    lv_obj_set_style_text_font(img_label, &chinese_24, LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(img_label, lv_color_white(), LV_STATE_DEFAULT);
    lv_obj_align(img_label, LV_ALIGN_TOP_LEFT, 10, 53);

    wp_image_dropdown = lv_dropdown_create(subContainer);
    lv_dropdown_set_options(wp_image_dropdown, "点击扫描获取列表");
    lv_dropdown_set_text(wp_image_dropdown, "请先扫描");
    lv_obj_set_size(wp_image_dropdown, 220, 40);
    lv_obj_align(wp_image_dropdown, LV_ALIGN_TOP_MID, 0, 78);
    lv_obj_set_style_text_font(wp_image_dropdown, &chinese_24, 0);
    lv_obj_t* img_dd_list = lv_dropdown_get_list(wp_image_dropdown);
    lv_obj_set_style_text_font(img_dd_list, &chinese_24, LV_PART_MAIN);
    lv_dropdown_set_symbol(wp_image_dropdown, NULL);

    // ===== 扫描SD卡按钮 =====
    wp_scan_btn = lv_btn_create(subContainer);
    lv_obj_set_size(wp_scan_btn, 220, 32);
    lv_obj_align(wp_scan_btn, LV_ALIGN_TOP_MID, 0, 122);
    lv_obj_set_style_bg_color(wp_scan_btn, lv_color_hex(0x3498db), 0);
    lv_obj_add_event_cb(wp_scan_btn, wp_scan_btn_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t* scan_label = lv_label_create(wp_scan_btn);
    lv_label_set_text(scan_label, "扫描SD卡/壁纸(5个)");
    lv_obj_set_style_text_font(scan_label, &chinese_24, 0);
    lv_obj_center(scan_label);

    // ===== 进度条 =====
    progressBar = lv_bar_create(subContainer);
    lv_obj_set_size(progressBar, 200, 12);
    lv_obj_align(progressBar, LV_ALIGN_CENTER, 0, 30);
    lv_bar_set_range(progressBar, 0, 100);
    lv_bar_set_value(progressBar, 0, LV_ANIM_OFF);

    // ===== 状态标签 =====
    progressLabel = lv_label_create(subContainer);
    lv_label_set_text(progressLabel, "选择槽位和图片后点击刷新");
    lv_obj_set_style_text_font(progressLabel, &chinese_24, LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(progressLabel, lv_color_white(), LV_STATE_DEFAULT);
    lv_obj_set_width(progressLabel, 220);
    lv_label_set_long_mode(progressLabel, LV_LABEL_LONG_WRAP);
    lv_obj_align(progressLabel, LV_ALIGN_CENTER, 0, 50);

    // ===== 刷新壁纸按钮 =====
    wp_refresh_btn = lv_btn_create(subContainer);
    lv_obj_set_size(wp_refresh_btn, 200, 40);
    lv_obj_align(wp_refresh_btn, LV_ALIGN_CENTER, 0, 95);
    lv_obj_set_style_bg_color(wp_refresh_btn, lv_color_hex(0x2ecc71), 0);
    lv_obj_add_event_cb(wp_refresh_btn, wp_refresh_btn_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t* btn_label = lv_label_create(wp_refresh_btn);
    lv_label_set_text(btn_label, "写入壁纸");
    lv_obj_set_style_text_font(btn_label, &chinese_24, 0);
    lv_obj_center(btn_label);

    current_state = STATE_IDLE;
    task_progress = 0;
    uiMonitorTimer = lv_timer_create(wallpaper_ui_monitor_cb, 50, NULL);

    // 自动触发一次扫描
    wp_scan_btn_cb(NULL);
}

static void start_wallpaper_task() {
    xTaskCreatePinnedToCore(wallpaper_worker_task, "WpTask", 20480, NULL, 1, &wp_task_handle, 0);
}

// ================= 主初始化函数 =================
void fs_create_settings(lv_obj_t* container) {
    mainList = lv_list_create(container);
    lv_obj_set_size(mainList, 240, 280);
    lv_obj_center(mainList);

    const char* item_texts[] = {
        "时间同步",
        "WiFi",
        "壁纸",
        "电量校准",
        "自动轮播间隔",
        "重新扫描SD卡",
        "天气设置"          
    };

    for (int i = 0; i < 7; i++) {
        lv_obj_t* btn = lv_list_add_btn(mainList, NULL, item_texts[i]);
        lv_obj_set_height(btn, 50);
        lv_obj_set_style_text_font(btn, &chinese_24, LV_STATE_DEFAULT);
        lv_obj_add_event_cb(btn, list_item_cb, LV_EVENT_CLICKED, NULL);
    }

    if (ioTimer == nullptr) {
        ioTimer = lv_timer_create(poll_timer_cb, 50, NULL);
    }
}