/*
 * 移植自参考工程 fs_game.cpp (2048 小游戏)
 * 与参考源码逐字一致, 仅一处消告警适配:
 *   - lv_label_set_text_fmt(score_label, "%u", ...) 的参数补 (unsigned) 强转
 */
#include "fullscreen_interfaces.h"

extern void fs_do_exit();

#define GRID_SIZE 4
#define BLOCK_SIZE 45
#define GAP 5
#define BOARD_SIZE (GRID_SIZE * BLOCK_SIZE + (GRID_SIZE + 1) * GAP)
#define ANIM_TIME 150

enum { MOVE_UP = 1, MOVE_DOWN, MOVE_LEFT, MOVE_RIGHT };

static uint8_t game_grid[GRID_SIZE][GRID_SIZE] = {0};
static lv_obj_t* block_objs[GRID_SIZE][GRID_SIZE] = {NULL};
static lv_obj_t* board_bg = NULL;
static lv_obj_t* score_label = NULL;
static lv_timer_t* exit_timer = NULL;
static uint32_t current_score = 0;
static uint8_t empty_count = 16;

// 活动标志和父对象指针，用于安全检查
static bool game_active = false;
static lv_obj_t* game_parent = NULL;

static void gesture_event_cb(lv_event_t* e);
static void fs_exit_game();  // 如果需要在其他函数中提前调用

// --- 动画回调 ---
static void anim_x_cb(void* var, int32_t v) { lv_obj_set_x((lv_obj_t*)var, (lv_coord_t)v); }
static void anim_y_cb(void* var, int32_t v) { lv_obj_set_y((lv_obj_t*)var, (lv_coord_t)v); }
static void anim_zoom_cb(void* var, int32_t v) { lv_obj_set_style_transform_zoom((lv_obj_t*)var, (lv_coord_t)v, 0); }

static void set_anim_pivot(lv_obj_t* obj) {
    lv_obj_set_style_transform_pivot_x(obj, BLOCK_SIZE / 2, 0);
    lv_obj_set_style_transform_pivot_y(obj, BLOCK_SIZE / 2, 0);
}

static void anim_spawn(lv_obj_t* obj) {
    set_anim_pivot(obj);
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, obj);
    lv_anim_set_values(&a, 128, 256);
    lv_anim_set_time(&a, ANIM_TIME);
    lv_anim_set_exec_cb(&a, anim_zoom_cb);
    lv_anim_start(&a);
}

static void anim_merge(lv_obj_t* obj) {
    set_anim_pivot(obj);
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, obj);
    lv_anim_set_values(&a, 256, 310);
    lv_anim_set_time(&a, ANIM_TIME / 2);
    lv_anim_set_playback_time(&a, ANIM_TIME / 2);
    lv_anim_set_exec_cb(&a, anim_zoom_cb);
    lv_anim_start(&a);
}

static void anim_move(lv_obj_t* obj, int32_t target_x, int32_t target_y) {
    lv_obj_move_foreground(obj);
    lv_anim_t ax, ay;
    lv_anim_init(&ax);
    lv_anim_set_var(&ax, obj);
    lv_anim_set_values(&ax, lv_obj_get_x(obj), target_x);
    lv_anim_set_time(&ax, ANIM_TIME);
    lv_anim_set_exec_cb(&ax, anim_x_cb);
    lv_anim_start(&ax);

    lv_anim_init(&ay);
    lv_anim_set_var(&ay, obj);
    lv_anim_set_values(&ay, lv_obj_get_y(obj), target_y);
    lv_anim_set_time(&ay, ANIM_TIME);
    lv_anim_set_exec_cb(&ay, anim_y_cb);
    lv_anim_start(&ay);
}

// --- 颜色逻辑 ---
static lv_color_t get_tile_color(uint8_t value) {
    switch (value) {
        case 0:  return lv_color_hex(0xcdc1b4);
        case 1:  return lv_color_hex(0xeee4da);
        case 2:  return lv_color_hex(0xede0c8);
        case 3:  return lv_color_hex(0xf2b179);
        case 4:  return lv_color_hex(0xf59563);
        case 5:  return lv_color_hex(0xf67c5f);
        case 6:  return lv_color_hex(0xf65e3b);
        default: return lv_color_hex(0xedcf72);
    }
}

static void update_tile(int row, int col) {
    if (!game_active) return; // 安全退出
    lv_obj_t* block = block_objs[row][col];
    uint8_t val = game_grid[row][col];

    lv_obj_set_style_bg_color(block, get_tile_color(val), 0);
    lv_obj_t* label = lv_obj_get_child(block, 0);

    if (val != 0) {
        if (!label) label = lv_label_create(block);
        lv_label_set_text_fmt(label, "%d", (int)pow(2, val));
        lv_obj_set_style_text_font(label, val < 7 ? &lv_font_montserrat_14 : &lv_font_montserrat_12, 0);
        lv_obj_set_style_text_color(label, val <= 2 ? lv_color_hex(0x776e65) : lv_color_white(), 0);
        lv_obj_center(label);
        lv_obj_set_style_bg_opa(block, LV_OPA_COVER, 0);
    } else {
        if (label) lv_obj_del(label);
        lv_obj_set_style_bg_opa(block, LV_OPA_0, 0);
    }
}

static void refresh_ui_static() {
    if (!game_active) return;
    lv_label_set_text_fmt(score_label, "%u", (unsigned)current_score);
    for (int i = 0; i < GRID_SIZE; i++) {
        for (int j = 0; j < GRID_SIZE; j++) {
            update_tile(i, j);
        }
    }
}

// --- 核心逻辑 ---
static void spawn_random_tile() {
    if (!game_active) return;
    if (empty_count == 0) return;
    int index = rand() % empty_count;
    int current = 0;
    for (int i = 0; i < GRID_SIZE; i++) {
        for (int j = 0; j < GRID_SIZE; j++) {
            if (game_grid[i][j] == 0) {
                if (current == index) {
                    game_grid[i][j] = (rand() % 10 == 0) ? 2 : 1;
                    empty_count--;
                    
                    lv_obj_t* block = block_objs[i][j];
                    lv_obj_set_style_bg_opa(block, LV_OPA_COVER, 0);
                    refresh_ui_static();
                    anim_spawn(block);
                    return;
                }
                current++;
            }
        }
    }
}

static void move_logic(uint8_t direction) {
    if (!game_active) return; // 安全检查

    bool moved = false;
    uint8_t combined[GRID_SIZE][GRID_SIZE] = {0};
    int dr = 0, dc = 0;
    if(direction == MOVE_UP) dr = -1;
    else if(direction == MOVE_DOWN) dr = 1;
    else if(direction == MOVE_LEFT) dc = -1;
    else if(direction == MOVE_RIGHT) dc = 1;

    for (int i = (dr == 1 ? GRID_SIZE-1 : 0); i >= 0 && i < GRID_SIZE; i += (dr == 1 ? -1 : 1)) {
        for (int j = (dc == 1 ? GRID_SIZE-1 : 0); j >= 0 && j < GRID_SIZE; j += (dc == 1 ? -1 : 1)) {
            if (game_grid[i][j] == 0) continue;

            int cr = i, cc = j;
            while (1) {
                int nr = cr + dr, nc = cc + dc;
                if (nr < 0 || nr >= GRID_SIZE || nc < 0 || nc >= GRID_SIZE) break;

                if (game_grid[nr][nc] == 0) {
                    game_grid[nr][nc] = game_grid[cr][cc];
                    game_grid[cr][cc] = 0;
                    cr = nr; cc = nc;
                    moved = true;
                } else if (game_grid[nr][nc] == game_grid[cr][cc] && !combined[nr][nc]) {
                    game_grid[nr][nc]++;
                    game_grid[cr][cc] = 0;
                    current_score += (uint32_t)pow(2, game_grid[nr][nc]);
                    combined[nr][nc] = 1;
                    empty_count++;
                    moved = true;
                    anim_merge(block_objs[nr][nc]);

                    update_tile(nr, nc);
                    update_tile(cr, cc);
                    break;
                } else break;
            }

            if (cr != i || cc != j) {
                int32_t target_x = GAP + cc * (BLOCK_SIZE + GAP);
                int32_t target_y = GAP + cr * (BLOCK_SIZE + GAP);
                anim_move(block_objs[i][j], target_x, target_y);

                lv_obj_t* temp = block_objs[i][j];
                block_objs[i][j] = block_objs[cr][cc];
                block_objs[cr][cc] = temp;

                int32_t origin_x = GAP + j * (BLOCK_SIZE + GAP);
                int32_t origin_y = GAP + i * (BLOCK_SIZE + GAP);
                lv_obj_set_pos(block_objs[i][j], origin_x, origin_y);

                update_tile(i, j);
                update_tile(cr, cc);
            }
        }
    }
    if (moved) spawn_random_tile();
}

// --- 退出清理函数 ---
static void fs_exit_game() {
    if (!game_active) return;

    // 移除手势事件回调
    lv_obj_remove_event_cb(lv_scr_act(), gesture_event_cb);

    // 删除定时器
    if (exit_timer) {
        lv_timer_del(exit_timer);
        exit_timer = NULL;
    }

    //清空对象指针
    for (int i = 0; i < GRID_SIZE; i++) {
        for (int j = 0; j < GRID_SIZE; j++) {
            block_objs[i][j] = NULL;
        }
    }

    // 4. 重置状态
    game_active = false;
    game_parent = NULL;
    board_bg = NULL;
    score_label = NULL;
    current_score = 0;
    empty_count = 16;
}

// 定时器回调
static void exit_timer_cb(lv_timer_t* t) {
    if (digitalRead(BUTTON_2) == 1) {
        fs_exit_game();          // 先清理游戏资源
        fs_do_exit();          // 返回上一个界面
    }
}

// 手势事件回调
static void gesture_event_cb(lv_event_t* e) {
    if (!game_active) return;    // 游戏未激活，不处理手势

    lv_dir_t dir = lv_indev_get_gesture_dir(lv_indev_get_act());
    if (dir == LV_DIR_TOP) move_logic(MOVE_UP);
    else if (dir == LV_DIR_BOTTOM) move_logic(MOVE_DOWN);
    else if (dir == LV_DIR_LEFT) move_logic(MOVE_LEFT);
    else if (dir == LV_DIR_RIGHT) move_logic(MOVE_RIGHT);
}

// 初始化函数
void fs_create_game(lv_obj_t* parent) {
    // 如果已有游戏在运行，先退出
    if (game_active) {
        fs_exit_game();
    }

    game_parent = parent;
    game_active = true;

    lv_obj_set_style_bg_color(parent, lv_color_hex(0xfaf8ef), 0);
    lv_obj_clear_flag(parent, LV_OBJ_FLAG_SCROLLABLE);

    // 标题
    lv_obj_t* title = lv_label_create(parent);
    lv_label_set_text(title, "2048");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(0x776e65), 0);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 20, 10);

    // 分数容器
    lv_obj_t* score_cnt = lv_obj_create(parent);
    lv_obj_set_size(score_cnt, 80, 35);
    lv_obj_align(score_cnt, LV_ALIGN_TOP_RIGHT, -20, 5);
    lv_obj_set_style_bg_color(score_cnt, lv_color_hex(0xbbada0), 0);
    lv_obj_set_style_border_width(score_cnt, 0, 0);
    lv_obj_clear_flag(score_cnt, LV_OBJ_FLAG_SCROLLABLE);

    score_label = lv_label_create(score_cnt);
    lv_obj_set_style_text_color(score_label, lv_color_white(), 0);
    lv_obj_align(score_label, LV_ALIGN_CENTER, 0, 0);

    // 棋盘背景容器
    board_bg = lv_obj_create(parent);
    lv_obj_set_size(board_bg, BOARD_SIZE, BOARD_SIZE);
    lv_obj_align(board_bg, LV_ALIGN_CENTER, 0, 25);
    lv_obj_set_style_bg_color(board_bg, lv_color_hex(0xbbada0), 0);
    lv_obj_set_style_pad_all(board_bg, 0, 0);
    lv_obj_set_style_border_width(board_bg, 0, 0);
    lv_obj_clear_flag(board_bg, LV_OBJ_FLAG_SCROLLABLE);

    //创建背景层
    for (int i = 0; i < GRID_SIZE; i++) {
        for (int j = 0; j < GRID_SIZE; j++) {
            lv_obj_t* bg_tile = lv_obj_create(board_bg);
            lv_obj_set_size(bg_tile, BLOCK_SIZE, BLOCK_SIZE);
            lv_obj_set_pos(bg_tile, GAP + j * (BLOCK_SIZE + GAP), GAP + i * (BLOCK_SIZE + GAP));
            lv_obj_set_style_bg_color(bg_tile, lv_color_hex(0xcdc1b4), 0);
            lv_obj_set_style_radius(bg_tile, 3, 0);
            lv_obj_set_style_border_width(bg_tile, 0, 0);
            lv_obj_clear_flag(bg_tile, LV_OBJ_FLAG_SCROLLABLE);
        }
    }

    //创建活动层
    for (int i = 0; i < GRID_SIZE; i++) {
        for (int j = 0; j < GRID_SIZE; j++) {
            block_objs[i][j] = lv_obj_create(board_bg);
            lv_obj_set_size(block_objs[i][j], BLOCK_SIZE, BLOCK_SIZE);
            lv_obj_set_pos(block_objs[i][j], GAP + j * (BLOCK_SIZE + GAP), GAP + i * (BLOCK_SIZE + GAP));
            lv_obj_set_style_radius(block_objs[i][j], 3, 0);
            lv_obj_set_style_border_width(block_objs[i][j], 0, 0);
            lv_obj_set_style_pad_all(block_objs[i][j], 0, 0);
            lv_obj_clear_flag(block_objs[i][j], LV_OBJ_FLAG_SCROLLABLE);
            
            lv_obj_set_style_bg_opa(block_objs[i][j], LV_OPA_0, 0);
            game_grid[i][j] = 0;
        }
    }

    lv_obj_remove_event_cb(lv_scr_act(), gesture_event_cb);
    lv_obj_add_event_cb(lv_scr_act(), gesture_event_cb, LV_EVENT_GESTURE, NULL);
    
    current_score = 0;
    empty_count = 16;
    spawn_random_tile();
    spawn_random_tile();

    exit_timer = lv_timer_create(exit_timer_cb, 50, NULL);
}