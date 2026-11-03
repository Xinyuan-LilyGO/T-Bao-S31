/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Mini-game launcher for the ESP-VoCat-S31 "喵伴" pet.
 *
 * The page shows a black launcher with app-style icons for 2048 and the
 * wooden-fish game. Tapping an icon starts that game inside this same page.
 * Black background, white text (project-wide dark theme).
 */
#include "page_game.h"

#include <stdlib.h>
#include <string.h>
#include "app_sfx.h"
#include "app_language.h"
#include "esp_xiaozhi_chat_display.h"
#include "font_awesome_symbols.h"
#include "lvgl.h"
#include "page_launcher.h"
#include "ui_page_style.h"
#include "launcher_icons.h"

#define BG lv_color_black()
#define FG lv_color_white()

/* --- sub-game state ------------------------------------------------------ */
typedef enum { SUB_LAUNCHER, SUB_2048, SUB_MUYU } sub_t;
static sub_t s_sub = SUB_LAUNCHER;
static void sub_show(sub_t s);
static void sub_launcher_cb(lv_event_t *e);

/* ==================== 敲木鱼 (merit counter) ==================== */
#define MUYU_MERITS_MAX 99999
#define MUYU_IMG_SZ 180
static int  s_muyu_merits = 0;
static lv_obj_t *s_muyu_btn = NULL;
static lv_obj_t *s_muyu_counter = NULL;
static lv_obj_t *s_muyu_hint = NULL;
static lv_obj_t *s_muyu_fx = NULL;        /* "+1" float label */
static lv_obj_t *s_muyu_img = NULL;       /* animated wooden-fish image */
static int s_muyu_frame = 0;              /* current GIF frame index */
static lv_image_dsc_t s_muyu_dsc = {0};

/* ==================== 2048 ==================== */
#define GRID_N      4
#define CELL_GAP    5
#define CELL_BASE   5
static int      s_board[GRID_N][GRID_N];
static int      s_score = 0;
static bool     s_over  = false;
static bool     s_won   = false;
static lv_obj_t *s_cells[GRID_N][GRID_N] = {{0}};
static lv_obj_t *s_score_label = NULL;
static lv_obj_t *s_msg_label = NULL;
static lv_obj_t *s_grid = NULL;

/* --- launcher widgets --- */
static lv_obj_t *s_launcher = NULL;

/* ==================== helpers ==================== */

/* ==================== 2048 logic ==================== */

static void spawn_tile(void)
{
    int empty[GRID_N * GRID_N][2];
    int n = 0;
    for (int r = 0; r < GRID_N; r++) {
        for (int c = 0; c < GRID_N; c++) {
            if (s_board[r][c] == 0) {
                empty[n][0] = r;
                empty[n][1] = c;
                n++;
            }
        }
    }
    if (n == 0) {
        return;
    }
    int idx = rand() % n;
    s_board[empty[idx][0]][empty[idx][1]] = (rand() % 10 == 0) ? 4 : 2;
}

static bool can_move_2048(void)
{
    for (int r = 0; r < GRID_N; r++) {
        for (int c = 0; c < GRID_N; c++) {
            if (s_board[r][c] == 0) {
                return true;
            }
            if (c + 1 < GRID_N && s_board[r][c] == s_board[r][c + 1]) {
                return true;
            }
            if (r + 1 < GRID_N && s_board[r][c] == s_board[r + 1][c]) {
                return true;
            }
        }
    }
    return false;
}

static bool slide_line(int line[GRID_N], int dir)
{
    int tmp[GRID_N] = {0};
    int idx = 0;
    if (dir == 0 || dir == 2) {
        for (int i = 0; i < GRID_N; i++) {
            if (line[i] != 0) {
                if (idx > 0 && tmp[idx - 1] == line[i]) {
                    tmp[idx - 1] *= 2;
                    s_score += tmp[idx - 1];
                } else {
                    tmp[idx++] = line[i];
                }
            }
        }
    } else {
        for (int i = GRID_N - 1; i >= 0; i--) {
            if (line[i] != 0) {
                if (idx > 0 && tmp[GRID_N - idx] == line[i]) {
                    tmp[GRID_N - idx] *= 2;
                    s_score += tmp[GRID_N - idx];
                } else {
                    idx++;
                    tmp[GRID_N - idx] = line[i];
                }
            }
        }
    }
    bool changed = memcmp(line, tmp, sizeof(tmp)) != 0;
    memcpy(line, tmp, sizeof(tmp));
    return changed;
}

static bool move_2048(ui_gesture_t g)
{
    bool changed = false;
    if (g == UI_GESTURE_SWIPE_LEFT || g == UI_GESTURE_SWIPE_RIGHT) {
        int dir = (g == UI_GESTURE_SWIPE_LEFT) ? 0 : 1;
        for (int r = 0; r < GRID_N; r++) {
            changed |= slide_line(s_board[r], dir);
        }
    } else {
        int dir = (g == UI_GESTURE_SWIPE_UP) ? 2 : 3;
        for (int c = 0; c < GRID_N; c++) {
            int line[GRID_N];
            for (int r = 0; r < GRID_N; r++) {
                line[r] = s_board[r][c];
            }
            if (slide_line(line, dir)) {
                changed = true;
            }
            for (int r = 0; r < GRID_N; r++) {
                s_board[r][c] = line[r];
            }
        }
    }
    return changed;
}

/* Dopamine (bright candy) tile palette — 2048. */
static lv_color_t tile_color(int v)
{
    switch (v) {
    case 0:     return lv_color_hex(0x2E2E3E);
    case 2:     return lv_color_hex(0x7C5CBF);   /* purple  */
    case 4:     return lv_color_hex(0x4C8CFF);   /* blue    */
    case 8:     return lv_color_hex(0x00B8A9);   /* teal    */
    case 16:    return lv_color_hex(0x3DDC84);   /* green   */
    case 32:    return lv_color_hex(0xF9CB3F);   /* yellow  */
    case 64:    return lv_color_hex(0xFF9800);   /* orange  */
    case 128:   return lv_color_hex(0xFF7043);   /* deep orange */
    case 256:   return lv_color_hex(0xFF5E7E);   /* pink    */
    case 512:   return lv_color_hex(0xE040FB);   /* magenta */
    case 1024:  return lv_color_hex(0x2979FF);   /* vivid blue */
    default:    return lv_color_hex(0xFFD600);   /* gold    */
    }
}

static void refresh_2048(void)
{
    char buf[12];
    for (int r = 0; r < GRID_N; r++) {
        for (int c = 0; c < GRID_N; c++) {
            int v = s_board[r][c];
            if (v > 0) {
                snprintf(buf, sizeof(buf), "%d", v);
                lv_label_set_text_fmt(lv_obj_get_child(s_cells[r][c], 0), "%s", buf);
            } else {
                lv_label_set_text(lv_obj_get_child(s_cells[r][c], 0), "");
            }
            lv_obj_set_style_bg_color(s_cells[r][c], tile_color(v), 0);
        }
    }
    if (s_score_label) {
        lv_label_set_text_fmt(s_score_label,
                              app_language_is_english() ? "Score %d" : "分数 %d",
                              s_score);
    }
    if (s_msg_label) {
        if (s_over) {
            lv_label_set_text(s_msg_label, app_language_text("游戏结束", "Game over"));
            lv_obj_clear_flag(s_msg_label, LV_OBJ_FLAG_HIDDEN);
        } else if (s_won) {
            lv_label_set_text(s_msg_label, app_language_text("2048 达成！", "2048 reached!"));
            lv_obj_clear_flag(s_msg_label, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(s_msg_label, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

static void restart_2048(void)
{
    memset(s_board, 0, sizeof(s_board));
    s_score = 0;
    s_over = false;
    s_won = false;
    spawn_tile();
    spawn_tile();
    refresh_2048();
}

static void build_2048(lv_obj_t *parent)
{
    /* Grid ~210x210 (smaller per user request: the old 300px grid didn't fit
     * on the 360px round screen). Title + score in one row, back button at the
     * bottom. */
    int avail = 210;
    int cell_size = (avail - 2 * CELL_BASE - (GRID_N - 1) * CELL_GAP) / GRID_N;
    s_grid = lv_obj_create(parent);
    lv_obj_set_size(s_grid, avail, avail);
    lv_obj_align(s_grid, LV_ALIGN_TOP_MID, 0, 40);
    lv_obj_set_style_bg_color(s_grid, lv_color_hex(0x232332), 0);
    lv_obj_set_style_border_width(s_grid, 0, 0);
    lv_obj_set_style_radius(s_grid, 10, 0);
    lv_obj_set_style_pad_all(s_grid, CELL_BASE, 0);

    for (int r = 0; r < GRID_N; r++) {
        for (int c = 0; c < GRID_N; c++) {
            lv_obj_t *cell = lv_obj_create(s_grid);
            lv_obj_set_size(cell, cell_size, cell_size);
            lv_obj_set_pos(cell, CELL_BASE + c * (cell_size + CELL_GAP),
                           CELL_BASE + r * (cell_size + CELL_GAP));
            lv_obj_set_style_radius(cell, 6, 0);
            lv_obj_set_style_border_width(cell, 0, 0);
            lv_obj_set_style_bg_color(cell, tile_color(0), 0);
            lv_obj_set_style_bg_opa(cell, LV_OPA_COVER, 0);
            lv_obj_set_style_shadow_width(cell, 0, 0);
            lv_obj_t *lab = lv_label_create(cell);
            lv_label_set_text(lab, "");
            lv_obj_set_style_text_align(lab, LV_TEXT_ALIGN_CENTER, 0);
            lv_obj_set_style_text_color(lab, lv_color_white(), 0);
            lv_obj_set_style_text_font(lab, LV_FONT_DEFAULT, 0);
            lv_obj_center(lab);
            s_cells[r][c] = cell;
        }
    }

    lv_obj_t *title = lv_label_create(parent);
    lv_label_set_text(title, "2048");
    lv_obj_set_style_text_color(title, FG, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 4);
    s_score_label = lv_label_create(parent);
    lv_label_set_text(s_score_label, app_language_text("分数 0", "Score 0"));
    lv_obj_set_style_text_color(s_score_label, FG, 0);
    lv_obj_align(s_score_label, LV_ALIGN_TOP_MID, 0, 4);
    lv_obj_set_style_translate_x(s_score_label, 72, 0);
    s_msg_label = lv_label_create(parent);
    lv_obj_set_style_text_color(s_msg_label, lv_color_hex(0xFF5E7E), 0);
    lv_obj_align(s_msg_label, LV_ALIGN_TOP_MID, 0, 330);
    lv_obj_add_flag(s_msg_label, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *back = lv_btn_create(parent);
    lv_obj_set_size(back, 56, 26);
    lv_obj_align(back, LV_ALIGN_BOTTOM_MID, 0, -2);
    lv_obj_set_style_radius(back, 13, 0);
    lv_obj_set_style_bg_color(back, lv_color_hex(0x3A3A4A), 0);
    lv_obj_add_event_cb(back, sub_launcher_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *bl = lv_label_create(back);
    lv_label_set_text(bl, app_language_text("返回", "Back"));
    lv_obj_set_style_text_color(bl, FG, 0);
    lv_obj_center(bl);
}

/* ==================== 敲木鱼 (merit counter) ==================== */

#include "muyu_gif.h"

/* Point the shared image descriptor at GIF frame `idx`. */
static void muyu_set_frame(int idx)
{
    if (s_muyu_img == NULL) {
        return;
    }
    s_muyu_dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
    s_muyu_dsc.header.cf = LV_COLOR_FORMAT_RGB565;
    s_muyu_dsc.header.w = GIF_MUYU_GIF_W;
    s_muyu_dsc.header.h = GIF_MUYU_GIF_H;
    s_muyu_dsc.data_size = GIF_MUYU_GIF_W * GIF_MUYU_GIF_H * 2;
    s_muyu_dsc.data = (const uint8_t *)gif_muyu_gif_frames[idx];
    lv_image_set_src(s_muyu_img, &s_muyu_dsc);
}

/* Advance the wooden-fish animation one frame (driven by page_game_on_tick). */
static void muyu_next_frame(void)
{
    if (s_muyu_img == NULL) {
        return;
    }
    s_muyu_frame = (s_muyu_frame + 1) % GIF_MUYU_GIF_FRAMES;
    muyu_set_frame(s_muyu_frame);
}

static void muyu_scale_exec(void *obj, int32_t val)
{
    lv_obj_t *b = (lv_obj_t *)obj;
    if (b) {
        lv_obj_set_style_transform_scale(b, val, 0);
    }
}

/* "+1" float-up label, then hide. Runs on LVGL task (LV_ANIM uses the same). */
static void muyu_fx_ready_cb(lv_anim_t *a)
{
    lv_obj_t *l = (lv_obj_t *)a->var;
    if (l) {
        lv_obj_add_flag(l, LV_OBJ_FLAG_HIDDEN);
    }
}

/* "+1" label floats up; hidden by the ready callback. Fading is optional and
 * cheap enough to skip — the quick move already reads as a "pop". */
static void muyu_fx_exec(void *obj, int32_t val)
{
    lv_obj_t *l = (lv_obj_t *)obj;
    if (l) {
        lv_obj_set_style_translate_y(l, val, 0);
    }
}

static void muyu_click_cb(lv_event_t *e)
{
    (void)e;
    s_muyu_merits++;
    if (s_muyu_merits > MUYU_MERITS_MAX) {
        s_muyu_merits = 1;
    }
    if (s_muyu_counter) {
        lv_label_set_text_fmt(s_muyu_counter,
                              app_language_is_english() ? "Merit %d" : "功德 %d",
                              s_muyu_merits);
    }
    /* Wood-block tap sound + squash animation. NOTE: LVGL9 transform_scale
     * uses 256 == 100%, so a squash is 256 -> ~224 (not 920/1000 which would
     * zoom the button to ~4x and blow the whole screen up). Pivot at the
     * button centre so it just "presses" in place. */
    app_sfx_play(APP_SFX_TAP);
    if (s_muyu_btn) {
        lv_obj_set_style_transform_pivot_x(s_muyu_btn, lv_pct(50), 0);
        lv_obj_set_style_transform_pivot_y(s_muyu_btn, lv_pct(50), 0);
        lv_obj_set_style_transform_scale(s_muyu_btn, 224, 0);
        lv_anim_t a;
        lv_anim_init(&a);
        lv_anim_set_var(&a, s_muyu_btn);
        lv_anim_set_exec_cb(&a, muyu_scale_exec);
        lv_anim_set_values(&a, 224, 256);
        lv_anim_set_time(&a, 140);
        lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
        lv_anim_start(&a);
    }
    /* "功德+1" floats up from the fish (translate_y relative to its base
     * position, so the absolute layout stays put). */
    if (s_muyu_fx) {
        lv_obj_clear_flag(s_muyu_fx, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_style_translate_y(s_muyu_fx, 0, 0);
        lv_anim_t f;
        lv_anim_init(&f);
        lv_anim_set_var(&f, s_muyu_fx);
        lv_anim_set_exec_cb(&f, muyu_fx_exec);
        lv_anim_set_values(&f, 0, -46);
        lv_anim_set_time(&f, 700);
        lv_anim_set_path_cb(&f, lv_anim_path_ease_out);
        lv_anim_set_ready_cb(&f, muyu_fx_ready_cb);
        lv_anim_start(&f);
    }
}

static void muyu_tap_cb(lv_event_t *e)
{
    muyu_click_cb(e);
}

static void build_muyu(lv_obj_t *parent)
{
    s_muyu_merits = 0;
    s_muyu_frame = 0;

    lv_obj_t *title = lv_label_create(parent);
    lv_label_set_text(title, app_language_text("敲木鱼 · 功德无量", "Wooden fish · endless merit"));
    lv_obj_set_style_text_color(title, FG, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 4);

    /* Wooden fish image button (animated GIF frames). */
    s_muyu_btn = lv_btn_create(parent);
    lv_obj_set_size(s_muyu_btn, MUYU_IMG_SZ + 24, MUYU_IMG_SZ + 24);
    lv_obj_align(s_muyu_btn, LV_ALIGN_CENTER, 0, -10);
    lv_obj_set_style_radius(s_muyu_btn, 20, 0);
    lv_obj_set_style_bg_color(s_muyu_btn, lv_color_hex(0x2A2118), 0);
    lv_obj_set_style_bg_color(s_muyu_btn, lv_color_hex(0x3A2A1A), LV_STATE_PRESSED);
    lv_obj_set_style_border_width(s_muyu_btn, 0, 0);
    lv_obj_add_event_cb(s_muyu_btn, muyu_tap_cb, LV_EVENT_CLICKED, NULL);
    s_muyu_img = lv_image_create(s_muyu_btn);
    lv_obj_set_size(s_muyu_img, MUYU_IMG_SZ, MUYU_IMG_SZ);
    lv_obj_center(s_muyu_img);
    muyu_set_frame(0);   /* show frame 0 + configure the descriptor */

    /* Merit counter (below the fish). */
    s_muyu_counter = lv_label_create(parent);
    lv_label_set_text(s_muyu_counter, app_language_text("功德 0", "Merit 0"));
    lv_obj_set_style_text_color(s_muyu_counter, lv_color_hex(0xFFD700), 0);
    lv_obj_align(s_muyu_counter, LV_ALIGN_CENTER, 0, 108);

    /* "+1" float label (starts hidden, above the fish). */
    s_muyu_fx = lv_label_create(parent);
    lv_label_set_text(s_muyu_fx, app_language_text("功德 +1", "Merit +1"));
    lv_obj_set_style_text_color(s_muyu_fx, lv_color_hex(0xFFD700), 0);
    lv_obj_align(s_muyu_fx, LV_ALIGN_CENTER, 0, -96);
    lv_obj_add_flag(s_muyu_fx, LV_OBJ_FLAG_HIDDEN);

    s_muyu_hint = lv_label_create(parent);
    lv_label_set_text(s_muyu_hint, app_language_text("点击木鱼 功德+1", "Tap the fish for +1 merit"));
    lv_obj_set_style_text_color(s_muyu_hint, lv_color_hex(0x888888), 0);
    lv_obj_align(s_muyu_hint, LV_ALIGN_TOP_MID, 0, 222);

    lv_obj_t *back = lv_btn_create(parent);
    lv_obj_set_size(back, 56, 26);
    lv_obj_align(back, LV_ALIGN_BOTTOM_MID, 0, -2);
    lv_obj_set_style_radius(back, 13, 0);
    lv_obj_set_style_bg_color(back, lv_color_hex(0x3A3A4A), 0);
    lv_obj_add_event_cb(back, sub_launcher_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *bl = lv_label_create(back);
    lv_label_set_text(bl, app_language_text("返回", "Back"));
    lv_obj_set_style_text_color(bl, FG, 0);
    lv_obj_center(bl);
}

/* ==================== Launcher ==================== */

static lv_obj_t *make_icon_btn(lv_obj_t *parent, const char *emoji, const char *title,
                               lv_event_cb_t cb, uint32_t color)
{
    lv_obj_t *b = lv_btn_create(parent);
    lv_obj_set_size(b, 138, 68);
    ui_page_style_button(b, color, 0x2A3942, 14);
    lv_obj_set_style_border_color(b, lv_color_hex(color), 0);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *e = lv_label_create(b);
    lv_label_set_text(e, emoji);
    lv_obj_set_style_text_align(e, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(e, LV_ALIGN_LEFT_MID, 12, 0);

    lv_obj_t *t = lv_label_create(b);
    lv_label_set_text(t, app_language_text(title, NULL));
    lv_obj_set_style_text_color(t, FG, 0);
    lv_obj_align(t, LV_ALIGN_LEFT_MID, 52, 0);
    return b;
}

static void icon_2048_cb(lv_event_t *e)
{
    (void)e;
    app_sfx_play(APP_SFX_TAP);
    sub_show(SUB_2048);
    restart_2048();
}

static void icon_muyu_cb(lv_event_t *e)
{
    (void)e;
    app_sfx_play(APP_SFX_TAP);
    sub_show(SUB_MUYU);
}

static void build_launcher(lv_obj_t *parent)
{
    s_launcher = lv_obj_create(parent);
    lv_obj_set_size(s_launcher, LV_HOR_RES, LV_VER_RES);
    ui_page_style_root(s_launcher);

    ui_page_make_back_button(s_launcher);
    ui_page_make_header_image(s_launcher, &launcher_icon_game, "游戏", "选择玩法");

    lv_obj_t *row = lv_obj_create(s_launcher);
    lv_obj_set_size(row, LV_HOR_RES - 20, 90);
    lv_obj_align(row, LV_ALIGN_TOP_MID, 0, 58);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_set_style_pad_column(row, 8, 0);
    lv_obj_set_style_pad_row(row, 8, 0);
    make_icon_btn(row, "🔢", "2048", icon_2048_cb, 0x5C8DFF);      /* blue  */
    make_icon_btn(row, "🪵", app_language_text("敲木鱼", "Wooden fish"), icon_muyu_cb, 0xF5B041);    /* amber */

    lv_obj_t *hint = lv_label_create(s_launcher);
    lv_label_set_text(hint, app_language_text("选择一个游戏", "Choose a game"));
    lv_obj_set_style_text_color(hint, lv_color_hex(0x8C9AA3), 0);
    lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -16);
}

/* --- sub switching ------------------------------------------------------- */

static void sub_launcher_cb(lv_event_t *e)
{
    (void)e;
    app_sfx_play(APP_SFX_TAP);
    sub_show(SUB_LAUNCHER);
}

static void sub_show(sub_t s)
{
    s_sub = s;
    lv_obj_t *root = esp_xiaozhi_chat_display_page_root();
    if (root == NULL) {
        return;
    }
    lv_obj_clean(root);
    lv_obj_set_style_bg_color(root, BG, 0);
    if (s == SUB_LAUNCHER) {
        build_launcher(root);
    } else if (s == SUB_2048) {
        build_2048(root);
    } else {
        build_muyu(root);
    }
}

/* --- page callbacks ------------------------------------------------------ */

esp_err_t page_game_on_enter(void)
{
    lv_obj_t *root = esp_xiaozhi_chat_display_page_root();
    if (root == NULL) {
        return ESP_FAIL;
    }
    sub_show(SUB_LAUNCHER);
    app_sfx_play(APP_SFX_PAGE);
    return ESP_OK;
}

esp_err_t page_game_on_exit(void)
{
    /* The page manager clears the page root on the next switch; any static
     * pointers into the widgets we built become dangling. Reset the sub-game
     * state and all cached widget pointers so a later tick / gesture can
     * never dereference a freed LVGL object (that caused Load access fault
     * crashes while switching pages). */
    s_sub = SUB_LAUNCHER;
    s_launcher = NULL;
    s_grid = NULL;
    s_score_label = NULL;
    s_msg_label = NULL;
    s_muyu_btn = NULL;
    s_muyu_counter = NULL;
    s_muyu_hint = NULL;
    s_muyu_fx = NULL;
    s_muyu_img = NULL;
    s_muyu_frame = 0;
    /* The GIF frames live in flash; the dsc just points at them. */
    for (int i = 0; i < GRID_N; i++) {
        for (int j = 0; j < GRID_N; j++) {
            s_cells[i][j] = NULL;
        }
    }
    return ESP_OK;
}

bool page_game_on_gesture(ui_gesture_t g)
{
    if (s_sub == SUB_2048 && s_grid == NULL) {
        return false;
    }
    if (s_sub == SUB_2048) {
        if (s_over) {
            if (g == UI_GESTURE_TAP) {
                restart_2048();
            }
            return false;   /* let swipe navigate back */
        }
        switch (g) {
        case UI_GESTURE_SWIPE_LEFT:
        case UI_GESTURE_SWIPE_RIGHT:
        case UI_GESTURE_SWIPE_UP:
        case UI_GESTURE_SWIPE_DOWN:
            if (move_2048(g)) {
                app_sfx_play(APP_SFX_TAP);
                spawn_tile();
                for (int r = 0; r < GRID_N && !s_won; r++) {
                    for (int c = 0; c < GRID_N; c++) {
                        if (s_board[r][c] == 2048) {
                            s_won = true;
                            break;
                        }
                    }
                }
                if (!can_move_2048()) {
                    s_over = true;
                }
            }
            refresh_2048();
            return true;
        default:
            return false;
        }
    }
    return false;
}

void page_game_on_tick(uint32_t ms)
{
    /* Wooden-fish GIF animation. */
    if (s_sub == SUB_MUYU && s_muyu_img != NULL) {
        static uint32_t s_muyu_last = 0;
        uint32_t dur = (s_muyu_frame < GIF_MUYU_GIF_FRAMES)
                       ? gif_muyu_gif_durations[s_muyu_frame] : 80;
        if (dur < 20) dur = 80;
        if (ms - s_muyu_last >= dur) {
            s_muyu_last = ms;
            muyu_next_frame();
        }
    }
}

const ui_page_t page_game = {
    .id = PAGE_GAME,
    .name = "game",
    .on_enter = page_game_on_enter,
    .on_exit = page_game_on_exit,
    .on_gesture = page_game_on_gesture,
    .on_tick = page_game_on_tick,
};
