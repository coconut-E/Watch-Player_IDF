/*
 * 移植自参考工程 (Arduino) 的 ui_manager.cpp
 *
 * 移植说明:
 *   - 保留原文件结构、函数名、布局常量与注释;
 *   - 底层替换: LGFX/Arduino → esp_lcd + CST816S + esp_timer (见 ui_core.c / arduino_compat.*)
 *   - 天气子系统 (get_today_weather_from_prefs / connect_saved_wifi / weather_task /
 *     Get_weather / create_weather_widget) 逐字移植自参考工程;
 *     底层替换: HTTPClient → compat/HTTPClient.h (官方 esp_http_client),
 *               ArduinoJson → 组件市场 bblanchon/arduinojson (v6),
 *               WiFi → compat/WiFi.h (esp_wifi)。
 */

#include "ui_manager.h"
#include <lvgl.h>
#include "menu.h"
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <WiFi.h>

#include "arduino_compat.h"
#include "ui_core.h"
#include "lcd.h"
#include "touch.h"

#include "esp_heap_caps.h"

LV_FONT_DECLARE(time_80);
LV_FONT_DECLARE(tip_display_35);
typedef enum {
    FS_TIME_SCREEN = 0,   // 时钟
    FS_PICTURE,           // 图片
    FS_NOVEL,             // 小说
    FS_VIDEO,             // 视频
    FS_MUSIC,             // 音乐
    FS_GAME,              // 游戏
    FS_CALCULATOR,        // 计算器
    FS_STOPWATCH,         // 计时/秒表
    FS_CALENDAR,          // 日历
    FS_COMIC,             // 漫画
    FS_SETTINGS,          // 设置
    FS_FILEMANAGER,       // 文件管理
    FS_WEATHER,           // 天气

    FS_FUNC_COUNT
} FullscreenFunction;
// 图片相关
#define CIRCLE_IMG_COUNT 12

 //全屏功能函数指针
typedef void (*FullscreenContentCreator)(lv_obj_t* container);

// 全屏功能函数指针
FullscreenContentCreator fs_creators[FS_FUNC_COUNT] = {
    [FS_TIME_SCREEN]   = fs_create_time,
    [FS_PICTURE]       = fs_create_picture,
    [FS_NOVEL]         = fs_create_novel,
    [FS_VIDEO]         = fs_create_video,
    [FS_MUSIC]         = fs_create_music,
    [FS_GAME]          = fs_create_game,
    [FS_CALCULATOR]    = fs_create_calculator,
    [FS_STOPWATCH]     = fs_create_stopwatch,
    [FS_CALENDAR]      = fs_create_calendar,
    [FS_COMIC]         = fs_create_comic,
    [FS_SETTINGS]      = fs_create_settings,
    [FS_FILEMANAGER]   = fs_create_filemanager,
    [FS_WEATHER]       = fs_create_weather,
};

//全局UI对象
lv_obj_t* circle_container = nullptr;
lv_obj_t* hour_label = nullptr;
lv_obj_t* minute_label = nullptr;
lv_obj_t* second_label = nullptr;
lv_obj_t* sliding_container = nullptr;
lv_obj_t* fullscreen_container = nullptr;
lv_obj_t* tip_container = nullptr;
lv_obj_t* tip_label = nullptr;
lv_timer_t* tip_timer = nullptr;
lv_obj_t* battery_area = nullptr;
lv_obj_t* battery_percent_label = nullptr;
lv_obj_t* battery_bar = nullptr;
lv_obj_t* battery_tip = nullptr;
volatile bool is_fullscreen_container_active = false;
bool is_tip_container_active = false;
bool is_sliding_container_expanded = false;
static lv_obj_t* main_container = nullptr;
static lv_timer_t* main_sleep_timer = nullptr;
static lv_obj_t* anim_canvas = nullptr;           // 用于绘制缩放动画的透明画布
static lv_color_t* canvas_buf = nullptr;          // 画布数据缓存区 (PSRAM)
static lv_color_t* snapshot_buf = nullptr;        // 截图数据缓存区 (PSRAM)
static bool fs_anim_buffers_valid = false;        // 标记PSRAM是否申请成功
static lv_point_t g_last_click_center = {120, 200};      // 上次点击的图标中心
static lv_point_t g_anim_start_center;                   // 动画起始中心
static lv_point_t g_anim_end_center;                     // 动画结束中心
static lv_color_t* bg_buf = nullptr;           // 主界面背景截图缓存 (PSRAM)
// 天气组件对象
static lv_obj_t* weather_container = nullptr;
static lv_obj_t* weather_img = nullptr;
static lv_obj_t* weather_temp_label = nullptr;
static lv_obj_t* weather_time_label = nullptr;
static lv_obj_t* weather_desc_label = nullptr;  
static lv_obj_t* weather_placeholder_btn = nullptr;  // 点击获取天气按钮
static lv_obj_t* weather_loading_arc = nullptr;      // 加载动画
static lv_anim_t weather_arc_anim;                    // 动画控制

static enum { KEY_IDLE, KEY_PRESSED, KEY_LONG_PRESS_TRIGGERED } key_state = KEY_IDLE;
static uint32_t key_press_start = 0;


// 天气数据缓存
struct WeatherData {
    int wmo_code;             // WMO天气代码
    int code_index;           // 图片索引 (0-9)
    float temperature_max;    // 最高温度
    float temperature_min;    // 最低温度
    char update_time[16];     // 日期 "MM/DD"
    bool valid;
};
static WeatherData s_cached_weather = {0, 0, 0.0f, 0.0f, "", false};
static volatile bool weather_update_pending = false;
static SemaphoreHandle_t weather_mutex = nullptr;
static lv_timer_t* weather_ui_timer = nullptr;

// 天气任务句柄
static TaskHandle_t weather_task_handle = nullptr;
static volatile bool weather_task_running = false;

lv_img_dsc_t menu_img_dsc[CIRCLE_IMG_COUNT];
lv_img_dsc_t weather_img_dsc[WEATHER_FRAME_CNT];

const int16_t container_x = 0;
const int16_t container_min_y = -103;
const int16_t container_max_y = 0;
const int16_t container_w = 240;
const int16_t container_h = 115;
const int16_t fs_container_hidden_y = 280;
const int16_t fs_container_show_y = 0;
const int16_t fs_container_w = 240;
const int16_t fs_container_h = 280;
const int16_t tip_container_hidden_y = -115;
const int16_t tip_container_show_y = 0;
const uint16_t tip_container_anim_time = 200;
const uint32_t tip_timer_interval = 1500;
static const size_t FS_BUF_SIZE = fs_container_w * fs_container_h * sizeof(lv_color_t);

uint32_t img_index = 0;

static unsigned long boot_time = 0;
static int32_t last_touch_y = 0;
static uint32_t first_exit_time = 0;       // 第一次退出全屏的时间戳
static bool first_exit_occurred = false;   // 是否已经发生过第一次退出


extern bool sd_card_initialized;
extern bool sd_card_init_event;// SD卡初始化失败事件
extern bool sd_card_scan_event;
extern bool sd_card_inserted;
extern int sd_card_insertion_event;
extern SemaphoreHandle_t sd_state_mutex;
extern QueueHandle_t scan_queue;


static void init_menu_images();
static void create_uptime_display(lv_obj_t* parent);
static void init_circle_layout(lv_obj_t* cont);
static void scroll_event_cb(lv_event_t* e);
static void image_click_event_cb(lv_event_t* e);
static void brightness_slider_event_cb(lv_event_t* e);
static void adsorb_anim_cb(void* var, int32_t v);
static void do_adsorb(void);
static void container_touch_event_cb(lv_event_t* e);
static void create_sliding_container();
static void fs_container_touch_event_cb(lv_event_t* e);
static void create_fullscreen_container(uint32_t func_index);
static void restore_main_ui_zorder();
static void create_time_container_without_anim();
static void tip_adsorb_anim_cb(void* var, int32_t v);
static void tip_container_delete_self();
static void tip_timer_cb(lv_timer_t* timer);
static void start_tip_timer(void);
static void stop_tip_timer(void);
static void reset_tip_timer(void);
static void ui_update_battery();
int weather_code_to_index(int code, int is_day);

static void main_sleep_timer_cb(lv_timer_t* timer) {
    board_sleep();
}

static void start_main_sleep_timer() {
    if (!main_sleep_timer) {
        main_sleep_timer = lv_timer_create(main_sleep_timer_cb, 300000, NULL);
    } else {
        lv_timer_reset(main_sleep_timer);
    }
}
static void stop_main_sleep_timer() {
    if (main_sleep_timer) {
        lv_timer_del(main_sleep_timer);
        main_sleep_timer = nullptr;
    }
}
//初始化图片描述符
static void init_menu_images() {
    for (int i = 0; i < CIRCLE_IMG_COUNT; i++) {
        menu_img_dsc[i].header.w = MENU_IMG_WIDTH;
        menu_img_dsc[i].header.h = MENU_IMG_HEIGHT;
        menu_img_dsc[i].data_size = MENU_IMG_WIDTH * MENU_IMG_HEIGHT * LV_COLOR_DEPTH / 8;
        menu_img_dsc[i].header.cf = LV_IMG_CF_TRUE_COLOR;
        menu_img_dsc[i].data = (const uint8_t*)&menu[i][0];
    }
}
//初始化图片描述符 (天气图标, 对应参考工程)
static void init_weather_images() {
    for (int i = 0; i < WEATHER_FRAME_CNT; i++) {
        weather_img_dsc[i].header.w = WEATHER_IMG_WIDTH;
        weather_img_dsc[i].header.h = WEATHER_IMG_HEIGHT;
        weather_img_dsc[i].data_size = WEATHER_IMG_WIDTH * WEATHER_IMG_HEIGHT * LV_COLOR_DEPTH / 8;
        weather_img_dsc[i].header.cf = LV_IMG_CF_TRUE_COLOR;
        weather_img_dsc[i].data = (const uint8_t*)&weather[i][0];
    }
}
//创建时间显示
static void create_uptime_display(lv_obj_t* parent) {
    hour_label = lv_label_create(parent);
    lv_obj_set_style_text_color(hour_label, lv_color_white(), LV_STATE_DEFAULT);
    lv_obj_set_style_text_font(hour_label, &time_80, LV_STATE_DEFAULT);
    lv_obj_align(hour_label, LV_ALIGN_TOP_LEFT, 20, 20);

    minute_label = lv_label_create(parent);
    lv_obj_set_style_text_color(minute_label, lv_color_white(), LV_STATE_DEFAULT);
    lv_obj_set_style_text_font(minute_label, &time_80, LV_STATE_DEFAULT);
    lv_obj_align_to(minute_label, hour_label, LV_ALIGN_OUT_BOTTOM_LEFT, 0, 5);

    second_label = lv_label_create(parent);
    lv_obj_set_style_text_color(second_label, lv_color_white(), LV_STATE_DEFAULT);
    lv_obj_set_style_text_font(second_label, &lv_font_montserrat_24, LV_STATE_DEFAULT);
    lv_obj_align_to(second_label, minute_label, LV_ALIGN_OUT_BOTTOM_LEFT, 0, 5);
}

//更新时间
void ui_update_uptime() {
    static long last_update = -1200;
    if (millis() - last_update >= 1000) {
        ui_update_battery();
        time_t now = time(nullptr);
        struct tm *ptm = gmtime(&now);
        if (ptm == nullptr) {
            lv_label_set_text(hour_label, "00");
            lv_label_set_text(minute_label, "00");
            lv_label_set_text(second_label, "00");
            last_update = millis();
            return;
        }
        char hour_buf[10], minute_buf[10], second_buf[10];
        snprintf(hour_buf, sizeof(hour_buf), "%02d", ptm->tm_hour);
        snprintf(minute_buf, sizeof(minute_buf), "%02d", ptm->tm_min);
        snprintf(second_buf, sizeof(second_buf), "%02d", ptm->tm_sec);
        lv_label_set_text(hour_label, hour_buf);
        lv_label_set_text(minute_label, minute_buf);
        lv_label_set_text(second_label, second_buf);
        last_update = millis();
    }
}
static void init_circle_layout(lv_obj_t* cont) {
    const float CENTER_SCALE_RATIO = 1.2f;   // 中心最大缩放
    const float MID_SCALE_RATIO    = 0.8f;   // 中间缩放
    const float EDGE_SCALE_RATIO   = 0.7f;   // 边缘最小缩放
    const int32_t LVGL_SCALE_BASE = 256;

    lv_area_t cont_a;
    lv_obj_get_coords(cont, &cont_a);
    int32_t cont_width = lv_area_get_width(&cont_a);
    int32_t cont_x_center = cont_a.x1 + cont_width / 2;
    int32_t r = cont_width / 2;   // 半径 = 120
    int32_t mid_dist = r / 2;      // 中间缩放对应的距离

    // 预计算缩放整数值
    const int32_t ZOOM_CENTER = (int32_t)(LVGL_SCALE_BASE * CENTER_SCALE_RATIO);
    const int32_t ZOOM_MID    = (int32_t)(LVGL_SCALE_BASE * MID_SCALE_RATIO);
    const int32_t ZOOM_EDGE   = (int32_t)(LVGL_SCALE_BASE * EDGE_SCALE_RATIO);

    uint32_t child_cnt = lv_obj_get_child_cnt(cont);
    for(uint32_t i = 0; i < child_cnt; i++) {
        lv_obj_t* child = lv_obj_get_child(cont, i);
        lv_area_t child_a;
        lv_obj_get_coords(child, &child_a);
        int32_t child_x_center = child_a.x1 + lv_area_get_width(&child_a) / 2;
        int32_t diff_x = LV_ABS(child_x_center - cont_x_center);
        if(diff_x > r) diff_x = r;   // 防止溢出

        //垂直偏移计算
        int32_t y;
        if(diff_x > r) {
            y = r;
        } else {
            uint32_t y_sqr = r * r - diff_x * diff_x;
            lv_sqrt_res_t res;
            lv_sqrt(y_sqr, &res, 0x8000);
            y = r - res.i + 5;
        }
        lv_obj_set_style_translate_y(child, y, LV_STATE_DEFAULT);
        lv_obj_set_style_opa(child, LV_OPA_COVER, LV_STATE_DEFAULT);

        // 缩放计算
        int32_t scale;
        if (diff_x <= mid_dist) {
            float t = (float)diff_x / mid_dist;
            scale = ZOOM_CENTER - (int32_t)((ZOOM_CENTER - ZOOM_MID) * t);
        } else {
            float t = (float)(diff_x - mid_dist) / (r - mid_dist);
            scale = ZOOM_MID - (int32_t)((ZOOM_MID - ZOOM_EDGE) * t);
        }

        // 限制范围
        if(scale < ZOOM_EDGE) scale = ZOOM_EDGE;
        if(scale > ZOOM_CENTER) scale = ZOOM_CENTER;

        lv_img_set_zoom(child, scale);
    }
}
static void scroll_event_cb(lv_event_t* e) {
    lv_obj_t* cont = lv_event_get_target(e);
    init_circle_layout(cont);
    if (main_sleep_timer) lv_timer_reset(main_sleep_timer);
}

//图片点击事件
static void image_click_event_cb(lv_event_t* e) {
    lv_obj_t* img = lv_event_get_target(e);
    img_index = (uint32_t)img->user_data;

    lv_area_t coords;
    lv_obj_get_coords(img, &coords);
    g_last_click_center.x = (coords.x1 + coords.x2) / 2;
    g_last_click_center.y = (coords.y1 + coords.y2) / 2;

    Serial.print("点击了第 ");
    Serial.print(img_index);
    Serial.println(" 号图片");

    // 需要 SD 卡的功能 (参考工程 ui_manager.cpp)
    bool is_sd_related_func = (img_index == FS_PICTURE     ||   // 图片
                               img_index == FS_NOVEL       ||   // 小说
                               img_index == FS_VIDEO       ||   // 视频
                               img_index == FS_MUSIC       ||   // 音乐
                               img_index == FS_COMIC       ||   // 漫画
                               img_index == FS_SETTINGS    ||   // 设置
                               img_index == FS_FILEMANAGER);    // 文件管理

    // 获取当前SD卡状态（扫描中、预热中）
    bool is_scanning = false;
    bool is_warming = false;
    if (sd_state_mutex && xSemaphoreTake(sd_state_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        is_scanning = sd_card_scan_event;
        is_warming = sd_card_warming;
        xSemaphoreGive(sd_state_mutex);
    }

    // 检查是否在第一次退出后的1秒冷却期内
    bool in_cooldown = first_exit_occurred && (millis() - first_exit_time < 1000);
    // 检查天气任务是否正在运行
    bool weather_running = weather_task_running;
    // 如果是SD相关功能，且处于禁止状态（扫描、预热、冷却期、天气任务运行中），则禁止进入
    if (is_sd_related_func && (is_scanning || is_warming || in_cooldown || weather_running)) {
        Serial.println("禁止进入SD相关功能: SD卡正在扫描/预热/退出冷却中/天气获取中");
        return;
    }

    if (!atomic_load_bool(&is_fullscreen_container_active)) {
        create_fullscreen_container(img_index);
    }
}

//创建环形图片列表
static void create_circle_list(lv_obj_t* parent) {
    circle_container = lv_obj_create(parent);
    lv_obj_set_size(circle_container, 240, 100);
    lv_obj_align(circle_container, LV_ALIGN_CENTER, 0, 80);
    lv_obj_set_flex_flow(circle_container, LV_FLEX_FLOW_ROW);
    lv_obj_add_event_cb(circle_container, scroll_event_cb, LV_EVENT_SCROLL, NULL);
    lv_obj_set_style_bg_opa(circle_container, LV_OPA_TRANSP, LV_STATE_DEFAULT);
    lv_obj_set_style_border_opa(circle_container, LV_OPA_TRANSP, LV_STATE_DEFAULT);
    lv_obj_set_style_pad_all(circle_container, 0, LV_STATE_DEFAULT);
    lv_obj_set_style_radius(circle_container, 0, LV_STATE_DEFAULT);
    lv_obj_set_scroll_dir(circle_container, LV_DIR_HOR);
    lv_obj_set_scroll_snap_x(circle_container, LV_SCROLL_SNAP_CENTER);
    lv_obj_set_scrollbar_mode(circle_container, LV_SCROLLBAR_MODE_OFF);
    for(uint32_t i = 0; i < CIRCLE_IMG_COUNT; i++) {
        lv_obj_t* img = lv_img_create(circle_container);
        lv_img_set_src(img, &menu_img_dsc[i]);
        lv_obj_set_size(img, 60, 60);
        lv_obj_add_flag(img, LV_OBJ_FLAG_CLICKABLE);
        lv_img_set_zoom(img, 256);
        lv_obj_clear_flag(img, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_clear_flag(img, LV_OBJ_FLAG_SCROLL_ELASTIC);
        img->user_data = (void*)i;
        lv_obj_add_event_cb(img, image_click_event_cb, LV_EVENT_CLICKED, NULL);
    }
    lv_obj_update_layout(circle_container);
    lv_obj_scroll_to_view(lv_obj_get_child(circle_container, 0), LV_ANIM_ON);
    init_circle_layout(circle_container);
}

static void check_power_key_timer_cb(lv_timer_t* timer) {
    //不在全屏容器中才检测
    if (atomic_load_bool(&is_fullscreen_container_active)) {
        // 如果在全屏中，重置状态
        key_state = KEY_IDLE;
        return;
    }
    int level = digitalRead(BUTTON_2);
    uint32_t now = millis();
    switch (key_state) {
        case KEY_IDLE:
            if (level == HIGH) {
                key_state = KEY_PRESSED;
                key_press_start = now;
            }
            break;
        case KEY_PRESSED:
            if (level == LOW) {
                // 松手，且未触发长按，直接回到空闲
                key_state = KEY_IDLE;
            } else {
                // 持续按下，检查是否超过1秒
                if (now - key_press_start >= 1000) {
                    // 长按触发：亮度设为0
                    display.setBrightness(0);
                    key_state = KEY_LONG_PRESS_TRIGGERED;
                }
            }
            break;
        case KEY_LONG_PRESS_TRIGGERED:
            if (level == LOW) {
                // 松手
                board_sleep();
            }
            break;
    }
}

// ================= 天气相关函数================
static bool get_today_weather_from_prefs(WeatherData &today) {
    xSemaphoreTake(weather_mutex, portMAX_DELAY);
    preferences.begin("watch", true);

    // 检查是否有第一天数据
    if (!preferences.isKey("day0_date")) {
        preferences.end();
        xSemaphoreGive(weather_mutex);
        return false;
    }

    // 读取所有天的数据
    struct DayData {
        char date[6];
        int code;
        float max;
        float min;
    } days[7];
    int dayCount = 0;
    for (int i = 0; i < 7; i++) {
        char key_date[16], key_code[16], key_max[16], key_min[16];
        snprintf(key_date, sizeof(key_date), "day%d_date", i);
        snprintf(key_code, sizeof(key_code), "day%d_code", i);
        snprintf(key_max, sizeof(key_max), "day%d_max", i);
        snprintf(key_min, sizeof(key_min), "day%d_min", i);
        if (preferences.isKey(key_date)) {
            String dateStr = preferences.getString(key_date, "");
            if (dateStr.length() > 0) {
                strlcpy(days[dayCount].date, dateStr.c_str(), sizeof(days[dayCount].date));
                days[dayCount].code = preferences.getInt(key_code, 0);
                days[dayCount].max = preferences.getFloat(key_max, 0);
                days[dayCount].min = preferences.getFloat(key_min, 0);
                dayCount++;
            }
        } else {
            break;
        }
    }
    preferences.end();

    if (dayCount == 0) {
        xSemaphoreGive(weather_mutex);
        return false;
    }

    // 获取当前本地日期字符串
    char today_str[6] = {0};
    time_t now = time(nullptr);
    struct tm *ptm = localtime(&now); 
    if (ptm != nullptr) {
        snprintf(today_str, sizeof(today_str), "%02d/%02d", ptm->tm_mon + 1, ptm->tm_mday);
    } else {
        // 时间获取失败，认为缓存无效
        today.valid = false;
        xSemaphoreGive(weather_mutex);
        return false;
    }

    // 查找匹配日期
    int found = -1;
    for (int i = 0; i < dayCount; i++) {
        if (strcmp(today_str, days[i].date) == 0) {
            found = i;
            break;
        }
    }

    if (found != -1) {
        today.code_index = weather_code_to_index(days[found].code, 1);
        today.temperature_max = days[found].max;
        today.temperature_min = days[found].min;
        today.wmo_code = days[found].code;
        strlcpy(today.update_time, days[found].date, sizeof(today.update_time));
        today.valid = true;
        xSemaphoreGive(weather_mutex);
        return true;
    }

    // 未找到，判断是否小于最小或大于最大
    if (strcmp(today_str, days[0].date) < 0) {
        // 小于最小，用第一天
        today.code_index = weather_code_to_index(days[0].code, 1);
        today.temperature_max = days[0].max;
        today.temperature_min = days[0].min;
        today.wmo_code = days[0].code;
        strlcpy(today.update_time, days[0].date, sizeof(today.update_time));
        today.valid = true;
        xSemaphoreGive(weather_mutex);
        return true;
    } else if (strcmp(today_str, days[dayCount-1].date) > 0) {
        // 大于最大，失效
        today.valid = false;
        xSemaphoreGive(weather_mutex);
        return false;
    } else {
        today.valid = false;
        xSemaphoreGive(weather_mutex);
        return false;
    }
}

static bool connect_saved_wifi() {
    WiFi.mode(WIFI_STA);
    WiFi.disconnect();
    int n = WiFi.scanNetworks();
    if (n == 0) return false;

    preferences.begin("watch", true);
    String ssid_list[5], pass_list[5];
    for (int i = 0; i < 5; i++) {
        char key_ssid[16], key_pass[16];
        snprintf(key_ssid, sizeof(key_ssid), "WiFiSSID%d", i+1);
        snprintf(key_pass, sizeof(key_pass), "WiFiPass%d", i+1);
        ssid_list[i] = preferences.getString(key_ssid, "");
        pass_list[i] = preferences.getString(key_pass, "");
    }
    preferences.end();

    for (int i = 0; i < 5; i++) {
        if (ssid_list[i].isEmpty()) continue;
        bool found = false;
        for (int j = 0; j < n; j++) {
            if (WiFi.SSID(j) == ssid_list[i]) {
                found = true;
                break;
            }
        }
        if (!found) continue;
        WiFi.setTxPower(WIFI_POWER_11dBm); 
        WiFi.begin(ssid_list[i].c_str(), pass_list[i].c_str());
        int timeout = 20; // 10s
        while (--timeout > 0 && WiFi.status() != WL_CONNECTED) {
            vTaskDelay(pdMS_TO_TICKS(500));
        }
        if (WiFi.status() == WL_CONNECTED) {
            return true;
        }
        WiFi.disconnect();
        Serial.println(WiFi.status());
    }
    return false;
}

const char* wmo_code_to_desc_cn(int code) {
    switch(code) {
        case 0: return "晴";
        case 1: return "大部晴";
        case 2: return "多云";
        case 3: return "阴";
        case 45: case 48: return "雾";
        case 51: case 53: case 55: return "毛毛雨";
        case 56: case 57: return "冻毛毛雨";
        case 61: return "小雨";
        case 63: return "中雨";
        case 65: return "大雨";
        case 66: case 67: return "冻雨";
        case 71: return "小雪";
        case 73: return "中雪";
        case 75: return "大雪";
        case 77: return "雪粒";
        case 80: return "阵雨";
        case 81: return "阵雨";
        case 82: return "强阵雨";
        case 85: return "阵雪";
        case 86: return "强阵雪";
        case 95: return "雷暴";
        case 96: case 99: return "雷暴伴冰雹";
        default: return "未知";
    }
}

int weather_code_to_index(int code, int is_day) {
    if (code == 0 || code == 1) return is_day ? 6 : 5; // 晴/晚晴
    if (code == 2) return is_day ? 9 : 4;              // 多云/晚多云
    if (code == 3) return 7;                           // 阴
    if (code == 45 || code == 48) return 2;            // 雾
    if ((code >= 51 && code <= 57) || (code >= 61 && code <= 67) || (code >= 80 && code <= 82)) return 8; // 雨
    if ((code >= 71 && code <= 77) || code == 85 || code == 86) return 0; // 雪
    if (code >= 95 && code <= 99) return 1;            // 雷阵雨
    return 9; // 默认多云
}
static void weather_task(void* param) {
    weather_task_running = true;
    bool success = false;

    // 从 Preferences 读取天气经纬度配置
    float weather_lat = 31.23f;
    float weather_lon = 121.47f;
    if (weather_mutex) xSemaphoreTake(weather_mutex, portMAX_DELAY);
    preferences.begin("watch", true);
    weather_lat = preferences.getFloat("weather_lat", 31.23f);
    weather_lon = preferences.getFloat("weather_lon", 121.47f);
    preferences.end();
    if (weather_mutex) xSemaphoreGive(weather_mutex);
    Serial.printf("天气经纬度: lat=%.2f, lon=%.2f\n", weather_lat, weather_lon);

    Serial.println("尝试连接保存的 WiFi...");
    if (connect_saved_wifi()) {
        Serial.println("WiFi 连接成功！");
        HTTPClient http;
        http.setTimeout(8000);
        http.setConnectTimeout(5000);
        // API：7天预报（使用 Preferences 中的经纬度）
        char url_buf[256];
        snprintf(url_buf, sizeof(url_buf),
            "http://api.open-meteo.com/v1/forecast?latitude=%.2f&longitude=%.2f&daily=weathercode,temperature_2m_max,temperature_2m_min&timezone=Asia/Shanghai",
            weather_lat, weather_lon);
        String url = String(url_buf);
   

        if (http.begin(url)) {
            int httpCode = http.GET();
            Serial.printf("HTTP响应码: %d\n", httpCode);

            if (httpCode == 200) {
                String payload = http.getString();
                Serial.println("收到响应:");
                Serial.println(payload);

                DynamicJsonDocument doc(3072);  // 缓冲区
                DeserializationError error = deserializeJson(doc, payload);
                if (!error) {
                    JsonArray timeArray = doc["daily"]["time"];
                    JsonArray codeArray = doc["daily"]["weathercode"];
                    JsonArray maxArray = doc["daily"]["temperature_2m_max"];
                    JsonArray minArray = doc["daily"]["temperature_2m_min"];

                    int cnt = min((int)timeArray.size(), 7);
                    if (cnt > 0) {
                        // 准备写入Preferences
                        xSemaphoreTake(weather_mutex, portMAX_DELAY);
                        preferences.begin("watch", false);

                        // 先清除旧的7天数据
                        for (int i = 0; i < 7; i++) {
                            char key[20];
                            snprintf(key, sizeof(key), "day%d_date", i); preferences.remove(key);
                            snprintf(key, sizeof(key), "day%d_code", i); preferences.remove(key);
                            snprintf(key, sizeof(key), "day%d_max", i);  preferences.remove(key);
                            snprintf(key, sizeof(key), "day%d_min", i);  preferences.remove(key);
                        }

                        // 写入新数据
                        for (int i = 0; i < cnt; i++) {
                            const char* dateStr = timeArray[i]; // "2026-03-02"
                            int year, month, day;
                            sscanf(dateStr, "%d-%d-%d", &year, &month, &day);
                            char dateMMDD[6];
                            snprintf(dateMMDD, sizeof(dateMMDD), "%02d/%02d", month, day);

                            int code = codeArray[i];
                            float maxTemp = maxArray[i];
                            float minTemp = minArray[i];

                            char key[20];
                            snprintf(key, sizeof(key), "day%d_date", i); preferences.putString(key, dateMMDD);
                            snprintf(key, sizeof(key), "day%d_code", i); preferences.putInt(key, code);
                            snprintf(key, sizeof(key), "day%d_max", i);  preferences.putFloat(key, maxTemp);
                            snprintf(key, sizeof(key), "day%d_min", i);  preferences.putFloat(key, minTemp);
                        }

                        preferences.end();
                        xSemaphoreGive(weather_mutex);
                        success = true;

                        // 更新缓存
                        WeatherData today;
                        if (get_today_weather_from_prefs(today)) {
                            xSemaphoreTake(weather_mutex, portMAX_DELAY);
                            s_cached_weather = today;
                            xSemaphoreGive(weather_mutex);
                        } else {
                            // 如果今天不在范围内或时间超前，缓存无效
                            xSemaphoreTake(weather_mutex, portMAX_DELAY);
                            s_cached_weather.valid = false;
                            xSemaphoreGive(weather_mutex);
                        }
                    } else {
                        Serial.println("daily数组为空");
                    }
                } else {
                    Serial.print("JSON解析失败: ");
                    Serial.println(error.c_str());
                }
            } else {
                Serial.println("HTTP请求失败");
            }
            http.end();
        } else {
            Serial.println("HTTP begin 失败");
        }
        WiFi.disconnect();
        Serial.println("WiFi已断开");
    } else {
        Serial.println("WiFi 连接失败！");
    }

    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
    WiFi.end();   // 关闭并释放 WiFi 资源 (deinit), 回收内存

    if (!success) {
        // 失败，缓存无效
        xSemaphoreTake(weather_mutex, portMAX_DELAY);
        s_cached_weather.valid = false;
        xSemaphoreGive(weather_mutex);
    }

    // 通知UI更新
    weather_update_pending = true;

    weather_task_running = false;
    weather_task_handle = nullptr;
    vTaskDelete(NULL);
}
void Get_weather() {
    if (weather_task_running) return; // 正在获取中

    // 显示加载动画
    lv_obj_clear_flag(weather_loading_arc, LV_OBJ_FLAG_HIDDEN);
    lv_anim_init(&weather_arc_anim);
    lv_anim_set_var(&weather_arc_anim, weather_loading_arc);
    lv_anim_set_exec_cb(&weather_arc_anim, [](void* obj, int32_t v) {
        lv_arc_set_value((lv_obj_t*)obj, v);
    });
    lv_anim_set_time(&weather_arc_anim, 1000);
    lv_anim_set_repeat_count(&weather_arc_anim, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_values(&weather_arc_anim, 0, 100);
    lv_anim_start(&weather_arc_anim);

    // 隐藏内容
    lv_obj_add_flag(weather_img, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(weather_temp_label, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(weather_time_label, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(weather_placeholder_btn, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(weather_desc_label, LV_OBJ_FLAG_HIDDEN);

    // 启动天气获取任务 (CPU0)
    xTaskCreatePinnedToCore(weather_task, "WeatherTask", 8192, NULL, 1, &weather_task_handle, 0);
}
static void weather_click_cb(lv_event_t* e) {
    if (atomic_load_bool(&is_fullscreen_container_active)) return;
    if (weather_task_running) return; // 正在获取中
    lv_obj_t* target = lv_event_get_target(e);
    lv_area_t coords;
    lv_obj_get_coords(target, &coords);
    g_last_click_center.x = (coords.x1 + coords.x2) / 2;
    g_last_click_center.y = (coords.y1 + coords.y2) / 2;
    // 如果缓存有效
    if (s_cached_weather.valid) {
        create_fullscreen_container(FS_WEATHER);
        return;
    }

    // 显示加载动画
    lv_obj_clear_flag(weather_loading_arc, LV_OBJ_FLAG_HIDDEN);
    lv_anim_init(&weather_arc_anim);
    lv_anim_set_var(&weather_arc_anim, weather_loading_arc);
    lv_anim_set_exec_cb(&weather_arc_anim, [](void* obj, int32_t v) {
        lv_arc_set_value((lv_obj_t*)obj, v);
    });
    lv_anim_set_time(&weather_arc_anim, 1000);
    lv_anim_set_repeat_count(&weather_arc_anim, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_values(&weather_arc_anim, 0, 100);
    lv_anim_start(&weather_arc_anim);

    // 隐藏内容
    lv_obj_add_flag(weather_img, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(weather_temp_label, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(weather_time_label, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(weather_placeholder_btn, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(weather_desc_label, LV_OBJ_FLAG_HIDDEN);

    // 启动天气获取任务 (CPU0)
    xTaskCreatePinnedToCore(weather_task, "WeatherTask", 8192, NULL, 1, &weather_task_handle, 0);
}
static void create_weather_widget(lv_obj_t* parent) {
    if (weather_mutex == NULL) weather_mutex = xSemaphoreCreateMutex();
    // 创建容器
    weather_container = lv_obj_create(parent);
    lv_obj_set_size(weather_container, 135, 150);
    lv_obj_set_pos(weather_container, 95, 20);  
    lv_obj_set_style_bg_opa(weather_container, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(weather_container, 0, 0);
    lv_obj_clear_flag(weather_container, LV_OBJ_FLAG_SCROLLABLE);

    // 加载动画 
    weather_loading_arc = lv_arc_create(weather_container);
    lv_arc_set_rotation(weather_loading_arc, 270);
    lv_arc_set_bg_angles(weather_loading_arc, 0, 360);
    lv_obj_remove_style(weather_loading_arc, NULL, LV_PART_KNOB);
    lv_obj_clear_flag(weather_loading_arc, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(weather_loading_arc, 80, 80);
    lv_obj_set_pos(weather_loading_arc, 35, 10);  
    lv_obj_add_flag(weather_loading_arc, LV_OBJ_FLAG_HIDDEN);

    // 图片 
    weather_img = lv_img_create(weather_container);
    lv_img_set_src(weather_img, NULL);
    lv_obj_set_size(weather_img, 100, 100);
    lv_obj_set_pos(weather_img, 25, 0);  
    lv_obj_add_flag(weather_img, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(weather_img, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(weather_img, weather_click_cb, LV_EVENT_CLICKED, NULL);
    // 初始化图片描述符
    init_weather_images();

    // 温度标签 v 
    weather_temp_label = lv_label_create(weather_container);
    lv_label_set_text(weather_temp_label, "");
    lv_obj_set_style_text_color(weather_temp_label, lv_color_white(), 0);
    lv_obj_set_style_text_font(weather_temp_label, &lv_font_montserrat_16, 0);
    lv_obj_set_style_bg_color(weather_temp_label, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(weather_temp_label, LV_OPA_50, 0);
    lv_obj_set_style_pad_all(weather_temp_label, 2, 0);
    lv_obj_set_pos(weather_temp_label, 55, 100); 
    lv_obj_add_flag(weather_temp_label, LV_OBJ_FLAG_HIDDEN);

    // 更新时间标签
    weather_time_label = lv_label_create(weather_container);
    lv_label_set_text(weather_time_label, "");
    lv_obj_set_style_text_color(weather_time_label, lv_color_white(), 0);
    lv_obj_set_style_text_font(weather_time_label, &lv_font_montserrat_12, 0);
    lv_obj_set_style_bg_color(weather_time_label, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(weather_time_label, LV_OPA_50, 0);
    lv_obj_set_style_pad_all(weather_time_label, 2, 0);
    lv_obj_set_pos(weather_time_label, 65, 120); 
    lv_obj_add_flag(weather_time_label, LV_OBJ_FLAG_HIDDEN);

    // 中文描述标签
    weather_desc_label = lv_label_create(weather_container);
    lv_obj_set_width(weather_desc_label, 26); 
    lv_label_set_long_mode(weather_desc_label, LV_LABEL_LONG_WRAP);
    lv_label_set_text(weather_desc_label, "");
    lv_obj_set_style_text_color(weather_desc_label, lv_color_white(), 0);
    lv_obj_set_style_text_font(weather_desc_label, &chinese_24, 0);
    lv_obj_set_style_bg_color(weather_desc_label, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(weather_desc_label, LV_OPA_50, 0);
    lv_obj_set_style_pad_all(weather_desc_label, 2, 0);
    lv_obj_set_pos(weather_desc_label, 0, 0);  
    lv_obj_add_flag(weather_desc_label, LV_OBJ_FLAG_HIDDEN);

    // “点击获取天气”按钮
    weather_placeholder_btn = lv_btn_create(weather_container);
    lv_obj_set_size(weather_placeholder_btn, 100,100);
    lv_obj_set_pos(weather_placeholder_btn, 0, 0); 
    lv_obj_set_style_bg_color(weather_placeholder_btn, lv_color_hex(0x444444), 0);
    lv_obj_add_event_cb(weather_placeholder_btn, weather_click_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t* label = lv_label_create(weather_placeholder_btn);
    lv_label_set_text(label, "点击\n获取天气");
    lv_obj_set_style_text_font(label, &chinese_24, LV_STATE_DEFAULT);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(label);


    // 读取 Preferences 中的缓存
    if (get_today_weather_from_prefs(s_cached_weather)) {
        // 显示缓存数据
        lv_img_set_src(weather_img, &weather_img_dsc[s_cached_weather.code_index]);
        lv_obj_clear_flag(weather_img, LV_OBJ_FLAG_HIDDEN);

        char temp_buf[32];
        snprintf(temp_buf, sizeof(temp_buf), "%.0f/%.0f°C", s_cached_weather.temperature_max, s_cached_weather.temperature_min);
        lv_label_set_text(weather_temp_label, temp_buf);
        lv_obj_clear_flag(weather_temp_label, LV_OBJ_FLAG_HIDDEN);

        lv_label_set_text(weather_time_label, s_cached_weather.update_time);
        lv_obj_clear_flag(weather_time_label, LV_OBJ_FLAG_HIDDEN);

        const char* desc = wmo_code_to_desc_cn(s_cached_weather.wmo_code);
        lv_label_set_text(weather_desc_label, desc);
        lv_obj_clear_flag(weather_desc_label, LV_OBJ_FLAG_HIDDEN);

        lv_obj_add_flag(weather_placeholder_btn, LV_OBJ_FLAG_HIDDEN);
    } else {
        // 无缓存或失效，显示按钮
        s_cached_weather.valid = false;
        lv_obj_clear_flag(weather_placeholder_btn, LV_OBJ_FLAG_HIDDEN);
    }
   

    // 创建互斥量和 UI 更新定时器
    weather_ui_timer = lv_timer_create([](lv_timer_t* t) {
        if (weather_update_pending) {
            weather_update_pending = false;
            if (s_cached_weather.valid) {
                lv_img_set_src(weather_img, &weather_img_dsc[s_cached_weather.code_index]);
                lv_obj_clear_flag(weather_img, LV_OBJ_FLAG_HIDDEN);

                char temp_buf[32];
                snprintf(temp_buf, sizeof(temp_buf), "%.0f/%.0f°C", s_cached_weather.temperature_max, s_cached_weather.temperature_min);
                lv_label_set_text(weather_temp_label, temp_buf);
                lv_obj_clear_flag(weather_temp_label, LV_OBJ_FLAG_HIDDEN);

                lv_label_set_text(weather_time_label, s_cached_weather.update_time);
                lv_obj_clear_flag(weather_time_label, LV_OBJ_FLAG_HIDDEN);

                const char* desc = wmo_code_to_desc_cn(s_cached_weather.wmo_code);
                lv_label_set_text(weather_desc_label, desc);
                lv_obj_clear_flag(weather_desc_label, LV_OBJ_FLAG_HIDDEN);

                lv_obj_add_flag(weather_placeholder_btn, LV_OBJ_FLAG_HIDDEN);
            } else {
                // 获取失败，显示按钮
                lv_obj_clear_flag(weather_placeholder_btn, LV_OBJ_FLAG_HIDDEN);
                lv_obj_add_flag(weather_img, LV_OBJ_FLAG_HIDDEN);
                lv_obj_add_flag(weather_temp_label, LV_OBJ_FLAG_HIDDEN);
                lv_obj_add_flag(weather_time_label, LV_OBJ_FLAG_HIDDEN);
                lv_obj_add_flag(weather_desc_label, LV_OBJ_FLAG_HIDDEN);
            }
            // 隐藏加载动画
            lv_anim_del(weather_loading_arc, NULL);
            lv_obj_add_flag(weather_loading_arc, LV_OBJ_FLAG_HIDDEN);
        }
    }, 200, NULL);
}

//亮度调节滑动条
static void brightness_slider_event_cb(lv_event_t* e) {
    lv_obj_t* slider = lv_event_get_target(e);
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_VALUE_CHANGED) {
        uint8_t brightness = lv_slider_get_value(slider);
        set_mapped_brightness(brightness);
        atomic_store_int(&current_brightness, brightness);
    }
    else if (code == LV_EVENT_RELEASED) {
        uint8_t brightness = lv_slider_get_value(slider);
        preferences.begin("watch", false);
        preferences.putUChar("brightness", brightness);
        preferences.end();
    }
}
//滑动容器动画
static void adsorb_anim_cb(void* var, int32_t v) {
    lv_obj_set_y((lv_obj_t*)var, v);
}

static void do_adsorb(void) {
    int32_t curr_y = lv_obj_get_y(sliding_container);
    int32_t target_y = curr_y;
    int32_t total_move_range = container_max_y - container_min_y;
    float expand_percent = (float)(curr_y - container_min_y) / total_move_range;

    const float EXPAND_THRESHOLD = 0.1f;   // 展开阈值：>10% 触发展开
    const float COLLAPSE_THRESHOLD = 0.9f; // 收缩阈值：<90% 触发收缩

    if (is_sliding_container_expanded) {
        // 当前是展开态：<90% 就收缩
        if (expand_percent < COLLAPSE_THRESHOLD) {
            target_y = container_min_y;  // 收缩
            is_sliding_container_expanded = false;  // 更新状态为收缩
        } else {
            target_y = container_max_y;  // 保持展开
        }
    } else {
        // 当前是收缩态：>10% 就展开
        if (expand_percent > EXPAND_THRESHOLD) {
            target_y = container_max_y;  // 展开
            is_sliding_container_expanded = true;  // 更新状态为展开
        } else {
            target_y = container_min_y;  // 保持收缩
        }
    }

    // 执行吸附动画
    lv_anim_t anim;
    lv_anim_init(&anim);
    lv_anim_set_var(&anim, sliding_container);
    lv_anim_set_exec_cb(&anim, adsorb_anim_cb);
    lv_anim_set_time(&anim, 200);
    lv_anim_set_values(&anim, curr_y, target_y);
    lv_anim_start(&anim);
}



static void container_touch_event_cb(lv_event_t* e) {
    lv_event_code_t code = lv_event_get_code(e);
    lv_obj_t* cont = lv_event_get_target(e);
    lv_indev_t* indev = lv_indev_get_act();
    lv_point_t touch_point;
    lv_indev_get_point(indev, &touch_point);

    if (main_sleep_timer) lv_timer_reset(main_sleep_timer);

    if (code == LV_EVENT_PRESSED) {
        last_touch_y = touch_point.y;
        lv_anim_del(cont, adsorb_anim_cb);
    } else if (code == LV_EVENT_PRESSING) {
        int32_t delta_y = touch_point.y - last_touch_y;
        last_touch_y = touch_point.y;
        int32_t new_y = lv_obj_get_y(cont) + delta_y;
        if (new_y < container_min_y) new_y = container_min_y;
        if (new_y > container_max_y) new_y = container_max_y;
        lv_obj_set_pos(cont, container_x, new_y);
    } else if (code == LV_EVENT_RELEASED) {
        do_adsorb();
    }
}

static void create_sliding_container() {
    sliding_container = lv_obj_create(lv_scr_act());
    lv_obj_set_size(sliding_container, container_w, container_h);
    lv_obj_set_style_bg_opa(sliding_container, LV_OPA_TRANSP, LV_STATE_DEFAULT);
    lv_obj_set_style_radius(sliding_container, 8, LV_STATE_DEFAULT);
    lv_obj_set_style_border_opa(sliding_container, LV_OPA_TRANSP, LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(sliding_container, 0, LV_STATE_DEFAULT);
    lv_obj_set_scrollbar_mode(sliding_container, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_scroll_dir(sliding_container, LV_DIR_NONE);

    // 背景矩形
    lv_obj_t* big_rect = lv_obj_create(sliding_container);
    lv_obj_set_size(big_rect, 215, 90);
    lv_obj_set_pos(big_rect, 0, 0);
    lv_obj_set_style_bg_color(big_rect, lv_color_make(200,200,200), LV_STATE_DEFAULT);
    lv_obj_set_style_radius(big_rect, 12, LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(big_rect, 0, LV_STATE_DEFAULT);
    lv_obj_clear_flag(big_rect, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t* small_rect = lv_obj_create(sliding_container);
    lv_obj_set_size(small_rect, 100, 8);
    lv_obj_set_pos(small_rect, 55, 91);
    lv_obj_set_style_bg_color(small_rect, lv_color_make(150,150,150), LV_STATE_DEFAULT);
    lv_obj_set_style_radius(small_rect, 4, LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(small_rect, 0, LV_STATE_DEFAULT);
    lv_obj_clear_flag(small_rect, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t* slider = lv_slider_create(sliding_container);
    lv_obj_set_size(slider, 165, 20);
    lv_obj_center(slider);
    lv_slider_set_range(slider, 1, 255);
    preferences.begin("watch", true);
    uint8_t saved_brightness = preferences.getUChar("brightness", 128); // 默认128
    preferences.end();
    lv_slider_set_value(slider, saved_brightness, LV_ANIM_OFF);

    lv_obj_set_style_bg_color(slider, lv_color_make(173,216,230), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(slider, lv_color_make(0,122,255), LV_PART_INDICATOR | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(slider, lv_color_make(0,80,180), LV_PART_KNOB | LV_STATE_DEFAULT);
    lv_obj_set_style_radius(slider, 10, LV_PART_KNOB | LV_STATE_DEFAULT);
    lv_obj_set_style_radius(slider, 10, LV_PART_INDICATOR | LV_STATE_DEFAULT);

    lv_obj_add_event_cb(slider, brightness_slider_event_cb, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(slider, brightness_slider_event_cb, LV_EVENT_RELEASED, NULL);

    lv_obj_set_pos(sliding_container, container_x, container_min_y);
    lv_obj_add_event_cb(sliding_container, container_touch_event_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(sliding_container, container_touch_event_cb, LV_EVENT_PRESSING, NULL);
    lv_obj_add_event_cb(sliding_container, container_touch_event_cb, LV_EVENT_RELEASED, NULL);
    lv_obj_clear_flag(sliding_container, LV_OBJ_FLAG_CLICK_FOCUSABLE);
    lv_obj_move_foreground(sliding_container);
    is_sliding_container_expanded = false;
}

//提示容器
static void tip_adsorb_anim_cb(void* var, int32_t v) {
    lv_obj_set_y((lv_obj_t*)var, v);
}

static void tip_container_delete_self() {
    stop_tip_timer();
    if (tip_container) {
        lv_obj_del(tip_container);
        tip_container = nullptr;
        tip_label = nullptr;
        is_tip_container_active = false;
    }
}

static void tip_timer_cb(lv_timer_t* timer) {
    hide_tip_container();
    tip_timer = nullptr;
}

static void start_tip_timer(void) {
    stop_tip_timer();
    tip_timer = lv_timer_create(tip_timer_cb, tip_timer_interval, NULL);
}

static void stop_tip_timer(void) {
    if (tip_timer) {
        lv_timer_del(tip_timer);
        tip_timer = nullptr;
    }
}

static void reset_tip_timer(void) {
    if (tip_timer) lv_timer_reset(tip_timer);
}

static void create_tip_container(const char* tip_text) {
    if (tip_container) lv_obj_del(tip_container);

    tip_container = lv_obj_create(lv_layer_top());
    lv_obj_set_size(tip_container, container_w, container_h);
    lv_obj_set_style_bg_opa(tip_container, LV_OPA_TRANSP, LV_STATE_DEFAULT);
    lv_obj_set_style_border_opa(tip_container, LV_OPA_TRANSP, LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(tip_container, 0, LV_STATE_DEFAULT);
    lv_obj_set_scrollbar_mode(tip_container, LV_SCROLLBAR_MODE_OFF);

    lv_obj_t* big_rect = lv_obj_create(tip_container);
    lv_obj_set_size(big_rect, 215, 90);
    lv_obj_set_pos(big_rect, 0, 0);
    lv_obj_set_style_bg_color(big_rect, lv_color_make(200,200,200), LV_STATE_DEFAULT);
    lv_obj_set_style_radius(big_rect, 12, LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(big_rect, 0, LV_STATE_DEFAULT);
    lv_obj_clear_flag(big_rect, LV_OBJ_FLAG_CLICKABLE);

    tip_label = lv_label_create(tip_container);
    lv_label_set_text(tip_label, tip_text ? tip_text : "提示信息");
    lv_obj_set_style_text_font(tip_label, &tip_display_35, LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(tip_label, lv_color_black(), LV_STATE_DEFAULT);
    lv_obj_set_style_text_align(tip_label, LV_TEXT_ALIGN_CENTER, LV_STATE_DEFAULT);
    lv_obj_set_width(tip_label, 200);
    lv_obj_center(tip_label);
    lv_label_set_long_mode(tip_label, LV_LABEL_LONG_WRAP);

    lv_obj_set_pos(tip_container, container_x, tip_container_hidden_y);
    lv_obj_clear_flag(tip_container, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(tip_container, LV_OBJ_FLAG_CLICK_FOCUSABLE);
    is_tip_container_active = true;
}

//提示函数
void ui_set_tip_text(const char* tip_text) {
    if (tip_container && tip_label) {
        lv_label_set_text(tip_label, tip_text);
        reset_tip_timer();
    }
}

void show_tip_container(const char* tip_text) {
    Serial.println(2);
    Serial.println(millis());
    //第一次退出全屏后的 1 秒冷却期内，不响应任何提示
    if (first_exit_occurred && (millis() - first_exit_time < 1000)) {
        return;
    }
    if (tip_container) {
        ui_set_tip_text(tip_text);
        return;
    }
    create_tip_container(tip_text);
    lv_obj_move_foreground(tip_container);

    lv_anim_t anim;
    lv_anim_init(&anim);
    lv_anim_set_var(&anim, tip_container);
    lv_anim_set_exec_cb(&anim, tip_adsorb_anim_cb);
    lv_anim_set_time(&anim, tip_container_anim_time);
    lv_anim_set_values(&anim, tip_container_hidden_y, tip_container_show_y);
    lv_anim_start(&anim);

    start_tip_timer();
}

void hide_tip_container(void) {
    if (!tip_container || !is_tip_container_active) return;
    stop_tip_timer();

    lv_anim_t anim;
    lv_anim_init(&anim);
    lv_anim_set_var(&anim, tip_container);
    lv_anim_set_exec_cb(&anim, tip_adsorb_anim_cb);
    lv_anim_set_time(&anim, tip_container_anim_time);
    lv_anim_set_values(&anim, tip_container_show_y, tip_container_hidden_y);
    lv_anim_set_ready_cb(&anim, [](lv_anim_t*) { tip_container_delete_self(); });
    lv_anim_start(&anim);
}

//SD卡状态函数
bool ui_is_sd_card_initialized(void) {
    bool result = false;
    if (sd_state_mutex && xSemaphoreTake(sd_state_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        result = sd_card_initialized;
        xSemaphoreGive(sd_state_mutex);
    }
    return result;
}

void ui_reset_sd_init_event(void) {
    if (sd_state_mutex && xSemaphoreTake(sd_state_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        sd_card_init_event = false;
        xSemaphoreGive(sd_state_mutex);
    }
}

void ui_process_scan_queue(void) {
    static bool is_scanning = false;
    static uint32_t last_scan_update = 0;

    bool scan_val = false;
    if (sd_state_mutex && xSemaphoreTake(sd_state_mutex, 0) == pdTRUE) {
        scan_val = sd_card_scan_event;
        xSemaphoreGive(sd_state_mutex);
    }

    if (!scan_val && is_scanning) {
        if (scan_queue) xQueueReset(scan_queue);
        is_scanning = false;
        return;
    }

    if (scan_val) {
        if (!is_scanning) is_scanning = true;

        uint32_t now = millis();
        if (now - last_scan_update < 10) return;
        last_scan_update = now;

        if (scan_queue) {
            ScanData scan_data, latest_data;
            latest_data.category_index = -1;
            while (xQueueReceive(scan_queue, &scan_data, 0) == pdTRUE) {
                latest_data = scan_data;
            }
            if (latest_data.category_index != -1) {
                char text[64];
                if (latest_data.category_index == -1) {
                    snprintf(text, sizeof(text), "%s", latest_data.category_name);
                } else {
                    snprintf(text, sizeof(text), "扫描中\n%s:%d",
                             latest_data.category_name, latest_data.current_count);
                }
                show_tip_container(text);
            }
        }
    }
}

void ui_check_sd_card_event(void) {
    bool event_val = false, scan_val = false, warming_val = false;
    int insert_event = 0;
    static bool last_scan_val = false;
    static bool last_warming_val = false;

    if (sd_state_mutex && xSemaphoreTake(sd_state_mutex, 0) == pdTRUE) {
        event_val = sd_card_init_event;
        scan_val = sd_card_scan_event;
        insert_event = sd_card_insertion_event;
        warming_val = sd_card_warming;
        xSemaphoreGive(sd_state_mutex);
    }

    // 插入/拔出事件处理
    if (insert_event == 1) {
        show_tip_container("SD卡已插入");
        if (sd_state_mutex) xSemaphoreTake(sd_state_mutex, 0);
        sd_card_insertion_event = 0;
        if (sd_state_mutex) xSemaphoreGive(sd_state_mutex);
    } else if (insert_event == -1) {
        show_tip_container("SD卡已拔出");
        if (sd_state_mutex) xSemaphoreTake(sd_state_mutex, 0);
        sd_card_insertion_event = 0;
        if (sd_state_mutex) xSemaphoreGive(sd_state_mutex);
    }

    // 检测预热状态变化
    if (warming_val && !last_warming_val) {
        // 预热开始
        show_tip_container("SD卡准备中");
    } else if (!warming_val && last_warming_val) {
        // 预热结束
        show_tip_container("SD卡已就绪");
    }
    last_warming_val = warming_val;

    // 初始化失败事件
    if (event_val && !scan_val) {
        show_tip_container("SD卡初始化失败");
        ui_reset_sd_init_event();
    }

    // 扫描结束提示
    if (last_scan_val && !scan_val) show_tip_container("扫描完成");
    if (scan_val) ui_process_scan_queue();
    last_scan_val = scan_val;
}

// 判断点是否在圆角矩形内
static bool is_point_in_rounded_rect(int32_t x, int32_t y, int32_t w, int32_t h, int32_t r) {
    if (x < 0 || x >= w || y < 0 || y >= h) return false;
    // 中间水平区域
    if (y >= r && y < h - r) return true;
    // 中间垂直区域）
    if (x >= r && x < w - r) return true;
    // 检查四个圆角区域
    int32_t dx, dy;
    if (x < r && y < r) { // 左上角
        dx = x - r;
        dy = y - r;
    } else if (x >= w - r && y < r) { // 右上角
        dx = x - (w - r - 1);
        dy = y - r;
    } else if (x < r && y >= h - r) { // 左下角
        dx = x - r;
        dy = y - (h - r - 1);
    } else if (x >= w - r && y >= h - r) { // 右下角
        dx = x - (w - r - 1);
        dy = y - (h - r - 1);
    } else {
        return true; // 其他情况
    }
    return (dx * dx + dy * dy) <= r * r;
}
// --------------------------------------------------------------
static void draw_scaled_snapshot(int32_t center_x, int32_t center_y, float scale) {
    if (!fs_anim_buffers_valid || !bg_buf) return;

    // 背景拷贝到画布
    memcpy(canvas_buf, bg_buf, fs_container_w * fs_container_h * sizeof(lv_color_t));

    //计算缩放后的目标尺寸
    int32_t tgt_w = (int32_t)(fs_container_w * scale);
    int32_t tgt_h = (int32_t)(fs_container_h * scale);
    int32_t x_start = center_x - tgt_w / 2;
    int32_t y_start = center_y - tgt_h / 2;

    //裁剪绘制区域
    int32_t draw_x_start = LV_MAX(x_start, 0);
    int32_t draw_y_start = LV_MAX(y_start, 0);
    int32_t draw_x_end = LV_MIN(x_start + tgt_w, fs_container_w);
    int32_t draw_y_end = LV_MIN(y_start + tgt_h, fs_container_h);

    float inv_scale = 1.0f / scale;
    int32_t corner_r = (int32_t)(48.0f * scale);
    int32_t max_r = LV_MIN(tgt_w, tgt_h) / 2;
    if (corner_r > max_r) corner_r = max_r;

    //绘制缩放后的截图
    for (int32_t y = draw_y_start; y < draw_y_end; y++) {
        int32_t src_y = (int32_t)((y - y_start) * inv_scale);
        if (src_y < 0) src_y = 0;
        if (src_y >= fs_container_h) src_y = fs_container_h - 1;

        uint32_t src_row_offset = src_y * fs_container_w;
        uint32_t dst_row_offset = y * fs_container_w;

        for (int32_t x = draw_x_start; x < draw_x_end; x++) {
            int32_t local_x = x - x_start;
            int32_t local_y = y - y_start;

            if (!is_point_in_rounded_rect(local_x, local_y, tgt_w, tgt_h, corner_r)) {
                continue;  // 保持背景
            }

            int32_t src_x = (int32_t)((x - x_start) * inv_scale);
            if (src_x < 0) src_x = 0;
            if (src_x >= fs_container_w) src_x = fs_container_w - 1;

            canvas_buf[dst_row_offset + x] = snapshot_buf[src_row_offset + src_x];
        }
    }

    lv_obj_invalidate(anim_canvas);
}
static int32_t cubic_ease_out_cb(const lv_anim_t* a) {
    float t = (float)a->act_time / a->time;
    if (t > 1.0f) t = 1.0f;

    float t_inv = 1.0f - t;
    float progress = 1.0f - (t_inv * t_inv * t_inv);

    return a->start_value + (int32_t)((a->end_value - a->start_value) * progress);
}
// 缩放动画的回调执行函数
static void fs_scale_anim_cb(void* var, int32_t v) {
    float t = v / 255.0f; // t 范围 0.0 ~ 1.0

    // 位置线性插值
    int32_t center_x = g_anim_start_center.x + (int32_t)((g_anim_end_center.x - g_anim_start_center.x) * t);
    int32_t center_y = g_anim_start_center.y + (int32_t)((g_anim_end_center.y - g_anim_start_center.y) * t);

    //指数插值
    float scale = 0.2f * powf(5.0f, t);

    draw_scaled_snapshot(center_x, center_y, scale);
}
// 退出动画结束后的收尾工作
static void fs_exit_anim_ready_cb(lv_anim_t* anim) {
    if (anim_canvas) {
        lv_obj_add_flag(anim_canvas, LV_OBJ_FLAG_HIDDEN);
    }
    if (fullscreen_container) {
        lv_obj_del(fullscreen_container);
        fullscreen_container = NULL;
        atomic_store_bool(&is_fullscreen_container_active, false);

        // 恢复主界面显示
        lv_obj_clear_flag(main_container, LV_OBJ_FLAG_HIDDEN);
        restore_main_ui_zorder();
        if (sliding_container) lv_obj_move_foreground(sliding_container);

        start_main_sleep_timer();

        if (!first_exit_occurred) {
            first_exit_occurred = true;
            first_exit_time = millis();
        }
    }
}

void fs_do_exit(void) {
    if (!fullscreen_container) return;

    if (fs_anim_buffers_valid) {
        //确保主界面可见并更新布局
        lv_obj_clear_flag(main_container, LV_OBJ_FLAG_HIDDEN);
        lv_obj_update_layout(main_container);

        //重新截取主界面到 bg_buf
        lv_img_dsc_t* bg_dsc = lv_snapshot_take(main_container, LV_IMG_CF_TRUE_COLOR);
        if (bg_dsc && bg_dsc->data) {
            memcpy(bg_buf, bg_dsc->data, FS_BUF_SIZE);
            lv_snapshot_free(bg_dsc);
        } else {
            memset(bg_buf, 0, FS_BUF_SIZE); // 截图失败清空
        }

        // 隐藏主界面
        lv_obj_add_flag(main_container, LV_OBJ_FLAG_HIDDEN);

        g_anim_start_center = g_last_click_center;   // 起点为图标中心
        g_anim_end_center.x = 120;                    // 终点为屏幕中心
        g_anim_end_center.y = 140;
        lv_obj_add_flag(main_container, LV_OBJ_FLAG_HIDDEN);

        // 抓取当前全屏容器截图到 snapshot_buf
        lv_img_dsc_t* dsc = lv_snapshot_take(fullscreen_container, LV_IMG_CF_TRUE_COLOR);
        if (dsc && dsc->data) {
            memcpy(snapshot_buf, dsc->data, FS_BUF_SIZE);
            lv_snapshot_free(dsc);
        }

        lv_obj_clear_flag(anim_canvas, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(anim_canvas);

        lv_obj_add_flag(fullscreen_container, LV_OBJ_FLAG_HIDDEN);

        // 启动缩小动画
        lv_anim_t anim;
        lv_anim_init(&anim);
        lv_anim_set_var(&anim, anim_canvas);
        lv_anim_set_exec_cb(&anim, fs_scale_anim_cb);
        lv_anim_set_time(&anim, 300);
        lv_anim_set_values(&anim, 255, 0);
        lv_anim_set_path_cb(&anim, cubic_ease_out_cb);
        lv_anim_set_ready_cb(&anim, fs_exit_anim_ready_cb);
        lv_anim_start(&anim);
    } else {
        // PSRAM不足时优雅降级：直接摧毁
        lv_obj_del(fullscreen_container);
        fullscreen_container = NULL;
        atomic_store_bool(&is_fullscreen_container_active, false);
        restore_main_ui_zorder();
        if (sliding_container) lv_obj_move_foreground(sliding_container);

        if (!first_exit_occurred) {
            first_exit_occurred = true;
            first_exit_time = millis();
            Serial.println("第一次退出全屏");
        }
    }
    start_main_sleep_timer();
}
static void fs_container_touch_event_cb(lv_event_t* e) {
    // 空实现，保留触摸响应
}
// 进入动画结束后的收尾工作
static void fs_entry_anim_ready_cb(lv_anim_t* anim) {
    if (anim_canvas) {
        lv_obj_add_flag(anim_canvas, LV_OBJ_FLAG_HIDDEN); // 隐藏画布
    }
    if (fullscreen_container) {
        lv_obj_clear_flag(fullscreen_container, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_y(fullscreen_container, 0); // 真正容器瞬移到屏幕中央
        lv_obj_move_foreground(fullscreen_container);
    }
}

static void create_fullscreen_container(uint32_t func_index) {
    if (func_index >= FS_FUNC_COUNT) return;

    stop_main_sleep_timer();
    if (fullscreen_container) {
        destroy_file_selection_list();
        lv_obj_del(fullscreen_container);
        fullscreen_container = NULL;
    }

    fullscreen_container = lv_obj_create(lv_scr_act());
    lv_obj_set_size(fullscreen_container, fs_container_w, fs_container_h);
    lv_obj_set_style_bg_color(fullscreen_container, lv_color_black(), LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(fullscreen_container, LV_OPA_COVER, LV_STATE_DEFAULT);
    lv_obj_set_style_radius(fullscreen_container, 0, LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(fullscreen_container, 0, LV_STATE_DEFAULT);
    lv_obj_set_scrollbar_mode(fullscreen_container, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_scroll_dir(fullscreen_container, LV_DIR_NONE);
    lv_obj_add_event_cb(fullscreen_container, fs_container_touch_event_cb, LV_EVENT_ALL, NULL);
    atomic_store_bool(&is_fullscreen_container_active, true);

    if (func_index < FS_FUNC_COUNT && fs_creators[func_index]) {
        fs_creators[func_index](fullscreen_container);
    }
    if (fs_anim_buffers_valid) {
        g_anim_start_center = g_last_click_center;
        g_anim_end_center.x = 120;
        g_anim_end_center.y = 140;
        lv_obj_clear_flag(main_container, LV_OBJ_FLAG_HIDDEN);
        lv_obj_update_layout(main_container);
        // 文件选择器在场时降低不透明度 (参考工程)
        if (g_file_selection_instance && g_file_selection_instance->view_container) {
            lv_obj_set_style_opa(g_file_selection_instance->view_container, 180, LV_PART_MAIN);
            lv_obj_update_layout(fullscreen_container);
        }

        lv_img_dsc_t* bg_dsc = lv_snapshot_take(main_container, LV_IMG_CF_TRUE_COLOR);
        if (bg_dsc && bg_dsc->data) {
            memcpy(bg_buf, bg_dsc->data, FS_BUF_SIZE);
            lv_snapshot_free(bg_dsc);
            } else {
                memset(bg_buf, 0, FS_BUF_SIZE);
        }

        lv_obj_add_flag(main_container, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_pos(fullscreen_container, 0, fs_container_hidden_y);

        lv_img_dsc_t* dsc = lv_snapshot_take(fullscreen_container, LV_IMG_CF_TRUE_COLOR);
        if (dsc && dsc->data) {
            memcpy(snapshot_buf, dsc->data, FS_BUF_SIZE);
            lv_snapshot_free(dsc);
        }

        // 显示动画画布并提到最前
        lv_obj_clear_flag(anim_canvas, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(anim_canvas);

        lv_obj_add_flag(fullscreen_container, LV_OBJ_FLAG_HIDDEN);

        // 创建放大进入动画
        lv_anim_t anim;
        lv_anim_init(&anim);
        lv_anim_set_var(&anim, anim_canvas);
        lv_anim_set_exec_cb(&anim, fs_scale_anim_cb);
        lv_anim_set_time(&anim, 300);
        lv_anim_set_values(&anim, 0, 255);
        lv_anim_set_path_cb(&anim, cubic_ease_out_cb);
        lv_anim_set_ready_cb(&anim, fs_entry_anim_ready_cb);
        lv_anim_start(&anim);
    } else {
        // PSRAM不足时：优雅降级，瞬移显示容器
        lv_obj_set_pos(fullscreen_container, 0, fs_container_show_y);
        lv_obj_move_foreground(fullscreen_container);
    }
}

static void restore_main_ui_zorder() {
    if (sliding_container) lv_obj_move_foreground(sliding_container);
    if (circle_container) lv_obj_move_foreground(circle_container);
    if (hour_label) lv_obj_move_foreground(hour_label);
    if (minute_label) lv_obj_move_foreground(minute_label);
    if (second_label) lv_obj_move_foreground(second_label);
}

static void create_time_container_without_anim() {
    stop_main_sleep_timer();
    if (fullscreen_container) {
        lv_obj_del(fullscreen_container);
        fullscreen_container = NULL;
    }
    fullscreen_container = lv_obj_create(lv_scr_act());
    lv_obj_set_size(fullscreen_container, fs_container_w, fs_container_h);
    lv_obj_set_style_bg_color(fullscreen_container, lv_color_black(), LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(fullscreen_container, LV_OPA_COVER, LV_STATE_DEFAULT);
    lv_obj_set_style_radius(fullscreen_container, 0, LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(fullscreen_container, 0, LV_STATE_DEFAULT);
    lv_obj_set_scrollbar_mode(fullscreen_container, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_scroll_dir(fullscreen_container, LV_DIR_NONE);
    lv_obj_set_pos(fullscreen_container, 0, fs_container_show_y);
    lv_obj_add_event_cb(fullscreen_container, fs_container_touch_event_cb, LV_EVENT_ALL, NULL);
    atomic_store_bool(&is_fullscreen_container_active, true);

    if (fs_creators[FS_TIME_SCREEN]) fs_creators[FS_TIME_SCREEN](fullscreen_container);
    lv_obj_move_foreground(fullscreen_container);
}

static void countdown_check_timer_cb(lv_timer_t* timer) {
    bool active = atomic_load_bool(&g_countdown_active);
    int remain = atomic_load_int(&g_countdown_remaining);

    if (active && remain <= 0) {
        show_tip_container("倒计时结束");
    }
}
static void create_battery_icon(lv_obj_t* parent) {
    battery_area = lv_obj_create(parent);
    lv_obj_set_size(battery_area, 24, 12);
    lv_obj_align(battery_area, LV_ALIGN_TOP_RIGHT, -20, 10);

    lv_obj_set_style_bg_opa(battery_area, LV_OPA_0, 0);
    lv_obj_set_style_border_width(battery_area, 1, 0);
    lv_obj_set_style_border_color(battery_area, lv_color_white(), 0);
    lv_obj_set_style_radius(battery_area, 1, 0);
    lv_obj_set_style_pad_all(battery_area, 0, 0);
    lv_obj_clear_flag(battery_area, LV_OBJ_FLAG_SCROLLABLE);

    // 电池正极小凸起
    battery_tip = lv_obj_create(parent);
    lv_obj_set_size(battery_tip, 2, 5);
    lv_obj_align_to(battery_tip, battery_area, LV_ALIGN_OUT_RIGHT_MID, 0, 0);
    lv_obj_set_style_bg_color(battery_tip, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(battery_tip, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(battery_tip, 0, 0);
    lv_obj_set_style_radius(battery_tip, 1, 0);

    battery_bar = lv_obj_create(battery_area);
    lv_obj_set_size(battery_bar, 0, lv_pct(100));
    lv_obj_set_align(battery_bar, LV_ALIGN_LEFT_MID);
    lv_obj_set_style_border_width(battery_bar, 0, 0);
    lv_obj_set_style_bg_opa(battery_bar, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(battery_bar, 0, 0);
    lv_obj_clear_flag(battery_bar, LV_OBJ_FLAG_SCROLLABLE);
    ui_update_battery();
    // 百分比标签
    battery_percent_label = lv_label_create(parent);
    lv_obj_set_style_text_font(battery_percent_label, &lv_font_montserrat_12, LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(battery_percent_label, lv_color_white(), LV_STATE_DEFAULT);
    lv_label_set_text(battery_percent_label, "100%");
    lv_obj_align_to(battery_percent_label, battery_area, LV_ALIGN_OUT_LEFT_MID, -2, 0);
}
static void ui_update_battery() {
    int pct = atomic_load_int(&battery_percentage);
    pct = constrain(pct, 0, 100);
    int bar_width = (pct * 22) / 100;
    lv_obj_set_width(battery_bar, bar_width);

    lv_color_t bat_color;
    if (pct <= 20)      bat_color = lv_palette_main(LV_PALETTE_RED);
    else if (pct <= 30) bat_color = lv_palette_main(LV_PALETTE_AMBER);
    else                bat_color = lv_palette_main(LV_PALETTE_GREEN);
    if (atomic_load_bool(&is_charging)) bat_color = lv_palette_main(LV_PALETTE_BLUE);
    lv_obj_set_style_bg_color(battery_bar, bat_color, LV_PART_MAIN);

    // 更新百分比标签
    if (battery_percent_label) {
        char buf[8];
        snprintf(buf, sizeof(buf), "%d%%", pct);
        lv_label_set_text(battery_percent_label, buf);
    }
}
static void main_loop_timer_cb(lv_timer_t* timer) {
    ui_update_uptime();
    ui_check_sd_card_event();
}

//UI初始化
void ui_init_all() {
    boot_time = millis();

    // 创建主容器，覆盖全屏，无边框透明背景
    main_container = lv_obj_create(lv_scr_act());
    lv_obj_set_size(main_container, 240, 280);
    lv_obj_set_pos(main_container, 0, 0);
    lv_obj_set_style_bg_opa(main_container, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(main_container, 0, 0);
    lv_obj_set_style_pad_all(main_container, 0, 0);
    lv_obj_clear_flag(main_container, LV_OBJ_FLAG_SCROLLABLE);

    init_menu_images();
    create_uptime_display(main_container);
    create_battery_icon(main_container);
    create_circle_list(main_container);
    create_weather_widget(main_container);

    // 滑动容器仍然创建在屏幕上
    create_sliding_container();

    if (sliding_container) lv_obj_move_foreground(sliding_container);

    // 创建时间全屏容器
    create_time_container_without_anim();

    lv_timer_create(countdown_check_timer_cb, 100, NULL);
    lv_timer_create(check_power_key_timer_cb, 50, NULL);
    lv_timer_create(main_loop_timer_cb, 100, NULL);

    // 初始化PSRAM截图缓存与画布
    canvas_buf = (lv_color_t*)heap_caps_malloc(FS_BUF_SIZE, MALLOC_CAP_SPIRAM);
    snapshot_buf = (lv_color_t*)heap_caps_malloc(FS_BUF_SIZE, MALLOC_CAP_SPIRAM);
    bg_buf = (lv_color_t*)heap_caps_malloc(FS_BUF_SIZE, MALLOC_CAP_SPIRAM);

    if (canvas_buf && snapshot_buf && bg_buf) {
        fs_anim_buffers_valid = true;
        anim_canvas = lv_canvas_create(lv_scr_act());
        lv_canvas_set_buffer(anim_canvas, canvas_buf, fs_container_w, fs_container_h, LV_IMG_CF_TRUE_COLOR_CHROMA_KEYED);
        lv_obj_add_flag(anim_canvas, LV_OBJ_FLAG_HIDDEN); // 初始状态下隐藏
        lv_obj_set_pos(anim_canvas, 0, 0);
    } else {
        Serial.println("内存不足：无法在PSRAM中分配用于缩放动画的缓存,退化为瞬间切换");
    }
}

/* 开机背光淡入 (定义于 app/boot_anim.cpp) */
extern "C" void boot_anim_start(void);

/* C 可调用入口: 初始化 LVGL + 主界面 + 启动 LVGL 任务 (供 app_main 使用) */
extern "C" void ui_start_all(void) {
    ui_core_display_init();
    ui_init_all();
    ui_core_start();
    /* 作为 UI 初始化的最后一个调用: 此时 UI 对象已建好、LVGL 任务已起,
     * 背光淡入不会早于首帧渲染 (对比原先在 main.c 里调用) */
    boot_anim_start();
}
