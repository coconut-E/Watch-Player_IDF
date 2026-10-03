#include "fullscreen_interfaces.h"

static lv_timer_t *gpio6_timer = NULL;

static void calendar_event_handler(lv_event_t * e){
    lv_event_code_t code = lv_event_get_code(e);
    lv_obj_t * obj = (lv_obj_t *)lv_event_get_current_target(e);

    if(code == LV_EVENT_VALUE_CHANGED) {
        lv_calendar_date_t date;
        if(lv_calendar_get_pressed_date(obj, &date)) {
            LV_LOG_USER("Clicked date: %02d.%02d.%d", date.day, date.month, date.year);
        }
    }
}

static void check_cb(lv_timer_t *timer){
    if(digitalRead(BUTTON_2)) {
        if(gpio6_timer) {
            lv_timer_del(gpio6_timer);
            gpio6_timer = NULL;
        }
        fs_do_exit();
        return; 
    }
}

void fs_create_calendar(lv_obj_t* container) {
    gpio6_timer = lv_timer_create(check_cb, 50, NULL);
    
    //创建日历控件
    lv_obj_t* calendar = lv_calendar_create(container);
    if(!calendar) {
        LV_LOG_ERROR("日历创建失败");
        return;
    }
    lv_obj_set_size(calendar, 220, 240);
    lv_obj_align(calendar, LV_ALIGN_TOP_MID, 0, 12);
    lv_obj_add_event_cb(calendar, calendar_event_handler, LV_EVENT_ALL, NULL);

    // 获取当前标准时间
    time_t now = time(nullptr);
    
    // 转换为本地时间结构体
    struct tm *ptm = localtime(&now);
    
    // 防止 ptm 为空指针
    if (ptm != nullptr) {
        // 设置当前日期为系统实际日期
        lv_calendar_set_today_date(calendar, 
                                   ptm->tm_year + 1900, 
                                   ptm->tm_mon + 1, 
                                   ptm->tm_mday);
        // 设置日历显示的月份为当前月份
        lv_calendar_set_showed_date(calendar, 
                                   ptm->tm_year + 1900, 
                                   ptm->tm_mon + 1);
        
        LV_LOG_USER("日历设置为当前日期: %04d-%02d-%02d", 
                   ptm->tm_year + 1900, ptm->tm_mon + 1, ptm->tm_mday);
    } else {
        // 时间未就绪时显示默认值
        lv_calendar_set_today_date(calendar, 2026, 1, 25);
        lv_calendar_set_showed_date(calendar, 2026, 1);
        LV_LOG_WARN("系统时间未就绪，使用默认日期");
    }
    
    // 添加日历头部
#if LV_USE_CALENDAR_HEADER_DROPDOWN
    lv_calendar_header_dropdown_create(calendar);   
#elif LV_USE_CALENDAR_HEADER_ARROW
    lv_calendar_header_arrow_create(calendar);
#endif
    
    LV_LOG_USER("日历界面创建完成");
}