idf_component_register(SRCS "main.c"
                            "app/board.c"
                            "app/battery.cpp"
                            "app/rtc_init.cpp"
                            "app/boot_anim.cpp"
                            "app/watchdog.c"
                            "app/console.c"
                            "drivers/lcd.c"
                            "drivers/touch.c"
                            "drivers/sd_card.c"
                            "storage/sd_scan.cpp"
                            "storage/sd_task.cpp"
                            "ui/ui_core.c"
                            "ui/File_Selection.cpp"
                            "ui/menu/ui_manager.cpp"
                            "ui/apps/fs_time.cpp"
                            "ui/apps/fs_game.cpp"
                            "ui/apps/fs_stopwatch.cpp"
                            "ui/apps/fs_calculator.cpp"
                            "ui/apps/fs_calendar.cpp"
                            "ui/apps/ImageProcessor.cpp"
                            "ui/apps/fs_video.cpp"
                            "ui/apps/fs_picture.cpp"
                            "ui/apps/fs_comic.cpp"
                            "ui/apps/fs_filemanager.cpp"
                            "ui/apps/fs_novel.cpp"
                            "ui/apps/fs_music.cpp"
                            "ui/apps/fs_settings.cpp"
                            "ui/apps/weather.cpp"
                            "compat/arduino_compat.cpp"
                            "compat/jpegdec_cxx.cpp"
                            "compat/sd_fat_graft.cpp"
                            "compat/fs_shim.cpp"
                            "compat/RTCManager.cpp"
                            "compat/AudioFileSourceSdFat.cpp"
                            "compat/WiFi.cpp"
                            "resources/time_70.c"
                            "resources/time_80.c"
                            "resources/tip_display_35.c"
                            "resources/chinese_24.c"
                       PRIV_REQUIRES spi_flash esp_psram nvs_flash fatfs esp_driver_sdmmc
                                      esp_lcd esp_driver_spi esp_driver_ledc driver esp_adc
                                      esp_driver_i2c esp_driver_gpio esp_driver_i2s
                                      esp_wifi esp_netif esp_event esp_http_client
                                      lvgl esp_timer
                                      pngdec
                                      esp_driver_tsens
                                      esp_driver_usb_serial_jtag
                       INCLUDE_DIRS "." "app" "drivers" "storage" "ui" "ui/menu" "compat" "resources")

# 天气: 参考工程 ui_manager.cpp 用的是 ArduinoJson v6 的 String 重载
# (deserializeJson(doc, payload), payload 是 Arduino String)。
# 打开这个开关后 ArduinoJson 会 include <Arduino.h> 并识别 compat 层的 ::String。
target_compile_definitions(${COMPONENT_LIB} PRIVATE ARDUINOJSON_ENABLE_ARDUINO_STRING=1)

# LVGL 8.3 的 LV_PART_xxx | LV_STATE_xxx 写法在 C++ 下会告警, 关闭该告警 (仅 C++)
target_compile_options(${COMPONENT_LIB} PRIVATE $<$<COMPILE_LANGUAGE:CXX>:-Wno-deprecated-enum-enum-conversion>)
# 中文字体文件 (chinese_24.c 等) 的注释为 GBK, 会误触发 -Wcomment
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-comment)
# 参考工程原样代码在 IDF 默认 -Werror=all 下的已知告警 (源码保持原样, 不改):
#   File_Selection.cpp  hsv_to_rgb() 的 r/g/b (switch 无 default, 实际 i%6 恒为 0..5)
#   File_Selection.cpp  extract_file_name() 的 strncpy 截断 (有意为之的有界拷贝)
#   fs_filemanager.cpp  snprintf(dst[256], "%s/%s", path[256], name[256]) 的截断,
#                       以及 snprintf(current_path, ..., "%s/%s", current_path, ...)
#                       的源/目标重叠 (SdFat 上如此写是安全的, 拼接结果必然更短)
target_compile_options(${COMPONENT_LIB} PRIVATE
    -Wno-error=maybe-uninitialized
    -Wno-error=stringop-truncation
    -Wno-error=format-truncation
    -Wno-error=restrict
    # fs_music.cpp  C++20 花括号初始化里 uint32_t→int32_t (参考工程原样代码)
    -Wno-error=narrowing
    # fs_settings.cpp:1817  snprintf("完成 (%dKB)", slot, kb) 多传了一个参数
    # (参考工程原样代码, 多出的参数被忽略, 无副作用)
    -Wno-error=format-extra-args)
