/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * SD-card video library and direct-to-panel playback page.
 */
#include "page_video.h"

#include "app_sfx.h"
#include "app_language.h"
#include "app_sdcard.h"
#include "app_video_overlay.h"
#include "app_video_player.h"
#include "esp_lvgl_port.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_xiaozhi_chat_display.h"
#include "font_awesome_symbols.h"
#include "lvgl.h"
#include "page_launcher.h"
#include "ui_page_style.h"
#include "launcher_icons.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TAG "PAGE_VIDEO"

#define VIDEO_MAX_ITEMS 32
#define VIDEO_LIST_Y 68
#define VIDEO_ROW_H 42
#define VIDEO_LIST_GAP 8
#define VIDEO_CONTROL_BAR_HEIGHT 58
#define VIDEO_CONTROL_BUTTON_SIZE 48
#define VIDEO_CONTROL_BUTTON_MARGIN 8
#define VIDEO_CONTROL_BAR_Y (LV_VER_RES - VIDEO_CONTROL_BAR_HEIGHT)
#define VIDEO_CONTROLS_HIDE_MS 3000U
#define VIDEO_ACTION_BADGE_MS 600U
#define VIDEO_MODE_BADGE_MS 2000U

#define VIDEO_BG lv_color_black()
#define VIDEO_FG lv_color_white()
#define VIDEO_DIM lv_color_hex(0xA0A0A8)
#define VIDEO_ACCENT lv_color_hex(0x4DA3FF)

typedef enum {
    VIDEO_PLAY_MODE_SINGLE_LOOP = 0,
    VIDEO_PLAY_MODE_RANDOM,
    VIDEO_PLAY_MODE_SEQUENTIAL,
} video_play_mode_t;

static video_play_mode_t video_cycle_mode(video_play_mode_t mode)
{
    switch (mode) {
    case VIDEO_PLAY_MODE_SINGLE_LOOP:
        return VIDEO_PLAY_MODE_RANDOM;
    case VIDEO_PLAY_MODE_RANDOM:
        return VIDEO_PLAY_MODE_SEQUENTIAL;
    case VIDEO_PLAY_MODE_SEQUENTIAL:
    default:
        return VIDEO_PLAY_MODE_SINGLE_LOOP;
    }
}

static int video_choose_next_index(video_play_mode_t mode, int current,
                                   int count, uint32_t random_value)
{
    if (count <= 0 || current < 0 || current >= count) {
        return -1;
    }
    if (mode == VIDEO_PLAY_MODE_SINGLE_LOOP) {
        return current;
    }
    if (mode == VIDEO_PLAY_MODE_SEQUENTIAL) {
        return current + 1 < count ? current + 1 : -1;
    }
    if (count == 1) {
        return current;
    }
    return (current + 1 + (int)(random_value % (uint32_t)(count - 1))) % count;
}

typedef enum {
    VIDEO_VIEW_LIBRARY = 0,
    VIDEO_VIEW_PLAYBACK,
} video_view_t;

LV_FONT_DECLARE(font_puhui_16_4);
LV_FONT_DECLARE(font_puhui_20_4);
LV_FONT_DECLARE(font_awesome_20_4);

static app_sdcard_video_item_t s_items[VIDEO_MAX_ITEMS];
static lv_obj_t *s_rows[VIDEO_MAX_ITEMS];
static int s_item_count;
static int s_current_item = -1;
static video_view_t s_view = VIDEO_VIEW_LIBRARY;
static video_play_mode_t s_play_mode = VIDEO_PLAY_MODE_SINGLE_LOOP;
static volatile bool s_video_mode;
static volatile bool s_transitioning;
static bool s_overlay_ready;
static bool s_controls_visible;
static uint32_t s_controls_deadline_ms;
static uint32_t s_action_badge_deadline_ms;
static uint32_t s_mode_badge_deadline_ms;
static lv_obj_t *s_status;
static lv_obj_t *s_list;

static esp_err_t video_start_item(int index, bool take_panel_ownership);
static void video_item_cb(lv_event_t *event);

static uint32_t video_now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static char video_lower_ascii(char value)
{
    if (value >= 'A' && value <= 'Z') {
        return (char)(value + ('a' - 'A'));
    }
    return value;
}

static int video_item_compare(const void *left, const void *right)
{
    const app_sdcard_video_item_t *a = left;
    const app_sdcard_video_item_t *b = right;
    size_t index = 0;
    while (a->title[index] != '\0' && b->title[index] != '\0') {
        const char a_char = video_lower_ascii(a->title[index]);
        const char b_char = video_lower_ascii(b->title[index]);
        if (a_char != b_char) {
            return (unsigned char)a_char - (unsigned char)b_char;
        }
        ++index;
    }
    return (unsigned char)a->title[index] - (unsigned char)b->title[index];
}

static const char *video_mode_label(video_play_mode_t mode)
{
    switch (mode) {
    case VIDEO_PLAY_MODE_RANDOM:
        return app_language_text("随机播放", "Random");
    case VIDEO_PLAY_MODE_SEQUENTIAL:
        return app_language_text("顺序播放", "Sequential");
    case VIDEO_PLAY_MODE_SINGLE_LOOP:
    default:
        return app_language_text("单曲循环", "Repeat one");
    }
}

static void video_set_status(const char *text)
{
    if (s_status != NULL && text != NULL) {
        lv_label_set_text(s_status, app_language_text(text, NULL));
    }
}

static void video_set_library_status(const char *prefix)
{
    if (s_item_count <= 0) {
        video_set_status("未找到 AVI/MJPEG 视频");
        return;
    }
    char status[96];
    if (prefix != NULL && prefix[0] != '\0') {
        const char *localized_prefix = app_language_text(prefix, NULL);
        snprintf(status, sizeof(status), app_language_is_english() ? "%s · %d videos · %s" : "%s · %d 个视频 · %s", localized_prefix,
                 s_item_count, video_mode_label(s_play_mode));
    } else {
        snprintf(status, sizeof(status), app_language_is_english() ? "%d videos · %s" : "%d 个视频 · %s", s_item_count,
                 video_mode_label(s_play_mode));
    }
    video_set_status(status);
}

static void video_mark_selected(int index)
{
    for (int i = 0; i < s_item_count && i < VIDEO_MAX_ITEMS; ++i) {
        if (s_rows[i] == NULL) {
            continue;
        }
        lv_obj_set_style_bg_color(
            s_rows[i], i == index ? lv_color_hex(0x263F5A) :
                                    lv_color_hex(0x20232A), 0);
    }
}

static void video_publish_overlay(bool controls_visible, bool paused,
                                  bool show_action_badge,
                                  app_video_overlay_action_t action)
{
    app_video_overlay_state_t state = {
        .controls_visible = controls_visible,
        .paused = paused,
        .show_action_badge = show_action_badge,
        .show_mode_badge = s_mode_badge_deadline_ms != 0,
        .mode = (app_video_overlay_mode_t)s_play_mode,
        .action = action,
    };
    esp_err_t ret = app_video_player_set_overlay_state(&state);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "publish video controls failed: %s",
                 esp_err_to_name(ret));
    }
}

static void video_show_controls(bool paused,
                                app_video_overlay_action_t action)
{
    const uint32_t now = video_now_ms();
    s_controls_visible = true;
    s_controls_deadline_ms = now + VIDEO_CONTROLS_HIDE_MS;
    s_action_badge_deadline_ms = action == APP_VIDEO_OVERLAY_ACTION_NONE
        ? 0 : now + VIDEO_ACTION_BADGE_MS;
    video_publish_overlay(true, paused,
                         action != APP_VIDEO_OVERLAY_ACTION_NONE, action);
}

static void video_cycle_play_mode(void)
{
    s_play_mode = video_cycle_mode(s_play_mode);
    s_controls_visible = true;
    s_controls_deadline_ms = video_now_ms() + VIDEO_CONTROLS_HIDE_MS;
    s_action_badge_deadline_ms = 0;
    s_mode_badge_deadline_ms = video_now_ms() + VIDEO_MODE_BADGE_MS;
    const bool paused = app_video_player_get_state() ==
                        APP_VIDEO_PLAYER_PAUSED;
    video_publish_overlay(true, paused, false,
                          APP_VIDEO_OVERLAY_ACTION_NONE);
}

static void video_autoplay_async(void *arg)
{
    (void)arg;
    if (ui_page_manager_current() != PAGE_VIDEO || s_item_count <= 0 ||
        s_video_mode || s_transitioning || !s_overlay_ready) {
        return;
    }
    (void)video_start_item(0, true);
}

static lv_obj_t *video_create_row(lv_obj_t *parent, int index)
{
    lv_obj_t *button = lv_btn_create(parent);
    if (button == NULL) {
        return NULL;
    }
    lv_obj_set_size(button, LV_HOR_RES - 24, VIDEO_ROW_H - 2);
    lv_obj_set_style_radius(button, 12, 0);
    lv_obj_set_style_bg_color(button, index == s_current_item
                               ? lv_color_hex(0x263F5A)
                               : lv_color_hex(0x20232A), 0);
    lv_obj_set_style_bg_color(button, lv_color_hex(0x303946),
                              LV_STATE_PRESSED);
    lv_obj_set_style_border_width(button, 1, 0);
    lv_obj_set_style_border_color(button, lv_color_hex(0x343943), 0);
    lv_obj_add_event_cb(button, video_item_cb, LV_EVENT_CLICKED,
                        (void *)(intptr_t)index);
    s_rows[index] = button;

    lv_obj_t *icon = lv_label_create(button);
    lv_label_set_text(icon, FONT_AWESOME_SD_CARD);
    lv_obj_set_style_text_font(icon, &font_awesome_20_4, 0);
    lv_obj_set_style_text_color(icon, VIDEO_ACCENT, 0);
    lv_obj_align(icon, LV_ALIGN_LEFT_MID, 10, 0);

    lv_obj_t *label = lv_label_create(button);
    lv_label_set_text(label, s_items[index].title);
    lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
    lv_obj_set_width(label, LV_HOR_RES - 84);
    lv_obj_set_style_text_font(label, &font_puhui_16_4, 0);
    lv_obj_set_style_text_color(label, VIDEO_FG, 0);
    lv_obj_align(label, LV_ALIGN_LEFT_MID, 42, 0);
    return button;
}

static void video_item_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_CLICKED || s_video_mode ||
        s_transitioning) {
        return;
    }
    int index = (int)(intptr_t)lv_event_get_user_data(event);
    if (index < 0 || index >= s_item_count) {
        return;
    }

    app_sfx_play(APP_SFX_TAP);
    s_current_item = index;
    video_mark_selected(index);
    video_set_library_status("正在播放");
    (void)video_start_item(index, true);
}

static esp_err_t video_stop_renderer(bool deinit_overlay)
{
    s_transitioning = true;
    esp_err_t first_error = app_video_player_stop();
    esp_err_t display_ret =
        esp_xiaozhi_chat_display_set_video_visible(false);
    if (first_error == ESP_OK && display_ret != ESP_OK) {
        first_error = display_ret;
    }
    s_video_mode = false;
    s_view = VIDEO_VIEW_LIBRARY;
    s_controls_visible = false;
    s_controls_deadline_ms = 0;
    s_action_badge_deadline_ms = 0;
    s_mode_badge_deadline_ms = 0;
    if (deinit_overlay) {
        app_video_overlay_deinit();
        s_overlay_ready = false;
    }
    s_transitioning = false;
    return first_error;
}

static esp_err_t video_show_library_after_stop(const char *status)
{
    esp_err_t ret = video_stop_renderer(false);
    bool locked = lvgl_port_lock(1000);
    if (locked) {
        video_mark_selected(s_current_item);
        if (status != NULL) {
            video_set_library_status(status);
        } else {
            video_set_library_status("选择视频播放");
        }
        lvgl_port_unlock();
    }
    return ret;
}

static esp_err_t video_start_item(int index, bool take_panel_ownership)
{
    if (index < 0 || index >= s_item_count || !s_overlay_ready ||
        s_transitioning) {
        return ESP_ERR_INVALID_ARG;
    }

    s_current_item = index;
    if (take_panel_ownership) {
        video_mark_selected(index);
        video_set_library_status("正在播放");
    }

    if (take_panel_ownership) {
        esp_err_t display_ret =
            esp_xiaozhi_chat_display_set_video_visible(true);
        if (display_ret != ESP_OK) {
            video_set_status("显示屏切换失败");
            return display_ret;
        }
    }

    esp_err_t ret = app_video_player_play(s_items[index].path);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "start '%s' failed: %s", s_items[index].path,
                 esp_err_to_name(ret));
        if (take_panel_ownership || s_video_mode) {
            (void)esp_xiaozhi_chat_display_set_video_visible(false);
        }
        s_video_mode = false;
        s_view = VIDEO_VIEW_LIBRARY;
        video_set_status(ret == ESP_ERR_NOT_SUPPORTED
                         ? "暂不支持该视频格式" : "视频无法播放");
        return ret;
    }

    s_view = VIDEO_VIEW_PLAYBACK;
    s_video_mode = true;
    s_transitioning = false;
    s_controls_visible = false;
    s_controls_deadline_ms = 0;
    s_action_badge_deadline_ms = 0;
    s_mode_badge_deadline_ms = 0;
    video_publish_overlay(false, false, false,
                          APP_VIDEO_OVERLAY_ACTION_NONE);
    return ESP_OK;
}

static bool video_point_in_button(int x, int y, int left)
{
    return x >= left && x < left + VIDEO_CONTROL_BUTTON_SIZE &&
           y >= VIDEO_CONTROL_BAR_Y + 5 &&
           y < VIDEO_CONTROL_BAR_Y + 5 + VIDEO_CONTROL_BUTTON_SIZE;
}

static void video_return_to_launcher(void)
{
    esp_err_t ret = video_stop_renderer(true);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "leave video page failed: %s", esp_err_to_name(ret));
        return;
    }
    (void)ui_page_manager_switch(PAGE_LAUNCHER);
}

static void video_toggle_pause(void)
{
    const app_video_player_state_t state = app_video_player_get_state();
    if (state == APP_VIDEO_PLAYER_PLAYING) {
        if (app_video_player_pause() == ESP_OK) {
            video_show_controls(true, APP_VIDEO_OVERLAY_ACTION_PAUSE);
        }
    } else if (state == APP_VIDEO_PLAYER_PAUSED) {
        if (app_video_player_resume() == ESP_OK) {
            video_show_controls(false, APP_VIDEO_OVERLAY_ACTION_PLAY);
        }
    }
}

static bool page_video_is_direct_mode(void)
{
    return s_video_mode && s_view == VIDEO_VIEW_PLAYBACK &&
           !s_transitioning;
}

static bool page_video_on_direct_gesture_at(ui_gesture_t gesture, int x, int y)
{
    if (!page_video_is_direct_mode()) {
        return false;
    }

    if ((gesture == UI_GESTURE_SWIPE_RIGHT && x >= 0 && x <= 28) ||
        (gesture == UI_GESTURE_SWIPE_DOWN && y >= 0 && y <= 28)) {
        video_return_to_launcher();
        return true;
    }

    if (gesture == UI_GESTURE_TAP) {
        const int right_button_left = LV_HOR_RES -
                                      VIDEO_CONTROL_BUTTON_MARGIN -
                                      VIDEO_CONTROL_BUTTON_SIZE;
        if (y >= VIDEO_CONTROL_BAR_Y &&
            video_point_in_button(x, y, VIDEO_CONTROL_BUTTON_MARGIN)) {
            video_cycle_play_mode();
            return true;
        }
        if (y >= VIDEO_CONTROL_BAR_Y &&
            video_point_in_button(x, y, right_button_left)) {
            (void)video_show_library_after_stop("选择视频播放");
            return true;
        }
        video_toggle_pause();
        return true;
    }

    return true;
}

static void video_autohide_controls(uint32_t now, bool paused)
{
    bool changed = false;
    bool show_action = s_action_badge_deadline_ms != 0;
    if (show_action && now >= s_action_badge_deadline_ms) {
        s_action_badge_deadline_ms = 0;
        show_action = false;
        changed = true;
    }
    if (s_mode_badge_deadline_ms != 0 &&
        now >= s_mode_badge_deadline_ms) {
        s_mode_badge_deadline_ms = 0;
        changed = true;
    }
    if (s_controls_visible && !paused && s_controls_deadline_ms != 0 &&
        now >= s_controls_deadline_ms) {
        s_controls_visible = false;
        s_controls_deadline_ms = 0;
        changed = true;
    }
    if (changed) {
        video_publish_overlay(s_controls_visible, paused, show_action,
                              APP_VIDEO_OVERLAY_ACTION_NONE);
    }
}

esp_err_t page_video_on_enter(void)
{
    app_sfx_play(APP_SFX_PAGE);
    s_item_count = 0;
    s_current_item = -1;
    s_view = VIDEO_VIEW_LIBRARY;
    s_play_mode = VIDEO_PLAY_MODE_SINGLE_LOOP;
    s_video_mode = false;
    s_transitioning = false;
    s_overlay_ready = false;
    s_controls_visible = false;
    s_controls_deadline_ms = 0;
    s_action_badge_deadline_ms = 0;
    s_mode_badge_deadline_ms = 0;
    s_status = NULL;
    s_list = NULL;
    memset(s_rows, 0, sizeof(s_rows));

    s_item_count = app_sdcard_scan_video(s_items, VIDEO_MAX_ITEMS);
    if (s_item_count > 1) {
        qsort(s_items, (size_t)s_item_count, sizeof(s_items[0]),
              video_item_compare);
    }

    esp_err_t player_ret = app_video_player_init();
    if (player_ret != ESP_OK) {
        ESP_LOGW(TAG, "video decoder initialization failed: %s",
                 esp_err_to_name(player_ret));
    }

    lv_obj_t *root = esp_xiaozhi_chat_display_page_root();
    if (root == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    ui_page_style_root(root);
    lv_obj_set_style_pad_all(root, 4, 0);
    ui_page_make_back_button(root);
    ui_page_make_header_image(root, &launcher_icon_video, "视频", "SD 媒体库");

    s_status = lv_label_create(root);
    lv_obj_set_style_text_font(s_status, &font_puhui_16_4, 0);
    lv_obj_set_style_text_color(s_status, VIDEO_DIM, 0);
    lv_obj_set_width(s_status, LV_HOR_RES - 24);
    lv_obj_set_style_text_align(s_status, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_status, LV_ALIGN_TOP_MID, 0, 55);

    s_list = lv_obj_create(root);
    lv_obj_set_size(s_list, LV_HOR_RES - 16, LV_VER_RES - VIDEO_LIST_Y - 18);
    lv_obj_set_pos(s_list, 4, VIDEO_LIST_Y + 10);
    lv_obj_set_style_bg_opa(s_list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_list, 0, 0);
    lv_obj_set_style_pad_all(s_list, 0, 0);
    lv_obj_set_style_pad_row(s_list, VIDEO_LIST_GAP, 0);
    lv_obj_set_layout(s_list, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(s_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_list, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START);
    lv_obj_set_scroll_dir(s_list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(s_list, LV_SCROLLBAR_MODE_AUTO);

    if (s_item_count <= 0) {
        video_set_status("未找到 AVI/MJPEG 视频");
        lv_obj_t *empty = lv_label_create(s_list);
        lv_label_set_text(empty, app_language_text("请将 MJPEG AVI 放入 SD 卡根目录", "Put an MJPEG AVI in the SD card root"));
        lv_obj_set_style_text_font(empty, &font_puhui_16_4, 0);
        lv_obj_set_style_text_color(empty, VIDEO_DIM, 0);
        lv_obj_set_width(empty, LV_HOR_RES - 28);
        lv_obj_set_style_text_align(empty, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_align(empty, LV_ALIGN_TOP_MID, 0, 72);
        return ESP_OK;
    }

    for (int i = 0; i < s_item_count; ++i) {
        (void)video_create_row(s_list, i);
    }
    video_set_library_status(NULL);

    esp_err_t overlay_ret = app_video_overlay_init(LV_HOR_RES, LV_VER_RES);
    if (overlay_ret != ESP_OK) {
        video_set_status("视频控件初始化失败");
        ESP_LOGW(TAG, "video overlay initialization failed: %s",
                 esp_err_to_name(overlay_ret));
        return ESP_OK;
    }
    s_overlay_ready = true;

    if (player_ret == ESP_OK &&
        lv_async_call(video_autoplay_async, NULL) != LV_RESULT_OK) {
        video_set_status("视频自动播放失败");
    }
    return ESP_OK;
}

esp_err_t page_video_on_exit(void)
{
    s_transitioning = true;
    esp_err_t player_ret = app_video_player_stop();
    esp_err_t display_ret =
        esp_xiaozhi_chat_display_set_video_visible(false);
    esp_err_t first_error = player_ret;
    if (first_error == ESP_OK && display_ret != ESP_OK) {
        first_error = display_ret;
    }
    s_video_mode = false;
    s_view = VIDEO_VIEW_LIBRARY;
    app_video_overlay_deinit();
    s_overlay_ready = false;
    s_transitioning = false;
    s_current_item = -1;
    s_controls_visible = false;
    s_controls_deadline_ms = 0;
    s_action_badge_deadline_ms = 0;
    s_mode_badge_deadline_ms = 0;
    s_status = NULL;
    s_list = NULL;
    memset(s_rows, 0, sizeof(s_rows));
    return first_error;
}

void page_video_on_tick(uint32_t ms)
{
    if (s_transitioning || !s_video_mode ||
        s_view != VIDEO_VIEW_PLAYBACK) {
        return;
    }

    const app_video_player_state_t state = app_video_player_get_state();
    video_autohide_controls(ms, state == APP_VIDEO_PLAYER_PAUSED);
    if (state != APP_VIDEO_PLAYER_STOPPED) {
        return;
    }

    const esp_err_t player_error = app_video_player_last_error();
    if (player_error != ESP_OK) {
        (void)video_show_library_after_stop(
            player_error == ESP_ERR_NOT_SUPPORTED
                ? "暂不支持该视频格式" : "视频无法播放");
        return;
    }

    const int next = video_choose_next_index(
        s_play_mode, s_current_item, s_item_count, esp_random());
    if (next >= 0) {
        if (video_start_item(next, false) != ESP_OK) {
            (void)video_show_library_after_stop("视频无法播放");
        }
    } else {
        (void)video_show_library_after_stop("播放结束");
    }
}

bool page_video_on_gesture_at(ui_gesture_t gesture, int x, int y)
{
    (void)gesture;
    (void)x;
    (void)y;
    return s_video_mode;
}

const ui_page_t page_video = {
    .id = PAGE_VIDEO,
    .name = "video",
    .on_enter = page_video_on_enter,
    .on_exit = page_video_on_exit,
    .on_gesture_at = page_video_on_gesture_at,
    .is_direct_mode = page_video_is_direct_mode,
    .on_direct_gesture_at = page_video_on_direct_gesture_at,
    .on_tick = page_video_on_tick,
};
