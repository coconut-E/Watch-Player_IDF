/*
 * 移植自参考工程 ui_manager.h
 */

#ifndef UI_MANAGER_H
#define UI_MANAGER_H

#include "fullscreen_interfaces.h"
#include "weather.h"

//SD卡状态
extern bool sd_card_initialized;
extern bool sd_card_init_event;// SD卡初始化失败事件
extern bool sd_card_scan_event;
extern int sd_card_insertion_event;
extern bool sd_card_warming;               // SD卡预热中
extern SemaphoreHandle_t sd_state_mutex;  // SD状态互斥锁

extern lv_img_dsc_t weather_img_dsc[WEATHER_FRAME_CNT];

// 功能函数声明
void fs_create_novel(lv_obj_t* container);
void fs_create_picture(lv_obj_t* container);
void fs_create_game(lv_obj_t* container);
void fs_create_comic(lv_obj_t* container);
void fs_create_calendar(lv_obj_t* container);
void fs_create_settings(lv_obj_t* container);
void fs_create_video(lv_obj_t* container);
void fs_create_time(lv_obj_t* container);
void fs_create_music(lv_obj_t* container);
void fs_create_stopwatch(lv_obj_t* container);
void fs_create_calculator(lv_obj_t* container);
void fs_create_filemanager(lv_obj_t* container);
void fs_create_weather(lv_obj_t* container);

// UI管理函数声明
void ui_init_all();
void ui_update_uptime();
void fs_do_exit(void);
void show_tip_container(const char* tip_text);  // 显示提示容器
void hide_tip_container(void);                  // 隐藏提示容器
void toggle_tip_container(void);                // 切换提示容器
void ui_set_tip_text(const char* tip_text);
void ui_process_scan_queue(void);
void Get_weather();

// SD卡事件处理函数
void ui_check_sd_card_event(void);
bool ui_is_sd_card_initialized(void);
void ui_reset_sd_init_event(void);

int weather_code_to_index(int code, int is_day);
const char* wmo_code_to_desc_cn(int code);

void board_sleep(void);

// 全局UI对象声明
extern lv_obj_t* circle_container;
extern lv_obj_t* hour_label;
extern lv_obj_t* minute_label;
extern lv_obj_t* second_label;
extern lv_obj_t* sliding_container;
extern lv_obj_t* fullscreen_container;
extern lv_obj_t* tip_container;                 // 提示容器
extern volatile bool is_fullscreen_container_active;
extern bool is_tip_container_active;            // 提示容器激活状态

#endif
