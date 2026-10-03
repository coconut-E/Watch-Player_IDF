/*
 * 外部 RTC (RX8130CE) 初始化
 *
 * 对应参考工程 watch.ino setup() 里的:
 *     if (rtcManager.begin()) {
 *         if (rtcManager.syncToSystem()) Serial.println("系统时间已从外部 RTC 同步");
 *         else                           Serial.println("RTC 同步失败");
 *     } else {
 *         Serial.println("外部 RTC 不存在");
 *     }
 *
 * 放在 board_init() 里调用, 与参考工程一致地排在 LVGL/UI 之前 ——
 * 这样开机时时钟界面一上来就能读到正确时间。
 * board.c 是 C 文件, 所以这里用 extern "C" 暴露一个 C 接口。
 *
 * 注意: 函数名不能叫 rtc_init —— ESP-IDF 的 esp_hw_support 里已有
 *       `void rtc_init(rtc_config_t cfg)`, 同名会让链接器直接解析到 IDF 那个,
 *       本文件的代码被静默丢弃 (二进制大小完全不变就是征兆)。
 */

#include "RTCManager.h"
#include "arduino_compat.h"

extern "C" void rtc_manager_init(void)
{
    if (rtcManager.begin()) {
        if (rtcManager.syncToSystem()) {
            Serial.println("系统时间已从外部 RTC 同步");
        } else {
            Serial.println("RTC 同步失败");
        }
    } else {
        Serial.println("外部 RTC 不存在");
    }
}
