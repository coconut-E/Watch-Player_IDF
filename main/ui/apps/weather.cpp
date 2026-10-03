#include "ui_manager.h"
#include "fullscreen_interfaces.h"

#define ZOOM_FROM_SIZE(orig, target) ((uint16_t)((target) * 256U / (orig)))
static lv_timer_t *gpio6_timer = NULL;

// 读取所有7天天气数据
static int read_all_weather_days(char dates[7][6], int wmo_codes[7], float max_temps[7], float min_temps[7]) {
    preferences.begin("watch", true);
    int count = 0;
    for (int i = 0; i < 7; i++) {
        char key_date[16], key_code[16], key_max[16], key_min[16];
        snprintf(key_date, sizeof(key_date), "day%d_date", i);
        snprintf(key_code, sizeof(key_code), "day%d_code", i);
        snprintf(key_max, sizeof(key_max), "day%d_max", i);
        snprintf(key_min, sizeof(key_min), "day%d_min", i);
        if (preferences.isKey(key_date)) {
            String dateStr = preferences.getString(key_date, "");
            if (dateStr.length() > 0) {
                strlcpy(dates[count], dateStr.c_str(), 6);
                wmo_codes[count] = preferences.getInt(key_code, 0);
                max_temps[count] = preferences.getFloat(key_max, 0);
                min_temps[count] = preferences.getFloat(key_min, 0);
                count++;
            }
        } else { break; }
    }
    preferences.end();
    return count;
}

static void BUTTON2_check_cb(lv_timer_t *timer) {
    if(digitalRead(BUTTON_2)) {
        if(gpio6_timer) {
            lv_timer_del(gpio6_timer);
            gpio6_timer = NULL;
        }
        fs_do_exit();
    }
}

// 按钮点击回调
static void refresh_btn_cb(lv_event_t* e) {
    if (gpio6_timer) {
        lv_timer_del(gpio6_timer);
        gpio6_timer = NULL;
    }
    fs_do_exit();
    Get_weather();
}

// 坐标映射：110 ~ 240
static lv_coord_t get_temp_y(float temp, float min_t, float max_t) {
    if (max_t == min_t) return 175;
    return (lv_coord_t)(240 - ((temp - min_t) / (max_t - min_t) * 130));
}

void fs_create_weather(lv_obj_t* container) {
    //容器样式初始化
    lv_obj_set_style_border_width(container, 0, 0);
    lv_obj_set_style_pad_all(container, 0, 0);
    lv_obj_set_scroll_dir(container, LV_DIR_HOR);
    lv_obj_set_scrollbar_mode(container, LV_SCROLLBAR_MODE_OFF); 

    lv_obj_t* content = lv_obj_create(container);
    lv_obj_set_size(content, 750, 280); 
    lv_obj_set_style_bg_opa(content, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(content, 0, 0);
    lv_obj_set_style_pad_all(content, 0, 0);
    lv_obj_clear_flag(content, LV_OBJ_FLAG_SCROLLABLE);

    //数据准备
    char dates[7][6];
    int wmo_codes[7];
    float max_temps[7], min_temps[7];
    int day_count = read_all_weather_days(dates, wmo_codes, max_temps, min_temps);

    if (day_count == 0) {
        lv_obj_t* label = lv_label_create(content);
        lv_label_set_text(label, "No Data");
        lv_obj_center(label);
        return;
    }

    float g_max = -99, g_min = 99;
    for(int i=0; i<day_count; i++) {
        if(max_temps[i] > g_max) g_max = max_temps[i];
        if(min_temps[i] < g_min) g_min = min_temps[i];
    }
    g_max += 2; g_min -= 2;

    static lv_point_t high_points[7];
    static lv_point_t low_points[7];

    //循环创建 UI
    for (int i = 0; i < day_count; i++) {
        int x_center = i * 100 + 50; 

        // 图片
        int img_idx = weather_code_to_index(wmo_codes[i], 1);
        lv_obj_t* img = lv_img_create(content);
        lv_img_set_src(img, &weather_img_dsc[img_idx]);
        lv_img_set_zoom(img, ZOOM_FROM_SIZE(100, 55)); 
        lv_obj_set_pos(img, x_center - 50, -10); 

        // 描述
        lv_obj_t* desc_label = lv_label_create(content);
        lv_label_set_text(desc_label, wmo_code_to_desc_cn(wmo_codes[i]));
        lv_obj_set_style_text_font(desc_label, &chinese_24, 0);
        lv_obj_set_style_text_color(desc_label, lv_color_white(), 0);
        lv_obj_set_width(desc_label, 100); 
        lv_obj_set_style_text_align(desc_label, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_pos(desc_label, i * 100, 65);

        // 计算点
        high_points[i].x = x_center;
        high_points[i].y = get_temp_y(max_temps[i], g_min, g_max);
        low_points[i].x = x_center;
        low_points[i].y = get_temp_y(min_temps[i], g_min, g_max);

        // 高温数字
        lv_obj_t* t_max_lab = lv_label_create(content);
        lv_label_set_text_fmt(t_max_lab, "%d°", (int)max_temps[i]);
        lv_obj_set_style_text_color(t_max_lab, lv_color_make(255, 255, 180), 0); 
        lv_obj_set_style_text_font(t_max_lab, &lv_font_montserrat_14, 0);
        lv_obj_set_pos(t_max_lab, x_center - 12, high_points[i].y - 22);

        // 低温数字
        lv_obj_t* t_min_lab = lv_label_create(content);
        lv_label_set_text_fmt(t_min_lab, "%d°", (int)min_temps[i]);
        lv_obj_set_style_text_color(t_min_lab, lv_color_make(180, 220, 255), 0);
        lv_obj_set_style_text_font(t_min_lab, &lv_font_montserrat_14, 0);
        lv_obj_set_pos(t_min_lab, x_center - 12, low_points[i].y + 8);

        // 日期
        lv_obj_t* date_label = lv_label_create(content);
        lv_label_set_text(date_label, dates[i]);
        lv_obj_set_style_text_font(date_label, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(date_label, lv_color_hex(0xCCCCCC), 0);
        lv_obj_set_width(date_label, 100);
        lv_obj_set_style_text_align(date_label, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_pos(date_label, i * 100, 250);
    }

    //曲线绘制
    lv_obj_t* line_high = lv_line_create(content);
    lv_line_set_points(line_high, high_points, day_count);
    lv_obj_set_style_line_width(line_high, 3, 0);
    lv_obj_set_style_line_color(line_high, lv_color_make(255, 255, 150), 0);
    lv_obj_set_style_line_rounded(line_high, true, 0);

    lv_obj_t* line_low = lv_line_create(content);
    lv_line_set_points(line_low, low_points, day_count);
    lv_obj_set_style_line_width(line_low, 3, 0);
    lv_obj_set_style_line_color(line_low, lv_color_make(150, 200, 255), 0);
    lv_obj_set_style_line_rounded(line_low, true, 0);

    if(!gpio6_timer) gpio6_timer = lv_timer_create(BUTTON2_check_cb, 50, NULL);

    //添加重新获取按钮
    lv_obj_t* refresh_btn = lv_btn_create(content);
    lv_obj_set_size(refresh_btn, 60, 60);
    lv_obj_set_style_radius(refresh_btn, 10, LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(refresh_btn, lv_palette_main(LV_PALETTE_BLUE), LV_STATE_DEFAULT);
    lv_obj_set_pos(refresh_btn, 680, 100); 
    
    lv_obj_t* refresh_label = lv_label_create(refresh_btn);
    lv_label_set_text(refresh_label, LV_SYMBOL_REFRESH);
    lv_obj_set_style_text_font(refresh_label, &lv_font_montserrat_24, 0);
    lv_obj_center(refresh_label);

    lv_obj_add_event_cb(refresh_btn, refresh_btn_cb, LV_EVENT_CLICKED, NULL);
}