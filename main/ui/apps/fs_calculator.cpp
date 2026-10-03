#include "fullscreen_interfaces.h"
#include <string>
#include <vector>
#include <stack>
#include <cmath>
#include <iomanip>
#include <sstream>

LV_FONT_DECLARE(chinese_24);

static lv_timer_t * io_exit_timer = NULL;

namespace CalcEngine {

    enum ResultType {
        OK,
        ERROR_SYNTAX,  // 语法错误
        ERROR_DIV_0,   // 除以零
        ERROR_OVERFLOW // 数值溢出
    };

    struct CalcResult {
        double value;
        ResultType type;
    };

    //判断是否为运算符
    bool is_op(char c) {
        return c == '+' || c == '-' || c == '*' || c == '/';
    }

    //获取优先级
    int get_priority(char op) {
        if (op == '+' || op == '-') return 1;
        if (op == '*' || op == '/') return 2;
        return 0;
    }

    //执行单步运算
    double apply_op(double a, double b, char op, bool &ok) {
        switch (op) {
            case '+': return a + b;
            case '-': return a - b;
            case '*': return a * b;
            case '/': 
                if (std::abs(b) < 1e-9) { ok = false; return 0; } // 简单的除零检查
                return a / b;
        }
        return 0;
    }

    // 核心计算函数
    CalcResult evaluate(std::string expr) {
        //替换中文符号，处理UTF-8多字节字符
        std::string clean_expr = "";
        for (size_t i = 0; i < expr.length(); ++i) {
            unsigned char c = (unsigned char)expr[i];
            if (c == ' ') continue; // 忽略空格
            
            // 简单处理中文乘除号 
            if (expr.substr(i, 2) == "×") { clean_expr += '*'; i++; continue; }
            if (expr.substr(i, 2) == "÷") { clean_expr += '/'; i++; continue; }
            
            // 保留数字、点、英文运算符和百分号
            if (isdigit(c) || c == '.' || is_op(c) || c == '%') { //保留%
                clean_expr += (char)c;
            }
        }

        if (clean_expr.empty()) return {0, OK};

        std::stack<double> values;
        std::stack<char> ops;

        for (size_t i = 0; i < clean_expr.length(); i++) {
            // 处理数字
            if (isdigit(clean_expr[i]) || clean_expr[i] == '.') {
                std::string num_str;
                // 向后读取完整的数字
                while (i < clean_expr.length() && (isdigit(clean_expr[i]) || clean_expr[i] == '.')) {
                    num_str += clean_expr[i];
                    i++;
                }
                
                double val = 0;
                try {
                    val = std::stod(num_str);
                } catch (...) {
                    return {0, ERROR_SYNTAX};
                }

                if (i < clean_expr.length() && clean_expr[i] == '%') {
                    val = val / 100.0; // 百分数转换
                    i++; // 跳过%
                }

                values.push(val);
                i--; // 回退一步
            }
            // 处理运算符
            else if (is_op(clean_expr[i])) {
                while (!ops.empty() && get_priority(ops.top()) >= get_priority(clean_expr[i])) {
                    if (values.size() < 2) return {0, ERROR_SYNTAX}; // 防止下溢
                    
                    double val2 = values.top(); values.pop();
                    double val1 = values.top(); values.pop();
                    
                    bool calc_ok = true;
                    double res = apply_op(val1, val2, ops.top(), calc_ok);
                    ops.pop();
                    
                    if (!calc_ok) return {0, ERROR_DIV_0};
                    values.push(res);
                }
                ops.push(clean_expr[i]);
            }
            // 非法字符
            else if (clean_expr[i] != '%') { // 排除合法的%
                return {0, ERROR_SYNTAX};
            }
        }

        // 处理剩余的运算符
        while (!ops.empty()) {
            if (values.size() < 2) return {0, ERROR_SYNTAX}; // 防止下溢
            
            double val2 = values.top(); values.pop();
            double val1 = values.top(); values.pop();
            
            bool calc_ok = true;
            double res = apply_op(val1, val2, ops.top(), calc_ok);
            ops.pop();

            if (!calc_ok) return {0, ERROR_DIV_0};
            values.push(res);
        }

        if (values.empty()) return {0, OK};
        if (values.size() > 1) return {0, ERROR_SYNTAX}; // 栈里剩多余数字，说明缺运算符

        double final_val = values.top();
        
        // 检查无穷大 (溢出)
        if (std::isinf(final_val)) return {0, ERROR_OVERFLOW};
        
        return {final_val, OK};
    }
}

// ==================== 退出逻辑====================
static void calculator_exit_handler() {
    if (io_exit_timer) {
        lv_timer_del(io_exit_timer);
        io_exit_timer = NULL;
    }
    fs_do_exit();
}

static void check_timer_cb(lv_timer_t * t) {
    if (digitalRead(BUTTON_2) == HIGH) {
        static uint8_t confirm_cnt = 0;
        if(++confirm_cnt > 1) {
            confirm_cnt = 0;
            calculator_exit_handler();
        }
    }
}

// ==================== UI 辅助函数====================
static bool is_input_operator(const char * s) {
    if (strcmp(s, "+") == 0 || strcmp(s, "-") == 0 || 
        strcmp(s, "×") == 0 || strcmp(s, "÷") == 0) 
        return true;
    return false;
}

// 判断字符是否为数字
static bool is_digit_char(const char c) {
    return c >= '0' && c <= '9';
}

// ==================== 按钮矩阵事件回调  ====================
static void btnm_event_cb(lv_event_t * e) {
    lv_obj_t * obj = lv_event_get_target(e);
    lv_obj_t * ta = (lv_obj_t *)lv_event_get_user_data(e);
    const char * txt = lv_btnmatrix_get_btn_text(obj, lv_btnmatrix_get_selected_btn(obj));
    const char * current_val = lv_textarea_get_text(ta);
    
    // 如果之前显示的是 Error，按下任意数字键应清空
    if (strcmp(current_val, "Error") == 0 || strcmp(current_val, "Inf") == 0 || strcmp(current_val, "NaN") == 0) {
        if (strcmp(txt, "C") != 0) {
            lv_textarea_set_text(ta, "");
            current_val = ""; // 更新指针指向
        }
    }
    
    size_t len = strlen(current_val);

    // 忽略占位符
    if (strcmp(txt, " ") == 0) return;

    if (strcmp(txt, "C") == 0) {
        lv_textarea_set_text(ta, "");
    } 
    else if (strcmp(txt, "=") == 0) {
        if(len == 0) return;
        
        // 调用新的计算引擎
        CalcEngine::CalcResult result = CalcEngine::evaluate(current_val);
        
        if (result.type == CalcEngine::OK) {
            char buf[64];
            // 智能格式化：如果是整数，不显示小数点
            if (std::abs(result.value - std::round(result.value)) < 1e-9 && std::abs(result.value) < 1e12) {
                snprintf(buf, sizeof(buf), "%lld", (long long)std::round(result.value));
            } else {
                snprintf(buf, sizeof(buf), "%.10g", result.value);
            }
            lv_textarea_set_text(ta, buf);
        } else if (result.type == CalcEngine::ERROR_DIV_0) {
            lv_textarea_set_text(ta, "Error"); // 除以0
        } else if (result.type == CalcEngine::ERROR_OVERFLOW) {
            lv_textarea_set_text(ta, "Inf");   // 溢出
        } else {
            lv_textarea_set_text(ta, "Error"); // 语法错误
        }
    }
    else {
        //百分号输入规则拦截
        char last_char = (len > 0) ? current_val[len - 1] : '\0';

        if (strcmp(txt, "%") == 0) {
            if (len == 0 || !is_digit_char(last_char)) return;
            if (last_char == '%') return;
            
            lv_textarea_add_text(ta, txt);
        } 
        else if (is_input_operator(txt)) {
            lv_textarea_add_text(ta, txt);
        }
        else if (is_digit_char(txt[0]) || strcmp(txt, ".") == 0) {
            if (last_char == '%') return;
            lv_textarea_add_text(ta, txt);
        }
        else {
            lv_textarea_add_text(ta, txt);
        }
    }
}

static void backspace_click_cb(lv_event_t * e) {
    lv_obj_t * ta = (lv_obj_t *)lv_event_get_user_data(e);
    lv_textarea_del_char(ta);
}

static void btnm_draw_cb(lv_event_t * e) {
    lv_obj_draw_part_dsc_t * dsc = lv_event_get_draw_part_dsc(e);
    if (dsc->part == LV_PART_ITEMS) {
        const char * txt = lv_btnmatrix_get_btn_text(lv_event_get_target(e), dsc->id);

        if (strcmp(txt, "+") == 0 || strcmp(txt, "-") == 0 || 
            strcmp(txt, "×") == 0 || strcmp(txt, "÷") == 0 || strcmp(txt, "=") == 0 || strcmp(txt, "%") == 0) {
            dsc->rect_dsc->bg_color = lv_palette_main(LV_PALETTE_ORANGE);
        } 
        else if (strcmp(txt, "C") == 0 || strcmp(txt, " ") == 0) {
            dsc->rect_dsc->bg_color = lv_color_make(80, 80, 80);
        }
        else {
            dsc->rect_dsc->bg_color = lv_color_make(45, 45, 45);
        }
    }
}

void fs_create_calculator(lv_obj_t* container) {
    lv_obj_set_style_bg_color(container, lv_color_black(), 0);

    // 显示屏
    lv_obj_t * ta = lv_textarea_create(container);
    lv_obj_set_size(ta, 230, 70);
    lv_obj_align(ta, LV_ALIGN_TOP_MID, 0, 5);
    lv_textarea_set_one_line(ta, false);
    lv_obj_set_style_text_font(ta, &chinese_24, 0); 
    lv_obj_set_style_bg_color(ta, lv_color_black(), 0);
    lv_obj_set_style_text_color(ta, lv_color_white(), 0);
    lv_obj_set_style_border_width(ta, 0, 0);
    lv_obj_set_style_text_align(ta, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_clear_flag(ta, LV_OBJ_FLAG_CLICK_FOCUSABLE);

    // 按钮地图
    static const char * btnm_map[] = {
        "C", "÷", "×", " ", "\n",
        "7", "8", "9", "-", "\n",
        "4", "5", "6", "+", "\n",
        "1", "2", "3", "%", "\n",
        "0", ".", "=", ""
    };

    lv_obj_t * btnm = lv_btnmatrix_create(container);
    lv_obj_set_size(btnm, 240, 200);
    lv_obj_align(btnm, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_btnmatrix_set_map(btnm, btnm_map);

    for(int i=0; i<20; i++) lv_btnmatrix_set_btn_width(btnm, i, 1);
    lv_btnmatrix_set_btn_ctrl(btnm, 3, LV_BTNMATRIX_CTRL_HIDDEN); 

    lv_obj_set_style_pad_row(btnm, 8, 0);
    lv_obj_set_style_pad_column(btnm, 8, 0);
    lv_obj_set_style_pad_all(btnm, 8, 0);
    lv_obj_set_style_bg_opa(btnm, 0, 0);
    lv_obj_set_style_border_width(btnm, 0, 0);
    lv_obj_set_style_radius(btnm, 12, LV_PART_ITEMS);
    lv_obj_set_style_text_color(btnm, lv_color_white(), LV_PART_ITEMS);
    lv_obj_set_style_text_font(btnm, &chinese_24, LV_PART_ITEMS);

    lv_obj_add_event_cb(btnm, btnm_event_cb, LV_EVENT_VALUE_CHANGED, ta);
    lv_obj_add_event_cb(btnm, btnm_draw_cb, LV_EVENT_DRAW_PART_BEGIN, NULL);

    // 独立退格按钮
    lv_obj_t * btn_back = lv_btn_create(container);
    lv_obj_set_size(btn_back, 48, 30);
    lv_obj_align(btn_back, LV_ALIGN_TOP_RIGHT, 3, 61);
    lv_obj_set_style_bg_color(btn_back, lv_color_make(80, 80, 80), 0);
    lv_obj_set_style_radius(btn_back, 12, 0);
    lv_obj_add_event_cb(btn_back, backspace_click_cb, LV_EVENT_CLICKED, ta);

    lv_obj_t * lbl_back = lv_label_create(btn_back);
    lv_label_set_text(lbl_back, LV_SYMBOL_BACKSPACE);
    lv_obj_set_style_text_font(lbl_back, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(lbl_back, lv_color_white(), 0);
    lv_obj_center(lbl_back);

    if (io_exit_timer) lv_timer_del(io_exit_timer);
    io_exit_timer = lv_timer_create(check_timer_cb, 50, NULL);
}