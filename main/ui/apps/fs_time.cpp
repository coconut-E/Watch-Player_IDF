/*
 * 移植自参考工程 fs_time.cpp (时钟界面 + 壁纸)
 *
 * 与参考源码逐字一致, 仅以下适配/修正:
 *   - sleep() -> board_sleep()  (避免调到 libc 的 sleep)
 *   - 补 <stdlib.h>
 *   - 修正参考工程 MJPEG 偏移 bug: 两处延迟定时器 lambda 无捕获, 只读
 *     mjpeg_player.mjpeg_offset/size, 但解析值未存进 player 恒为 0;
 *     现补上赋值, 从预览之后的 MJPEG 数据段开始解码
 * 底层由既有 compat 层接管:
 *   - LittleFS/File -> compat/fs_shim.h (真实 littlefs 分区 /littlefs)
 *   - JPEGDEC       -> bitbank2/jpegdec 组件 + compat/jpegdec_cxx.cpp
 *   - malloc 大块    -> CONFIG_SPIRAM_USE_MALLOC=y 自动落 PSRAM
 */

#include "fullscreen_interfaces.h"
#include <time.h>
#include <stdlib.h>
#include "wallpaper_format.h"
#include <JPEGDEC.h>

// 静态变量声明
static lv_obj_t* bg_img = nullptr;
static lv_obj_t* time_label = nullptr;
static lv_obj_t* separator_label = nullptr;
static lv_obj_t* date_label = nullptr;
static lv_timer_t* time_timer = nullptr;
static uint8_t* wallpaper_data = nullptr;

static lv_obj_t* battery_bar = nullptr;
static lv_obj_t* battery_area = nullptr;
static lv_obj_t* battery_percent_label = nullptr;
static lv_obj_t* battery_cont = nullptr;

// 手势相关变量
static lv_point_t gesture_start;
static bool gesture_active = false;
static const int16_t SWIPE_THRESHOLD = 35;

// 关机动画相关
static bool shutting_down = false;
static lv_timer_t* shutdown_timer = nullptr;

// 屏幕尺寸常量
#define SCREEN_WIDTH 240
#define SCREEN_HEIGHT 280

// Y坐标常量
#define TIME_POS_Y 80
#define SEPARATOR_POS_Y 125
#define DATE_POS_Y 140

static lv_timer_t* detect_timer = nullptr;
static lv_timer_t* time_sleep_timer = nullptr;

static const char* weekdays_cn[] = {
    "星期日", "星期一", "星期二", "星期三", "星期四", "星期五", "星期六"
};

// ================= 壁纸选择相关 =================
#define WP_SELECT_COUNT 5
#define ZOOM_CENTER (120 * 256 / 280) // 居中高度150px对应的缩放值
#define ZOOM_EDGE (80 * 256 / 280)   // 两侧高度100px对应的缩放值
#define WP_ANIM_TIME 200             // 壁纸选择动画时间

static lv_obj_t* wp_select_cont = nullptr;
static lv_timer_t* lp_timer = nullptr;     // 长按1秒定时器
static lv_img_dsc_t wp_select_dscs[WP_SELECT_COUNT];
static uint8_t* wp_select_bufs[WP_SELECT_COUNT] = {nullptr};
static uint8_t* white_img_data = nullptr;
static lv_img_dsc_t white_img_dsc;

// ================= 上下遮罩动画全局变量 =================
static lv_obj_t* g_mask_top = nullptr;   // 上半遮罩板 (显示壁纸上半)
static lv_obj_t* g_mask_bot = nullptr;   // 下半遮罩板 (显示壁纸下半)
static bool g_mask_opening = false;      // true=滑开露出列表, false=合拢盖住列表
static int g_target_slot = 0;

// ================= MJPEG 播放器 =================
#include <esp_heap_caps.h>

#define MJPEG_STREAM_BUF_SIZE 51200      // 50KB 流缓冲区
#define FRAME_BUF_SIZE (SCREEN_WIDTH * SCREEN_HEIGHT * 2)  // 134400 bytes

static struct {
    // Core 0: 解码任务
    TaskHandle_t    decode_task;
    uint8_t*        stream_buf;          // 流缓冲区 (PSRAM)
    uint16_t*       psram_buf[2];        // 双帧缓冲 (PSRAM)
    int             decode_idx;          // 当前解码缓冲区索引 (Core 0 读写)
    QueueHandle_t   frame_queue;         // 帧索引队列 (长度1)
    File            file;                // LittleFS 文件句柄
    uint32_t        bytes_buffered;
    uint32_t        mjpeg_offset;
    uint32_t        mjpeg_size;
    volatile bool   decoding;            // 解码任务运行标志

    // Core 1: LVGL 显示
    lv_timer_t*     poll_timer;          // 高频轮询定时器
    lv_img_dsc_t    frame_dsc;           // 帧描述符
    bool            playing;
    uint8_t         wp_type;
    uint32_t        saved_refr_period;   // 保存的 LVGL 刷新周期

    // 统计
    volatile uint32_t frame_count;
    volatile uint32_t drop_count;
} mjpeg_player;

/* MJPEG 启动延迟: 首次(开机)也延迟, 保证头部写入的 RGB565 预览先显示为首帧,
 * 之后再启动动态解码 (原值 0 会让解码在开机第一个 tick 就与预览首刷抢资源) */
static int delayTime = 300;
// 解码目标指针（解码前设置，回调中读取）
static uint16_t* g_decode_target = nullptr;

static void reload_wallpaper(int slot_num);

// ================= JPEG 解码回调 =================
static int mjpeg_draw_callback(JPEGDRAW *pDraw) {
    if (!g_decode_target) return 0;
    int x = pDraw->x, y = pDraw->y, w = pDraw->iWidth, h = pDraw->iHeight;
    uint16_t *pSrc = pDraw->pPixels;
    if (x < 0 || y < 0 || x + w > SCREEN_WIDTH || y + h > SCREEN_HEIGHT) return 1;
    for (int i = 0; i < h; i++) {
        uint32_t dst_off = (uint32_t)(y + i) * SCREEN_WIDTH + x;
        memcpy(&g_decode_target[dst_off], &pSrc[i * w], w * 2);
    }
    return 1;
}

// ================= Core 0: MJPEG 解码任务 =================
static void mjpeg_decode_task_func(void* param) {
    auto& p = mjpeg_player;
    JPEGDEC* decoder = new JPEGDEC();

    Serial.printf("[MJPEG_DEC] Core%d 启动, offset=%u, size=%u\n",
        xPortGetCoreID(), p.mjpeg_offset, p.mjpeg_size);

    p.file.seek(p.mjpeg_offset);
    p.bytes_buffered = 0;
    p.decode_idx = 0;
    p.frame_count = 0;
    p.drop_count = 0;

    while (p.decoding) {
        // 填充流缓冲区
        if (p.bytes_buffered < MJPEG_STREAM_BUF_SIZE && p.file.available()) {
            int space = MJPEG_STREAM_BUF_SIZE - p.bytes_buffered;
            int bytesRead = p.file.read(p.stream_buf + p.bytes_buffered, space);
            if (bytesRead > 0) p.bytes_buffered += bytesRead;
        }

        // 缓冲区空 → 循环回放
        if (p.bytes_buffered == 0) {
            p.file.seek(p.mjpeg_offset);
            int bytesRead = p.file.read(p.stream_buf, MJPEG_STREAM_BUF_SIZE);
            if (bytesRead > 0) {
                p.bytes_buffered = bytesRead;
            } else {
                vTaskDelay(pdMS_TO_TICKS(5)); // ✅ 防止空转死循环
                continue;
            }
        }

        // 寻找 JPEG 帧头 0xFF 0xD8
        uint32_t frameStart = 0;
        bool foundStart = false;
        while (frameStart + 1 < p.bytes_buffered) {
            if (p.stream_buf[frameStart] == 0xFF && p.stream_buf[frameStart + 1] == 0xD8) {
                foundStart = true;
                break;
            }
            frameStart++;
        }
        if (!foundStart) {
            p.bytes_buffered = 0;
            vTaskDelay(pdMS_TO_TICKS(2)); 
            continue;
        }

        // 寻找 JPEG 帧尾 0xFF 0xD9
        uint32_t frameEnd = frameStart + 2;
        bool foundEnd = false;
        while (frameEnd + 1 < p.bytes_buffered) {
            if (p.stream_buf[frameEnd] == 0xFF && p.stream_buf[frameEnd + 1] == 0xD9) {
                frameEnd += 2;
                foundEnd = true;
                break;
            }
            frameEnd++;
        }

        // 残缺帧：移到缓冲区前面 + 立即再读
        if (!foundEnd) {
            uint32_t partial = p.bytes_buffered - frameStart;
            memmove(p.stream_buf, p.stream_buf + frameStart, partial);
            p.bytes_buffered = partial;
            if (p.bytes_buffered < MJPEG_STREAM_BUF_SIZE && p.file.available()) {
                int space = MJPEG_STREAM_BUF_SIZE - p.bytes_buffered;
                int bytesRead = p.file.read(p.stream_buf + p.bytes_buffered, space);
                if (bytesRead > 0) p.bytes_buffered += bytesRead;
            }
            vTaskDelay(pdMS_TO_TICKS(1)); // ✅ 防止空转
            continue;
        }
        while (p.decoding && uxQueueMessagesWaiting(p.frame_queue) > 0) {
            vTaskDelay(pdMS_TO_TICKS(1));
        }
        if (!p.decoding) break;
        // 解码当前帧到 psram_buf[decode_idx]
        uint32_t jpegDataLen = frameEnd - frameStart;
        g_decode_target = p.psram_buf[p.decode_idx];

        if (decoder->openRAM(p.stream_buf + frameStart, jpegDataLen, mjpeg_draw_callback)) {
            decoder->setPixelType(RGB565_LITTLE_ENDIAN);
            decoder->decode(0, 0, 0);
            decoder->close();
            p.frame_count++;

            // 尝试将缓冲区索引入队
            int buf_idx = p.decode_idx;
            if (xQueueSend(p.frame_queue, &buf_idx, pdMS_TO_TICKS(100)) == pdTRUE) {
                p.decode_idx = 1 - p.decode_idx;
            } else {
                p.drop_count++;
            }
        }

        //消费已解码数据
        uint32_t remaining = p.bytes_buffered - frameEnd;
        if (remaining > 0) {
            memmove(p.stream_buf, p.stream_buf + frameEnd, remaining);
        }
        p.bytes_buffered = remaining;

        // 定期打印统计
        if (p.frame_count > 0 && p.frame_count % 100 == 0) {
            Serial.printf("[MJPEG_DEC] frames=%u drops=%u\n", p.frame_count, p.drop_count);
        }


        vTaskDelay(pdMS_TO_TICKS(1));
    }

    delete decoder;
    Serial.printf("[MJPEG_DEC] 退出 frames=%u drops=%u\n", p.frame_count, p.drop_count);
    p.decode_task = nullptr;
    vTaskDelete(NULL);
}

// ================= Core 1: LVGL 轮询定时器 =================
static void mjpeg_poll_timer_cb(lv_timer_t* timer) {
    auto& p = mjpeg_player;
    if (!p.playing) return;

    int buf_idx = -1;
    if (xQueueReceive(p.frame_queue, &buf_idx, 0) == pdTRUE) {
        p.frame_dsc.data = (const uint8_t*)p.psram_buf[buf_idx];
        lv_img_set_src(bg_img, &p.frame_dsc);
        lv_obj_invalidate(bg_img);
        lv_refr_now(NULL);
    }                                                                                                                                                                                                                                                                   
}

// ================= 停止 MJPEG 播放 =================
static void stop_mjpeg_playback() {
    auto& p = mjpeg_player;
    if (!p.playing) return;

    Serial.printf("[MJPEG_STOP] 停止播放 frames=%u drops=%u\n", p.frame_count, p.drop_count);
    p.playing = false;

    if (p.poll_timer) {
        lv_timer_del(p.poll_timer);
        p.poll_timer = nullptr;
    }

    // 通知解码任务退出
    p.decoding = false;
    // 等待任务结束
    for (int i = 0; i < 20 && p.decode_task; i++) {
        vTaskDelay(pdMS_TO_TICKS(5));
    }
    if (p.decode_task) {
        Serial.println("[MJPEG_STOP] 强制终止解码任务");
        vTaskDelete(p.decode_task);
        p.decode_task = nullptr;
    }

    if (p.file) p.file.close();
    LittleFS.end();
                    
    if (p.psram_buf[0]) { heap_caps_free(p.psram_buf[0]); p.psram_buf[0] = nullptr; }
    if (p.psram_buf[1]) { heap_caps_free(p.psram_buf[1]); p.psram_buf[1] = nullptr; }
    if (p.stream_buf)   { heap_caps_free(p.stream_buf);   p.stream_buf = nullptr; }

    if (p.frame_queue) {
        int dummy;
        while (xQueueReceive(p.frame_queue, &dummy, 0) == pdTRUE) {}
        vQueueDelete(p.frame_queue);
        p.frame_queue = nullptr;
    }

    // ✅ 恢复 LVGL 刷新周期
    lv_disp_t* disp = lv_disp_get_default();
    if (disp && disp->refr_timer) {
        lv_timer_set_period(disp->refr_timer, p.saved_refr_period);
        Serial.printf("[MJPEG_STOP] LVGL刷新周期恢复: %ums\n", p.saved_refr_period);
    }
    Serial.println("[MJPEG_STOP] 资源已释放");
}

// ================= 启动 MJPEG 播放 =================
static void start_mjpeg_playback(const char* filepath, uint32_t mjpegOffset, uint32_t mjpegSize) {
    auto& p = mjpeg_player;
    if (p.playing) stop_mjpeg_playback();

    Serial.printf("[MJPEG_START] ===== 启动双核双缓冲播放器 =====\n");
    Serial.printf("[MJPEG_START] %s offset=%u size=%u(%uKB)\n",
        filepath, mjpegOffset, mjpegSize, mjpegSize / 1024);

    size_t frame_buf_size = FRAME_BUF_SIZE;
    p.psram_buf[0] = (uint16_t*)heap_caps_malloc(frame_buf_size, MALLOC_CAP_SPIRAM);
    p.psram_buf[1] = (uint16_t*)heap_caps_malloc(frame_buf_size, MALLOC_CAP_SPIRAM);
    p.stream_buf   = (uint8_t*)heap_caps_malloc(MJPEG_STREAM_BUF_SIZE, MALLOC_CAP_SPIRAM);

    if (!p.psram_buf[0] || !p.psram_buf[1] || !p.stream_buf) {
        Serial.println("[MJPEG_START] PSRAM 分配失败!");
        if (p.psram_buf[0]) { heap_caps_free(p.psram_buf[0]); p.psram_buf[0] = nullptr; }
        if (p.psram_buf[1]) { heap_caps_free(p.psram_buf[1]); p.psram_buf[1] = nullptr; }
        if (p.stream_buf)   { heap_caps_free(p.stream_buf);   p.stream_buf = nullptr; }
        return;
    }

    memset(p.psram_buf[0], 0, frame_buf_size);
    memset(p.psram_buf[1], 0, frame_buf_size);
    memset(p.stream_buf, 0, MJPEG_STREAM_BUF_SIZE);

    p.frame_queue = xQueueCreate(1, sizeof(int));

    if (!LittleFS.begin(true, "/littlefs", 10, "littlefs")) {
        Serial.println("[MJPEG_START] LittleFS 挂载失败!");
        goto cleanup_mem;
    }
    p.file = LittleFS.open(filepath, FILE_READ);
    if (!p.file) {
        Serial.println("[MJPEG_START] 文件打开失败!");
        LittleFS.end();
        goto cleanup_mem;
    }

    p.mjpeg_offset = mjpegOffset;
    p.mjpeg_size = mjpegSize;
    p.bytes_buffered = 0;
    p.frame_count = 0;
    p.drop_count = 0;

    //拉长 LVGL 刷新周期
    {
        lv_disp_t* disp = lv_disp_get_default();
        if (disp && disp->refr_timer) {
            p.saved_refr_period = disp->refr_timer->period;
            lv_timer_set_period(disp->refr_timer, 5000);
            Serial.printf("[MJPEG_START] LVGL刷新周期: %u → 5000ms\n", p.saved_refr_period);
        } else {
            p.saved_refr_period = 20;
        }
    }

    p.frame_dsc.header.always_zero = 0;
    p.frame_dsc.header.w = SCREEN_WIDTH;
    p.frame_dsc.header.h = SCREEN_HEIGHT;
    p.frame_dsc.header.cf = LV_IMG_CF_TRUE_COLOR;
    p.frame_dsc.data_size = frame_buf_size;
    p.frame_dsc.data = (const uint8_t*)p.psram_buf[0];

    p.playing = true;
    p.decoding = true;

    p.poll_timer = lv_timer_create(mjpeg_poll_timer_cb, 5, NULL);

    xTaskCreatePinnedToCore(
        mjpeg_decode_task_func, "MJPEGDec",
        8192, NULL, 2,
        &p.decode_task, 0
    );
    Serial.printf("[MJPEG_START] ===== 启动完成 =====\n");
    return;

cleanup_mem:
    heap_caps_free(p.psram_buf[0]); p.psram_buf[0] = nullptr;
    heap_caps_free(p.psram_buf[1]); p.psram_buf[1] = nullptr;
    heap_caps_free(p.stream_buf);   p.stream_buf = nullptr;
    vQueueDelete(p.frame_queue);    p.frame_queue = nullptr;
}



// 初始化纯白图片数据
static void init_white_img_data() {
    if(white_img_data) return;
    size_t sz = SCREEN_WIDTH * SCREEN_HEIGHT * 2;
    white_img_data = (uint8_t*)malloc(sz);
    if(white_img_data) {
        memset(white_img_data, 0xFF, sz);
        white_img_dsc.header.w = SCREEN_WIDTH;
        white_img_dsc.header.h = SCREEN_HEIGHT;
        white_img_dsc.data_size = sz;
        white_img_dsc.header.cf = LV_IMG_CF_TRUE_COLOR;
        white_img_dsc.data = white_img_data;
    }
}
static void reload_wallpaper(int slot_num) {
    // 先停止可能正在播放的 MJPEG
    stop_mjpeg_playback();

    uint8_t* old_wp = wallpaper_data;
    wallpaper_data = nullptr;
    bool loaded = false;
    uint8_t wp_type = WP_TYPE_STATIC;
    uint32_t mjpeg_offset = 0;
    uint32_t mjpeg_size = 0;

    Serial.printf("[WALLPAPER] 加载槽位 %d\n", slot_num);

    if (LittleFS.begin(true, "/littlefs", 10, "littlefs")) {
        char path[32];
        snprintf(path, sizeof(path), "/wallpaper%d.bin", slot_num);
        if (LittleFS.exists(path)) {
            File file = LittleFS.open(path, FILE_READ);
            if (file) {
                Serial.printf("[WALLPAPER] 文件大小: %u bytes\n", file.size());

                // 读取文件头
                wp_header_t header;
                memset(&header, 0, sizeof(header));
                if (file.read((uint8_t*)&header, sizeof(header)) == sizeof(header)) {
                    // 打印头部数据
                    Serial.printf("[WALLPAPER] 头部: magic=%.4s type=%d mjpeg_size=%u\n",
                        header.magic, header.type, header.mjpeg_size);

                    if (memcmp(header.magic, WP_MAGIC, WP_MAGIC_LEN) == 0) {
                        wp_type = header.type;
                        mjpeg_size = header.mjpeg_size;
                        mjpeg_offset = WP_HEADER_SIZE + WP_PREVIEW_SIZE;
                        Serial.printf("[WALLPAPER] 新格式! type=%s mjpeg_offset=%u mjpeg_size=%u\n",
                            wp_type == WP_TYPE_MJPEG ? "MJPEG" : "STATIC",
                            mjpeg_offset, mjpeg_size);
                    } else {
                        Serial.println("[WALLPAPER] 无魔数，旧格式");
                        file.seek(0);
                        wp_type = WP_TYPE_STATIC;
                    }
                } else {
                    Serial.println("[WALLPAPER] 读取头部失败，旧格式");
                    file.seek(0);
                    wp_type = WP_TYPE_STATIC;
                }

                size_t expected_size = WP_PREVIEW_SIZE;
                wallpaper_data = (uint8_t*)malloc(expected_size);
                if (wallpaper_data) {
                    size_t rd = file.read(wallpaper_data, expected_size);
                    Serial.printf("[WALLPAPER] 读取预览: 请求=%u 实际=%u\n", expected_size, rd);
                    if (rd >= expected_size) {
                        loaded = true;
                    }
                }
                file.close();
            } else {
                Serial.println("[WALLPAPER] 无法打开文件!");
            }
        } else {
            Serial.printf("[WALLPAPER] 文件不存在: %s\n", path);
        }
        //在启动MJPEG之前先卸载，让start_mjpeg_playback自己管理
        LittleFS.end();
    } else {
        Serial.println("[WALLPAPER] LittleFS 挂载失败!");
    }

    // 显示预览
    static lv_img_dsc_t img_dsc;
    if (loaded && wallpaper_data) {
        img_dsc.header.w = SCREEN_WIDTH;
        img_dsc.header.h = SCREEN_HEIGHT;
        img_dsc.header.cf = LV_IMG_CF_TRUE_COLOR;
        img_dsc.data = wallpaper_data;
        img_dsc.data_size = WP_PREVIEW_SIZE;
        lv_img_set_src(bg_img, &img_dsc);
        lv_obj_set_style_bg_opa(bg_img, LV_OPA_TRANSP, 0);
        lv_obj_align(bg_img, LV_ALIGN_CENTER, 0, 0);
        lv_obj_t* parent = lv_obj_get_parent(bg_img);
        if(parent) lv_obj_scroll_to(parent, 0, 0, LV_ANIM_OFF);
        Serial.println("[WALLPAPER] 预览已显示");
    } else {
        lv_img_set_src(bg_img, NULL);
        lv_obj_set_style_bg_color(bg_img, lv_color_white(), 0);
        lv_obj_set_style_bg_opa(bg_img, LV_OPA_COVER, 0);
        Serial.println("[WALLPAPER] 无壁纸，显示白色");
    }
    if (old_wp) free(old_wp);

    mjpeg_player.wp_type = wp_type;
    /* 移植修正: 参考工程此处漏了把解析出的 offset/size 存进 player,
     * 而下面的延迟定时器 lambda 无捕获, 只能读 p.mjpeg_offset/p.mjpeg_size,
     * 结果恒为 0 → MJPEG 从文件头(含 16B 头 + 预览)开始解码。
     * 存进去后与参考意图一致: 从预览之后的 MJPEG 数据段开始。 */
    mjpeg_player.mjpeg_offset = mjpeg_offset;
    mjpeg_player.mjpeg_size = mjpeg_size;

    // 如果是 MJPEG 类型，启动播放
    if (loaded && wp_type == WP_TYPE_MJPEG) {
        int delay_slot = slot_num; // 捕获到局部变量
        lv_timer_t* delay_timer = lv_timer_create([](lv_timer_t* t) {
            lv_timer_del(t); // 一次性定时器
            int slot = (int)(uintptr_t)t->user_data;
            auto& p = mjpeg_player;
            if (p.playing) return; // 防重入
            char filepath[32];
            snprintf(filepath, sizeof(filepath), "/wallpaper%d.bin", slot);
            Serial.printf("[WALLPAPER] 延迟启动MJPEG播放: %s offset=%u size=%u\n", 
                filepath, p.mjpeg_offset, p.mjpeg_size);
            start_mjpeg_playback(filepath, p.mjpeg_offset, p.mjpeg_size);
        }, 300, (void*)(uintptr_t)delay_slot);
        lv_timer_set_repeat_count(delay_timer, 1);
    } else {
        Serial.printf("[WALLPAPER] 不启动MJPEG (loaded=%d, type=%d)\n", loaded, wp_type);
    }
}


// 更新选择界面的缩放 (竖向列表: 离中心越远越小)
static void update_wp_layout(lv_obj_t* cont) {
    lv_area_t cont_a;
    lv_obj_get_coords(cont, &cont_a);
    int32_t cont_height = lv_area_get_height(&cont_a);
    int32_t cont_y_center = cont_a.y1 + cont_height / 2;
    int32_t r = cont_height / 2;

    uint32_t child_cnt = lv_obj_get_child_cnt(cont);
    for(uint32_t i = 0; i < child_cnt; i++) {
        lv_obj_t* child = lv_obj_get_child(cont, i);
        lv_area_t child_a;
        lv_obj_get_coords(child, &child_a);
        int32_t child_y_center = child_a.y1 + lv_area_get_height(&child_a) / 2;
        int32_t diff_y = LV_ABS(child_y_center - cont_y_center);
        if(diff_y > r) diff_y = r;

        float t = (float)diff_y / r;
        int32_t scale = ZOOM_CENTER - (int32_t)((ZOOM_CENTER - ZOOM_EDGE) * t);
        lv_img_set_zoom(child, scale);
    }
}

static void wp_scroll_event_cb(lv_event_t* e) {
    update_wp_layout(lv_event_get_target(e));
}

static void close_wp_selector() {
    if(wp_select_cont) {
        lv_obj_del(wp_select_cont);
        wp_select_cont = nullptr;
    }
    for(int i = 0; i < WP_SELECT_COUNT; i++) {
        if(wp_select_bufs[i]) {
            free(wp_select_bufs[i]);
            wp_select_bufs[i] = nullptr;
        }
    }
}

// ================= 上下遮罩动画 =================
// 创建上下两块遮罩板, 各显示壁纸的上半/下半 (子图被面板裁剪)。
// opening=true: 初始铺满屏幕 (待滑开露出列表); false: 初始在屏幕外 (待合拢盖住列表)。
// 注意: 遮罩板显示的是壁纸图像本身, 中间露出的才是后面的选择器列表 (不是黑色)。
static void create_wp_mask(const lv_img_dsc_t* dsc, bool opening) {
    g_mask_opening = opening;
    g_mask_top = lv_obj_create(wp_select_cont);
    g_mask_bot = lv_obj_create(wp_select_cont);
    lv_obj_t* panels[2] = { g_mask_top, g_mask_bot };
    for(int k = 0; k < 2; k++) {
        lv_obj_t* p = panels[k];
        lv_obj_set_size(p, SCREEN_WIDTH, SCREEN_HEIGHT / 2);
        lv_obj_set_style_bg_opa(p, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(p, 0, 0);
        lv_obj_set_style_radius(p, 0, 0);
        lv_obj_set_style_pad_all(p, 0, 0);
        lv_obj_clear_flag(p, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_clear_flag(p, LV_OBJ_FLAG_OVERFLOW_VISIBLE); // 裁剪子图到面板内

        lv_obj_t* im = lv_img_create(p);
        lv_obj_set_size(im, SCREEN_WIDTH, SCREEN_HEIGHT);
        lv_obj_set_pos(im, 0, (k == 0) ? 0 : -(SCREEN_HEIGHT / 2));
        lv_obj_clear_flag(im, LV_OBJ_FLAG_SCROLLABLE);
        lv_img_set_src(im, dsc);
    }
    lv_obj_set_pos(g_mask_top, 0, opening ? 0 : -(SCREEN_HEIGHT / 2));
    lv_obj_set_pos(g_mask_bot, 0, opening ? (SCREEN_HEIGHT / 2) : SCREEN_HEIGHT);
    lv_obj_move_foreground(g_mask_top);
    lv_obj_move_foreground(g_mask_bot);
}

static void wp_mask_anim_cb(void* var, int32_t v) {
    (void)var;
    int32_t half = SCREEN_HEIGHT / 2;
    if(g_mask_opening) {
        lv_obj_set_y(g_mask_top, -v);
        lv_obj_set_y(g_mask_bot, half + v);
    } else {
        lv_obj_set_y(g_mask_top, -half + v);
        lv_obj_set_y(g_mask_bot, SCREEN_HEIGHT - v);
    }
}

static void wp_mask_open_ready_cb(lv_anim_t* a) {
    (void)a;
    if(g_mask_top) { lv_obj_del(g_mask_top); g_mask_top = nullptr; }
    if(g_mask_bot) { lv_obj_del(g_mask_bot); g_mask_bot = nullptr; }
}

static void wp_mask_close_ready_cb(lv_anim_t* a) {
    (void)a;
    reload_wallpaper(g_target_slot);
    if(wp_select_cont) { lv_obj_del(wp_select_cont); wp_select_cont = nullptr; }
    g_mask_top = nullptr;
    g_mask_bot = nullptr;
    for(int i = 0; i < WP_SELECT_COUNT; i++) {
        if(wp_select_bufs[i]) { free(wp_select_bufs[i]); wp_select_bufs[i] = nullptr; }
    }
}

// ================= 点击事件 =================
static void wp_item_click_cb(lv_event_t* e) {
    // 遮罩动画进行中, 忽略再次点击 (防止 enter/exit 动画叠加)
    if (g_mask_top != nullptr) return;
    if (time_sleep_timer) {
        lv_timer_reset(time_sleep_timer);
    }
    lv_obj_t* img = lv_event_get_target(e);
    g_target_slot = (int)(uintptr_t)img->user_data;
    preferences.begin("watch", false);
    preferences.putInt("wp_slot", g_target_slot);
    preferences.end();

    if(g_target_slot < 1 || g_target_slot > WP_SELECT_COUNT ||
       !wp_select_dscs[g_target_slot - 1].data || !wp_select_cont) {
        reload_wallpaper(g_target_slot);
        close_wp_selector();
        return;
    }

    // 退出动画：上下两块遮罩 (显示选中壁纸的上/下半) 从两边向中间合拢盖住列表
    create_wp_mask(&wp_select_dscs[g_target_slot - 1], false);
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, g_mask_top);
    lv_anim_set_time(&a, WP_ANIM_TIME);
    lv_anim_set_values(&a, 0, SCREEN_HEIGHT / 2);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_set_exec_cb(&a, wp_mask_anim_cb);
    lv_anim_set_ready_cb(&a, wp_mask_close_ready_cb);
    lv_anim_start(&a);
}

static void show_wallpaper_selector() {
    init_white_img_data();

    wp_select_cont = lv_obj_create(lv_scr_act());
    lv_obj_set_size(wp_select_cont, SCREEN_WIDTH, SCREEN_HEIGHT);
    lv_obj_align(wp_select_cont, LV_ALIGN_CENTER, 0, 0); 
    lv_obj_set_style_pad_all(wp_select_cont, 0, 0);
    lv_obj_set_style_border_width(wp_select_cont, 0, 0);
    lv_obj_set_style_bg_color(wp_select_cont, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(wp_select_cont, LV_OPA_COVER, 0);
    lv_obj_clear_flag(wp_select_cont, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_add_event_cb(wp_select_cont, [](lv_event_t* e){
        if(lv_event_get_target(e) == wp_select_cont) {
            int cur_slot = 1;
            preferences.begin("watch", true);
            cur_slot = preferences.getInt("wp_slot", 1);
            preferences.end();
            lv_obj_t* list_cont = lv_obj_get_child(wp_select_cont, 0);
            if(list_cont) {
                lv_obj_t* target = lv_obj_get_child(list_cont, cur_slot - 1);
                if(target) {
                    lv_event_send(target, LV_EVENT_CLICKED, NULL);
                    return;
                }
            }
            close_wp_selector(); 
        }
    }, LV_EVENT_CLICKED, NULL);

    lv_obj_t* list_cont = lv_obj_create(wp_select_cont);
    lv_obj_set_size(list_cont, SCREEN_WIDTH, SCREEN_HEIGHT); 
    lv_obj_align(list_cont, LV_ALIGN_CENTER, 0, 0); 
    lv_obj_set_flex_flow(list_cont, LV_FLEX_FLOW_COLUMN);
    lv_obj_add_event_cb(list_cont, wp_scroll_event_cb, LV_EVENT_SCROLL, NULL);
    lv_obj_set_style_bg_opa(list_cont, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(list_cont, 0, 0);
    lv_obj_set_style_pad_row(list_cont, -110, 0); 
    lv_obj_set_style_pad_left(list_cont, 0, 0);
    lv_obj_set_style_pad_right(list_cont, 0, 0);
    lv_obj_set_scroll_dir(list_cont, LV_DIR_VER);
    lv_obj_set_scroll_snap_y(list_cont, LV_SCROLL_SNAP_CENTER);
    lv_obj_set_scrollbar_mode(list_cont, LV_SCROLLBAR_MODE_OFF);

    LittleFS.begin(true, "/littlefs", 10, "littlefs");
    for (int i = 0; i < WP_SELECT_COUNT; i++) {
        lv_obj_t* img = lv_img_create(list_cont);
        char path[32];
        snprintf(path, sizeof(path), "/wallpaper%d.bin", i + 1);
        bool loaded = false;
        if (LittleFS.exists(path)) {
            File file = LittleFS.open(path, FILE_READ);
            if (file) {
                //读取头部，跳过，只取预览
                wp_header_t header;
                if (file.read((uint8_t*)&header, sizeof(header)) == sizeof(header)) {
                    if (memcmp(header.magic, WP_MAGIC, WP_MAGIC_LEN) != 0) {
                        file.seek(0); // 旧格式，从头开始
                    }
                } else {
                    file.seek(0);
                }

                size_t sz = WP_PREVIEW_SIZE;
                if (file.size() >= (WP_HEADER_SIZE + WP_PREVIEW_SIZE) ||  // 新格式
                    file.size() >= sz) {                                   // 旧格式
                    wp_select_bufs[i] = (uint8_t*)malloc(sz);
                    if (wp_select_bufs[i]) {
                        size_t rd = file.read(wp_select_bufs[i], sz);
                        if (rd >= sz) {
                            wp_select_dscs[i].header.w = SCREEN_WIDTH;
                            wp_select_dscs[i].header.h = SCREEN_HEIGHT;
                            wp_select_dscs[i].header.cf = LV_IMG_CF_TRUE_COLOR;
                            wp_select_dscs[i].data_size = sz;
                            wp_select_dscs[i].data = wp_select_bufs[i];
                            lv_img_set_src(img, &wp_select_dscs[i]);
                            loaded = true;
                        }
                    }
                }
                file.close();
            }
        }
        if (!loaded && white_img_data)
            lv_img_set_src(img, &white_img_dsc);
        lv_obj_set_size(img, SCREEN_WIDTH, SCREEN_HEIGHT);
        lv_img_set_pivot(img, SCREEN_WIDTH / 2, SCREEN_HEIGHT / 2);
        lv_img_set_zoom(img, ZOOM_EDGE);
        lv_obj_add_flag(img, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_clear_flag(img, LV_OBJ_FLAG_SCROLLABLE);
        img->user_data = (void*)(uintptr_t)(i + 1);
        lv_obj_add_event_cb(img, wp_item_click_cb, LV_EVENT_CLICKED, NULL);
    }
    lv_obj_set_flex_align(list_cont, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    LittleFS.end();

    lv_obj_update_layout(list_cont);

    int cur_slot = 1;
    preferences.begin("watch", true);
    cur_slot = preferences.getInt("wp_slot", 1);
    preferences.end();
    if(cur_slot >= 1 && cur_slot <= WP_SELECT_COUNT) {
        lv_obj_t* target = lv_obj_get_child(list_cont, cur_slot - 1);
        if(target) lv_obj_scroll_to_view(target, LV_ANIM_OFF);
    }
    update_wp_layout(list_cont);

    // 进入动画：上下两块遮罩 (显示当前壁纸的上/下半) 向两边滑开, 露出选择器列表
    static lv_img_dsc_t enter_dsc;
    enter_dsc.header.always_zero = 0;
    enter_dsc.header.w = SCREEN_WIDTH;
    enter_dsc.header.h = SCREEN_HEIGHT;
    enter_dsc.header.cf = LV_IMG_CF_TRUE_COLOR;
    enter_dsc.data_size = SCREEN_WIDTH * SCREEN_HEIGHT * 2;
    enter_dsc.data = wallpaper_data ? wallpaper_data : white_img_data;
    if(enter_dsc.data) {
        create_wp_mask(&enter_dsc, true);
        lv_anim_t a;
        lv_anim_init(&a);
        lv_anim_set_var(&a, g_mask_top);
        lv_anim_set_time(&a, WP_ANIM_TIME);
        lv_anim_set_values(&a, 0, SCREEN_HEIGHT / 2);
        lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
        lv_anim_set_exec_cb(&a, wp_mask_anim_cb);
        lv_anim_set_ready_cb(&a, wp_mask_open_ready_cb);
        lv_anim_start(&a);
    }
}

// ================= 长按1秒逻辑 =================
static void lp_timer_cb(lv_timer_t* t) {
    lp_timer = nullptr;
    stop_mjpeg_playback();
    
    // ✅ 重置睡眠定时器，防止在壁纸选择器中误触发关机
    if (time_sleep_timer) {
        lv_timer_reset(time_sleep_timer);
    }
    
    show_wallpaper_selector();
}



static void cont_pressed_cb(lv_event_t* e) {
    if(lp_timer) lv_timer_del(lp_timer);
    lp_timer = lv_timer_create(lp_timer_cb, 1000, NULL);
    lv_timer_set_repeat_count(lp_timer, 1);
}

static void cont_released_cb(lv_event_t* e) {
    if(lp_timer) {
        lv_timer_del(lp_timer);
        lp_timer = nullptr;
    }
}

static void cont_scroll_cb(lv_event_t* e) {
    if(lp_timer) {
        lv_timer_del(lp_timer);
        lp_timer = nullptr;
    }
}

// ================= 时钟界面基础逻辑 =================
static void update_time_display() {
    time_t now = time(nullptr);
    struct tm *ptm = gmtime(&now);

    if (ptm == nullptr) {
        if (time_label) lv_label_set_text(time_label, "00:00");
        if (date_label) lv_label_set_text(date_label, "1/1 星期日");
        return;
    }

    int hours = ptm->tm_hour;
    int minutes = ptm->tm_min;
    int month = ptm->tm_mon + 1;
    int day = ptm->tm_mday;
    int weekday = ptm->tm_wday;

    char time_buf[10];
    snprintf(time_buf, sizeof(time_buf), "%02d:%02d", hours, minutes);

    char date_buf[30];
    snprintf(date_buf, sizeof(date_buf), "%d/%d %s", month, day, weekdays_cn[weekday]);

    if (time_label) lv_label_set_text(time_label, time_buf);
    if (date_label) lv_label_set_text(date_label, date_buf);

    if (battery_bar) {
        int pct = atomic_load_int(&battery_percentage);
        pct = constrain(pct, 0, 100);
        int bar_width = (pct * 22) / 100;
        lv_obj_set_width(battery_bar, bar_width);
        lv_color_t bat_color;
        if (pct <= 20) bat_color = lv_palette_main(LV_PALETTE_RED);
        else if (pct <= 30) bat_color = lv_palette_main(LV_PALETTE_AMBER);
        else bat_color = lv_palette_main(LV_PALETTE_GREEN);
        if (atomic_load_bool(&is_charging)) bat_color = lv_palette_main(LV_PALETTE_BLUE);
        lv_obj_set_style_bg_color(battery_bar, bat_color, LV_PART_MAIN);
    }

    if (battery_percent_label) {
        int pct = atomic_load_int(&battery_percentage);
        pct = constrain(pct, 0, 100);
        char buf[8];
        snprintf(buf, sizeof(buf), "%d%%", pct);
        lv_label_set_text(battery_percent_label, buf);
    }
}

static void shutdown_anim_cb(lv_timer_t* timer) {
    static int lin_brightness = -1;
    if (lin_brightness == -1) {
        lin_brightness = atomic_load_int(&current_brightness);
    }
    lin_brightness -= 5;
    if (lin_brightness <= 0) {
        lin_brightness = -1;
        set_mapped_brightness(0);
        atomic_store_int(&current_brightness, 0);
        lv_timer_del(shutdown_timer);
        shutdown_timer = nullptr;
        shutting_down = false;
        board_sleep();
    } else {
        set_mapped_brightness(lin_brightness);
        atomic_store_int(&current_brightness, lin_brightness);
    }
}

static void time_timer_cb(lv_timer_t * timer) {
    update_time_display();
}

static void detect_timer_cb(lv_timer_t * timer) {
    static bool last_state = LOW;
    bool current_state = digitalRead(BUTTON_2);
    if (current_state == LOW && last_state == HIGH) {
        if (shutting_down) {
            last_state = current_state;
            return;
        }
        atomic_store_int(&slow_start_exit_flag, 1);
        shutting_down = true;
        stop_mjpeg_playback();
        shutdown_timer = lv_timer_create(shutdown_anim_cb, 5, NULL);
    }
    last_state = current_state;

    if(digitalRead(BUTTON_1)){
        uint8_t current = atomic_load_int(&current_brightness);
        if (current < 255) current += 5;
        atomic_store_int(&current_brightness, current);
        set_mapped_brightness(current);
        preferences.begin("watch", false);
        preferences.putUChar("brightness", current);
        preferences.end();
    } else if(digitalRead(BUTTON_3)){
        uint8_t current = atomic_load_int(&current_brightness);
        if (current > 1) current -= 5;
        else current = 1;
        atomic_store_int(&current_brightness, current);
        set_mapped_brightness(current);
        preferences.begin("watch", false);
        preferences.putUChar("brightness", current);
        preferences.end();
    }
}

static void time_sleep_timer_cb(lv_timer_t* timer) {
    if (shutting_down) return;
    atomic_store_int(&slow_start_exit_flag, 1);
    shutting_down = true;
    stop_mjpeg_playback();
    shutdown_timer = lv_timer_create(shutdown_anim_cb, 5, NULL);
}
static void cleanup_time_resources() {
    // 先停止 MJPEG 播放
    stop_mjpeg_playback();

    if (time_timer) { lv_timer_del(time_timer); time_timer = nullptr; }
    if (detect_timer) { lv_timer_del(detect_timer); detect_timer = nullptr; }
    if (time_sleep_timer) { lv_timer_del(time_sleep_timer); time_sleep_timer = nullptr; }
    if (lp_timer) { lv_timer_del(lp_timer); lp_timer = nullptr; }
    if (wallpaper_data) { free(wallpaper_data); wallpaper_data = nullptr; }
    if (bg_img) { lv_obj_del(bg_img); bg_img = nullptr; }
    if (time_label) { lv_obj_del(time_label); time_label = nullptr; }
    if (separator_label) { lv_obj_del(separator_label); separator_label = nullptr; }
    if (date_label) { lv_obj_del(date_label); date_label = nullptr; }
    if (battery_bar) { lv_obj_del(battery_bar); battery_bar = nullptr; }
    if (battery_area) { lv_obj_del(battery_area); battery_area = nullptr; }
    if (battery_percent_label) { lv_obj_del(battery_percent_label); battery_percent_label = nullptr; }
    if (battery_cont) { lv_obj_del(battery_cont); battery_cont = nullptr; }
}

static void touch_event_cb(lv_event_t* e) {
    lv_event_code_t code = lv_event_get_code(e);
    lv_indev_t* indev = lv_indev_get_act();
    if (code == LV_EVENT_PRESSED) {
        lv_indev_get_point(indev, &gesture_start);
        gesture_active = true;
    } else if (code == LV_EVENT_RELEASED) {
        if (!gesture_active) return;
        lv_point_t gesture_end;
        lv_indev_get_point(indev, &gesture_end);
        int16_t dy = gesture_end.y - gesture_start.y;
        if (dy > SWIPE_THRESHOLD) {
            // ✅ 壁纸选择器或遮罩动画运行中，忽略退出手势
            if (wp_select_cont != nullptr || g_mask_top != nullptr) {
                gesture_active = false;
                return;
            }
            fs_do_exit();
            cleanup_time_resources();
        }
        gesture_active = false;
    } else if (code == LV_EVENT_SCROLL) {
        lv_point_t vect;
        lv_indev_get_vect(indev, &vect);
        if (vect.y > SWIPE_THRESHOLD) {
            // ✅ 同上，壁纸选择器或遮罩动画运行中忽略
            if (wp_select_cont != nullptr || g_mask_top != nullptr) {
                return;
            }
            fs_do_exit();
            cleanup_time_resources();
        }
    }
}

void fs_create_time(lv_obj_t* container) {
    lv_obj_clear_flag(container, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(container, LV_OBJ_FLAG_SCROLL_ELASTIC);

    bg_img = lv_img_create(container);
    lv_obj_set_size(bg_img, SCREEN_WIDTH, SCREEN_HEIGHT);
    lv_obj_align(bg_img, LV_ALIGN_CENTER, 0, 0);

    int wp_slot = 1;
    preferences.begin("watch", true);
    wp_slot = preferences.getInt("wp_slot", 1);
    preferences.end();

    bool wallpaper_loaded = false;
    uint8_t wp_type = WP_TYPE_STATIC;
    uint32_t mjpeg_offset = 0;
    uint32_t mjpeg_size = 0;

    if (LittleFS.begin(true, "/littlefs", 10, "littlefs")) {
        char path[32];
        snprintf(path, sizeof(path), "/wallpaper%d.bin", wp_slot);
        const char* load_path = LittleFS.exists(path) ? path
                     : (LittleFS.exists("/wallpaper.bin") ? "/wallpaper.bin" : nullptr);
        if (load_path) {
            File file = LittleFS.open(load_path, FILE_READ);
            if (file) {
                // ✅ 读取头部判断类型
                wp_header_t header;
                memset(&header, 0, sizeof(header));
                bool has_header = false;
                if (file.read((uint8_t*)&header, sizeof(header)) == sizeof(header)) {
                    if (memcmp(header.magic, WP_MAGIC, WP_MAGIC_LEN) == 0) {
                        wp_type = header.type;
                        mjpeg_size = header.mjpeg_size;
                        mjpeg_offset = WP_HEADER_SIZE + WP_PREVIEW_SIZE;
                        has_header = true;
                    }
                }
                if (!has_header) {
                    file.seek(0);
                }

                size_t expected_size = WP_PREVIEW_SIZE;
                if (file.size() >= (has_header ? WP_HEADER_SIZE + WP_PREVIEW_SIZE : expected_size)) {
                    wallpaper_data = (uint8_t*)malloc(expected_size);
                    if (wallpaper_data) {
                        size_t rd = file.read(wallpaper_data, expected_size);
                        if (rd >= expected_size) {
                            static lv_img_dsc_t img_dsc;
                            img_dsc.header.w = SCREEN_WIDTH;
                            img_dsc.header.h = SCREEN_HEIGHT;
                            img_dsc.header.cf = LV_IMG_CF_TRUE_COLOR;
                            img_dsc.data = wallpaper_data;
                            img_dsc.data_size = expected_size;
                            lv_img_set_src(bg_img, &img_dsc);
                            wallpaper_loaded = true;
                        }
                    }
                }
                file.close();
            }
        }
        LittleFS.end();
    }

    if (!wallpaper_loaded) {
        lv_obj_set_style_bg_color(bg_img, lv_color_white(), LV_STATE_DEFAULT);
        lv_obj_set_style_bg_opa(bg_img, LV_OPA_COVER, LV_STATE_DEFAULT);
    }

    /* 移植修正: 同 reload_wallpaper, 把解析出的 offset/size 存进 player,
     * 供无捕获的延迟定时器 lambda 使用 (否则恒为 0)。 */
    mjpeg_player.mjpeg_offset = mjpeg_offset;
    mjpeg_player.mjpeg_size = mjpeg_size;

    // ✅ 如果是 MJPEG 类型，启动播放
    if (wallpaper_loaded && wp_type == WP_TYPE_MJPEG) {
        int delay_slot = wp_slot;
        lv_timer_t* delay_timer = lv_timer_create([](lv_timer_t* t) {
            lv_timer_del(t);
            int slot = (int)(uintptr_t)t->user_data;
            auto& p = mjpeg_player;
            if (p.playing) return;
            char filepath[32];
            snprintf(filepath, sizeof(filepath), "/wallpaper%d.bin", slot);
            Serial.printf("[FS_TIME] 延迟启动MJPEG: %s offset=%u size=%u\n", 
                filepath, p.mjpeg_offset, p.mjpeg_size);
            start_mjpeg_playback(filepath, p.mjpeg_offset, p.mjpeg_size);
        }, delayTime, (void*)(uintptr_t)delay_slot);
        delayTime = 600;
        lv_timer_set_repeat_count(delay_timer, 1);
    }

    time_label = lv_label_create(container);
    lv_label_set_text(time_label, "00:00");
    lv_obj_set_style_text_font(time_label, &time_70, LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(time_label, lv_color_black(), LV_STATE_DEFAULT);
    lv_obj_set_style_text_align(time_label, LV_TEXT_ALIGN_CENTER, LV_STATE_DEFAULT);
    lv_obj_align(time_label, LV_ALIGN_TOP_MID, 0, TIME_POS_Y);

    separator_label = lv_label_create(container);
    lv_label_set_text(separator_label, "------------");
    lv_obj_set_style_text_font(separator_label, &chinese_24, LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(separator_label, lv_color_black(), LV_STATE_DEFAULT);
    lv_obj_set_style_text_align(separator_label, LV_TEXT_ALIGN_CENTER, LV_STATE_DEFAULT);
    lv_obj_align(separator_label, LV_ALIGN_TOP_MID, 0, SEPARATOR_POS_Y);

    date_label = lv_label_create(container);
    lv_label_set_text(date_label, "1/1 星期日");
    lv_obj_set_style_text_font(date_label, &chinese_24, LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(date_label, lv_color_black(), LV_STATE_DEFAULT);
    lv_obj_set_style_text_align(date_label, LV_TEXT_ALIGN_CENTER, LV_STATE_DEFAULT);
    lv_obj_align(date_label, LV_ALIGN_TOP_MID, 0, DATE_POS_Y);

    battery_cont = lv_obj_create(container);
    lv_obj_set_size(battery_cont, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_align(battery_cont, LV_ALIGN_TOP_MID, 0, -5);
    lv_obj_set_style_bg_opa(battery_cont, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(battery_cont, 0, 0);
    lv_obj_set_style_pad_all(battery_cont, 0, 0);
    lv_obj_set_flex_flow(battery_cont, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_flex_cross_place(battery_cont, LV_FLEX_ALIGN_CENTER, 0);
    lv_obj_set_style_pad_column(battery_cont, 2, 0);

    battery_percent_label = lv_label_create(battery_cont);
    lv_obj_set_style_text_font(battery_percent_label, &lv_font_montserrat_12, LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(battery_percent_label, lv_color_make(200, 200, 200), LV_STATE_DEFAULT);
    lv_label_set_text(battery_percent_label, "100%");

    battery_area = lv_obj_create(battery_cont);
    lv_obj_set_size(battery_area, 24, 12);
    lv_obj_set_style_bg_opa(battery_area, LV_OPA_0, 0);
    lv_obj_set_style_border_width(battery_area, 1, 0);
    lv_obj_set_style_border_color(battery_area, lv_color_make(200, 200, 200), 0);
    lv_obj_set_style_radius(battery_area, 1, 0);
    lv_obj_set_style_pad_all(battery_area, 0, 0);
    lv_obj_clear_flag(battery_area, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* battery_tip = lv_obj_create(container);
    lv_obj_set_size(battery_tip, 2, 5);
    lv_obj_align_to(battery_tip, battery_area, LV_ALIGN_OUT_RIGHT_MID, 0, 0);
    lv_obj_set_style_bg_color(battery_tip, lv_color_make(200, 200, 200), 0);
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

    update_time_display();

    time_timer = lv_timer_create(time_timer_cb, 1000, NULL);
    detect_timer = lv_timer_create(detect_timer_cb, 50, NULL);
    time_sleep_timer = lv_timer_create(time_sleep_timer_cb, 60000, NULL);

    lv_obj_add_event_cb(container, touch_event_cb, LV_EVENT_ALL, NULL);

    lv_obj_add_event_cb(container, cont_pressed_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(container, cont_released_cb, LV_EVENT_RELEASED, NULL);
    lv_obj_add_event_cb(container, cont_scroll_cb, LV_EVENT_SCROLL, NULL);

    lv_obj_t* hint_label = lv_label_create(container);
    lv_label_set_text(hint_label, "下拉返回");
    lv_obj_set_style_text_font(hint_label, &chinese_24, LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(hint_label, lv_color_make(100, 100, 100), LV_STATE_DEFAULT);
    lv_obj_set_style_text_align(hint_label, LV_TEXT_ALIGN_CENTER, LV_STATE_DEFAULT);
    lv_obj_align(hint_label, LV_ALIGN_BOTTOM_MID, 0, 10);
}