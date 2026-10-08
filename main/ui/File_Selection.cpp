#include "File_Selection.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

// 常量定义
#define CONTAINER_HEIGHT 30
#define CONTAINER_SPACING 10
#define SCROLL_THRESHOLD_TOP -40
#define SCROLL_THRESHOLD_BOTTOM 320
#define TELEPORT_OFFSET (CONTAINER_HEIGHT + CONTAINER_SPACING)
#define MIN_Y_DIFFERENCE (TELEPORT_OFFSET / 2)
#define DEFAULT_VALUE_THRESHOLD 50
#define MAX_FILE_COUNT 100
#define UPDATE_INTERVAL 5

// 弧形参数
#define CIRCLE_CENTER_X 420
#define CIRCLE_CENTER_Y 140
#define CIRCLE_RADIUS 300

FileSelectionInstance* g_file_selection_instance = NULL;

// 全局消息框对象
lv_obj_t* msgbox_bg = NULL;
lv_obj_t* msgbox = NULL;

// 外部声明
extern bool sd_force_scan;
extern SemaphoreHandle_t sd_state_mutex;
extern bool sd_card_initialized;  
// 退出回调函数声明
void fs_do_exit();


// 内部函数声明
static void container_click_cb(lv_event_t* e);
static void scroll_event_cb(lv_event_t* e);
static void update_timer_cb(lv_timer_t* timer);
static void load_sd_file_list(FileSelectionInstance* instance);
static int get_display_value(int actual_value, FileSelectionInstance* instance);
static void update_container_value(FileSelectionInstance* instance, int container_index, int new_actual_value);
static lv_color_t generate_rainbow_color(int index, FileSelectionInstance* instance);
static lv_obj_t* find_container_with_min_y(FileSelectionInstance* instance);
static lv_obj_t* find_container_with_max_y(FileSelectionInstance* instance);
static int find_container_with_min_value(FileSelectionInstance* instance);
static int find_container_with_max_value(FileSelectionInstance* instance);
static int32_t get_container_y(lv_obj_t* container);
static void set_container_y_with_arc(FileSelectionInstance* instance, lv_obj_t* container, int32_t y);
static int32_t calculate_arc_y(int32_t linear_y, int32_t scroll_y, int32_t container_height);
static void apply_arc_effects(lv_obj_t* container, int32_t screen_y, int32_t container_height);
static void check_and_adjust_containers(FileSelectionInstance* instance);
static void quick_check_containers(FileSelectionInstance* instance);
static void show_file_not_found_msgbox(void);
static void msgbox_btn_cb(lv_event_t * e);
static void fade_out_anim_cb(void* var, int32_t value);
static void fade_out_anim_complete_cb(lv_anim_t* a);

// 供外部调用的销毁函数
void destroy_file_selection_list(void) {
    if (g_file_selection_instance) {
        file_selection_destroy(g_file_selection_instance);
        g_file_selection_instance = NULL;
    }
}

// 保存用户选择的文件名
static void save_selected_file(file_type_t file_type, const char* filename) {
    if (!filename || strlen(filename) == 0) return;

    if (!preferences.begin("watch", false)) {
        printf("无法打开 Preferences 命名空间: watch\n");
        return;
    }

    char key[64];
    snprintf(key, sizeof(key), "selected_%s", get_file_type_folder_name(file_type));

    preferences.putString(key, filename);
    preferences.end(); 
    
    printf("历史选择已保存: %s -> %s (Namespace: watch)\n", filename, key);
}


// 读取上次选择的文件名
static bool load_selected_file(file_type_t file_type, char* filename, size_t max_len) {
    if (!preferences.begin("watch", true)) {
        return false;
    }

    char key[64];
    snprintf(key, sizeof(key), "selected_%s", get_file_type_folder_name(file_type));

    String value = preferences.getString(key, "");
    preferences.end(); 

    if (value.length() > 0) {
        strncpy(filename, value.c_str(), max_len - 1);
        filename[max_len - 1] = '\0';
        return true;
    }
    
    return false;
}


// 获取文件类型对应的文件名
const char* get_file_type_filename(file_type_t file_type) {
    switch(file_type) {
        case FILE_TYPE_VIDEO: return "video.txt";
        case FILE_TYPE_NOVEL: return "novel.txt"; 
        case FILE_TYPE_MUSIC: return "music.txt";
        case FILE_TYPE_COMIC: return "comic.txt";
        default: return "video.txt";
    }
}

// 提取不含后缀的文件名
void extract_file_name(const char* full_name, char* name, uint32_t max_len) {
    if (!full_name || !name || max_len == 0) return;
    strncpy(name, full_name, max_len - 1);
    name[max_len - 1] = '\0';
    char* dot = strrchr(name, '.');
    if (dot) *dot = '\0';
}

// 获取文件类型对应的文件夹名称
const char* get_file_type_folder_name(file_type_t file_type) {
    switch (file_type) {
        case FILE_TYPE_VIDEO: return "视频";
        case FILE_TYPE_NOVEL: return "小说";
        case FILE_TYPE_MUSIC: return "音乐";
        case FILE_TYPE_COMIC: return "漫画";
        default: return "视频";
    }
}

// 检查文件是否存在
bool check_file_exists(file_type_t file_type, const char* filename) {
    if (!filename || strlen(filename) == 0) return false;
    
    char full_path[MAX_FILE_PATH_LEN];
    const char* folder_name = get_file_type_folder_name(file_type);
    snprintf(full_path, sizeof(full_path), "/%s/%s", folder_name, filename);
    printf("检查文件是否存在: %s\n", full_path);
    
    FsFile file;
    if (!file.open(full_path, O_RDONLY)) {
        printf("文件不存在: %s\n", full_path);
        return false;
    }
    file.close();
    printf("文件存在: %s\n", full_path);
    return true;
}

// 强制清理文件选择器资源
void file_selection_force_clean(void) {
    if (g_file_selection_instance) {
        if (g_file_selection_instance->timer) {
            lv_timer_del(g_file_selection_instance->timer);
            g_file_selection_instance->timer = NULL;
        }
        lv_anim_del(g_file_selection_instance->view_container, NULL);
        
        if (g_file_selection_instance->scroll_content) {
            lv_obj_del(g_file_selection_instance->scroll_content);
            g_file_selection_instance->scroll_content = NULL;
        }
        if (g_file_selection_instance->view_container) {
            lv_obj_del(g_file_selection_instance->view_container);
            g_file_selection_instance->view_container = NULL;
        }
        
        if (g_file_selection_instance->file_list) {
            free(g_file_selection_instance->file_list);
            g_file_selection_instance->file_list = NULL;
        }
        if (g_file_selection_instance->containers) {
            free(g_file_selection_instance->containers);
            g_file_selection_instance->containers = NULL;
        }
        if (g_file_selection_instance->container_values) {
            free(g_file_selection_instance->container_values);
            g_file_selection_instance->container_values = NULL;
        }
        
        free(g_file_selection_instance);
        g_file_selection_instance = NULL;
    }
}

// 显示找不到文件的提示框
static void show_file_not_found_msgbox(void) {
    msgbox_bg = lv_obj_create(lv_scr_act());
    lv_obj_set_size(msgbox_bg, 240, 280);
    lv_obj_align(msgbox_bg, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(msgbox_bg, lv_color_black(), LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(msgbox_bg, LV_OPA_50, LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(msgbox_bg, 0, LV_STATE_DEFAULT);
    lv_obj_clear_flag(msgbox_bg, LV_OBJ_FLAG_SCROLLABLE);

    msgbox = lv_obj_create(msgbox_bg);
    lv_obj_set_size(msgbox, 200, 150);
    lv_obj_align(msgbox, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(msgbox, lv_color_make(50, 50, 50), LV_STATE_DEFAULT);
    lv_obj_set_style_border_color(msgbox, lv_color_white(), LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(msgbox, 2, LV_STATE_DEFAULT);
    lv_obj_set_style_radius(msgbox, 10, LV_STATE_DEFAULT);
    lv_obj_clear_flag(msgbox, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t * label = lv_label_create(msgbox);
    lv_label_set_text(label, "找不到该文件\n是否重新扫描");
    lv_obj_set_style_text_color(label, lv_color_white(), LV_STATE_DEFAULT);
    lv_obj_set_style_text_font(label, &chinese_24, LV_STATE_DEFAULT);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, LV_STATE_DEFAULT);
    lv_obj_align(label, LV_ALIGN_CENTER, 0, -25);
    lv_obj_clear_flag(label, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t * btn_cont = lv_obj_create(msgbox);
    lv_obj_set_size(btn_cont, 180, 40);
    lv_obj_align(btn_cont, LV_ALIGN_BOTTOM_MID, 0, -5);
    lv_obj_set_style_bg_opa(btn_cont, LV_OPA_TRANSP, LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(btn_cont, 0, LV_STATE_DEFAULT);
    lv_obj_set_flex_flow(btn_cont, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(btn_cont, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(btn_cont, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t * btn_yes = lv_btn_create(btn_cont);
    lv_obj_set_size(btn_yes, 70, 30);
    lv_obj_set_style_bg_color(btn_yes, lv_color_make(0, 100, 0), LV_STATE_DEFAULT);
    lv_obj_set_style_radius(btn_yes, 5, LV_STATE_DEFAULT);
    lv_obj_clear_flag(btn_yes, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t * label_yes = lv_label_create(btn_yes);
    lv_label_set_text(label_yes, "是");
    lv_obj_set_style_text_color(label_yes, lv_color_white(), LV_STATE_DEFAULT);
    lv_obj_set_style_text_font(label_yes, &chinese_24, LV_STATE_DEFAULT);
    lv_obj_center(label_yes);
    lv_obj_add_event_cb(btn_yes, msgbox_btn_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t * btn_no = lv_btn_create(btn_cont);
    lv_obj_set_size(btn_no, 70, 30);
    lv_obj_set_style_bg_color(btn_no, lv_color_make(100, 0, 0), LV_STATE_DEFAULT);
    lv_obj_set_style_radius(btn_no, 5, LV_STATE_DEFAULT);
    lv_obj_clear_flag(btn_no, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t * label_no = lv_label_create(btn_no);
    lv_label_set_text(label_no, "否");
    lv_obj_set_style_text_color(label_no, lv_color_white(), LV_STATE_DEFAULT);
    lv_obj_set_style_text_font(label_no, &chinese_24, LV_STATE_DEFAULT);
    lv_obj_center(label_no);
    lv_obj_add_event_cb(btn_no, msgbox_btn_cb, LV_EVENT_CLICKED, NULL);
}

// 消息框按钮点击回调
static void msgbox_btn_cb(lv_event_t * e) {
    lv_obj_t * btn = (lv_obj_t *)lv_event_get_target(e);
    lv_obj_t * label = lv_obj_get_child(btn, 0);
    const char * txt = lv_label_get_text(label);
    
    if (strcmp(txt, "是") == 0) {
        printf("用户选择重新扫描\n");
        // 获取文件选择器实例
        FileSelectionInstance* instance = g_file_selection_instance;
        if (instance && instance->callback) {
            // 调用上层回调，传递特殊文件名
            instance->callback(SCAN_SPECIAL_FILENAME, instance->callback_user_data);
        }
    } else if (strcmp(txt, "否") == 0) {
        printf("用户选择不重新扫描\n");
    }
    
    if (msgbox_bg) {
        lv_obj_del(msgbox_bg);
        msgbox_bg = NULL;
        msgbox = NULL;
    }
}
uint32_t read_sd_file_list(file_info_t* file_list, uint32_t max_count, file_type_t file_type) {
    if (!file_list || max_count == 0) return 0;
    
    uint32_t count = 0;
    FsFile file;
    char line[256];
    
    char filepath[128];
    const char* filename = get_file_type_filename(file_type);
    snprintf(filepath, sizeof(filepath), "/ScanList/%s", filename);
    
    printf("尝试打开文件: %s\n", filepath);
    
    if (!file.open(filepath, O_RDONLY)) {
        printf("无法打开文件: %s\n", filepath);
        return 0;
    }
    
    while (file.fgets(line, sizeof(line)) && count < max_count) {
        line[strcspn(line, "\r\n")] = 0;
        if (strlen(line) == 0) continue;
        if (strncmp(line, "FileCount:", 10) == 0) {
            printf("文件总数: %s\n", line);
            break;
        }
        const char* filename = line;
        extract_file_name(filename, file_list[count].name, MAX_FILE_NAME_LEN);
        strncpy(file_list[count].full_name, filename, MAX_FILE_NAME_LEN - 1);
        file_list[count].full_name[MAX_FILE_NAME_LEN - 1] = '\0';
        file_list[count].size = 0;
        count++;
    }
    
    file.close();
    return count;
}
// HSV转RGB颜色
static lv_color_t hsv_to_rgb(float h, float s, float v) {
    float r, g, b;
    int i = (int)(h * 6);
    float f = h * 6 - i;
    float p = v * (1 - s);
    float q = v * (1 - f * s);
    float t = v * (1 - (1 - f) * s);
    
    switch (i % 6) {
        case 0: r = v; g = t; b = p; break;
        case 1: r = q; g = v; b = p; break;
        case 2: r = p; g = v; b = t; break;
        case 3: r = p; g = q; b = v; break;
        case 4: r = t; g = p; b = v; break;
        case 5: r = v; g = p; b = q; break;
    }
    return lv_color_make((uint8_t)(r * 255), (uint8_t)(g * 255), (uint8_t)(b * 255));
}

// 获取显示值
static int get_display_value(int actual_value, FileSelectionInstance* instance) {
    if (actual_value < 0) {
        int adjusted = instance->value_threshold + (actual_value % instance->value_threshold);
        return (adjusted == instance->value_threshold) ? 0 : adjusted;
    }
    return actual_value % instance->value_threshold;
}

// 生成彩虹色
static lv_color_t generate_rainbow_color(int index, FileSelectionInstance* instance) {
    float hue = (index % instance->value_threshold) / (float)instance->value_threshold;
    return hsv_to_rgb(hue, 0.8f, 0.9f);
}

// 获取容器Y坐标
static int32_t get_container_y(lv_obj_t* container) {
    return lv_obj_get_y(container);
}

// 计算弧形X偏移
static int32_t calculate_arc_y(int32_t linear_y, int32_t scroll_y, int32_t container_height) {
    int32_t screen_y = linear_y - scroll_y;
    int32_t container_center_y = screen_y + container_height / 2;
    int32_t diff_y = container_center_y - CIRCLE_CENTER_Y;
    int32_t abs_diff_y = abs(diff_y);
    
    int32_t x_offset = 0;
    
    if (abs_diff_y <= CIRCLE_RADIUS) {
        int32_t y_squared = diff_y * diff_y;
        int32_t radius_squared = CIRCLE_RADIUS * CIRCLE_RADIUS;
        
        if (radius_squared >= y_squared) {
            x_offset = (int32_t)(CIRCLE_CENTER_X - sqrtf(radius_squared - y_squared));
            x_offset = x_offset - 120;
            if (x_offset < -120) x_offset = -120;
            if (x_offset > 30) x_offset = 30;
        }
    } else {
        x_offset = -120;
    }
    return x_offset;
}

// 应用弧形效果
static void apply_arc_effects(lv_obj_t* container, int32_t screen_y, int32_t container_height) {
    int32_t container_center_y = screen_y + container_height / 2;
    int32_t abs_diff_y = abs(container_center_y - CIRCLE_CENTER_Y);
    
    float distance_factor = 1.0f - (float)abs_diff_y / 320.0f;
    if (distance_factor < 0.5f) distance_factor = 0.5f;
    if (distance_factor > 1.0f) distance_factor = 1.0f;
    
    int32_t radius = (int32_t)(container_height / 2 * distance_factor);
    lv_obj_set_style_radius(container, radius, LV_PART_MAIN);
    
    lv_opa_t opa = (lv_opa_t)(200 * distance_factor + 55);
    lv_obj_set_style_bg_opa(container, opa, LV_PART_MAIN);
    lv_obj_set_style_border_opa(container, opa, LV_PART_MAIN);
    
    lv_obj_set_style_shadow_width(container, (int32_t)(10 * distance_factor), LV_PART_MAIN);
}

// 设置容器Y坐标并应用弧形效果
static void set_container_y_with_arc(FileSelectionInstance* instance, lv_obj_t* container, int32_t y) {
    lv_obj_set_y(container, y);
    
    int32_t scroll_y = lv_obj_get_scroll_y(instance->scroll_content);
    int32_t screen_y = y - scroll_y;
    
    int32_t x_offset = calculate_arc_y(y, scroll_y, CONTAINER_HEIGHT);
    lv_obj_set_style_translate_x(container, x_offset, LV_PART_MAIN);
    
    apply_arc_effects(container, screen_y, CONTAINER_HEIGHT);
     // 强制更新滚动容器的布局
    lv_obj_update_layout(instance->scroll_content);
}

// 淡出动画回调
static void fade_out_anim_cb(void* var, int32_t value) {
    lv_obj_t* obj = (lv_obj_t*)var;
    lv_obj_set_style_opa(obj, value, LV_PART_MAIN);
}

// 淡出动画完成回调
static void fade_out_anim_complete_cb(lv_anim_t* a) {
    AnimationCompleteData* cb_data = (AnimationCompleteData*)a->user_data;
    if (!cb_data || !cb_data->instance) return;
    
    FileSelectionInstance* instance = cb_data->instance;
    const char* filename = cb_data->filename;
    
    printf("淡出动画完成，准备回调文件: %s\n", filename);
    instance->is_animating = false;
    
    if (instance->callback) {
        instance->callback(filename, instance->callback_user_data);
    }
    
    file_selection_destroy(instance);
    free(cb_data);
}

// 容器点击回调
static void container_click_cb(lv_event_t* e) {
    FileSelectionInstance* instance = (FileSelectionInstance*)lv_event_get_user_data(e);
    if (!instance || !instance->is_active || instance->is_animating) return;
    
    lv_obj_t* container = (lv_obj_t*)lv_event_get_target(e);
    
    int container_index = -1;
    for (int i = 0; i < instance->container_count; i++) {
        if (instance->containers[i] == container) {
            container_index = i;
            break;
        }
    }
    if (container_index == -1) return;
    
    int actual_value = instance->container_values[container_index];
    int display_index = get_display_value(actual_value, instance);
    
    if (instance->file_count > 0 && display_index < instance->file_count) {
        const char* filename = instance->file_list[display_index].full_name;
        printf("用户选择了文件: %s\n", filename);

        if (!check_file_exists(instance->file_type, filename)) {
            show_file_not_found_msgbox();
            return;
        }
        
        save_selected_file(instance->file_type, filename);
        
        strncpy(instance->selected_filename, filename, MAX_FILE_NAME_LEN - 1);
        instance->selected_filename[MAX_FILE_NAME_LEN - 1] = '\0';
        instance->selected_index = display_index;
        
        instance->is_animating = true;
        file_selection_set_active(instance, false);
        
        lv_anim_init(&instance->fade_out_anim);
        lv_anim_set_var(&instance->fade_out_anim, instance->view_container);
        lv_anim_set_values(&instance->fade_out_anim, LV_OPA_COVER, LV_OPA_TRANSP);
        lv_anim_set_time(&instance->fade_out_anim, 500);
        lv_anim_set_exec_cb(&instance->fade_out_anim, fade_out_anim_cb);
        
        AnimationCompleteData* cb_data = (AnimationCompleteData*)malloc(sizeof(AnimationCompleteData));
        if (cb_data) {
            cb_data->instance = instance;
            cb_data->filename = instance->selected_filename;
        }
        
        lv_anim_set_user_data(&instance->fade_out_anim, cb_data);
        lv_anim_set_ready_cb(&instance->fade_out_anim, fade_out_anim_complete_cb);
        lv_anim_start(&instance->fade_out_anim);
    } else {
        printf("无文件，点击无效\n");
    }
}

// 查找Y值最小的容器
static lv_obj_t* find_container_with_min_y(FileSelectionInstance* instance) {
    int32_t minY = INT32_MAX;
    lv_obj_t* minContainer = NULL;
    for (int i = 0; i < instance->container_count; i++) {
        int32_t y = get_container_y(instance->containers[i]);
        if (y < minY) {
            minY = y;
            minContainer = instance->containers[i];
        }
    }
    return minContainer;
}

// 查找Y值最大的容器
static lv_obj_t* find_container_with_max_y(FileSelectionInstance* instance) {
    int32_t maxY = INT32_MIN;
    lv_obj_t* maxContainer = NULL;
    for (int i = 0; i < instance->container_count; i++) {
        int32_t y = get_container_y(instance->containers[i]);
        if (y > maxY) {
            maxY = y;
            maxContainer = instance->containers[i];
        }
    }
    return maxContainer;
}

// 查找具有最小值的容器
static int find_container_with_min_value(FileSelectionInstance* instance) {
    int minVal = INT_MAX;
    int minIndex = 0;
    for (int i = 0; i < instance->container_count; i++) {
        if (instance->container_values[i] < minVal) {
            minVal = instance->container_values[i];
            minIndex = i;
        }
    }
    return minIndex;
}

// 查找具有最大值的容器
static int find_container_with_max_value(FileSelectionInstance* instance) {
    int maxVal = INT_MIN;
    int maxIndex = 0;
    for (int i = 0; i < instance->container_count; i++) {
        if (instance->container_values[i] > maxVal) {
            maxVal = instance->container_values[i];
            maxIndex = i;
        }
    }
    return maxIndex;
}

// 更新容器的显示值
static void update_container_value(FileSelectionInstance* instance, int container_index, int new_actual_value) {
    instance->container_values[container_index] = new_actual_value;
    
    int display_index = get_display_value(new_actual_value, instance);
    
    lv_obj_t* label = lv_obj_get_child(instance->containers[container_index], 0);
    if (label) {
        char text[256] = {0};
        if (instance->file_count > 0 && display_index < instance->file_count) {
            snprintf(text, sizeof(text), "%s", instance->file_list[display_index].name);
        } else {
            snprintf(text, sizeof(text), "无文件");
        }
        
        lv_label_set_text(label, text);
        lv_obj_set_style_text_font(label, &chinese_24, LV_PART_MAIN);
        lv_obj_set_style_text_color(label, lv_color_white(), LV_PART_MAIN);
        lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_LEFT, LV_PART_MAIN);
        lv_obj_set_x(label, 0);
        lv_obj_set_y(label, -12);

        lv_obj_clear_flag(label, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_scroll_dir(label, LV_DIR_NONE);
        lv_obj_add_flag(label, LV_OBJ_FLAG_EVENT_BUBBLE);
        
        lv_color_t rainbow_color = generate_rainbow_color(display_index, instance);
        lv_obj_set_style_bg_color(instance->containers[container_index], rainbow_color, LV_PART_MAIN);
    }
}

// 检查并调整容器位置
static void check_and_adjust_containers(FileSelectionInstance* instance) {
    if (!instance->is_active) return;
    
    uint32_t current_time = millis();
    if (current_time - instance->last_check_time < 5) return;
    instance->last_check_time = current_time;
    
    int32_t scroll_y = lv_obj_get_scroll_y(instance->scroll_content);
    
    int most_needed_index = -1;
    int32_t most_needed_distance = 0;
    
    for (int i = 0; i < instance->container_count; i++) {
        int32_t abs_y = get_container_y(instance->containers[i]);
        int32_t screen_y = abs_y - scroll_y;
        
        if (screen_y + CONTAINER_HEIGHT < SCROLL_THRESHOLD_TOP) {
            int32_t distance = screen_y + CONTAINER_HEIGHT - SCROLL_THRESHOLD_TOP;
            if (distance < most_needed_distance) {
                most_needed_distance = distance;
                most_needed_index = i;
            }
        }
        else if (screen_y > SCROLL_THRESHOLD_BOTTOM) {
            int32_t distance = screen_y - SCROLL_THRESHOLD_BOTTOM;
            if (distance > most_needed_distance) {
                most_needed_distance = distance;
                most_needed_index = i;
            }
        }
    }
    
    if (most_needed_index >= 0) {
        int32_t current_y = get_container_y(instance->containers[most_needed_index]);
        int32_t screen_y = current_y - scroll_y;
        int32_t target_y = 0;
        int new_value = 0;
        
        if (screen_y + CONTAINER_HEIGHT < SCROLL_THRESHOLD_TOP) {
            lv_obj_t* maxContainer = find_container_with_max_y(instance);
            if (maxContainer) {
                target_y = get_container_y(maxContainer) + TELEPORT_OFFSET;
                int maxValueIndex = find_container_with_max_value(instance);
                new_value = instance->container_values[maxValueIndex] + 1;
                
                update_container_value(instance, most_needed_index, new_value);
                
                if (abs(current_y - target_y) > MIN_Y_DIFFERENCE) {
                    set_container_y_with_arc(instance, instance->containers[most_needed_index], target_y);
                }
            }
        }
        else if (screen_y > SCROLL_THRESHOLD_BOTTOM) {
            lv_obj_t* minContainer = find_container_with_min_y(instance);
            if (minContainer) {
                target_y = get_container_y(minContainer) - TELEPORT_OFFSET;
                int minValueIndex = find_container_with_min_value(instance);
                new_value = instance->container_values[minValueIndex] - 1;
                
                update_container_value(instance, most_needed_index, new_value);
                
                if (abs(current_y - target_y) > MIN_Y_DIFFERENCE) {
                    set_container_y_with_arc(instance, instance->containers[most_needed_index], target_y);
                }
            }
        }
        lv_obj_update_layout(instance->containers[most_needed_index]);
    }
}

// 快速检查函数
static void quick_check_containers(FileSelectionInstance* instance) {
    if (!instance->is_active) return;
    
    int32_t scroll_y = lv_obj_get_scroll_y(instance->scroll_content);
    int32_t screen_height = lv_obj_get_height(instance->view_container);
    
    int most_needed_index = -1;
    bool need_top_adjust = false;
    
    for (int i = 0; i < instance->container_count; i++) {
        int32_t abs_y = get_container_y(instance->containers[i]);
        int32_t screen_y = abs_y - scroll_y;
        
        if (screen_y + CONTAINER_HEIGHT < -150) {
            most_needed_index = i;
            need_top_adjust = true;
            break;
        }
        else if (screen_y > screen_height + 150) {
            most_needed_index = i;
            need_top_adjust = false;
            break;
        }
    }
    
    if (most_needed_index >= 0) {
        int32_t current_y = get_container_y(instance->containers[most_needed_index]);
        int32_t target_y = 0;
        int new_value = 0;
        
        if (need_top_adjust) {
            lv_obj_t* maxContainer = find_container_with_max_y(instance);
            if (maxContainer) {
                int32_t max_abs_y = get_container_y(maxContainer);
                target_y = max_abs_y + TELEPORT_OFFSET;
                int maxValueIndex = find_container_with_max_value(instance);
                new_value = instance->container_values[maxValueIndex] + 1;
                
                update_container_value(instance, most_needed_index, new_value);
                
                if (abs(current_y - target_y) > MIN_Y_DIFFERENCE) {
                    set_container_y_with_arc(instance, instance->containers[most_needed_index], target_y);
                }
            }
        }
        else {
            lv_obj_t* minContainer = find_container_with_min_y(instance);
            if (minContainer) {
                int32_t min_abs_y = get_container_y(minContainer);
                target_y = min_abs_y - TELEPORT_OFFSET;
                int minValueIndex = find_container_with_min_value(instance);
                new_value = instance->container_values[minValueIndex] - 1;
                
                update_container_value(instance, most_needed_index, new_value);
                
                if (abs(current_y - target_y) > MIN_Y_DIFFERENCE) {
                    set_container_y_with_arc(instance, instance->containers[most_needed_index], target_y);
                }
            }
        }
    }
}

// 滚动事件回调
static void scroll_event_cb(lv_event_t* e) {
    lv_event_code_t code = lv_event_get_code(e);
    lv_obj_t* obj = (lv_obj_t*)lv_event_get_current_target(e);
    FileSelectionInstance* instance = (FileSelectionInstance*)lv_obj_get_user_data(obj);
    
    if (!instance || !instance->is_active) return;
    
    if (code == LV_EVENT_SCROLL) {
        int32_t scroll_y = lv_obj_get_scroll_y(instance->scroll_content);
        
        for (int i = 0; i < instance->container_count; i++) {
            int32_t abs_y = get_container_y(instance->containers[i]);
            int32_t screen_y = abs_y - scroll_y;
            
            int32_t x_offset = calculate_arc_y(abs_y, scroll_y, CONTAINER_HEIGHT);
            lv_obj_set_style_translate_x(instance->containers[i], x_offset, LV_PART_MAIN);
            
            apply_arc_effects(instance->containers[i], screen_y, CONTAINER_HEIGHT);
        }
        
        int32_t scroll_delta = abs(scroll_y - instance->last_scroll_y);
        instance->last_scroll_y = scroll_y;
        
        if (scroll_delta > 20) {
            quick_check_containers(instance);
        }
    }
    else if (code == LV_EVENT_SCROLL_END) {
        check_and_adjust_containers(instance);
    }
}

// 定时器回调
static void update_timer_cb(lv_timer_t* timer) {
    FileSelectionInstance* instance = (FileSelectionInstance*)timer->user_data;
    if (instance && instance->is_active) {
        check_and_adjust_containers(instance);
    }
}

// 加载SD卡文件列表
static void load_sd_file_list(FileSelectionInstance* instance) {
    bool sd_initialized = false;
    
    // 获取互斥锁检查SD卡状态
    if (xSemaphoreTake(sd_state_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        sd_initialized = sd_card_initialized;
        xSemaphoreGive(sd_state_mutex);
    }
    if (!sd_initialized) {
        printf("SD卡未初始化，设置为无文件状态\n");
        instance->file_list = (file_info_t*)malloc(sizeof(file_info_t) * MAX_FILE_COUNT);
        if (instance->file_list) {
            instance->file_count = 0;
            instance->value_threshold = DEFAULT_VALUE_THRESHOLD;
        }
        return;
    }
    
    instance->file_list = (file_info_t*)malloc(sizeof(file_info_t) * MAX_FILE_COUNT);
    if (!instance->file_list) {
        instance->value_threshold = DEFAULT_VALUE_THRESHOLD;
        instance->file_count = 0;
        return;
    }
    
    instance->file_count = read_sd_file_list(instance->file_list, MAX_FILE_COUNT, instance->file_type);
    
    if (instance->file_count > 0) {
        instance->value_threshold = instance->file_count;
    } else {
        instance->value_threshold = DEFAULT_VALUE_THRESHOLD;
    }
    
    printf("文件列表加载完成：数量=%d，阈值=%d\n", instance->file_count, instance->value_threshold);
}

// 创建文件选择实例
FileSelectionInstance* file_selection_create(lv_obj_t* parent, 
                                           file_type_t file_type,
                                           file_selected_callback_t callback,
                                           void* user_data) {
    const int x = -10;
    const int y = -10;
    const int width = 240;
    const int height = 280;
    const int container_count = 10;
    
    FileSelectionInstance* instance = (FileSelectionInstance*)malloc(sizeof(FileSelectionInstance));
    if (!instance) return NULL;
    
    memset(instance, 0, sizeof(FileSelectionInstance));
    instance->container_count = container_count;
    instance->value_threshold = DEFAULT_VALUE_THRESHOLD;
    instance->file_count = 0;
    instance->file_list = NULL;
    instance->callback = callback;
    instance->callback_user_data = user_data;
    instance->file_type = file_type;
    instance->is_active = true;
    
    load_sd_file_list(instance);
    
    instance->containers = (lv_obj_t**)malloc(sizeof(lv_obj_t*) * container_count);
    if (!instance->containers) {
        free(instance->file_list);
        free(instance);
        return NULL;
    }
    
    instance->container_values = (int*)malloc(sizeof(int) * container_count);
    if (!instance->container_values) {
        free(instance->containers);
        free(instance->file_list);
        free(instance);
        return NULL;
    }
    
    // 创建可视区域容器
    instance->view_container = lv_obj_create(parent);
    lv_obj_set_size(instance->view_container, width, height);
    lv_obj_set_pos(instance->view_container, x, y);
    lv_obj_set_style_border_width(instance->view_container, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(instance->view_container, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_pad_all(instance->view_container, 0, LV_PART_MAIN);
    lv_obj_set_scrollbar_mode(instance->view_container, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_scroll_dir(instance->view_container, LV_DIR_NONE);
    
    // 创建滚动内容容器
    instance->scroll_content = lv_obj_create(instance->view_container);
    lv_obj_set_size(instance->scroll_content, width, height);
    lv_obj_set_pos(instance->scroll_content, 0, 0);
    lv_obj_set_style_border_width(instance->scroll_content, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(instance->scroll_content, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_pad_all(instance->scroll_content, 0, LV_PART_MAIN);
    lv_obj_set_scroll_dir(instance->scroll_content, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(instance->scroll_content, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_scroll_snap_y(instance->scroll_content, LV_SCROLL_SNAP_CENTER);
    
    // 存储实例指针
    lv_obj_set_user_data(instance->scroll_content, instance);
    
    lv_obj_add_event_cb(instance->scroll_content, scroll_event_cb, LV_EVENT_SCROLL, NULL);
    lv_obj_add_event_cb(instance->scroll_content, scroll_event_cb, LV_EVENT_SCROLL_END, NULL);
    
    // 创建容器
    for (int i = 0; i < container_count; i++) {
        instance->containers[i] = lv_obj_create(instance->scroll_content);
        lv_obj_set_size(instance->containers[i], width, CONTAINER_HEIGHT);
        instance->container_values[i] = i;
        
        int display_index = get_display_value(i, instance);
        lv_color_t rainbow_color = generate_rainbow_color(display_index, instance);
        lv_obj_set_style_bg_color(instance->containers[i], rainbow_color, LV_PART_MAIN);
        lv_obj_set_style_bg_opa(instance->containers[i], LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_radius(instance->containers[i], CONTAINER_HEIGHT/2, LV_PART_MAIN);
        lv_obj_set_style_border_width(instance->containers[i], 2, LV_PART_MAIN);
        lv_obj_set_style_border_color(instance->containers[i], lv_color_white(), LV_PART_MAIN);
        lv_obj_set_style_shadow_width(instance->containers[i], 10, LV_PART_MAIN);
        lv_obj_set_style_shadow_color(instance->containers[i], lv_color_make(0, 0, 0), LV_PART_MAIN);
        lv_obj_set_style_shadow_opa(instance->containers[i], LV_OPA_50, LV_PART_MAIN);
        lv_obj_set_style_shadow_ofs_x(instance->containers[i], 0, LV_PART_MAIN);
        lv_obj_set_style_shadow_ofs_y(instance->containers[i], 5, LV_PART_MAIN);

        lv_obj_set_scroll_dir(instance->containers[i], LV_DIR_NONE);
        
        lv_obj_add_event_cb(instance->containers[i], container_click_cb, LV_EVENT_CLICKED, instance);
        lv_obj_add_flag(instance->containers[i], LV_OBJ_FLAG_CLICKABLE);
        
        lv_obj_t* label = lv_label_create(instance->containers[i]);
        char text[256] = {0};
        if (instance->file_count > 0 && display_index < instance->file_count) {
            snprintf(text, sizeof(text), "%s", instance->file_list[display_index].name);
        } else {
            snprintf(text, sizeof(text), "无文件");
        }
        lv_label_set_text(label, text);
        lv_obj_set_style_text_font(label, &chinese_24, LV_PART_MAIN);
        lv_obj_set_style_text_color(label, lv_color_white(), LV_PART_MAIN);
        lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_LEFT, LV_PART_MAIN);
        lv_obj_set_x(label, 0);
        lv_obj_set_y(label, -12);

        lv_obj_clear_flag(label, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_scroll_dir(label, LV_DIR_NONE);
        
        lv_obj_set_scrollbar_mode(instance->containers[i], LV_SCROLLBAR_MODE_OFF);
        
        int32_t initial_y = i * TELEPORT_OFFSET;
        set_container_y_with_arc(instance, instance->containers[i], initial_y);
    }
    
    // 读取历史文件
    int history_index = -1;
    char history_filename[MAX_FILE_NAME_LEN] = {0};
    if (load_selected_file(instance->file_type, history_filename, sizeof(history_filename))) {
        for (int i = 0; i < instance->file_count; i++) {
            if (strcmp(instance->file_list[i].full_name, history_filename) == 0) {
                history_index = i;
                printf("找到历史文件索引: %d\n", history_index);
                break;
            }
        }
    }
    
    const int center_idx = 3;
    int base_value = (history_index >= 0) ? history_index : 0;
    
    for (int i = 0; i < instance->container_count; i++) {
        int delta = i - center_idx;
        int actual_value = (base_value + delta) % instance->value_threshold;
        if (actual_value < 0) actual_value += instance->value_threshold;
        
        instance->container_values[i] = actual_value;
        
        lv_obj_t* container = instance->containers[i];
        lv_obj_t* label = lv_obj_get_child(container, 0);
        int display_index = get_display_value(actual_value, instance);
        char text[256] = {0};
        if (instance->file_count > 0 && display_index < instance->file_count) {
            snprintf(text, sizeof(text), "%s", instance->file_list[display_index].name);
        } else {
            snprintf(text, sizeof(text), "无文件");
        }
        lv_label_set_text(label, text);
        
        lv_color_t rainbow_color = generate_rainbow_color(display_index, instance);
        lv_obj_set_style_bg_color(container, rainbow_color, LV_PART_MAIN);
    }
    
    instance->timer = lv_timer_create(update_timer_cb, UPDATE_INTERVAL, instance);
    instance->last_check_time = millis();
    instance->last_scroll_y = lv_obj_get_scroll_y(instance->scroll_content);
    
    g_file_selection_instance = instance;
    
    if (instance) {
        /* 进入不透明度淡入已移除: 直接以完全不透明显示 */
        lv_obj_set_style_opa(instance->view_container, LV_OPA_COVER, LV_PART_MAIN);

        memset(&instance->fade_out_anim, 0, sizeof(lv_anim_t));

        instance->is_animating = false;
        instance->selected_index = -1;
        instance->selected_filename[0] = '\0';
    }
    
    return instance;
}

// 销毁文件选择实例
void file_selection_destroy(FileSelectionInstance* instance) {
    if (!instance) return;
    
    if (lv_anim_count_running() > 0) {
        lv_anim_del(instance->view_container, NULL);
    }
    
    if (g_file_selection_instance == instance) {
        g_file_selection_instance = NULL;
    }
    
    if (instance->timer) {
        lv_timer_del(instance->timer);
        instance->timer = NULL;
    }
    
    if (instance->file_list) {
        free(instance->file_list);
    }
    if (instance->containers) {
        free(instance->containers);
    }
    if (instance->container_values) {
        free(instance->container_values);
    }
    if (instance->scroll_content) {
        lv_obj_del(instance->scroll_content);
    }
    if (instance->view_container) {
        lv_obj_del(instance->view_container);
    }
    
    free(instance);
}

// 设置文件选择器激活状态
void file_selection_set_active(FileSelectionInstance* instance, bool active) {
    if (instance) {
        instance->is_active = active;
    }
}