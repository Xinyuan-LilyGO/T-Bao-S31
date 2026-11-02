/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Cached app-grid launcher (手机桌面) for the ESP-VoCat-S31 "喵伴".
 *
 * The desktop is rendered once into a PSRAM-backed RGB565 canvas. During a
 * drag, LVGL only moves that one image object; it does not lay out and redraw
 * twelve buttons and twelve labels on every touch sample.
 */
#include "page_launcher.h"

#include <math.h>
#include <stdlib.h>
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "app_sfx.h"
#include "app_language.h"
#include "esp_xiaozhi_chat_app.h"
#include "esp_xiaozhi_chat_display.h"
#include "lvgl.h"
#include "src/draw/lv_draw_image.h"
#include "src/draw/lv_draw_label.h"
#include "launcher_icons.h"
#include "font_awesome_symbols.h"
#include "ui_page_style.h"

#define TAG "PAGE_LAUNCHER"

#define LAUNCHER_N    12
#define LAUNCHER_VIEW_Y       0
#define LAUNCHER_VIEW_H       LV_VER_RES
#define LAUNCHER_CACHE_W      LV_HOR_RES
#define LAUNCHER_CACHE_H      LAUNCHER_VIEW_H
#define LAUNCHER_CACHE_BYTES  ((size_t)LAUNCHER_CACHE_W * \
                               (size_t)LAUNCHER_CACHE_H * sizeof(lv_color_t))
#define LAUNCHER_BUBBLE_SIZE  56
#define LAUNCHER_MIN_OFFSET_X (-48.0f)
#define LAUNCHER_MAX_OFFSET_X 48.0f
#define LAUNCHER_MIN_OFFSET_Y (-34.0f)
#define LAUNCHER_MAX_OFFSET_Y 34.0f
#define LAUNCHER_INERTIA_MS   16
#define LAUNCHER_DRAG_TOP     0
#define LAUNCHER_LABEL_WIDTH  72
#define LAUNCHER_LABEL_GAP    2
#define LAUNCHER_LAUNCH_MS    120

LV_FONT_DECLARE(font_puhui_16_4);
LV_FONT_DECLARE(font_puhui_20_4);
LV_FONT_DECLARE(font_awesome_20_4);
LV_FONT_DECLARE(font_awesome_30_4);

typedef struct {
    ui_page_id_t page;
    const char  *icon;   /* Font Awesome icon */
    const char  *label_zh;
    const char  *label_en;
    uint32_t     color;
    int16_t      center_x;
    int16_t      center_y;
    const lv_image_dsc_t *image;
} launcher_item_t;

static const launcher_item_t s_items[LAUNCHER_N] = {
    { PAGE_DIALOGUE,    FONT_AWESOME_COMMENT,        "对话", "Chat", 0x2094FA,  45,  28, &launcher_icon_dialogue },
    { PAGE_CAMERA,      FONT_AWESOME_IMAGE,          "摄像头", "Camera", 0xFF9500, 122,  28, &launcher_icon_camera },
    { PAGE_MUSIC,       FONT_AWESOME_MUSIC,          "音乐", "Music", 0xFFD60A, 199,  28, &launcher_icon_music },
    { PAGE_MOTION,      FONT_AWESOME_LOCATION_ARROW, "移动", "Motion", 0x34C759, 276,  28, &launcher_icon_motion },
    { PAGE_GAME,        FONT_AWESOME_PLAY,           "游戏", "Games", 0x34C759,  45, 112, &launcher_icon_game },
    { PAGE_TIMER,       FONT_AWESOME_BELL,           "计时器", "Timer", 0x00F5EA, 122, 112, &launcher_icon_timer },
    { PAGE_VIDEO,       FONT_AWESOME_SD_CARD,        "视频", "Video", 0x4DA3FF, 199, 112, &launcher_icon_video },
    { PAGE_EMOTION,     FONT_AWESOME_USER_ROBOT,     "表情", "Expressions", 0xAF52DE, 276, 112, &launcher_icon_emotion },
    { PAGE_XIAOZHI,     FONT_AWESOME_AI_CHIP,        "小智AI", "Xiaozhi AI", 0x2094FA,  45, 196, &launcher_icon_xiaozhi },
    { PAGE_SETTINGS,    FONT_AWESOME_GEAR,           "设置", "Settings", 0x8E8E93, 122, 196, &launcher_icon_settings },
    { PAGE_FACE_FOLLOW, FONT_AWESOME_USER_ROBOT,     "人脸跟随", "Face Follow", 0x34C759, 199, 196, &launcher_icon_face_follow },
    { PAGE_FACE,        FONT_AWESOME_HOME,           "返回", "Home", 0x8E8E93, 276, 196, &launcher_icon_back },
};

static lv_obj_t *s_grid = NULL;
static lv_obj_t *s_cache_canvas = NULL;
static lv_obj_t *s_touch_layer = NULL;
static lv_color_t *s_cache_buffer = NULL;
static lv_timer_t *s_inertia_timer = NULL;
static lv_timer_t *s_launch_timer = NULL;
static lv_timer_t *s_face_follow_guard_timer = NULL;
static lv_obj_t *s_face_follow_guard_popup = NULL;
static ui_page_id_t s_pending_page = PAGE_LAUNCHER;
static bool s_dragging = false;
static bool s_drag_moved = false;
static bool s_suppress_click = false;
static int32_t s_drag_start_x = 0;
static int32_t s_drag_start_y = 0;
static int32_t s_drag_last_x = 0;
static int32_t s_drag_last_y = 0;
static float s_drag_velocity_x = 0.0f;
static float s_drag_velocity_y = 0.0f;
static float s_offset_x = 0.0f;
static float s_offset_y = 0.0f;
static int32_t s_cache_x = 0;
static int32_t s_cache_y = 0;

static void launcher_cancel_pending_launch(void);
static void launcher_touch_cb(lv_event_t *e);
static void launcher_hide_face_follow_guard_popup(void);

static void launcher_face_follow_guard_cancel_cb(lv_event_t *event)
{
    (void)event;
    app_sfx_play(APP_SFX_TAP);
    launcher_hide_face_follow_guard_popup();
}

static void launcher_face_follow_guard_continue_cb(lv_event_t *event)
{
    (void)event;
    app_sfx_play(APP_SFX_TAP);
    launcher_hide_face_follow_guard_popup();
    ui_page_manager_allow_face_follow_once();
    (void)ui_page_manager_switch(PAGE_FACE_FOLLOW);
}

static bool xiaozhi_blocks_face_follow(void)
{
    const esp_xiaozhi_chat_app_state_t state =
        esp_xiaozhi_chat_app_get_state();
    return state == ESP_XIAOZHI_CHAT_APP_STATE_STARTING ||
           state == ESP_XIAOZHI_CHAT_APP_STATE_ENABLED ||
           state == ESP_XIAOZHI_CHAT_APP_STATE_STOPPING;
}

static void launcher_hide_face_follow_guard_popup(void)
{
    if (s_face_follow_guard_timer != NULL) {
        lv_timer_delete(s_face_follow_guard_timer);
        s_face_follow_guard_timer = NULL;
    }
    if (s_face_follow_guard_popup != NULL) {
        lv_obj_del(s_face_follow_guard_popup);
        s_face_follow_guard_popup = NULL;
    }
}

static void launcher_face_follow_guard_timer_cb(lv_timer_t *timer)
{
    if (s_face_follow_guard_timer == timer) {
        s_face_follow_guard_timer = NULL;
    }
    lv_timer_delete(timer);
    if (s_face_follow_guard_popup != NULL) {
        lv_obj_del(s_face_follow_guard_popup);
        s_face_follow_guard_popup = NULL;
    }
}

static void launcher_show_face_follow_guard_popup(void)
{
    if (s_grid == NULL) {
        return;
    }

    launcher_hide_face_follow_guard_popup();

    s_face_follow_guard_popup = lv_obj_create(s_grid);
    if (s_face_follow_guard_popup == NULL) {
        return;
    }
    lv_obj_set_size(s_face_follow_guard_popup, LV_HOR_RES - 48, 154);
    lv_obj_center(s_face_follow_guard_popup);
    ui_page_style_panel(s_face_follow_guard_popup);
    lv_obj_set_style_pad_all(s_face_follow_guard_popup, 14, 0);
    lv_obj_clear_flag(s_face_follow_guard_popup,
                      LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE |
                      LV_OBJ_FLAG_SCROLL_CHAIN);

    lv_obj_t *label = lv_label_create(s_face_follow_guard_popup);
    if (label == NULL) {
        launcher_hide_face_follow_guard_popup();
        return;
    }
    lv_obj_set_width(label, LV_HOR_RES - 76);
    lv_obj_set_height(label, 58);
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    lv_label_set_text(label,
                      app_language_text("请先关闭小智 AI，再开启人脸跟随，避免运行卡顿",
                                        "Disable Xiaozhi AI before enabling face follow to avoid lag."));
    lv_obj_set_style_text_font(label, &font_puhui_20_4, 0);
    lv_obj_set_style_text_color(label, lv_color_white(), 0);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(label, LV_ALIGN_TOP_MID, 0, 8);

    lv_obj_t *cancel_button = lv_btn_create(s_face_follow_guard_popup);
    if (cancel_button == NULL) {
        launcher_hide_face_follow_guard_popup();
        return;
    }
    lv_obj_set_size(cancel_button, 96, 38);
    lv_obj_align(cancel_button, LV_ALIGN_BOTTOM_LEFT, 16, -12);
    ui_page_style_button(cancel_button, 0x34C759, 0x269A43, 18);
    lv_obj_add_event_cb(cancel_button, launcher_face_follow_guard_cancel_cb,
                        LV_EVENT_CLICKED, NULL);
    lv_obj_t *cancel_label = lv_label_create(cancel_button);
    lv_label_set_text(cancel_label, app_language_text("确认", "OK"));
    lv_obj_set_style_text_font(cancel_label, &font_puhui_16_4, 0);
    lv_obj_set_style_text_color(cancel_label, lv_color_white(), 0);
    lv_obj_center(cancel_label);

    lv_obj_t *continue_button = lv_btn_create(s_face_follow_guard_popup);
    if (continue_button == NULL) {
        launcher_hide_face_follow_guard_popup();
        return;
    }
    lv_obj_set_size(continue_button, 96, 38);
    lv_obj_align(continue_button, LV_ALIGN_BOTTOM_RIGHT, -16, -12);
    ui_page_style_button(continue_button, 0xE5484D, 0xB83232, 18);
    lv_obj_add_event_cb(continue_button,
                        launcher_face_follow_guard_continue_cb,
                        LV_EVENT_CLICKED, NULL);
    lv_obj_t *continue_label = lv_label_create(continue_button);
    lv_label_set_text(continue_label,
                      app_language_text("继续进入", "Continue"));
    lv_obj_set_style_text_font(continue_label, &font_puhui_16_4, 0);
    lv_obj_set_style_text_color(continue_label, lv_color_white(), 0);
    lv_obj_center(continue_label);

    s_face_follow_guard_timer = lv_timer_create(
        launcher_face_follow_guard_timer_cb, 2500, NULL);
    if (s_face_follow_guard_timer == NULL) {
        launcher_hide_face_follow_guard_popup();
    }
}

static void launcher_back_button_cb(lv_event_t *e)
{
    (void)e;
    app_sfx_play(APP_SFX_TAP);
    ui_page_manager_switch(PAGE_LAUNCHER);
}

static void launcher_cancel_pending_launch(void)
{
    if (s_launch_timer != NULL) {
        lv_timer_delete(s_launch_timer);
        s_launch_timer = NULL;
    }
}

static void launcher_launch_timer_cb(lv_timer_t *timer)
{
    if (s_launch_timer == timer) {
        s_launch_timer = NULL;
    }
    lv_timer_delete(timer);
    if (s_grid != NULL) {
        ui_page_manager_switch(s_pending_page);
    }
}

static void launcher_schedule_launch(ui_page_id_t page)
{
    if (s_grid == NULL) {
        return;
    }

    if (page == PAGE_FACE_FOLLOW && xiaozhi_blocks_face_follow()) {
        app_sfx_play(APP_SFX_TAP);
        launcher_show_face_follow_guard_popup();
        ESP_LOGW(TAG, "face follow launch blocked while Xiaozhi AI is active");
        return;
    }

    app_sfx_play(APP_SFX_TAP);
    s_pending_page = page;
    launcher_cancel_pending_launch();
    s_launch_timer = lv_timer_create(launcher_launch_timer_cb,
                                     LAUNCHER_LAUNCH_MS, NULL);
    if (s_launch_timer == NULL) {
        ui_page_manager_switch(page);
    }
}

static void launcher_draw_text(lv_layer_t *layer, const char *text,
                               const lv_font_t *font, lv_color_t color,
                               const lv_area_t *area)
{
    lv_draw_label_dsc_t dsc;
    lv_draw_label_dsc_init(&dsc);
    dsc.text = text;
    dsc.text_static = 1;
    dsc.font = font;
    dsc.color = color;
    dsc.align = LV_TEXT_ALIGN_CENTER;
    dsc.flag = LV_TEXT_FLAG_EXPAND;
    lv_draw_label(layer, &dsc, area);
}

static void launcher_draw_bubble(lv_layer_t *layer,
                                 const launcher_item_t *item)
{
    if (item->image != NULL) {
        lv_draw_image_dsc_t image_dsc;
        lv_draw_image_dsc_init(&image_dsc);
        image_dsc.src = item->image;

        const int32_t image_width = item->image->header.w;
        const int32_t image_height = item->image->header.h;
        lv_area_t image_area;
        lv_area_set(&image_area,
                    item->center_x - image_width / 2,
                    item->center_y - image_height / 2,
                    item->center_x + (image_width + 1) / 2 - 1,
                    item->center_y + (image_height + 1) / 2 - 1);
        lv_draw_image(layer, &image_dsc, &image_area);
        return;
    }

    const int32_t x = item->center_x - LAUNCHER_BUBBLE_SIZE / 2;
    const int32_t y = item->center_y - LAUNCHER_BUBBLE_SIZE / 2;

    lv_draw_rect_dsc_t bubble_dsc;
    lv_draw_rect_dsc_init(&bubble_dsc);
    bubble_dsc.radius = LAUNCHER_BUBBLE_SIZE / 2;
    bubble_dsc.bg_color = lv_color_hex(item->color);
    bubble_dsc.bg_opa = LV_OPA_COVER;
    bubble_dsc.border_width = 1;
    bubble_dsc.border_color = lv_color_white();
    bubble_dsc.border_opa = LV_OPA_40;

    lv_area_t bubble_area;
    lv_area_set(&bubble_area, x, y,
                x + LAUNCHER_BUBBLE_SIZE - 1,
                y + LAUNCHER_BUBBLE_SIZE - 1);
    lv_draw_rect(layer, &bubble_dsc, &bubble_area);

    const int32_t icon_height = lv_font_get_line_height(&font_awesome_30_4);
    lv_area_t icon_area;
    lv_area_set(&icon_area,
                item->center_x - LAUNCHER_BUBBLE_SIZE / 2,
                item->center_y - icon_height / 2,
                item->center_x + LAUNCHER_BUBBLE_SIZE / 2 - 1,
                item->center_y - icon_height / 2 + icon_height - 1);
    launcher_draw_text(layer, item->icon, &font_awesome_30_4,
                       lv_color_white(), &icon_area);
}

static void launcher_draw_item_label(lv_layer_t *layer,
                                     const launcher_item_t *item)
{
    lv_area_t label_area;
    lv_area_set(&label_area,
                item->center_x - LAUNCHER_LABEL_WIDTH / 2,
                item->center_y + LAUNCHER_BUBBLE_SIZE / 2 + LAUNCHER_LABEL_GAP,
                item->center_x + LAUNCHER_LABEL_WIDTH / 2 - 1,
                item->center_y + LAUNCHER_BUBBLE_SIZE / 2 + LAUNCHER_LABEL_GAP + 21);
    launcher_draw_text(layer, app_language_text(item->label_zh, item->label_en), &font_puhui_16_4,
                       lv_color_hex(0xD5D9E0), &label_area);
}

static void launcher_render_cache(void)
{
    if (s_cache_canvas == NULL || s_cache_buffer == NULL) {
        return;
    }

    lv_canvas_fill_bg(s_cache_canvas, lv_color_black(), LV_OPA_COVER);

    lv_layer_t layer;
    lv_canvas_init_layer(s_cache_canvas, &layer);
    for (int i = 0; i < LAUNCHER_N; ++i) {
        launcher_draw_bubble(&layer, &s_items[i]);
        launcher_draw_item_label(&layer, &s_items[i]);
    }
    lv_canvas_finish_layer(s_cache_canvas, &layer);
}

static void launcher_set_cache_position(void)
{
    if (s_cache_canvas == NULL) {
        return;
    }

    const int32_t x = (int32_t)lroundf(s_offset_x);
    const int32_t y = (int32_t)lroundf(s_offset_y);
    if (x == s_cache_x && y == s_cache_y) {
        return;
    }

    s_cache_x = x;
    s_cache_y = y;
    lv_obj_set_pos(s_cache_canvas, x, y);
}

static bool launcher_create_cache(lv_obj_t *parent)
{
    s_cache_buffer = (lv_color_t *)heap_caps_malloc(
        LAUNCHER_CACHE_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (s_cache_buffer == NULL) {
        ESP_LOGE(TAG, "launcher cache allocation failed (%u bytes)",
                 (unsigned)LAUNCHER_CACHE_BYTES);
        return false;
    }

    s_cache_canvas = lv_canvas_create(parent);
    if (s_cache_canvas == NULL) {
        heap_caps_free(s_cache_buffer);
        s_cache_buffer = NULL;
        return false;
    }

    lv_canvas_set_buffer(s_cache_canvas, s_cache_buffer,
                         LAUNCHER_CACHE_W, LAUNCHER_CACHE_H,
                         LV_COLOR_FORMAT_RGB565);
    lv_obj_set_size(s_cache_canvas, LAUNCHER_CACHE_W, LAUNCHER_CACHE_H);
    lv_obj_set_pos(s_cache_canvas, 0, 0);
    lv_obj_add_flag(s_cache_canvas, LV_OBJ_FLAG_IGNORE_LAYOUT);
    lv_obj_clear_flag(s_cache_canvas, LV_OBJ_FLAG_CLICKABLE |
                      LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_SCROLL_CHAIN);
    lv_obj_set_style_bg_opa(s_cache_canvas, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_cache_canvas, 0, 0);
    lv_obj_set_style_pad_all(s_cache_canvas, 0, 0);
    lv_obj_set_style_radius(s_cache_canvas, 0, 0);
    return true;
}

static bool launcher_create_touch_layer(lv_obj_t *parent)
{
    s_touch_layer = lv_obj_create(parent);
    if (s_touch_layer == NULL) {
        return false;
    }

    lv_obj_set_size(s_touch_layer, LV_HOR_RES, LAUNCHER_VIEW_H);
    lv_obj_set_pos(s_touch_layer, 0, LAUNCHER_VIEW_Y);
    lv_obj_add_flag(s_touch_layer, LV_OBJ_FLAG_IGNORE_LAYOUT);
    lv_obj_add_flag(s_touch_layer, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(s_touch_layer, LV_OBJ_FLAG_SCROLLABLE |
                      LV_OBJ_FLAG_SCROLL_CHAIN | LV_OBJ_FLAG_SCROLL_ELASTIC |
                      LV_OBJ_FLAG_SCROLL_MOMENTUM);
    lv_obj_set_style_bg_opa(s_touch_layer, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_touch_layer, 0, 0);
    lv_obj_set_style_shadow_width(s_touch_layer, 0, 0);
    lv_obj_set_style_pad_all(s_touch_layer, 0, 0);
    lv_obj_set_style_radius(s_touch_layer, 0, 0);
    lv_obj_add_event_cb(s_touch_layer, launcher_touch_cb, LV_EVENT_ALL, NULL);
    return true;
}

static void launcher_move_by(float dx, float dy)
{
    s_offset_x += dx;
    s_offset_y += dy;

    if (s_offset_x < LAUNCHER_MIN_OFFSET_X) {
        s_offset_x = LAUNCHER_MIN_OFFSET_X;
    } else if (s_offset_x > LAUNCHER_MAX_OFFSET_X) {
        s_offset_x = LAUNCHER_MAX_OFFSET_X;
    }
    if (s_offset_y < LAUNCHER_MIN_OFFSET_Y) {
        s_offset_y = LAUNCHER_MIN_OFFSET_Y;
    } else if (s_offset_y > LAUNCHER_MAX_OFFSET_Y) {
        s_offset_y = LAUNCHER_MAX_OFFSET_Y;
    }

    launcher_set_cache_position();
}

static void launcher_stop_inertia(void)
{
    if (s_inertia_timer != NULL) {
        lv_timer_delete(s_inertia_timer);
        s_inertia_timer = NULL;
    }
}

static void launcher_inertia_cb(lv_timer_t *timer)
{
    const float before_x = s_offset_x;
    const float before_y = s_offset_y;

    s_drag_velocity_x *= 0.90f;
    s_drag_velocity_y *= 0.90f;
    launcher_move_by(s_drag_velocity_x, s_drag_velocity_y);

    if ((s_offset_x <= LAUNCHER_MIN_OFFSET_X && s_drag_velocity_x < 0.0f) ||
        (s_offset_x >= LAUNCHER_MAX_OFFSET_X && s_drag_velocity_x > 0.0f)) {
        s_drag_velocity_x *= -0.22f;
    }
    if ((s_offset_y <= LAUNCHER_MIN_OFFSET_Y && s_drag_velocity_y < 0.0f) ||
        (s_offset_y >= LAUNCHER_MAX_OFFSET_Y && s_drag_velocity_y > 0.0f)) {
        s_drag_velocity_y *= -0.22f;
    }

    const bool still_moving = hypotf(s_drag_velocity_x, s_drag_velocity_y) > 0.16f ||
                              before_x != s_offset_x || before_y != s_offset_y;
    if (!still_moving) {
        if (s_inertia_timer == timer) {
            s_inertia_timer = NULL;
        }
        lv_timer_delete(timer);
    }
}

static void launcher_start_inertia(void)
{
    launcher_stop_inertia();
    if (hypotf(s_drag_velocity_x, s_drag_velocity_y) <= 0.16f) {
        return;
    }
    s_inertia_timer = lv_timer_create(launcher_inertia_cb,
                                      LAUNCHER_INERTIA_MS, NULL);
}

static bool launcher_event_point(lv_event_t *e, lv_point_t *point)
{
    lv_indev_t *indev = lv_event_get_indev(e);
    if (indev == NULL || point == NULL) {
        return false;
    }
    lv_indev_get_point(indev, point);
    return true;
}

static int launcher_hit_test(int x, int y)
{
    const int32_t local_x = x - (int32_t)lroundf(s_offset_x);
    const int32_t local_y = y - (int32_t)lroundf(s_offset_y);
    const int32_t radius = LAUNCHER_BUBBLE_SIZE / 2;
    const int32_t radius_sq = radius * radius;

    for (int i = 0; i < LAUNCHER_N; ++i) {
        const int32_t dx = local_x - s_items[i].center_x;
        const int32_t dy = local_y - s_items[i].center_y;
        if (dx * dx + dy * dy <= radius_sq) {
            return i;
        }
    }
    return -1;
}

static void launcher_finish_drag(void)
{
    if (!s_dragging) {
        return;
    }

    s_dragging = false;
    s_suppress_click = s_drag_moved;
    if (s_drag_moved) {
        launcher_start_inertia();
    } else {
        s_drag_velocity_x = 0.0f;
        s_drag_velocity_y = 0.0f;
    }
}

static void launcher_touch_cb(lv_event_t *e)
{
    const lv_event_code_t code = lv_event_get_code(e);
    lv_point_t point;

    if (code == LV_EVENT_PRESSED) {
        if (!launcher_event_point(e, &point)) {
            return;
        }
        launcher_cancel_pending_launch();
        launcher_stop_inertia();
        s_dragging = true;
        s_drag_moved = false;
        s_suppress_click = false;
        s_drag_start_x = point.x;
        s_drag_start_y = point.y;
        s_drag_last_x = point.x;
        s_drag_last_y = point.y;
        s_drag_velocity_x = 0.0f;
        s_drag_velocity_y = 0.0f;
        return;
    }

    if (code == LV_EVENT_PRESSING) {
        if (!s_dragging || !launcher_event_point(e, &point)) {
            return;
        }

        const int32_t dx = point.x - s_drag_last_x;
        const int32_t dy = point.y - s_drag_last_y;
        s_drag_last_x = point.x;
        s_drag_last_y = point.y;
        if (dx != 0 || dy != 0) {
            s_drag_velocity_x = (float)dx;
            s_drag_velocity_y = (float)dy;
            launcher_move_by((float)dx, (float)dy);
        }
        if (abs(point.x - s_drag_start_x) > 6 ||
            abs(point.y - s_drag_start_y) > 6) {
            s_drag_moved = true;
        }
        return;
    }

    if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
        launcher_finish_drag();
        return;
    }

    if (code == LV_EVENT_CLICKED) {
        if (s_suppress_click) {
            s_suppress_click = false;
            return;
        }
        if (!launcher_event_point(e, &point)) {
            return;
        }
        const int item_index = launcher_hit_test(point.x, point.y);
        if (item_index >= 0) {
            launcher_schedule_launch(s_items[item_index].page);
        }
    }
}

lv_obj_t *ui_page_make_back_button(lv_obj_t *root)
{
    if (root == NULL) {
        return NULL;
    }

    lv_obj_t *button = lv_btn_create(root);
    if (button == NULL) {
        return NULL;
    }

    lv_obj_set_size(button, 36, 34);
    lv_obj_set_pos(button, 6, 6);
    lv_obj_set_style_radius(button, 11, 0);
    lv_obj_set_style_bg_color(button, lv_color_hex(0x151E24), 0);
    lv_obj_set_style_bg_color(button, lv_color_hex(0x2A3942),
                              LV_STATE_PRESSED);
    lv_obj_set_style_border_width(button, 1, 0);
    lv_obj_set_style_border_color(button, lv_color_hex(0x2A3942), 0);
    lv_obj_set_style_border_opa(button, LV_OPA_COVER, 0);
    lv_obj_set_style_shadow_width(button, 0, 0);
    lv_obj_set_style_pad_all(button, 0, 0);
    lv_obj_add_event_cb(button, launcher_back_button_cb, LV_EVENT_CLICKED,
                        NULL);

    lv_obj_t *icon = lv_image_create(button);
    lv_image_set_src(icon, &launcher_icon_back);
    lv_obj_set_size(icon, 22, 22);
    lv_image_set_inner_align(icon, LV_IMAGE_ALIGN_CONTAIN);
    lv_obj_center(icon);
    return button;
}

esp_err_t page_launcher_on_enter(void)
{
    lv_obj_t *root = esp_xiaozhi_chat_display_page_root();
    if (root == NULL) {
        return ESP_FAIL;
    }
    ui_page_style_root(root);
    /* The launcher fills the shared page root edge-to-edge. */
    lv_obj_set_style_pad_all(root, 0, 0);

    s_grid = lv_obj_create(root);
    if (s_grid == NULL) {
        return ESP_ERR_NO_MEM;
    }
    lv_obj_set_size(s_grid, LV_HOR_RES, LAUNCHER_VIEW_H);
    lv_obj_set_pos(s_grid, 0, LAUNCHER_VIEW_Y);
    lv_obj_add_flag(s_grid, LV_OBJ_FLAG_IGNORE_LAYOUT |
                    LV_OBJ_FLAG_OVERFLOW_VISIBLE);
    lv_obj_set_style_bg_opa(s_grid, LV_OPA_TRANSP, 0);
    lv_obj_set_style_radius(s_grid, 0, 0);
    lv_obj_set_style_border_width(s_grid, 0, 0);
    lv_obj_set_style_shadow_width(s_grid, 0, 0);
    lv_obj_set_style_pad_all(s_grid, 0, 0);
    lv_obj_clear_flag(s_grid, LV_OBJ_FLAG_SCROLLABLE |
                      LV_OBJ_FLAG_SCROLL_CHAIN);

    s_offset_x = 0.0f;
    s_offset_y = 0.0f;
    s_cache_x = 0;
    s_cache_y = 0;
    s_dragging = false;
    s_drag_moved = false;
    s_suppress_click = false;
    s_drag_velocity_x = 0.0f;
    s_drag_velocity_y = 0.0f;
    s_pending_page = PAGE_LAUNCHER;
    launcher_cancel_pending_launch();
    launcher_stop_inertia();

    if (!launcher_create_cache(s_grid) ||
        !launcher_create_touch_layer(s_grid)) {
        ESP_LOGE(TAG, "launcher cache/touch layer creation failed");
        if (s_grid != NULL) {
            lv_obj_del(s_grid);
        }
        s_grid = NULL;
        s_cache_canvas = NULL;
        s_touch_layer = NULL;
        if (s_cache_buffer != NULL) {
            heap_caps_free(s_cache_buffer);
            s_cache_buffer = NULL;
        }
        lv_obj_set_style_pad_all(root, 4, 0);
        return ESP_ERR_NO_MEM;
    }
    launcher_render_cache();

    app_sfx_play(APP_SFX_PAGE);
    return ESP_OK;
}

esp_err_t page_launcher_on_exit(void)
{
    launcher_cancel_pending_launch();
    launcher_stop_inertia();
    launcher_hide_face_follow_guard_popup();
    s_dragging = false;

    /* Delete the canvas before releasing its externally owned PSRAM buffer. */
    if (s_grid != NULL) {
        lv_obj_del(s_grid);
    }
    s_grid = NULL;
    s_cache_canvas = NULL;
    s_touch_layer = NULL;
    if (s_cache_buffer != NULL) {
        heap_caps_free(s_cache_buffer);
        s_cache_buffer = NULL;
    }

    lv_obj_t *root = esp_xiaozhi_chat_display_page_root();
    if (root != NULL) {
        /* Restore the shared root's default inset for the other pages. */
        lv_obj_set_style_pad_all(root, 4, 0);
    }
    return ESP_OK;
}

void page_launcher_on_tick(uint32_t ms)
{
    (void)ms;
}

bool page_launcher_on_gesture(ui_gesture_t g)
{
    /* Swipe down returns to the emote face (home). */
    if (g == UI_GESTURE_SWIPE_DOWN) {
        app_sfx_play(APP_SFX_PAGE);
        extern esp_err_t ui_page_manager_switch(ui_page_id_t);
        ui_page_manager_switch(PAGE_FACE);
        return true;
    }
    return false;
}

bool page_launcher_on_gesture_at(ui_gesture_t g, int x, int y)
{
    (void)x;

    /* Touches in the desktop belong to its two-axis drag handler. Consume the
     * independent gesture so a drag cannot turn into page navigation. */
    if (s_grid != NULL && y >= LAUNCHER_DRAG_TOP && g != UI_GESTURE_TAP) {
        return true;
    }

    /* Keep an edge gesture as the explicit return affordance. */
    if (g == UI_GESTURE_SWIPE_DOWN || g == UI_GESTURE_SWIPE_RIGHT) {
        app_sfx_play(APP_SFX_PAGE);
        ui_page_manager_switch(PAGE_FACE);
        return true;
    }

    /* The prototype reserves only down/right for leaving the desktop. */
    if (g == UI_GESTURE_SWIPE_LEFT) {
        return true;
    }

    return page_launcher_on_gesture(g);
}

const ui_page_t page_launcher = {
    .id = PAGE_LAUNCHER,
    .name = "launcher",
    .on_enter = page_launcher_on_enter,
    .on_exit = page_launcher_on_exit,
    .on_tick = page_launcher_on_tick,
    .on_gesture = page_launcher_on_gesture,
    .on_gesture_at = page_launcher_on_gesture_at,
};
