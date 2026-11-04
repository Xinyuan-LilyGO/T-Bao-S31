/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Focus timer page for the ESP-VoCat-S31 "喵伴".
 *
 * The page uses a compact ring dial as the primary timer control. Tapping the
 * center opens a vertical roller so the focus duration can be adjusted without
 * exposing a dense collection of numeric controls on the main screen.
 */
#include "page_timer.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "app_sfx.h"
#include "app_language.h"
#include "esp_attr.h"
#include "esp_xiaozhi_chat_display.h"
#include "launcher_icons.h"
#include "lvgl.h"
#include "page_launcher.h"
#include "ui_page_style.h"

LV_FONT_DECLARE(font_puhui_16_4);
LV_FONT_DECLARE(font_puhui_20_4);

#define TIMER_MAX_MINUTES 1439
#define TIMER_DEFAULT_MINUTES 25
#define TIMER_PRESET_COUNT 4
#define TIMER_OPTIONS_BUFFER_SIZE (TIMER_MAX_MINUTES * 6 + 1)

#define TIMER_ACCENT lv_color_hex(0xFF786F)
#define TIMER_PANEL lv_color_hex(0x151E24)
#define TIMER_LINE lv_color_hex(0x2A3942)
#define TIMER_TEXT lv_color_hex(0xF4F7F8)
#define TIMER_MUTED lv_color_hex(0x8C9AA3)

static const int s_preset_minutes[TIMER_PRESET_COUNT] = { 25, 5, 15, 60 };
static const char *s_preset_names[TIMER_PRESET_COUNT] = {
    "25 分", "5 分", "15 分", "1 小时",
};

static lv_obj_t *s_dial = NULL;
static lv_obj_t *s_dial_button = NULL;
static lv_obj_t *s_time_label = NULL;
static lv_obj_t *s_state_label = NULL;
static lv_obj_t *s_caption_label = NULL;
static lv_obj_t *s_start_label = NULL;
static lv_obj_t *s_time_overlay = NULL;
static lv_obj_t *s_duration_roller = NULL;
static lv_obj_t *s_selected_duration_label = NULL;
static lv_obj_t *s_preset_buttons[TIMER_PRESET_COUNT] = { 0 };

/* ~8.6 KiB of option labels built once for the roller widget. Plain UI text,
 * no DMA/ISR use; keep it in PSRAM to leave the DIRAM for the heap. */
static EXT_RAM_BSS_ATTR char s_duration_options[TIMER_OPTIONS_BUFFER_SIZE];
static bool s_duration_options_ready;

static int32_t s_duration_ms = TIMER_DEFAULT_MINUTES * 60000;
static int32_t s_remaining_ms = TIMER_DEFAULT_MINUTES * 60000;
static int32_t s_draft_minutes = TIMER_DEFAULT_MINUTES;
static bool s_running;
static bool s_paused;
static bool s_finished;
static uint32_t s_last_tick;

static void refresh_time(void);
static void update_progress(void);
static void update_controls(void);

static int clamp_minutes(int minutes)
{
    if (minutes < 1) {
        return 1;
    }
    if (minutes > TIMER_MAX_MINUTES) {
        return TIMER_MAX_MINUTES;
    }
    return minutes;
}

static void build_duration_options(void)
{
    if (s_duration_options_ready) {
        return;
    }

    size_t offset = 0;
    for (int minute = 1; minute <= TIMER_MAX_MINUTES; ++minute) {
        int written = snprintf(&s_duration_options[offset],
                               sizeof(s_duration_options) - offset,
                               "%d%s", minute,
                               minute == TIMER_MAX_MINUTES ? "" : "\n");
        if (written <= 0 || (size_t)written >= sizeof(s_duration_options) - offset) {
            s_duration_options[0] = '\0';
            return;
        }
        offset += (size_t)written;
    }
    s_duration_options_ready = true;
}

static void format_duration(int minutes, char *buffer, size_t buffer_size)
{
    minutes = clamp_minutes(minutes);
    if (minutes >= 60) {
        if (app_language_is_english()) {
            snprintf(buffer, buffer_size, "%dh %02dm", minutes / 60,
                     minutes % 60);
        } else {
            snprintf(buffer, buffer_size, "%d 小时%02d 分钟", minutes / 60,
                     minutes % 60);
        }
    } else {
        snprintf(buffer, buffer_size, app_language_is_english() ? "%d min" : "%d 分钟", minutes);
    }
}

static void format_clock_ms(int32_t remaining_ms, char *buffer,
                            size_t buffer_size)
{
    int total_seconds = (int)((remaining_ms + 999) / 1000);
    if (total_seconds < 0) {
        total_seconds = 0;
    }
    if (total_seconds >= 3600) {
        snprintf(buffer, buffer_size, "%02d:%02d:%02d", total_seconds / 3600,
                 (total_seconds / 60) % 60, total_seconds % 60);
    } else {
        snprintf(buffer, buffer_size, "%02d:%02d", total_seconds / 60,
                 total_seconds % 60);
    }
}

static void update_progress(void)
{
    if (s_dial == NULL) {
        return;
    }

    int32_t value = 0;
    if (s_duration_ms > 0 && s_remaining_ms > 0) {
        value = (int32_t)(((int64_t)s_remaining_ms * 1000) /
                          s_duration_ms);
        if (value > 1000) {
            value = 1000;
        }
    }
    lv_arc_set_value(s_dial, value);
}

static void refresh_time(void)
{
    if (s_time_label == NULL) {
        return;
    }

    char time_text[20];
    char duration_text[32];
    format_clock_ms(s_remaining_ms, time_text, sizeof(time_text));
    format_duration((int)(s_duration_ms / 60000), duration_text,
                    sizeof(duration_text));
    lv_label_set_text(s_time_label, time_text);
    if (s_caption_label != NULL) {
        lv_label_set_text_fmt(s_caption_label,
                              app_language_is_english() ? "%s focus" : "%s专注",
                              duration_text);
    }
    if (s_state_label != NULL) {
        const char *state = s_finished ? app_language_text("时间到", "Time is up") :
                            !s_running ? app_language_text("准备开始", "Ready to start") :
                            (s_paused ? app_language_text("已暂停", "Paused") :
                             app_language_text("专注进行中", "Focus in progress"));
        lv_label_set_text(s_state_label, state);
    }
    update_progress();
}

static void update_controls(void)
{
    if (s_start_label != NULL) {
        const char *text = s_finished ? app_language_text("重新开始", "Restart") :
                           !s_running ? app_language_text("开始专注", "Start focus") :
                           (s_paused ? app_language_text("继续专注", "Resume focus") :
                            app_language_text("暂停专注", "Pause focus"));
        lv_label_set_text(s_start_label, text);
    }

    const bool can_edit = !s_running && !s_finished;
    if (s_dial_button != NULL) {
        if (can_edit) {
            lv_obj_clear_state(s_dial_button, LV_STATE_DISABLED);
        } else {
            lv_obj_add_state(s_dial_button, LV_STATE_DISABLED);
        }
    }
    for (int i = 0; i < TIMER_PRESET_COUNT; ++i) {
        if (s_preset_buttons[i] == NULL) {
            continue;
        }
        if (can_edit) {
            lv_obj_clear_state(s_preset_buttons[i], LV_STATE_DISABLED);
        } else {
            lv_obj_add_state(s_preset_buttons[i], LV_STATE_DISABLED);
        }
        const bool selected = s_duration_ms / 60000 == s_preset_minutes[i];
        lv_obj_set_style_bg_color(s_preset_buttons[i],
                                  selected ? lv_color_hex(0x492A2C) :
                                             TIMER_PANEL, 0);
        lv_obj_set_style_bg_opa(s_preset_buttons[i],
                                selected ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_color(s_preset_buttons[i],
                                      selected ? TIMER_ACCENT : TIMER_LINE, 0);
    }
}

static void set_duration_minutes(int minutes)
{
    minutes = clamp_minutes(minutes);
    s_duration_ms = (int32_t)minutes * 60000;
    s_remaining_ms = s_duration_ms;
    s_running = false;
    s_paused = false;
    s_finished = false;
    s_last_tick = ui_page_manager_now_ms();
    refresh_time();
    update_controls();
}

static void start_button_cb(lv_event_t *event)
{
    (void)event;
    app_sfx_play(APP_SFX_TAP);

    if (s_finished) {
        s_remaining_ms = s_duration_ms;
        s_finished = false;
        s_running = true;
        s_paused = false;
        s_last_tick = ui_page_manager_now_ms();
    } else if (!s_running) {
        s_running = true;
        s_paused = false;
        s_last_tick = ui_page_manager_now_ms();
    } else {
        s_paused = !s_paused;
        if (!s_paused) {
            s_last_tick = ui_page_manager_now_ms();
        }
    }
    refresh_time();
    update_controls();
}

static void reset_button_cb(lv_event_t *event)
{
    (void)event;
    app_sfx_play(APP_SFX_TAP);
    s_remaining_ms = s_duration_ms;
    s_running = false;
    s_paused = false;
    s_finished = false;
    s_last_tick = ui_page_manager_now_ms();
    refresh_time();
    update_controls();
}

static void preset_button_cb(lv_event_t *event)
{
    app_sfx_play(APP_SFX_TAP);
    const int index = (int)(intptr_t)lv_event_get_user_data(event);
    if (index < 0 || index >= TIMER_PRESET_COUNT || s_running || s_finished) {
        return;
    }
    set_duration_minutes(s_preset_minutes[index]);
}

static void update_dialog_selection(void)
{
    if (s_duration_roller == NULL) {
        return;
    }

    s_draft_minutes = (int)lv_roller_get_selected(s_duration_roller) + 1;
    if (s_selected_duration_label != NULL) {
        char duration_text[32];
        format_duration(s_draft_minutes, duration_text, sizeof(duration_text));
        lv_label_set_text(s_selected_duration_label, duration_text);
    }
}

static void duration_roller_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) == LV_EVENT_VALUE_CHANGED) {
        update_dialog_selection();
    }
}

static void close_duration_dialog(void)
{
    if (s_time_overlay != NULL) {
        lv_obj_del(s_time_overlay);
        s_time_overlay = NULL;
    }
    s_duration_roller = NULL;
    s_selected_duration_label = NULL;
}

static void duration_cancel_cb(lv_event_t *event)
{
    (void)event;
    app_sfx_play(APP_SFX_TAP);
    close_duration_dialog();
}

static void duration_apply_cb(lv_event_t *event)
{
    (void)event;
    app_sfx_play(APP_SFX_TAP);
    if (s_duration_roller != NULL) {
        update_dialog_selection();
        set_duration_minutes(s_draft_minutes);
    }
    close_duration_dialog();
}

static lv_obj_t *dialog_button(lv_obj_t *parent, const char *text,
                               lv_event_cb_t callback, uint32_t color)
{
    lv_obj_t *button = lv_btn_create(parent);
    if (button == NULL) {
        return NULL;
    }
    lv_obj_set_size(button, 112, 38);
    ui_page_style_button(button, color, 0x30343A, 12);
    lv_obj_add_event_cb(button, callback, LV_EVENT_CLICKED, NULL);
    lv_obj_t *label = lv_label_create(button);
    lv_label_set_text(label, app_language_text(text, NULL));
    lv_obj_set_style_text_font(label, &font_puhui_16_4, 0);
    lv_obj_set_style_text_color(label, TIMER_TEXT, 0);
    lv_obj_center(label);
    return button;
}

static void show_duration_dialog(void)
{
    if (s_time_overlay != NULL || s_running || s_finished) {
        return;
    }

    build_duration_options();
    if (!s_duration_options_ready) {
        return;
    }

    lv_obj_t *root = esp_xiaozhi_chat_display_page_root();
    if (root == NULL) {
        return;
    }

    s_draft_minutes = clamp_minutes((int)(s_duration_ms / 60000));
    s_time_overlay = lv_obj_create(root);
    if (s_time_overlay == NULL) {
        return;
    }
    lv_obj_set_size(s_time_overlay, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_pos(s_time_overlay, 0, 0);
    lv_obj_set_style_bg_color(s_time_overlay, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_time_overlay, LV_OPA_80, 0);
    lv_obj_set_style_border_width(s_time_overlay, 0, 0);
    lv_obj_set_style_pad_all(s_time_overlay, 0, 0);
    lv_obj_clear_flag(s_time_overlay, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *dialog = lv_obj_create(s_time_overlay);
    lv_obj_set_size(dialog, 286, 248);
    lv_obj_center(dialog);
    ui_page_style_panel(dialog);
    lv_obj_set_style_pad_all(dialog, 12, 0);

    lv_obj_t *title = lv_label_create(dialog);
    lv_label_set_text(title, app_language_text("设置专注时长", "Set focus duration"));
    lv_obj_set_style_text_font(title, &font_puhui_20_4, 0);
    lv_obj_set_style_text_color(title, TIMER_TEXT, 0);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 0, 0);

    lv_obj_t *close = lv_btn_create(dialog);
    lv_obj_set_size(close, 34, 32);
    lv_obj_align(close, LV_ALIGN_TOP_RIGHT, 0, -2);
    ui_page_style_button(close, 0x151E24, 0x2A3942, 10);
    lv_obj_add_event_cb(close, duration_cancel_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *close_label = lv_label_create(close);
    lv_label_set_text(close_label, "×");
    lv_obj_set_style_text_font(close_label, &font_puhui_20_4, 0);
    lv_obj_set_style_text_color(close_label, TIMER_MUTED, 0);
    lv_obj_center(close_label);

    s_selected_duration_label = lv_label_create(dialog);
    lv_obj_set_width(s_selected_duration_label, 250);
    lv_obj_set_style_text_align(s_selected_duration_label, LV_TEXT_ALIGN_CENTER,
                                0);
    lv_obj_set_style_text_font(s_selected_duration_label, &font_puhui_20_4, 0);
    lv_obj_set_style_text_color(s_selected_duration_label, TIMER_ACCENT, 0);
    lv_obj_align(s_selected_duration_label, LV_ALIGN_TOP_MID, 0, 35);

    lv_obj_t *hint = lv_label_create(dialog);
    lv_label_set_text(hint, app_language_text("上下滑动刻度调整 · 每格 1 分钟", "Swipe the scale; each step is 1 minute"));
    lv_obj_set_width(hint, 250);
    lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(hint, &font_puhui_16_4, 0);
    lv_obj_set_style_text_color(hint, TIMER_MUTED, 0);
    lv_obj_align(hint, LV_ALIGN_TOP_MID, 0, 62);

    s_duration_roller = lv_roller_create(dialog);
    lv_obj_set_size(s_duration_roller, 178, 86);
    lv_obj_align(s_duration_roller, LV_ALIGN_TOP_MID, 0, 86);
    lv_roller_set_options(s_duration_roller, s_duration_options,
                          LV_ROLLER_MODE_NORMAL);
    lv_roller_set_visible_row_count(s_duration_roller, 5);
    lv_roller_set_selected(s_duration_roller, (uint32_t)(s_draft_minutes - 1),
                           LV_ANIM_OFF);
    lv_obj_set_style_bg_color(s_duration_roller, TIMER_PANEL, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_duration_roller, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_text_font(s_duration_roller, &font_puhui_16_4,
                               LV_PART_MAIN);
    lv_obj_set_style_text_color(s_duration_roller, TIMER_MUTED, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_duration_roller, lv_color_hex(0x492A2C),
                              LV_PART_SELECTED);
    lv_obj_set_style_text_font(s_duration_roller, &font_puhui_16_4,
                               LV_PART_SELECTED);
    lv_obj_set_style_text_color(s_duration_roller, TIMER_ACCENT,
                                LV_PART_SELECTED);
    lv_obj_set_style_border_width(s_duration_roller, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(s_duration_roller, TIMER_LINE, LV_PART_MAIN);
    lv_obj_add_event_cb(s_duration_roller, duration_roller_cb,
                        LV_EVENT_VALUE_CHANGED, NULL);

    lv_obj_t *cancel = dialog_button(dialog, "取消", duration_cancel_cb,
                                     0x151E24);
    if (cancel != NULL) {
        lv_obj_align(cancel, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    }
    lv_obj_t *apply = dialog_button(dialog, "确认时长", duration_apply_cb,
                                    0xFF786F);
    if (apply != NULL) {
        lv_obj_align(apply, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
    }
    update_dialog_selection();
}

static void dial_button_cb(lv_event_t *event)
{
    (void)event;
    app_sfx_play(APP_SFX_TAP);
    show_duration_dialog();
}

static lv_obj_t *make_preset_button(lv_obj_t *parent, int index)
{
    lv_obj_t *button = lv_btn_create(parent);
    if (button == NULL) {
        return NULL;
    }
    lv_obj_set_size(button, 68, 28);
    ui_page_style_button(button, 0x000000, 0x2A3942, 10);
    lv_obj_add_event_cb(button, preset_button_cb, LV_EVENT_CLICKED,
                        (void *)(intptr_t)index);
    lv_obj_t *label = lv_label_create(button);
    lv_label_set_text(label, app_language_text(s_preset_names[index], NULL));
    lv_obj_set_style_text_font(label, &font_puhui_16_4, 0);
    lv_obj_set_style_text_color(label, TIMER_MUTED, 0);
    lv_obj_center(label);
    return button;
}

static bool page_timer_on_gesture_at(ui_gesture_t gesture, int x, int y)
{
    (void)x;
    (void)y;
    /* The roller owns vertical swipes while the duration dialog is open. */
    return s_time_overlay != NULL && gesture != UI_GESTURE_NONE;
}

esp_err_t page_timer_on_enter(void)
{
    lv_obj_t *root = esp_xiaozhi_chat_display_page_root();
    if (root == NULL) {
        return ESP_FAIL;
    }

    s_dial = NULL;
    s_dial_button = NULL;
    s_time_label = NULL;
    s_state_label = NULL;
    s_caption_label = NULL;
    s_start_label = NULL;
    s_time_overlay = NULL;
    s_duration_roller = NULL;
    s_selected_duration_label = NULL;
    for (int i = 0; i < TIMER_PRESET_COUNT; ++i) {
        s_preset_buttons[i] = NULL;
    }
    if (s_duration_ms < 60000 ||
        s_duration_ms > TIMER_MAX_MINUTES * 60000) {
        s_duration_ms = TIMER_DEFAULT_MINUTES * 60000;
    }
    s_remaining_ms = s_duration_ms;
    s_draft_minutes = (int32_t)(s_duration_ms / 60000);
    s_running = false;
    s_paused = false;
    s_finished = false;
    s_last_tick = ui_page_manager_now_ms();

    ui_page_style_root(root);
    ui_page_make_back_button(root);
    ui_page_make_header_image(root, &launcher_icon_timer, "专注计时", "环形钟面");

    s_dial = lv_arc_create(root);
    lv_obj_set_size(s_dial, 184, 184);
    lv_obj_align(s_dial, LV_ALIGN_TOP_MID, 0, 56);
    lv_arc_set_bg_angles(s_dial, 0, 360);
    lv_arc_set_range(s_dial, 0, 1000);
    lv_arc_set_value(s_dial, 1000);
    lv_obj_clear_flag(s_dial, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_opa(s_dial, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_dial, 0, LV_PART_MAIN);
    lv_obj_set_style_arc_width(s_dial, 9, LV_PART_MAIN);
    lv_obj_set_style_arc_color(s_dial, lv_color_hex(0x2A373D), LV_PART_MAIN);
    lv_obj_set_style_arc_opa(s_dial, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_arc_width(s_dial, 9, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(s_dial, TIMER_ACCENT, LV_PART_INDICATOR);
    lv_obj_set_style_arc_rounded(s_dial, true, LV_PART_INDICATOR);
    lv_obj_set_style_arc_opa(s_dial, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_opa(s_dial, LV_OPA_0, LV_PART_KNOB);

    s_dial_button = lv_btn_create(root);
    lv_obj_set_size(s_dial_button, 146, 146);
    lv_obj_align(s_dial_button, LV_ALIGN_TOP_MID, 0, 75);
    lv_obj_set_style_bg_color(s_dial_button, TIMER_PANEL, 0);
    lv_obj_set_style_bg_opa(s_dial_button, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_dial_button, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(s_dial_button, 1, 0);
    lv_obj_set_style_border_color(s_dial_button, TIMER_LINE, 0);
    lv_obj_set_style_shadow_width(s_dial_button, 0, 0);
    lv_obj_add_event_cb(s_dial_button, dial_button_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *mode = lv_label_create(s_dial_button);
    lv_label_set_text(mode, app_language_text("专注", "FOCUS"));
    lv_obj_set_style_text_font(mode, &font_puhui_16_4, 0);
    lv_obj_set_style_text_color(mode, TIMER_MUTED, 0);
    lv_obj_align(mode, LV_ALIGN_CENTER, 0, -38);

    s_time_label = lv_label_create(s_dial_button);
    lv_obj_set_style_text_font(s_time_label, &lv_font_montserrat_30, 0);
    lv_obj_set_style_text_color(s_time_label, TIMER_TEXT, 0);
    lv_obj_set_style_text_align(s_time_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(s_time_label, 136);
    lv_obj_align(s_time_label, LV_ALIGN_CENTER, 0, -8);

    s_caption_label = lv_label_create(s_dial_button);
    lv_obj_set_style_text_font(s_caption_label, &font_puhui_16_4, 0);
    lv_obj_set_style_text_color(s_caption_label, TIMER_MUTED, 0);
    lv_obj_set_style_text_align(s_caption_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(s_caption_label, 136);
    lv_obj_align(s_caption_label, LV_ALIGN_CENTER, 0, 29);

    s_state_label = lv_label_create(root);
    lv_obj_set_style_text_font(s_state_label, &font_puhui_16_4, 0);
    lv_obj_set_style_text_color(s_state_label, TIMER_MUTED, 0);
    lv_obj_set_width(s_state_label, LV_HOR_RES - 24);
    lv_obj_set_style_text_align(s_state_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_state_label, LV_ALIGN_TOP_MID, 0, 236);

    lv_obj_t *start_button = lv_btn_create(root);
    lv_obj_set_size(start_button, 178, 40);
    lv_obj_align(start_button, LV_ALIGN_TOP_MID, -22, 255);
    ui_page_style_button(start_button, 0xFF786F, 0xE85E5A, 15);
    lv_obj_add_event_cb(start_button, start_button_cb, LV_EVENT_CLICKED, NULL);
    s_start_label = lv_label_create(start_button);
    lv_obj_set_style_text_font(s_start_label, &font_puhui_16_4, 0);
    lv_obj_set_style_text_color(s_start_label, lv_color_hex(0x291618), 0);
    lv_obj_center(s_start_label);

    lv_obj_t *reset_button = lv_btn_create(root);
    lv_obj_set_size(reset_button, 48, 40);
    lv_obj_align(reset_button, LV_ALIGN_TOP_MID, 100, 255);
    ui_page_style_button(reset_button, 0x151E24, 0x2A3942, 15);
    lv_obj_add_event_cb(reset_button, reset_button_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *reset_label = lv_label_create(reset_button);
    lv_label_set_text(reset_label, "↺");
    lv_obj_set_style_text_font(reset_label, &font_puhui_20_4, 0);
    lv_obj_set_style_text_color(reset_label, TIMER_MUTED, 0);
    lv_obj_center(reset_label);

    lv_obj_t *presets = lv_obj_create(root);
    lv_obj_set_size(presets, LV_HOR_RES - 26, 28);
    lv_obj_align(presets, LV_ALIGN_TOP_MID, 0, 298);
    lv_obj_set_flex_flow(presets, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(presets, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_bg_opa(presets, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(presets, 0, 0);
    lv_obj_set_style_pad_all(presets, 0, 0);
    lv_obj_set_style_pad_column(presets, 5, 0);
    lv_obj_clear_flag(presets, LV_OBJ_FLAG_SCROLLABLE);
    for (int i = 0; i < TIMER_PRESET_COUNT; ++i) {
        s_preset_buttons[i] = make_preset_button(presets, i);
    }

    refresh_time();
    update_controls();
    app_sfx_play(APP_SFX_PAGE);
    return ESP_OK;
}

esp_err_t page_timer_on_exit(void)
{
    s_time_overlay = NULL;
    s_duration_roller = NULL;
    s_selected_duration_label = NULL;
    s_dial = NULL;
    s_dial_button = NULL;
    s_time_label = NULL;
    s_state_label = NULL;
    s_caption_label = NULL;
    s_start_label = NULL;
    for (int i = 0; i < TIMER_PRESET_COUNT; ++i) {
        s_preset_buttons[i] = NULL;
    }
    return ESP_OK;
}

void page_timer_on_tick(uint32_t ms)
{
    if (!s_running || s_remaining_ms <= 0 || s_paused || s_finished) {
        return;
    }

    int32_t dt = (int32_t)(ms - s_last_tick);
    s_last_tick = ms;
    if (dt <= 0) {
        return;
    }
    if (dt > 1000) {
        dt = 1000;
    }

    s_remaining_ms -= dt;
    if (s_remaining_ms <= 0) {
        s_remaining_ms = 0;
        s_running = false;
        s_finished = true;
        s_paused = false;
        refresh_time();
        update_controls();
        app_sfx_play(APP_SFX_ALARM);
        return;
    }
    refresh_time();
}

const ui_page_t page_timer = {
    .id = PAGE_TIMER,
    .name = "timer",
    .on_enter = page_timer_on_enter,
    .on_exit = page_timer_on_exit,
    .on_gesture_at = page_timer_on_gesture_at,
    .on_tick = page_timer_on_tick,
};
