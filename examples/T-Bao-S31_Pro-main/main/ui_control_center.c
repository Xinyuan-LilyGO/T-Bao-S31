/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Pull-down control center for the ESP-VoCat-S31 "喵伴" pet.
 *
 * The layout follows the ElenixOS-inspired reference screen: a dark surface,
 * a compact header, four rounded control tiles, and a connection status row.
 * Volume and brightness open a focused adjustment view with one vertical
 * slider. All object work remains on LVGL's task through lv_async_call.
 */
#include "ui_control_center.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "esp_wifi.h"
#include "wifi_provisioning.h"
#include "esp_xiaozhi_chat_app.h"
#include "esp_xiaozhi_chat_display.h"
#include "ui_page_manager.h"
#include "page_settings.h"
#include "battery_monitor.h"
#include "font_awesome_symbols.h"
#include "app_sfx.h"
#include "app_language.h"
#include "lvgl.h"

static const char *TAG = "CTRL_CENTER";

LV_FONT_DECLARE(font_puhui_20_4);
LV_FONT_DECLARE(font_puhui_16_4);
LV_FONT_DECLARE(font_awesome_20_4);

/* The 320x320 S31 display needs a little inset so the rounded panel does not
 * crowd the display edge. The expressions are based on LVGL's active display
 * size, so the same layout remains usable if the board profile changes. */
#define PANEL_W             (LV_HOR_RES - 32)
#define PANEL_H             (LV_VER_RES - 32)
#define PANEL_OPEN_Y        16
#define PANEL_RADIUS        26
#define CC_CONTENT_PAD      20
#define CC_TILE_GAP         8
#define CC_TILE_W           ((PANEL_W - (CC_CONTENT_PAD * 2) - CC_TILE_GAP) / 2)
#define CC_TILE_H           80
#define CC_TILE_Y           62
#define CC_TILE_SECOND_ROW_Y (CC_TILE_Y + CC_TILE_H + CC_TILE_GAP)
#define CC_STATUS_Y         (PANEL_H - 58)
#define CC_FOOT_Y           (PANEL_H - 29)
#define CC_ADJUST_SLIDER_H  164

#define CC_PANEL_BG         lv_color_hex(0x111214)
#define CC_SURFACE          lv_color_hex(0x24272B)
#define CC_SURFACE_PRESSED  lv_color_hex(0x30343A)
#define CC_TRACK            lv_color_hex(0x2B2E33)
#define CC_BLUE             lv_color_hex(0x2094FA)
#define CC_ORANGE           lv_color_hex(0xFF9500)
#define CC_GREEN            lv_color_hex(0x34C759)
#define CC_RED              lv_color_hex(0xE5484D)
#define CC_TEXT             lv_color_white()
#define CC_MUTED            lv_color_hex(0x858A92)
#define CC_DIM              lv_color_hex(0x5E636B)
#define CC_BORDER           lv_color_hex(0x3A3D42)

typedef enum {
    CC_TILE_VOLUME = 0,
    CC_TILE_BRIGHTNESS,
    CC_TILE_WIFI,
    CC_TILE_BATTERY,
} control_tile_kind_t;

typedef enum {
    CC_ADJUST_NONE = 0,
    CC_ADJUST_VOLUME,
    CC_ADJUST_BRIGHTNESS,
} adjust_mode_t;

static lv_obj_t *s_panel = NULL;
static lv_obj_t *s_adjust_view = NULL;
static lv_obj_t *s_adjust_slider = NULL;
static lv_obj_t *s_adjust_back = NULL;
static lv_obj_t *s_adjust_title = NULL;
static lv_obj_t *s_adjust_value = NULL;
static lv_obj_t *s_adjust_min_icon = NULL;
static lv_obj_t *s_adjust_max_icon = NULL;
static lv_obj_t *s_adjust_hint = NULL;

static lv_obj_t *s_volume_tile = NULL;
static lv_obj_t *s_brightness_tile = NULL;
static lv_obj_t *s_wifi_tile = NULL;
static lv_obj_t *s_battery_tile_value = NULL;
static lv_obj_t *s_battery_tile_status = NULL;
static lv_obj_t *s_battery_progress = NULL;
static lv_obj_t *s_volume_tile_value = NULL;
static lv_obj_t *s_brightness_tile_value = NULL;
static lv_obj_t *s_wifi_tile_label = NULL;
static lv_obj_t *s_wifi_tile_value = NULL;
static lv_obj_t *s_wifi_icon = NULL;
static lv_obj_t *s_volume_progress = NULL;
static lv_obj_t *s_brightness_progress = NULL;
static lv_obj_t *s_wifi_status = NULL;
static lv_obj_t *s_wifi_status_dot = NULL;
static lv_obj_t *s_collapse_btn = NULL;

static bool s_open = false;
static bool s_inited = false;
static bool s_pending_open = false;
static bool s_saved_face = false;
static int s_wi_fi_confirm = 0;   /* 0 = normal, 1 = "再点一次确认" */
static adjust_mode_t s_adjust_mode = CC_ADJUST_NONE;

static void update_control_values(void);
static void open_adjust_view(adjust_mode_t mode);
static void close_adjust_view(void);
static void cc_restore_face_async(void *user_data);

static void set_percent_label(lv_obj_t *label, int value)
{
    if (label == NULL) {
        return;
    }
    char text[8];
    snprintf(text, sizeof(text), "%d%%", value);
    lv_label_set_text(label, app_language_text(text, NULL));
}

static lv_obj_t *make_text_label(lv_obj_t *parent, const char *text,
                                 const lv_font_t *font, lv_color_t color)
{
    lv_obj_t *label = lv_label_create(parent);
    if (label == NULL) {
        return NULL;
    }
    lv_label_set_text(label, app_language_text(text, NULL));
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_color(label, color, 0);
    return label;
}

static bool wifi_is_connected(void)
{
    wifi_ap_record_t record = {0};
    return esp_wifi_sta_get_ap_info(&record) == ESP_OK;
}

static void set_status_text(const char *text, bool attention)
{
    if (s_wifi_status != NULL) {
        lv_label_set_text(s_wifi_status, text);
        lv_obj_set_style_text_color(s_wifi_status, attention ? CC_ORANGE : CC_MUTED, 0);
    }
    if (s_wifi_status_dot != NULL) {
        lv_obj_set_style_bg_color(s_wifi_status_dot, attention ? CC_ORANGE : CC_GREEN, 0);
    }
}

static void refresh_wifi_status(void)
{
    const bool connected = wifi_is_connected();
    if (s_wifi_tile_value != NULL) {
        lv_label_set_text(s_wifi_tile_value,
                          app_language_text(connected ? "已连接" : "未连接", NULL));
    }
    if (s_wifi_icon != NULL) {
        lv_label_set_text(s_wifi_icon, connected ? LV_SYMBOL_WIFI : LV_SYMBOL_CLOSE);
    }
    set_status_text(app_language_text(connected ? "WiFi 已连接" : "WiFi 未连接", NULL), false);
}

static void update_volume_ui(void)
{
    const int value = esp_xiaozhi_chat_app_get_volume();
    set_percent_label(s_volume_tile_value, value);
    if (s_volume_progress != NULL) {
        lv_bar_set_value(s_volume_progress, value, LV_ANIM_OFF);
    }
    if (s_adjust_mode == CC_ADJUST_VOLUME) {
        set_percent_label(s_adjust_value, value);
        lv_slider_set_value(s_adjust_slider, value, LV_ANIM_OFF);
    }
}

static void update_brightness_ui(void)
{
    const int value = esp_xiaozhi_chat_app_get_brightness();
    set_percent_label(s_brightness_tile_value, value);
    if (s_brightness_progress != NULL) {
        lv_bar_set_value(s_brightness_progress, value, LV_ANIM_OFF);
    }
    if (s_adjust_mode == CC_ADJUST_BRIGHTNESS) {
        set_percent_label(s_adjust_value, value);
        lv_slider_set_value(s_adjust_slider, value, LV_ANIM_OFF);
    }
}

static const char *battery_tile_connection_text(
    const battery_monitor_state_t *state)
{
    if (state == NULL || !state->device_present || !state->part_id_valid) {
        return app_language_text("检测中", "Checking...");
    }
    if (state->data_stale) {
        return app_language_text("数据暂未更新", "Data temporarily stale");
    }
    if (state->charge_done) {
        return app_language_text("已充满", "Full");
    }
    if (state->charging) {
        return app_language_text("充电中", "Charging");
    }
    if (state->input_present || state->power_good || state->power_good_pin) {
        return app_language_text("充电中", "Charging");
    }
    return app_language_text("电池供电", "Battery power");
}

static void update_battery_ui(void)
{
    if (s_battery_tile_value == NULL || s_battery_tile_status == NULL ||
        s_battery_progress == NULL) {
        return;
    }

    battery_monitor_state_t state = {0};
    const bool available = battery_monitor_get_state(&state) == ESP_OK &&
                           state.device_present && state.part_id_valid;
    const bool percent_valid = available && (state.valid || state.data_stale) &&
                               state.battery_percent >= 0 &&
                               state.battery_percent <= 100;

    if (percent_valid) {
        lv_label_set_text_fmt(s_battery_tile_value, "%d%%",
                              state.battery_percent);
        lv_label_set_text(s_battery_tile_status,
                          battery_tile_connection_text(&state));
        lv_bar_set_value(s_battery_progress, state.battery_percent,
                         LV_ANIM_OFF);
    } else {
        lv_label_set_text(s_battery_tile_value, "--");
        lv_label_set_text(s_battery_tile_status,
                          battery_tile_connection_text(available ? &state : NULL));
        lv_bar_set_value(s_battery_progress, 0, LV_ANIM_OFF);
    }
}

static void apply_volume_value(int value)
{
    if (esp_xiaozhi_chat_app_preview_volume(value) != ESP_OK) {
        return;
    }
    update_volume_ui();
}

static void apply_brightness_value(int value)
{
    if (esp_xiaozhi_chat_app_preview_brightness(value) != ESP_OK) {
        return;
    }
    update_brightness_ui();
}

/* --- event handlers ------------------------------------------------------ */

static void adjust_slider_cb(lv_event_t *e)
{
    lv_obj_t *slider = lv_event_get_target_obj(e);
    if (slider == NULL || s_adjust_mode == CC_ADJUST_NONE) {
        return;
    }

    const int value = (int)lv_slider_get_value(slider);
    const lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
        if (s_adjust_mode == CC_ADJUST_VOLUME) {
            esp_xiaozhi_chat_app_commit_volume();
        } else {
            esp_xiaozhi_chat_app_commit_brightness();
        }
        return;
    }
    if (s_adjust_mode == CC_ADJUST_VOLUME) {
        apply_volume_value(value);
    } else {
        apply_brightness_value(value);
    }
}

static void wifi_btn_cb(lv_event_t *e)
{
    (void)e;
    app_sfx_play(APP_SFX_TAP);
    if (s_wi_fi_confirm == 0) {
        /* First tap arms the destructive action and leaves the device usable. */
        s_wi_fi_confirm = 1;
        if (s_wifi_tile != NULL) {
            lv_obj_set_style_bg_color(s_wifi_tile, lv_color_hex(0x49331A), 0);
        }
        if (s_wifi_tile_label != NULL) {
            lv_label_set_text(s_wifi_tile_label, app_language_text("再次确认", NULL));
        }
        if (s_wifi_tile_value != NULL) {
            lv_label_set_text(s_wifi_tile_value, app_language_text("重置", NULL));
        }
        set_status_text(app_language_text("再次点击确认重置 WiFi", "Tap again to reset WiFi"), true);
    } else {
        /* Second tap erases credentials and reboots into provisioning. */
        if (s_wifi_tile_label != NULL) {
            lv_label_set_text(s_wifi_tile_label, app_language_text("重置中", NULL));
        }
        if (s_wifi_tile_value != NULL) {
            lv_label_set_text(s_wifi_tile_value, app_language_text("请稍候", NULL));
        }
        set_status_text(app_language_text("正在重置 WiFi...", "Resetting WiFi..."), true);
        wifi_creds_erase_and_reboot();
    }
}

static void control_tile_cb(lv_event_t *e)
{
    const control_tile_kind_t kind =
        (control_tile_kind_t)(intptr_t)lv_event_get_user_data(e);
    if (kind != CC_TILE_WIFI) {
        app_sfx_play(APP_SFX_TAP);
    }
    switch (kind) {
    case CC_TILE_VOLUME:
        open_adjust_view(CC_ADJUST_VOLUME);
        break;
    case CC_TILE_BRIGHTNESS:
        open_adjust_view(CC_ADJUST_BRIGHTNESS);
        break;
    case CC_TILE_WIFI:
        wifi_btn_cb(e);
        break;
    case CC_TILE_BATTERY:
        ui_control_center_close();
        if (ui_page_manager_switch(PAGE_SETTINGS) == ESP_OK) {
            page_settings_open_battery();
        }
        break;
    default:
        break;
    }
}

/* Tap on the panel background (not on a control) closes the panel. LVGL
 * preserves the child event target, so taps on cards and the slider do not
 * accidentally close the control center. */
static void panel_bg_cb(lv_event_t *e)
{
    if (lv_event_get_target_obj(e) != s_panel) {
        return;
    }
    ui_control_center_close();
}

static void collapse_btn_cb(lv_event_t *e)
{
    (void)e;
    app_sfx_play(APP_SFX_TAP);
    ui_control_center_close();
}

static void adjust_back_cb(lv_event_t *e)
{
    (void)e;
    app_sfx_play(APP_SFX_TAP);
    close_adjust_view();
}

/* --- control tiles ------------------------------------------------------- */

static lv_obj_t *build_control_tile(const char *icon, const char *title,
                                    lv_color_t accent, control_tile_kind_t kind,
                                    int x, int y)
{
    lv_obj_t *tile = lv_btn_create(s_panel);
    if (tile == NULL) {
        return NULL;
    }
    lv_obj_set_size(tile, CC_TILE_W, CC_TILE_H);
    lv_obj_set_pos(tile, x, y);
    lv_obj_set_style_radius(tile, 22, 0);
    lv_obj_set_style_bg_color(tile, CC_SURFACE, 0);
    lv_obj_set_style_bg_color(tile, CC_SURFACE_PRESSED, LV_STATE_PRESSED);
    lv_obj_set_style_border_width(tile, 1, 0);
    lv_obj_set_style_border_color(tile, CC_BORDER, 0);
    lv_obj_set_style_pad_all(tile, 11, 0);
    lv_obj_set_style_pad_row(tile, 3, 0);
    lv_obj_set_flex_flow(tile, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(tile, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START);
    lv_obj_add_event_cb(tile, control_tile_cb, LV_EVENT_CLICKED,
                        (void *)(intptr_t)kind);

    lv_obj_t *icon_bg = lv_obj_create(tile);
    lv_obj_set_size(icon_bg, 30, 30);
    lv_obj_set_style_radius(icon_bg, 15, 0);
    lv_obj_set_style_bg_color(icon_bg, accent, 0);
    lv_obj_set_style_bg_opa(icon_bg, LV_OPA_30, 0);
    lv_obj_set_style_border_width(icon_bg, 0, 0);
    lv_obj_clear_flag(icon_bg, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *icon_label = make_text_label(icon_bg, icon, &font_awesome_20_4, accent);
    if (icon_label != NULL) {
        lv_obj_center(icon_label);
    }

    lv_obj_t *title_label = make_text_label(tile, title, &font_puhui_16_4, CC_TEXT);
    if (kind == CC_TILE_WIFI) {
        s_wifi_tile_label = title_label;
        s_wifi_icon = icon_label;
    }

    lv_obj_t *value_label = make_text_label(tile, "", &font_puhui_16_4, CC_MUTED);
    if (value_label != NULL) {
        lv_obj_add_flag(value_label, LV_OBJ_FLAG_IGNORE_LAYOUT);
        lv_obj_set_width(value_label, CC_TILE_W - 28);
        lv_obj_set_style_text_align(value_label, LV_TEXT_ALIGN_RIGHT, 0);
        lv_obj_align(value_label, LV_ALIGN_TOP_RIGHT, -1, 0);
    }

    if (kind == CC_TILE_BATTERY) {
        if (value_label != NULL) {
            lv_obj_set_width(value_label, 48);
            lv_obj_align(value_label, LV_ALIGN_TOP_RIGHT, -1, 0);
        }
        s_battery_tile_status = make_text_label(tile, "检测中",
                                                &font_puhui_16_4, CC_MUTED);
        if (s_battery_tile_status != NULL) {
            lv_obj_add_flag(s_battery_tile_status, LV_OBJ_FLAG_IGNORE_LAYOUT);
            lv_obj_set_width(s_battery_tile_status, 64);
            lv_label_set_long_mode(s_battery_tile_status, LV_LABEL_LONG_DOT);
            lv_obj_set_style_text_align(s_battery_tile_status,
                                        LV_TEXT_ALIGN_RIGHT, 0);
            lv_obj_align(s_battery_tile_status, LV_ALIGN_TOP_RIGHT, -1, 22);
        }
    }

    lv_obj_t *progress = lv_bar_create(tile);
    lv_obj_set_size(progress, CC_TILE_W - 22, 5);
    lv_bar_set_range(progress, 0, 100);
    lv_obj_set_style_radius(progress, 3, LV_PART_MAIN);
    lv_obj_set_style_radius(progress, 3, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(progress, CC_TRACK, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(progress, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(progress, accent, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(progress, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_clear_flag(progress, LV_OBJ_FLAG_CLICKABLE);

    switch (kind) {
    case CC_TILE_VOLUME:
        s_volume_tile = tile;
        s_volume_tile_value = value_label;
        s_volume_progress = progress;
        break;
    case CC_TILE_BRIGHTNESS:
        s_brightness_tile = tile;
        s_brightness_tile_value = value_label;
        s_brightness_progress = progress;
        break;
    case CC_TILE_WIFI:
        s_wifi_tile = tile;
        s_wifi_tile_value = value_label;
        lv_obj_add_flag(progress, LV_OBJ_FLAG_HIDDEN);
        break;
    case CC_TILE_BATTERY:
        s_battery_tile_value = value_label;
        s_battery_progress = progress;
        break;
    default:
        break;
    }
    return tile;
}

static void build_adjust_view(void)
{
    s_adjust_view = lv_obj_create(s_panel);
    lv_obj_set_size(s_adjust_view, PANEL_W, PANEL_H);
    lv_obj_set_pos(s_adjust_view, 0, 0);
    lv_obj_set_style_radius(s_adjust_view, PANEL_RADIUS, 0);
    lv_obj_set_style_bg_color(s_adjust_view, CC_PANEL_BG, 0);
    lv_obj_set_style_bg_opa(s_adjust_view, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_adjust_view, 0, 0);
    lv_obj_set_style_pad_all(s_adjust_view, 0, 0);
    lv_obj_add_flag(s_adjust_view, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_HIDDEN);

    s_adjust_back = lv_btn_create(s_adjust_view);
    lv_obj_set_size(s_adjust_back, 36, 36);
    lv_obj_set_pos(s_adjust_back, CC_CONTENT_PAD, 16);
    lv_obj_set_style_radius(s_adjust_back, 18, 0);
    lv_obj_set_style_bg_color(s_adjust_back, CC_SURFACE, 0);
    lv_obj_set_style_bg_color(s_adjust_back, CC_SURFACE_PRESSED, LV_STATE_PRESSED);
    lv_obj_set_style_border_width(s_adjust_back, 1, 0);
    lv_obj_set_style_border_color(s_adjust_back, CC_BORDER, 0);
    lv_obj_add_event_cb(s_adjust_back, adjust_back_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *back_icon = make_text_label(s_adjust_back, LV_SYMBOL_LEFT,
                                          &font_awesome_20_4, CC_TEXT);
    if (back_icon != NULL) {
        lv_obj_center(back_icon);
    }

    s_adjust_title = make_text_label(s_adjust_view, "音量", &font_puhui_20_4, CC_TEXT);
    if (s_adjust_title != NULL) {
        lv_obj_set_pos(s_adjust_title, 70, 20);
    }
    s_adjust_value = make_text_label(s_adjust_view, "0%", &font_puhui_20_4, CC_BLUE);
    if (s_adjust_value != NULL) {
        lv_obj_set_width(s_adjust_value, 76);
        lv_obj_set_style_text_align(s_adjust_value, LV_TEXT_ALIGN_RIGHT, 0);
        lv_obj_align(s_adjust_value, LV_ALIGN_TOP_RIGHT, -CC_CONTENT_PAD, 20);
    }

    s_adjust_slider = lv_slider_create(s_adjust_view);
    lv_obj_set_width(s_adjust_slider, 44);
    lv_obj_set_height(s_adjust_slider, CC_ADJUST_SLIDER_H);
    lv_obj_set_pos(s_adjust_slider, (PANEL_W - 44) / 2, 72);
    lv_slider_set_range(s_adjust_slider, 0, 100);
    lv_obj_set_style_radius(s_adjust_slider, 22, LV_PART_MAIN);
    lv_obj_set_style_radius(s_adjust_slider, 22, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(s_adjust_slider, CC_TRACK, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_adjust_slider, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_adjust_slider, CC_BLUE, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(s_adjust_slider, lv_color_hex(0xE6EBF0), LV_PART_KNOB);
    lv_obj_set_style_bg_opa(s_adjust_slider, LV_OPA_COVER, LV_PART_KNOB);
    lv_obj_set_style_radius(s_adjust_slider, 22, LV_PART_KNOB);
    lv_obj_set_style_pad_all(s_adjust_slider, 0, 0);
    lv_obj_add_event_cb(s_adjust_slider, adjust_slider_cb, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(s_adjust_slider, adjust_slider_cb, LV_EVENT_RELEASED, NULL);
    lv_obj_add_event_cb(s_adjust_slider, adjust_slider_cb, LV_EVENT_PRESS_LOST, NULL);

    s_adjust_min_icon = make_text_label(s_adjust_view, LV_SYMBOL_MINUS,
                                        &font_awesome_20_4, CC_DIM);
    if (s_adjust_min_icon != NULL) {
        lv_obj_set_pos(s_adjust_min_icon, (PANEL_W / 2) - 86, 244);
    }
    s_adjust_max_icon = make_text_label(s_adjust_view, LV_SYMBOL_PLUS,
                                        &font_awesome_20_4, CC_DIM);
    if (s_adjust_max_icon != NULL) {
        lv_obj_set_pos(s_adjust_max_icon, (PANEL_W / 2) + 76, 244);
    }
    s_adjust_hint = make_text_label(s_adjust_view, "拖动调整",
                                    &font_puhui_16_4, CC_DIM);
    if (s_adjust_hint != NULL) {
        lv_obj_set_width(s_adjust_hint, 120);
        lv_obj_set_style_text_align(s_adjust_hint, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_align(s_adjust_hint, LV_ALIGN_TOP_MID, 0, 246);
    }
}

static void build_panel(void)
{
    lv_obj_t *top = lv_layer_top();

    s_panel = lv_obj_create(top);
    lv_obj_set_size(s_panel, PANEL_W, PANEL_H);
    lv_obj_set_pos(s_panel, (LV_HOR_RES - PANEL_W) / 2, -PANEL_H);
    lv_obj_set_style_radius(s_panel, PANEL_RADIUS, 0);
    lv_obj_set_style_bg_color(s_panel, CC_PANEL_BG, 0);
    lv_obj_set_style_bg_opa(s_panel, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_panel, 1, 0);
    lv_obj_set_style_border_color(s_panel, CC_BORDER, 0);
    lv_obj_set_style_pad_all(s_panel, 0, 0);
    lv_obj_set_style_clip_corner(s_panel, true, 0);
    lv_obj_add_flag(s_panel, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_panel, panel_bg_cb, LV_EVENT_CLICKED, NULL);

    s_collapse_btn = lv_btn_create(s_panel);
    lv_obj_set_size(s_collapse_btn, 36, 36);
    lv_obj_set_pos(s_collapse_btn, CC_CONTENT_PAD, 16);
    lv_obj_set_style_radius(s_collapse_btn, 18, 0);
    lv_obj_set_style_bg_color(s_collapse_btn, CC_SURFACE, 0);
    lv_obj_set_style_bg_color(s_collapse_btn, CC_SURFACE_PRESSED, LV_STATE_PRESSED);
    lv_obj_set_style_border_width(s_collapse_btn, 1, 0);
    lv_obj_set_style_border_color(s_collapse_btn, CC_BORDER, 0);
    lv_obj_add_event_cb(s_collapse_btn, collapse_btn_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *close_icon = make_text_label(s_collapse_btn, LV_SYMBOL_CLOSE,
                                           &font_awesome_20_4, CC_TEXT);
    if (close_icon != NULL) {
        lv_obj_center(close_icon);
    }

    lv_obj_t *title = make_text_label(s_panel, "控制中心", &font_puhui_20_4, CC_TEXT);
    if (title != NULL) {
        lv_obj_set_pos(title, 70, 14);
    }
    lv_obj_t *subtitle = make_text_label(s_panel, "设备快捷控制",
                                         &font_puhui_16_4, CC_DIM);
    if (subtitle != NULL) {
        lv_obj_set_pos(subtitle, 70, 39);
    }

    build_control_tile(LV_SYMBOL_VOLUME_MAX, "音量", CC_BLUE,
                       CC_TILE_VOLUME, CC_CONTENT_PAD, CC_TILE_Y);
    build_control_tile(LV_SYMBOL_CHARGE, "亮度", CC_ORANGE,
                       CC_TILE_BRIGHTNESS, CC_CONTENT_PAD + CC_TILE_W + CC_TILE_GAP,
                       CC_TILE_Y);
    build_control_tile(LV_SYMBOL_WIFI, "WiFi", CC_GREEN,
                       CC_TILE_WIFI, CC_CONTENT_PAD, CC_TILE_SECOND_ROW_Y);
    build_control_tile(FONT_AWESOME_BATTERY_CHARGING, "电池", CC_GREEN,
                       CC_TILE_BATTERY, CC_CONTENT_PAD + CC_TILE_W + CC_TILE_GAP,
                       CC_TILE_SECOND_ROW_Y);

    /* The reference uses a compact connection row instead of a large
     * destructive button, so WiFi reset remains discoverable but does not
     * dominate the control center. */
    lv_obj_t *status_row = lv_obj_create(s_panel);
    lv_obj_set_size(status_row, PANEL_W - (CC_CONTENT_PAD * 2), 20);
    lv_obj_set_pos(status_row, CC_CONTENT_PAD, CC_STATUS_Y);
    lv_obj_set_style_bg_opa(status_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(status_row, 0, 0);
    lv_obj_set_style_pad_all(status_row, 0, 0);
    lv_obj_set_flex_flow(status_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(status_row, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(status_row, LV_OBJ_FLAG_CLICKABLE);

    s_wifi_status_dot = lv_obj_create(status_row);
    lv_obj_set_size(s_wifi_status_dot, 6, 6);
    lv_obj_set_style_radius(s_wifi_status_dot, 3, 0);
    lv_obj_set_style_bg_color(s_wifi_status_dot, CC_GREEN, 0);
    lv_obj_set_style_border_width(s_wifi_status_dot, 0, 0);
    lv_obj_clear_flag(s_wifi_status_dot, LV_OBJ_FLAG_CLICKABLE);
    s_wifi_status = make_text_label(status_row, "WiFi 未连接",
                                    &font_puhui_16_4, CC_MUTED);
    if (s_wifi_status != NULL) {
        lv_obj_set_style_pad_left(s_wifi_status, 6, 0);
    }

    lv_obj_t *swipe_hint = make_text_label(s_panel, "上滑收起",
                                           &font_puhui_16_4, CC_DIM);
    if (swipe_hint != NULL) {
        lv_obj_set_pos(swipe_hint, CC_CONTENT_PAD, CC_FOOT_Y);
    }
    lv_obj_t *status_hint = make_text_label(s_panel, "设备状态",
                                            &font_puhui_16_4, CC_DIM);
    if (status_hint != NULL) {
        lv_obj_set_width(status_hint, PANEL_W - (CC_CONTENT_PAD * 2));
        lv_obj_set_style_text_align(status_hint, LV_TEXT_ALIGN_RIGHT, 0);
        lv_obj_set_pos(status_hint, 0, CC_FOOT_Y);
    }

    build_adjust_view();
    update_control_values();
}

/* --- adjustment view ---------------------------------------------------- */

static void open_adjust_view(adjust_mode_t mode)
{
    if (s_adjust_view == NULL || s_adjust_slider == NULL) {
        return;
    }
    s_adjust_mode = mode;
    const bool volume = mode == CC_ADJUST_VOLUME;
    const int value = volume ? esp_xiaozhi_chat_app_get_volume()
                             : esp_xiaozhi_chat_app_get_brightness();
    const lv_color_t accent = volume ? CC_BLUE : CC_ORANGE;

    lv_label_set_text(s_adjust_title,
                      app_language_text(volume ? "音量" : "亮度", NULL));
    set_percent_label(s_adjust_value, value);
    lv_obj_set_style_text_color(s_adjust_value, accent, 0);
    lv_obj_set_style_bg_color(s_adjust_slider, accent, LV_PART_INDICATOR);
    lv_slider_set_value(s_adjust_slider, value, LV_ANIM_OFF);
    lv_label_set_text(s_adjust_min_icon, volume ? LV_SYMBOL_MUTE : LV_SYMBOL_MINUS);
    lv_label_set_text(s_adjust_max_icon, volume ? LV_SYMBOL_VOLUME_MAX : LV_SYMBOL_PLUS);
    lv_label_set_text(s_adjust_hint,
                      app_language_text(volume ? "拖动调整音量" : "拖动调整亮度", NULL));
    lv_obj_clear_flag(s_adjust_view, LV_OBJ_FLAG_HIDDEN);
}

static void close_adjust_view(void)
{
    s_adjust_mode = CC_ADJUST_NONE;
    if (s_adjust_view != NULL) {
        lv_obj_add_flag(s_adjust_view, LV_OBJ_FLAG_HIDDEN);
    }
}

static void update_control_values(void)
{
    update_volume_ui();
    update_brightness_ui();
    refresh_wifi_status();
    update_battery_ui();
}

/* --- public API ---------------------------------------------------------- */

esp_err_t ui_control_center_init(void)
{
    /* Deferred construction is required because init may run while the emote
     * renderer owns the panel and LVGL flushing is paused. */
    s_inited = true;
    ESP_LOGI(TAG, "control center armed (lazy build)");
    return ESP_OK;
}

static void ensure_built(void)
{
    if (s_panel != NULL) {
        return;
    }
    if (!lvgl_port_lock(1000)) {
        return;
    }
    build_panel();
    lvgl_port_unlock();
    ESP_LOGI(TAG, "control center panel built");
}

static void cc_close_anim_ready_cb(lv_anim_t *a)
{
    lv_obj_t *panel = (lv_obj_t *)a->var;
    if (panel == NULL) {
        return;
    }
    lv_obj_add_flag(panel, LV_OBJ_FLAG_HIDDEN);
    /* This callback runs from LVGL's animation-list traversal. Renderer
     * handoff must happen after the traversal returns, or it can invalidate
     * the animation list while lv_anim.c is still walking it. */
    if (s_saved_face && lv_async_call(cc_restore_face_async, NULL) != LV_RESULT_OK) {
        ESP_LOGW(TAG, "failed to defer face renderer restore");
    }
}

static void cc_restore_face_async(void *user_data)
{
    (void)user_data;
    if (!s_saved_face) {
        return;
    }

    if (s_pending_open || s_open) {
        return;
    }
    if (ui_page_manager_current() != PAGE_FACE) {
        s_saved_face = false;
        return;
    }

    s_saved_face = false;
    esp_err_t ret = esp_xiaozhi_chat_display_set_face_visible(true);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "failed to restore face renderer: %s", esp_err_to_name(ret));
    }
}

static void cc_do_open(void *user_data)
{
    (void)user_data;
    s_pending_open = false;
    ensure_built();
    if (s_panel == NULL) {
        if (s_saved_face && lv_async_call(cc_restore_face_async, NULL) != LV_RESULT_OK) {
            ESP_LOGW(TAG, "failed to defer face renderer restore");
        }
        s_open = false;
        return;
    }

    s_wi_fi_confirm = 0;
    close_adjust_view();
    update_control_values();
    lv_anim_del(s_panel, (lv_anim_exec_xcb_t)lv_obj_set_y);
    lv_obj_clear_flag(s_panel, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_y(s_panel, -PANEL_H);

    lv_anim_t anim;
    lv_anim_init(&anim);
    lv_anim_set_var(&anim, s_panel);
    lv_anim_set_exec_cb(&anim, (lv_anim_exec_xcb_t)lv_obj_set_y);
    lv_anim_set_values(&anim, -PANEL_H, PANEL_OPEN_Y);
    lv_anim_set_time(&anim, 200);
    lv_anim_set_path_cb(&anim, lv_anim_path_ease_in);
    lv_anim_start(&anim);

    s_open = true;
    app_sfx_play(APP_SFX_PAGE);
}

static void cc_do_close(void *user_data)
{
    (void)user_data;
    close_adjust_view();
    if (s_panel != NULL) {
        lv_anim_del(s_panel, (lv_anim_exec_xcb_t)lv_obj_set_y);
        lv_anim_t anim;
        lv_anim_init(&anim);
        lv_anim_set_var(&anim, s_panel);
        lv_anim_set_exec_cb(&anim, (lv_anim_exec_xcb_t)lv_obj_set_y);
        lv_anim_set_values(&anim, lv_obj_get_y(s_panel), -PANEL_H - 8);
        lv_anim_set_time(&anim, 160);
        lv_anim_set_path_cb(&anim, lv_anim_path_ease_in);
        lv_anim_set_ready_cb(&anim, cc_close_anim_ready_cb);
        lv_anim_start(&anim);
    } else if (s_saved_face && lv_async_call(cc_restore_face_async, NULL) != LV_RESULT_OK) {
        ESP_LOGW(TAG, "failed to defer face renderer restore");
    }
}

void ui_control_center_open(void)
{
    if (!s_inited || s_open || s_pending_open) {
        return;
    }
    s_pending_open = true;
    s_saved_face = (ui_page_manager_current() == PAGE_FACE);
    /* The emote face and LVGL cannot flush the same panel simultaneously. */
    esp_xiaozhi_chat_display_set_face_visible(false);
    lv_async_call(cc_do_open, NULL);
}

void ui_control_center_close(void)
{
    if (!s_inited || !s_open) {
        return;
    }
    /* Clear the flag before scheduling LVGL work so the gesture layer does not
     * keep consuming input while the closing animation is running. */
    s_open = false;
    lv_async_call(cc_do_close, NULL);
}

bool ui_control_center_is_open(void)
{
    return s_open;
}

bool ui_control_center_on_gesture(ui_gesture_t g, int x, int y)
{
    (void)x;
    if (!s_inited) {
        return false;
    }
    if (s_open) {
        /* In the card view, an upward swipe closes the panel from any tile.
         * In the adjustment view the same motion belongs to the vertical
         * slider, so it must remain consumed by LVGL instead. */
        if (g == UI_GESTURE_SWIPE_UP && s_adjust_mode == CC_ADJUST_NONE) {
            ui_control_center_close();
        } else if (g == UI_GESTURE_SWIPE_DOWN && y < 60) {
            ui_control_center_close();
        }
        return true;
    }

    /* Only the face page owns the pull-down gesture. Other pages retain their
     * normal navigation behavior. */
    if (g == UI_GESTURE_SWIPE_DOWN && ui_page_manager_current() == PAGE_FACE) {
        ui_control_center_open();
        return true;
    }
    return false;
}
