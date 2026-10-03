#include "fullscreen_interfaces.h"
#include "File.h"
#include <cstring>
#include <ctype.h>


static lv_obj_t* mainList = nullptr;
static lv_timer_t* poll_timer = nullptr;

// 状态管理变量
static char current_path[256] = "/";
static FsFile current_dir;      
static bool is_first_page = true;

//用于暂存被挤掉的第30/31个文件
static char pending_filename[256] = {0};
static bool pending_is_dir = false;
static bool has_pending_file = false;

//记录当前目录是否根本打不开
static bool dir_open_failed = false;

//记录是否有文件被删除
static bool file_changed = false;

//弹窗相关静态变量
static lv_obj_t* active_msgbox = nullptr;
static char del_target_path[256] = {0}; // 记录待删除文件的绝对路径

static lv_img_dsc_t file_img_dsc[FILE_FRAME_CNT];

static void init_file_icons() {
    for (int i = 0; i < FILE_FRAME_CNT; i++) {
        file_img_dsc[i].header.w = FILE_IMG_WIDTH;
        file_img_dsc[i].header.h = FILE_IMG_HEIGHT;
        file_img_dsc[i].data_size = FILE_IMG_WIDTH * FILE_IMG_HEIGHT * LV_COLOR_DEPTH / 8;
        file_img_dsc[i].header.cf = LV_IMG_CF_TRUE_COLOR;
        file_img_dsc[i].data = (const uint8_t*)&File[i][0];
    }
}

static bool endswith_ignore_case(const char* str, const char* suffix) {
    if (!str || !suffix) return false;
    size_t str_len = strlen(str);
    size_t suffix_len = strlen(suffix);
    if (suffix_len > str_len) return false;
    const char* p = str + str_len - suffix_len;
    while (*suffix) {
        if (tolower((unsigned char)*p) != tolower((unsigned char)*suffix)) return false;
        p++;
        suffix++;
    }
    return true;
}

static uint8_t get_file_icon_index(const char* filename, bool isDir) {
    if (isDir) return 0;
    if (endswith_ignore_case(filename, ".mjpeg") || endswith_ignore_case(filename, ".mp4") || endswith_ignore_case(filename, ".avi")) return 1;
    if (endswith_ignore_case(filename, ".png") || endswith_ignore_case(filename, ".jpg") || endswith_ignore_case(filename, ".jpeg")) return 2;
    if (endswith_ignore_case(filename, ".mp3") || endswith_ignore_case(filename, ".flac") || endswith_ignore_case(filename, ".wav")) return 3;
    if (endswith_ignore_case(filename, ".txt")) return 4;
    if (endswith_ignore_case(filename, ".cmj")) return 5;
    return 6;
}

static void refresh_file_list();

static void open_new_directory(const char* path) {
    if (current_dir.isOpen()) {
        current_dir.close(); 
    }
    dir_open_failed = false;
    has_pending_file = false;
    pending_filename[0] = '\0';
    is_first_page = true;

    if (!current_dir.open(path, O_READ)) {
        dir_open_failed = true; 
    }
}

static void go_back() {
    if (strcmp(current_path, "/") == 0) {
        if (poll_timer) {
            lv_timer_del(poll_timer);
            poll_timer = nullptr;
        }
        current_dir.close(); 
        
        // 退出前先关闭可能存在的文件详情弹窗，防止崩溃
        if (active_msgbox) {
            lv_obj_del(active_msgbox);
            active_msgbox = nullptr;
        }
        
        // 如果有文件变动，只设置全局标志位，交由系统的 sd_init_task 自动处理
        if (file_changed) {
            if (xSemaphoreTake(sd_state_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
                sd_force_scan = true;
                xSemaphoreGive(sd_state_mutex);
            }
        }
        
        fs_do_exit();
        return;
    }

    char* last_slash = strrchr(current_path, '/');
    if (last_slash != NULL) {
        if (last_slash == current_path) {
            current_path[1] = '\0'; 
        } else {
            *last_slash = '\0';
        }
    }
    open_new_directory(current_path); 
    refresh_file_list();
}



static void poll_timer_cb(lv_timer_t* timer) {
    static uint8_t last_btn_state = LOW;
    uint8_t current_btn_state = digitalRead(BUTTON_2);
    if (last_btn_state == LOW && current_btn_state == HIGH) {
        go_back();
    }
    last_btn_state = current_btn_state;
}

//关闭弹窗回调
static void close_msgbox_cb(lv_event_t* e) {
    if (active_msgbox) {
        lv_obj_del(active_msgbox);
        active_msgbox = nullptr;
    }
}

//删除按钮回调
static void delete_btn_cb(lv_event_t* e) {
    lv_obj_t* btn = lv_event_get_target(e);
    lv_obj_t* lbl = lv_obj_get_child(btn, 0);
    const char* txt = lv_label_get_text(lbl);
    
    if (strcmp(txt, "删除") == 0) {
        lv_label_set_text(lbl, "确定");
    } 
    else if (strcmp(txt, "确定") == 0) {
        if (sd.exists(del_target_path)) {
            sd.remove(del_target_path);
            file_changed = true; // 标记文件发生变化
        }
        close_msgbox_cb(NULL); // 关闭弹窗
        
        // 删完文件后，重新打开当前目录并刷新列表
        open_new_directory(current_path);
        refresh_file_list();
    }
}

//显示文件详情弹窗
static void show_file_details(const char* filename) {
    if (active_msgbox) lv_obj_del(active_msgbox); 
    
    // 拼接绝对路径
    if (strcmp(current_path, "/") == 0) {
        snprintf(del_target_path, sizeof(del_target_path), "/%s", filename);
    } else {
        snprintf(del_target_path, sizeof(del_target_path), "%s/%s", current_path, filename);
    }
    
    // 获取文件大小
    char size_str[32] = "0 B";
    FsFile f;
    if (f.open(del_target_path, O_READ)) {
        uint32_t size = f.fileSize();
        if (size < 1024) snprintf(size_str, sizeof(size_str), "%lu B", size);
        else if (size < 1024 * 1024) snprintf(size_str, sizeof(size_str), "%.1f KB", size / 1024.0);
        else snprintf(size_str, sizeof(size_str), "%.1f MB", size / (1024.0 * 1024.0));
        f.close();
    }
    
    // 创建弹窗 UI 
    active_msgbox = lv_obj_create(lv_layer_top());
    lv_obj_set_size(active_msgbox, 220, 160); 
    lv_obj_center(active_msgbox);
    lv_obj_set_style_bg_color(active_msgbox, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(active_msgbox, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(active_msgbox, 2, 0);
    lv_obj_set_style_border_color(active_msgbox, lv_color_black(), 0);
    lv_obj_set_flex_flow(active_msgbox, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(active_msgbox, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(active_msgbox, 10, 0);
    
    // 路径 Label 
    lv_obj_t* lbl_path = lv_label_create(active_msgbox);
    lv_label_set_text(lbl_path, del_target_path);
    lv_obj_set_style_text_font(lbl_path, &chinese_24, 0); 
    lv_label_set_long_mode(lbl_path, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(lbl_path, 190);

    lv_obj_set_style_pad_bottom(lbl_path, 8, 0);
    
    // 大小 Label
    lv_obj_t* lbl_size = lv_label_create(active_msgbox);
    lv_label_set_text_fmt(lbl_size, "大小: %s", size_str);
    lv_obj_set_style_text_font(lbl_size, &chinese_24, 0);
    
    // 按钮行容器
    lv_obj_t* btn_row = lv_obj_create(active_msgbox);
    lv_obj_remove_style_all(btn_row);
    lv_obj_set_size(btn_row, 190, 40);
    lv_obj_set_flex_flow(btn_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(btn_row, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(btn_row, LV_OBJ_FLAG_CLICKABLE);
    
    // 关闭按钮
    lv_obj_t* btn_close = lv_btn_create(btn_row);
    lv_obj_set_size(btn_close, 80, 35);
    lv_obj_t* lbl_close = lv_label_create(btn_close);
    lv_label_set_text(lbl_close, "关闭");
    lv_obj_set_style_text_font(lbl_close, &chinese_24, 0);
    lv_obj_add_event_cb(btn_close, close_msgbox_cb, LV_EVENT_CLICKED, NULL);
    
    // 删除按钮 
    lv_obj_t* btn_del = lv_btn_create(btn_row);
    lv_obj_set_size(btn_del, 80, 35);
    lv_obj_set_style_bg_color(btn_del, lv_color_make(220, 50, 50), 0);
    lv_obj_t* lbl_del = lv_label_create(btn_del);
    lv_label_set_text(lbl_del, "删除");
    lv_obj_set_style_text_font(lbl_del, &chinese_24, 0);
    lv_obj_add_event_cb(btn_del, delete_btn_cb, LV_EVENT_CLICKED, NULL);
}


static void list_item_cb(lv_event_t* e) {
    lv_obj_t* btn = lv_event_get_target(e);
    uint32_t btn_type = (uint32_t)lv_obj_get_user_data(btn);

    if (btn_type == 3) {
        open_new_directory(current_path);
        refresh_file_list();
    }
    else if (btn_type == 2) {
        is_first_page = false;
        refresh_file_list();
    } 
    else if (btn_type == 1) {
        const char* folder_name = lv_list_get_btn_text(mainList, btn);
        if (strcmp(current_path, "/") == 0) {
            snprintf(current_path, sizeof(current_path), "/%s", folder_name);
        } else {
            snprintf(current_path, sizeof(current_path), "%s/%s", current_path, folder_name);
        }
        open_new_directory(current_path); 
        refresh_file_list();
    } 
    //点击的是普通文件 (btn_type == 0)
    else if (btn_type == 0) {
        const char* file_name = lv_list_get_btn_text(mainList, btn);
        show_file_details(file_name);
    }
}

static void add_file_btn_to_list(const char* filename, bool isDir) {
    uint8_t icon_idx = get_file_icon_index(filename, isDir);
    lv_obj_t* btn = lv_list_add_btn(mainList, &file_img_dsc[0], filename);
    lv_obj_set_height(btn, 50);
    lv_obj_set_style_text_font(btn, &chinese_24, LV_STATE_DEFAULT);
    
    lv_obj_t* img_obj = lv_obj_get_child(btn, 0);
    if (img_obj) {
        lv_img_set_src(img_obj, &file_img_dsc[icon_idx]);
    }
    
    lv_obj_set_user_data(btn, (void*)(isDir ? 1 : 0)); // 0代表普通文件
    lv_obj_add_event_cb(btn, list_item_cb, LV_EVENT_CLICKED, NULL);
}

static void add_special_btn(const char* text, uint32_t type) {
    lv_obj_t* btn = lv_list_add_btn(mainList, NULL, text);
    lv_obj_set_height(btn, 50);
    lv_obj_set_style_text_font(btn, &chinese_24, LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(btn, lv_color_hex(0x0000FF), LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(btn, lv_color_white(), LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, LV_STATE_DEFAULT);
    lv_obj_set_user_data(btn, (void*)type);
    lv_obj_add_event_cb(btn, list_item_cb, LV_EVENT_CLICKED, NULL);
}

static void refresh_file_list() {
    lv_obj_clean(mainList);

    if (dir_open_failed) {
        for (int i = 0; i < 10; i++) {
            lv_obj_t* btn = lv_list_add_btn(mainList, NULL, "无文件");
            lv_obj_set_height(btn, 50);
            lv_obj_set_style_text_font(btn, &chinese_24, LV_STATE_DEFAULT);
        }
        return;
    }

    uint8_t displayed = 0; 

    if (!is_first_page) {
        add_special_btn("回到顶部", 3);
        displayed++;
    }

    if (has_pending_file) {
        add_file_btn_to_list(pending_filename, pending_is_dir);
        displayed++;
        has_pending_file = false; 
        pending_filename[0] = '\0';
    }

    FsFile file;
    char filename[256];
    uint8_t max_files_before_pending = 29;

    while (file.openNext(&current_dir, O_READ)) {
        if (!file.isHidden()) {
            file.getName(filename, sizeof(filename));
            if (strcmp(filename, ".") != 0 && strcmp(filename, "..") != 0) {
                if (displayed == max_files_before_pending) {
                    strcpy(pending_filename, filename);
                    pending_is_dir = file.isDirectory();
                    has_pending_file = true;
                    file.close();
                    break; 
                } else {
                    add_file_btn_to_list(filename, file.isDirectory());
                    displayed++;
                }
            }
        }
        file.close();
    }
    
    if (has_pending_file) {
        add_special_btn("下一页", 2);
    }
    else if (displayed == 0) {
        for (int i = 0; i < 10; i++) {
            lv_obj_t* btn = lv_list_add_btn(mainList, NULL, "无文件");
            lv_obj_set_height(btn, 50);
            lv_obj_set_style_text_font(btn, &chinese_24, LV_STATE_DEFAULT);
        }
    }
}

void fs_create_filemanager(lv_obj_t* container) {
    init_file_icons(); 
    file_changed = false; // 每次进入文件管理器重置状态

    mainList = lv_list_create(container);
    lv_obj_set_size(mainList, 240, 280);
    lv_obj_center(mainList);

    strcpy(current_path, "/");
    open_new_directory(current_path); 
    refresh_file_list();

    if (poll_timer == nullptr) {
        poll_timer = lv_timer_create(poll_timer_cb, 50, NULL);
    }
}
