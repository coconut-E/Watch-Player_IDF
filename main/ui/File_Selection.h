#ifndef FILE_SELECTION_H 
#define FILE_SELECTION_H

#include <lvgl.h>
#include <Arduino.h>
#include <SdFat.h>
#include <Preferences.h> 

extern Preferences preferences;
// 文件路径最大长度宏定义
#define MAX_FILE_PATH_LEN 256
#define SCAN_SPECIAL_FILENAME "#SCAN#"   // 表示用户选择重新扫描

//文件类型枚举
typedef enum {
    FILE_TYPE_VIDEO,
    FILE_TYPE_NOVEL,
    FILE_TYPE_MUSIC,
    FILE_TYPE_COMIC
} file_type_t;

// 回调函数类型
typedef void (*file_selected_callback_t)(const char* filename, void* user_data);

// 文件信息结构体
#define MAX_FILE_NAME_LEN 64
typedef struct {
    char name[MAX_FILE_NAME_LEN];     // 不含后缀的文件名
    char full_name[MAX_FILE_NAME_LEN];// 完整文件名
    uint32_t size;
} file_info_t;

// 中文字体声明
LV_FONT_DECLARE(chinese_24);

// 全局消息框对象声明
extern lv_obj_t* msgbox_bg;
extern lv_obj_t* msgbox;

// 文件选择器实例结构体
typedef struct {
    // 界面相关
    lv_obj_t* view_container;
    lv_obj_t* scroll_content;
    lv_obj_t** containers;
    int* container_values;
    
    // 文件列表相关
    file_info_t* file_list;
    int file_count;
    
    // 回调函数
    file_selected_callback_t callback;
    void* callback_user_data;
    
    // 状态管理
    int32_t last_scroll_y;
    uint32_t last_check_time;
    int container_count;
    int value_threshold;
    lv_timer_t* timer;
    file_type_t file_type;
    bool is_active;

    bool is_animating;                // 是否正在动画中

    // 动画相关
    lv_anim_t fade_in_anim;           // 淡入动画
    lv_anim_t fade_out_anim;          // 淡出动画
    int selected_index;               // 选择的索引
    char selected_filename[MAX_FILE_NAME_LEN]; // 选择的文件名
} FileSelectionInstance;

typedef struct {
    FileSelectionInstance* instance;
    const char* filename;
} AnimationCompleteData;

// 核心函数声明
FileSelectionInstance* file_selection_create(lv_obj_t* parent, 
                                           file_type_t file_type,
                                           file_selected_callback_t callback,
                                           void* user_data);

void file_selection_destroy(FileSelectionInstance* instance);
void file_selection_set_active(FileSelectionInstance* instance, bool active);
const char* get_file_type_filename(file_type_t file_type);
void extract_file_name(const char* full_name, char* name, uint32_t max_len);
uint32_t read_sd_file_list(file_info_t* file_list, uint32_t max_count, file_type_t file_type);

// 函数声明
bool check_file_exists(file_type_t file_type, const char* filename);
const char* get_file_type_folder_name(file_type_t file_type);
void file_selection_force_clean(void);

// 全局实例和销毁函数声明（供外部调用）
extern FileSelectionInstance* g_file_selection_instance;
void destroy_file_selection_list(void);

#endif // FILE_SELECTION_H