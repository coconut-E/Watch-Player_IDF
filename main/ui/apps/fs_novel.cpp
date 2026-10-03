/*
 * 移植自参考工程 fs_novel.cpp (小说阅读器)
 * 与参考源码逐字一致, 仅一处修正:
 *   - novel_reader_ui() 中定时器句柄写回全局 novel_ui_timer
 *     (参考工程用局部变量遮蔽了同名全局, 导致全局句柄恒为空)
 */
#include "fullscreen_interfaces.h"

extern void fs_do_exit();                    
extern FileSelectionInstance* g_file_selection_instance;     
extern SdFs sd;
LV_FONT_DECLARE(chinese_24);


#define DEBOUNCE_THRESHOLD      20      // BUTTON2 防抖阈值
#define STATE_CHANGE_COOLDOWN    500   // 状态切换冷却时间
#define TIMER_PERIOD             10     // BUTTON 检测定时器周期
#define NOVEL_CONTAINER_WIDTH    236
#define NOVEL_CONTAINER_HEIGHT   238
#define NOVEL_CONTAINER_X_OFFSET -11
#define NOVEL_CONTAINER_Y_OFFSET 8
#define PAGE_BUFFER_SIZE         1024   // 每次读取的字节数

// 翻页 BUTTON 相关
#define PAGE_DEBOUNCE_THRESHOLD  50     // BUTTON 防抖阈值
#define PAGE_COOLDOWN_MS          200   // 翻页冷却时间


#define HISTORY_FILE_PATH        "/novel_hist.txt"   // 历史记录文件路径
#define OFFSET_FIELD_WIDTH       10                // 偏移量字段宽度


// BUTTON2（界面切换）
static bool last_BUTTON2_level = false;
static uint32_t stable_start_BUTTON2 = 0;

// BUTTON1（上一页）
static bool last_BUTTON1_level = false;
static uint32_t stable_start_BUTTON1 = 0;
static uint32_t last_page_turn_BUTTON1 = 0;   // 上次翻页时间

// BUTTON3（下一页）
static bool last_BUTTON3_level = false;
static uint32_t stable_start_BUTTON3 = 0;
static uint32_t last_page_turn_BUTTON3 = 0;

// SD卡状态检测
static bool last_sd_card_state = true;   

static lv_timer_t* gpio_timer = NULL;       // 统一定时器
static lv_obj_t* g_container = NULL;         // 保存父容器指针
static bool novel_state_machine = false;     // 当前是否处于小说阅读界面
static bool need_switch_to_novel = false;    // 是否需要切换到小说阅读界面
static uint32_t last_state_change_ms = 0;    // 上次状态切换时间
static String current_book_path = "";         // 当前打开的小说文件完整路径

// ==================== 小说阅读界面相关静态变量 ====================
static FsFile novel_file;                         // 打开的小说文件对象
static lv_obj_t* novel_label = nullptr;           // 显示小说内容的标签
static lv_obj_t* novel_container = nullptr;       // 小说内容容器
static lv_obj_t* progress_label = nullptr;        // 阅读进度百分比标签
static uint32_t file_position = 0;                 // 当前文件读取位置（下一个要读取的字节）
static uint32_t file_total_size = 0;               // 文件总字节数
static bool file_opened = false;                    // 文件是否已打开

static lv_obj_t* time_label_novel = nullptr;
static lv_obj_t* percent_label_novel = nullptr;
static lv_obj_t* battery_area_novel = nullptr;
static lv_obj_t* battery_bar_novel = nullptr;
static lv_obj_t* battery_tip_novel = nullptr;
static lv_timer_t* novel_ui_timer = nullptr;

// ==================== 历史记录相关变量 ====================
static String current_file_name = "";              // 当前小说文件名（不含路径）
static uint32_t current_page_start = 0;             // 当前显示页的起始偏移

// ==================== 函数声明（静态） ====================
static void gpio_check_cb(lv_timer_t* timer);
static void fs_cleanup_novel_gpio_timer(void);
static void novel_file_selected_cb(const char* filename, void* user_data);
static void clean_novel_interface(void);
static int get_utf8_char_len(uint8_t first_byte);
static String read_1kb_with_utf8_safe(void);
static bool open_novel_file(void);
static void load_next_page(void);
static void load_prev_page(void);
static void update_reading_progress(void);       

// ==================== 历史记录函数声明 ====================
static void save_reading_progress(void);
static uint32_t load_reading_progress(const String& filename);

// 外部接口函数声明
void novel_reader_ui(lv_obj_t* parent);

// ==================== GPIO 检测定时器回调 ====================
static void gpio_check_cb(lv_timer_t* timer) {
    uint32_t now = lv_tick_get();

    // SD卡拔出检测
    bool current_sd_state = false;
    // 线程安全读取SD卡状态
    if (xSemaphoreTake(sd_state_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        current_sd_state = sd_card_inserted;
        xSemaphoreGive(sd_state_mutex);
    }

    // 检测下降沿（t→f）：SD卡被拔出
    if (last_sd_card_state == true && current_sd_state == false) {
        Serial.println("⚠️ 检测到SD卡拔出，清理小说资源并返回");
        // 调用返回函数
        fs_do_exit();
        
        // 清理所有小说相关资源
        clean_novel_interface();       // 清理小说阅读界面
        destroy_file_selection_list(); // 销毁文件选择器
        fs_cleanup_novel_gpio_timer(); // 清理定时器
        
        
        // 更新状态变量
        novel_state_machine = false;
        need_switch_to_novel = false;
        last_sd_card_state = current_sd_state; // 更新最后状态
        return; // 直接返回，不执行后续逻辑
    }
    // 更新SD卡状态记录
    last_sd_card_state = current_sd_state;


    //检测 BUTTON2
    bool BUTTON2 = digitalRead(BUTTON_2);
    if (BUTTON2 != last_BUTTON2_level) {
        stable_start_BUTTON2 = now;
        last_BUTTON2_level = BUTTON2;
    }
    else if (BUTTON2 == 1 && (now - stable_start_BUTTON2) >= DEBOUNCE_THRESHOLD) {
        if (now - last_state_change_ms > STATE_CHANGE_COOLDOWN) {
            last_state_change_ms = now;

            if (novel_state_machine) {
                // 当前在小说阅读界面 -> 退出到文件选择器
                if (fullscreen_container) lv_obj_move_foreground(fullscreen_container);
                clean_novel_interface();          
                file_selection_create(g_container, FILE_TYPE_NOVEL,
                                      novel_file_selected_cb, NULL);
                novel_state_machine = false;
            } else {
                // 当前在文件选择器界面 -> 退出整个功能
                fs_do_exit();
                destroy_file_selection_list();
                
                if (gpio_timer) {
                    lv_timer_del(gpio_timer);
                    gpio_timer = NULL;
                    LV_LOG_USER("GPIO检测定时器已自删");
                }
                return;  // 定时器已删除，直接返回
            }
        }
    }

    // 检测 BUTTON1（上一页）
    if (novel_state_machine) {
        bool BUTTON1 = digitalRead(BUTTON_1);
        if (BUTTON1 != last_BUTTON1_level) {
            stable_start_BUTTON1 = now;
            last_BUTTON1_level = BUTTON1;
        }
        else if (BUTTON1 == 1 && (now - stable_start_BUTTON1) >= PAGE_DEBOUNCE_THRESHOLD) {
            if (now - last_page_turn_BUTTON1 > PAGE_COOLDOWN_MS) {
                last_page_turn_BUTTON1 = now;
                load_prev_page();        
            }
        }

        // 检测 BUTTON3
        bool BUTTON3 = digitalRead(BUTTON_3);
        if (BUTTON3 != last_BUTTON3_level) {
            stable_start_BUTTON3 = now;
            last_BUTTON3_level = BUTTON3;
        }
        else if (BUTTON3 == 1 && (now - stable_start_BUTTON3) >= PAGE_DEBOUNCE_THRESHOLD) {
            if (now - last_page_turn_BUTTON3 > PAGE_COOLDOWN_MS) {
                last_page_turn_BUTTON3 = now;
                load_next_page();         
            }
        }
    }

    // 处理文件选择完成后的切换请求
    if (need_switch_to_novel) {
        destroy_file_selection_list();                    // 销毁文件选择器
        novel_reader_ui(g_container);                     // 创建小说阅读界面
        novel_state_machine = true;
        need_switch_to_novel = false;
    }
}

// ==================== GPIO 定时器清理 ====================
static void fs_cleanup_novel_gpio_timer(void) {
    if (gpio_timer) {
        lv_timer_del(gpio_timer);
        gpio_timer = NULL;
        LV_LOG_USER("小说界面GPIO定时器已清理");
    }
}

// ==================== 小说文件选择回调 ====================
static void novel_file_selected_cb(const char* filename, void* user_data) {
    if (strcmp(filename, SCAN_SPECIAL_FILENAME) == 0) {
        Serial.println("用户选择重新扫描小说文件");
        // 返回主界面
        fs_do_exit();
        // 清理小说界面（关闭文件、保存进度、删除UI）
        clean_novel_interface();
        // 销毁文件选择器
        destroy_file_selection_list();
        // 清理 GPIO 定时器
        fs_cleanup_novel_gpio_timer(); 
        // 设置 SD 卡重新扫描标志
        if (xSemaphoreTake(sd_state_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
            sd_force_scan = true;
            xSemaphoreGive(sd_state_mutex);
        }

        return;
    }
    
    Serial.printf("选中小说文件：%s\n", filename);
    current_book_path = String("/小说/") + filename;
    current_file_name = String(filename);
    need_switch_to_novel = true;
}

// ==================== 创建小说文件选择界面 ====================
void fs_create_novel(lv_obj_t* container) {
    g_container = container;

    // 确保之前的定时器已清理
    fs_cleanup_novel_gpio_timer();

    // 初始化 按钮 状态
    last_BUTTON2_level = digitalRead(BUTTON_2);
    stable_start_BUTTON2 = lv_tick_get();
    last_BUTTON1_level = digitalRead(BUTTON_1);
    stable_start_BUTTON1 = lv_tick_get();
    last_BUTTON3_level = digitalRead(BUTTON_3);
    stable_start_BUTTON3 = lv_tick_get();

    // 初始化SD卡状态
    if (xSemaphoreTake(sd_state_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        last_sd_card_state = sd_card_inserted;
        xSemaphoreGive(sd_state_mutex);
    } else {
        last_sd_card_state = true; // 默认初始化为已插入
    }

    // 创建统一 GPIO 检测定时器
    gpio_timer = lv_timer_create(gpio_check_cb, TIMER_PERIOD, NULL);
    if (!gpio_timer) {
        LV_LOG_WARN("GPIO检测定时器创建失败");
    } else {
        LV_LOG_USER("GPIO检测定时器已启动");
    }

    // 创建文件选择器（小说类型）
    g_file_selection_instance = file_selection_create(
        container,
        FILE_TYPE_NOVEL,
        novel_file_selected_cb,
        NULL
    );

    LV_LOG_USER("小说文件选择界面创建完成");
}

// ==================== 清理小说功能（外部接口） ====================
void fs_cleanup_novel(void) {
    destroy_file_selection_list();
    fs_cleanup_novel_gpio_timer();
}

// ==================== 小说阅读界面相关函数 ====================

// 获取 UTF-8 字符字节数
static int get_utf8_char_len(uint8_t first_byte) {
    if ((first_byte & 0x80) == 0x00) return 1;
    if ((first_byte & 0xE0) == 0xC0) return 2;
    if ((first_byte & 0xF0) == 0xE0) return 3;
    if ((first_byte & 0xF8) == 0xF0) return 4;
    return 1;   // 无效字节，按单字节处理
}

// 安全读取 1KB 数据（避免截断 UTF-8 字符）
static String read_1kb_with_utf8_safe(void) {
    uint8_t buffer[PAGE_BUFFER_SIZE];
    String result = "";

    if (!novel_file || !file_opened) return result;

    uint32_t start_pos = novel_file.position();
    int bytes_read = novel_file.read(buffer, PAGE_BUFFER_SIZE);
    if (bytes_read <= 0) return result;

    size_t valid_bytes = bytes_read;
    if (bytes_read == PAGE_BUFFER_SIZE) {
        // 从末尾向前查找完整的 UTF-8 字符边界
        for (int i = bytes_read - 1; i >= 0; i--) {
            uint8_t c = buffer[i];
            if ((c & 0xC0) != 0x80) {          // 不是后续字节
                int char_len = get_utf8_char_len(c);
                if (i + char_len > bytes_read) {
                    valid_bytes = i;            // 截断不完整的字符
                    break;
                } else {
                    valid_bytes = bytes_read;   // 最后一个字符完整
                    break;
                }
            }
        }
    }

    // 更新文件指针
    uint32_t new_pos = start_pos + valid_bytes;
    novel_file.seek(new_pos);
    file_position = new_pos;

    // 将有效数据转换为 String（保留所有字符）
    for (size_t i = 0; i < valid_bytes; i++) {
        char c = (char)buffer[i];
        // 保留控制字符（如换行）和可打印字符，其余替换为空格（可根据需要调整）
        if (c == '\n' || c == '\t' || c == '\r') {
            result += c;
        } else if (c >= 32 && c <= 126) {
            result += c;
        } else if ((buffer[i] & 0x80) != 0) {
            result += c;   // UTF-8 多字节字符直接保留
        } else {
            result += ' ';
        }
    }
    return result;
}

// 保存阅读进度
// ==================== 保存阅读进度 ====================
static void save_reading_progress(void) {
    if (!file_opened || current_file_name.length() == 0) {
        Serial.println("⚠️ 没有打开的小说文件，不保存进度");
        return;
    }

    // 要保存的偏移量：当前显示页的起始偏移
    uint32_t offset_to_save = current_page_start;

    // 挂载 LittleFS
    if (!LittleFS.begin(true, "/littlefs", 10, "littlefs")) {
        Serial.println("❌ LittleFS 挂载失败");
        return;
    }

    // 构建待查找的行前缀
    String target_prefix = current_file_name + ":";
    size_t prefix_len = target_prefix.length();

    // 用于暂存找到的位置信息
    bool found = false;
    uint32_t offset_field_pos = 0;

    // 尝试以 "r+" 模式打开（读写，不截断）
    File history_file = LittleFS.open(HISTORY_FILE_PATH, "r+");
    
    if (history_file) {
        // 逐行扫描查找已有记录
        history_file.seek(0);
        while (history_file.available()) {
            uint32_t line_start_pos = history_file.position();
            String line = history_file.readStringUntil('\n');
            if (line.length() == 0) continue;
            
            if (line.startsWith(target_prefix)) {
                found = true;
                offset_field_pos = line_start_pos + prefix_len;
                break;
            }
        }
    }

    // 准备偏移量字符串：左对齐，补空格到10位
    char offset_str[OFFSET_FIELD_WIDTH + 1];
    snprintf(offset_str, sizeof(offset_str), "%-10lu", offset_to_save);

    if (found && history_file) {
        // 找到了记录，定位到偏移字段，覆盖写入10个字符
        history_file.seek(offset_field_pos);
        history_file.write((uint8_t*)offset_str, OFFSET_FIELD_WIDTH);
        history_file.close();
        Serial.printf("✅ 更新进度：%s 偏移 %lu\n", current_file_name.c_str(), offset_to_save);
    } else {
        // 没找到记录或文件不存在，关闭之前可能打开的文件
        if (history_file) history_file.close();
        
        // 使用 "a" 模式打开（不存在会创建，存在会在末尾追加）
        history_file = LittleFS.open(HISTORY_FILE_PATH, "a");
        if (history_file) {
            String new_line = target_prefix + offset_str + "\n";
            history_file.write((uint8_t*)new_line.c_str(), new_line.length());
            history_file.close();
            Serial.printf("✅ 新增进度：%s 偏移 %lu\n", current_file_name.c_str(), offset_to_save);
        } else {
            Serial.println("❌ 无法创建/打开历史记录文件");
        }
    }

    // 卸载 LittleFS
    LittleFS.end();
}


// ==================== 加载阅读进度 ====================
static uint32_t load_reading_progress(const String& filename) {
    if (filename.length() == 0) return 0;

    // 挂载 LittleFS
    if (!LittleFS.begin(true, "/littlefs", 10, "littlefs")) {
        Serial.println("ℹ️ LittleFS 挂载失败，从0开始");
        return 0;
    }

    // 以只读模式打开
    File history_file = LittleFS.open(HISTORY_FILE_PATH, "r");
    if (!history_file) {
        Serial.println("ℹ️ 历史记录文件不存在，从0开始");
        LittleFS.end();
        return 0;
    }

    String target_prefix = filename + ":";
    size_t prefix_len = target_prefix.length();

    history_file.seek(0);
    while (history_file.available()) {
        String line = history_file.readStringUntil('\n');
        if (line.startsWith(target_prefix)) {
            // 提取偏移部分（冒号后的10个字符）
            if (line.length() > prefix_len) {
                String offset_part = line.substring(prefix_len);
                // 去除末尾可能的空格和换行
                offset_part.trim();
                uint32_t offset = offset_part.toInt();
                history_file.close();
                LittleFS.end();
                Serial.printf("📖 加载历史进度：%s 偏移 %lu\n", filename.c_str(), offset);
                return offset;
            }
        }
    }
    
    history_file.close();
    LittleFS.end();
    Serial.printf("ℹ️ 无历史记录，%s 从0开始\n", filename.c_str());
    return 0;
}


// ==================== 打开小说文件（加载历史进度 + 获取文件总大小） ====================
static bool open_novel_file(void) {
    novel_file = sd.open(current_book_path.c_str(), O_RDONLY);
    if (!novel_file) {
        Serial.println("❌ 无法打开小说文件");
        return false;
    }

    //获取文件总大小
    file_total_size = novel_file.size();
    Serial.printf("📄 文件总大小：%lu 字节\n", file_total_size);

    // 尝试加载历史进度
    uint32_t saved_offset = load_reading_progress(current_file_name);
    if (saved_offset > 0) {
        // 定位到保存的偏移（防止偏移超过文件大小）
        if (saved_offset <= file_total_size) {
            if (novel_file.seek(saved_offset)) {
                file_position = saved_offset;
                current_page_start = saved_offset; // 当前页起始即为保存的偏移
                Serial.printf("🔍 跳转到历史偏移 %lu\n", saved_offset);
            } else {
                // 定位失败，回退到0
                file_position = 0;
                current_page_start = 0;
                novel_file.seek(0);
                Serial.println("⚠️ 历史偏移定位失败，从0开始");
            }
        } else {
            // 历史偏移超过文件大小，重置为0
            file_position = 0;
            current_page_start = 0;
            novel_file.seek(0);
            Serial.println("⚠️ 历史偏移超过文件大小，从0开始");
        }
    } else {
        file_position = 0;
        current_page_start = 0;
    }

    file_opened = true;
    Serial.println("✅ 小说文件打开成功");
    return true;
}

// ==================== 更新阅读进度百分比显示 ====================
static void update_reading_progress(void) {
    if (!progress_label || !file_opened) return;

    float progress = 0.0f;
    // 安全计算百分比，避免除零错误
    if (file_total_size > 0) {
        // 使用当前页起始位置计算进度，更符合阅读体验
        progress = (float)current_page_start / file_total_size * 100.0f;
        // 防止进度超过100%
        if (progress > 100.0f) progress = 100.0f;
    }

    // 格式化输出，保留两位小数
    char progress_str[20];
    snprintf(progress_str, sizeof(progress_str), "%.2f%%", progress);
    
    // 更新标签文本
    lv_label_set_text(progress_label, progress_str);
}

// ==================== 加载下一页（current_page_start + 更新进度） ====================
static void load_next_page(void) {
    if (!file_opened && !open_novel_file()) return;

    // 记录读取前的偏移（即当前页起始）
    uint32_t old_pos = novel_file.position();
    String content = read_1kb_with_utf8_safe();
    if (content.length() == 0) {
        lv_label_set_text(novel_label, "📖 已读完所有内容");
        // 最后一页强制显示100%
        current_page_start = file_total_size;
        update_reading_progress();
        return;
    }

    // 更新当前页起始为 old_pos（即刚读取的页面的起始）
    current_page_start = old_pos;

    lv_label_set_text(novel_label, content.c_str());
    lv_obj_scroll_to_y(novel_container, 0, LV_ANIM_OFF);
    Serial.printf("当前页起始: %lu, 下一页位置: %lu\n", current_page_start, file_position);
    
    // 更新进度显示
    update_reading_progress();
}

// ==================== 加载上一页（更新 current_page_start + 更新进度） ====================
static void load_prev_page(void) {
    if (!file_opened || file_position <= PAGE_BUFFER_SIZE) return;

    uint32_t new_pos = (file_position > 2 * PAGE_BUFFER_SIZE) ?
                        file_position - 2 * PAGE_BUFFER_SIZE : 0;
    novel_file.seek(new_pos);
    file_position = new_pos;

    // 重新读取一页，并设置当前页起始
    uint32_t old_pos = novel_file.position();
    String content = read_1kb_with_utf8_safe();
    if (content.length() == 0) return;

    current_page_start = old_pos;
    lv_label_set_text(novel_label, content.c_str());
    lv_obj_scroll_to_y(novel_container, 0, LV_ANIM_OFF);
    Serial.printf("上一页，当前页起始: %lu\n", current_page_start);
    
    // 更新进度显示
    update_reading_progress();
}

// ==================== 清理小说界面资源 ====================
static void clean_novel_interface(void) {
    if (file_opened) {
        save_reading_progress();
        novel_file.close();
        file_opened = false;
        file_position = 0;
        current_page_start = 0;
        file_total_size = 0;
        Serial.println("📂 小说文件已关闭，进度已保存");
    }
    if (novel_label) {
        lv_obj_del(novel_label);
        novel_label = nullptr;
    }
    if (novel_container) {
        lv_obj_del(novel_container);
        novel_container = nullptr;
    }
    if (progress_label) {
        lv_obj_del(progress_label);
        progress_label = nullptr;
    }
        if (novel_ui_timer) {
        lv_timer_del(novel_ui_timer);
        novel_ui_timer = nullptr;
    }
    if (time_label_novel) {
        lv_obj_del(time_label_novel);
        time_label_novel = nullptr;
    }
    if (percent_label_novel) {
        lv_obj_del(percent_label_novel);
        percent_label_novel = nullptr;
    }
    if (battery_area_novel) {
        lv_obj_del(battery_area_novel);
        battery_area_novel = nullptr;
    }
    if (battery_tip_novel) {
        lv_obj_del(battery_tip_novel);
        battery_tip_novel = nullptr;
    }
}
static void novel_ui_timer_callback(lv_timer_t* t) {
    // 更新时间
    time_t now = time(nullptr);
    struct tm* ptm = gmtime(&now);
    if (ptm && time_label_novel) {
        char buf[6];
        snprintf(buf, sizeof(buf), "%02d:%02d", ptm->tm_hour, ptm->tm_min);
        lv_label_set_text(time_label_novel, buf);
    }

    // 更新电池
    if (percent_label_novel && battery_bar_novel) {
        int pct = atomic_load_int(&battery_percentage);
        pct = constrain(pct, 0, 100); 
        char pct_buf[8];
        snprintf(pct_buf, sizeof(pct_buf), "%d%%", pct);
        lv_label_set_text(percent_label_novel, pct_buf);

        int bar_width = (pct * 22) / 100;   
        lv_obj_set_width(battery_bar_novel, bar_width);

        lv_color_t bat_color;
        if (pct <= 20)      bat_color = lv_palette_main(LV_PALETTE_RED);
        else if (pct <= 30) bat_color = lv_palette_main(LV_PALETTE_AMBER);
        else                bat_color = lv_palette_main(LV_PALETTE_GREEN);
        if (atomic_load_bool(&is_charging)) bat_color = lv_palette_main(LV_PALETTE_BLUE);
        lv_obj_set_style_bg_color(battery_bar_novel, bat_color, LV_PART_MAIN);
    }
}
// ==================== 创建小说阅读界面 ====================
void novel_reader_ui(lv_obj_t* parent) {
    // 创建小说内容容器
    novel_container = lv_obj_create(parent);
    lv_obj_set_size(novel_container, NOVEL_CONTAINER_WIDTH, NOVEL_CONTAINER_HEIGHT);
    lv_obj_set_pos(novel_container, NOVEL_CONTAINER_X_OFFSET, NOVEL_CONTAINER_Y_OFFSET);
    lv_obj_set_style_bg_color(novel_container, lv_color_make(20, 20, 30), LV_STATE_DEFAULT);
    lv_obj_set_style_border_color(novel_container, lv_color_make(60, 60, 80), LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(novel_container, 2, LV_STATE_DEFAULT);
    lv_obj_set_style_radius(novel_container, 10, LV_STATE_DEFAULT);
    lv_obj_set_style_pad_all(novel_container, 2, LV_STATE_DEFAULT);
    lv_obj_set_scrollbar_mode(novel_container, LV_SCROLLBAR_MODE_AUTO);   // 允许滚动

    // 创建文本标签
    novel_label = lv_label_create(novel_container);
    lv_obj_set_width(novel_label, 220);
    lv_obj_set_style_text_font(novel_label, &chinese_24, LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(novel_label, lv_color_white(), LV_STATE_DEFAULT);
    lv_obj_set_style_text_line_space(novel_label, 0, LV_STATE_DEFAULT);
    lv_label_set_long_mode(novel_label, LV_LABEL_LONG_WRAP);   // 自动换行

    // 进度标签
    progress_label = lv_label_create(parent);
    // 位置
    lv_obj_align_to(progress_label, novel_container, LV_ALIGN_OUT_BOTTOM_MID, 0, 1);
    lv_obj_set_style_text_color(progress_label, lv_color_make(255, 255, 0), LV_STATE_DEFAULT);
    lv_obj_set_style_text_font(progress_label, &lv_font_montserrat_16, LV_STATE_DEFAULT);
    lv_label_set_text(progress_label, "0.00%");

    //左上角时间显示
    time_label_novel = lv_label_create(parent);
    lv_obj_set_style_text_font(time_label_novel, &lv_font_montserrat_12, LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(time_label_novel, lv_color_white(), LV_STATE_DEFAULT);
    lv_label_set_text(time_label_novel, "00:00");
    lv_obj_align(time_label_novel, LV_ALIGN_TOP_LEFT, 5, -7);

    //右上角电池百分比 
    percent_label_novel = lv_label_create(parent);
    lv_obj_set_style_text_font(percent_label_novel, &lv_font_montserrat_12, LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(percent_label_novel, lv_color_white(), LV_STATE_DEFAULT);
    lv_label_set_text(percent_label_novel, "100%");

    //电池图标
    battery_area_novel = lv_obj_create(parent);
    lv_obj_set_size(battery_area_novel, 24, 12);
    lv_obj_set_style_bg_opa(battery_area_novel, LV_OPA_0, 0);
    lv_obj_set_style_border_width(battery_area_novel, 1, 0);
    lv_obj_set_style_border_color(battery_area_novel, lv_color_white(), 0);
    lv_obj_set_style_radius(battery_area_novel, 1, 0);
    lv_obj_set_style_pad_all(battery_area_novel, 0, 0);
    lv_obj_clear_flag(battery_area_novel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(battery_area_novel, LV_ALIGN_TOP_RIGHT, -5, -5);

    // 电池正极小凸起
    battery_tip_novel = lv_obj_create(parent);
    lv_obj_set_size(battery_tip_novel, 2, 5);
    lv_obj_align_to(battery_tip_novel, battery_area_novel, LV_ALIGN_OUT_RIGHT_MID, 0, 0);
    lv_obj_set_style_bg_color(battery_tip_novel, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(battery_tip_novel, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(battery_tip_novel, 0, 0);
    lv_obj_set_style_radius(battery_tip_novel, 1, 0);
    lv_obj_align_to(percent_label_novel, battery_area_novel, LV_ALIGN_OUT_LEFT_MID, 0, 0);
    
    // 内部电量条
    battery_bar_novel = lv_obj_create(battery_area_novel);
    lv_obj_set_size(battery_bar_novel, 0, lv_pct(100));
    lv_obj_set_align(battery_bar_novel, LV_ALIGN_LEFT_MID);
    lv_obj_set_style_border_width(battery_bar_novel, 0, 0);
    lv_obj_set_style_bg_opa(battery_bar_novel, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(battery_bar_novel, 0, 0);
    lv_obj_clear_flag(battery_bar_novel, LV_OBJ_FLAG_SCROLLABLE);


    novel_ui_timer_callback(NULL);

    novel_ui_timer = lv_timer_create(novel_ui_timer_callback, 1000, NULL);

    if (sliding_container) lv_obj_move_foreground(sliding_container);

    // 打开文件并加载第一页
    if (open_novel_file()) {
        uint32_t start_pos = novel_file.position();
        String content = read_1kb_with_utf8_safe();
        if (content.length() > 0) {
            current_page_start = start_pos;
            lv_label_set_text(novel_label, content.c_str());
            lv_obj_scroll_to_y(novel_container, 0, LV_ANIM_OFF);
            Serial.printf("初始页起始: %lu\n", current_page_start);
        } else {
            lv_label_set_text(novel_label, "📖 文件为空");
        }
        update_reading_progress();
    } else {
        lv_label_set_text(novel_label, "❌ 无法加载小说文件\n请检查SD卡和文件路径");
        update_reading_progress();
    }
}