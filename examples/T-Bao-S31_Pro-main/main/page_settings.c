/*
 * SPDX-License-Identifier: Apache-2.0
 */
#include "page_settings.h"

#include <inttypes.h>
#include <string.h>

#include "app_sfx.h"
#include "app_settings.h"
#include "app_language.h"
#include "app_sdcard.h"
#include "battery_monitor.h"
#include "esp_app_desc.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_xiaozhi_chat_app.h"
#include "esp_xiaozhi_chat_display.h"
#include "font_awesome_symbols.h"
#include "lvgl.h"
#include "page_launcher.h"
#include "ui_page_style.h"
#include "wifi_provisioning.h"

LV_FONT_DECLARE(font_awesome_20_4);
LV_FONT_DECLARE(font_puhui_16_4);
LV_FONT_DECLARE(font_puhui_20_4);

#define SETTINGS_TEXT       0xF4F7F8
#define SETTINGS_MUTED      0x8C9AA3
#define SETTINGS_BLUE       0x2094FA
#define SETTINGS_ORANGE     0xFF9500
#define SETTINGS_RED        0xE5484D
#define SETTINGS_LINE       0x2A3942
#define SETTINGS_CARD_BG    0x17191E
#define SETTINGS_CARD_DOWN  0x24272D
#define SETTINGS_ARROW      0x858A93
#define SETTINGS_CARD_WIDTH (LV_HOR_RES - 32)
#define SETTINGS_CARD_HEIGHT 78

typedef enum {
    SETTINGS_ACTION_NONE = 0,
    SETTINGS_ACTION_RESTART,
    SETTINGS_ACTION_SLEEP,
    SETTINGS_ACTION_RESET_PREFERENCES,
    SETTINGS_ACTION_RESET_WIFI,
} settings_action_t;

typedef enum {
    SETTINGS_SLIDER_VOLUME = 0,
    SETTINGS_SLIDER_BRIGHTNESS,
} settings_slider_t;

typedef enum {
    SETTINGS_VIEW_HOME = 0,
    SETTINGS_VIEW_BATTERY,
    SETTINGS_VIEW_WLAN,
    SETTINGS_VIEW_BLUETOOTH,
    SETTINGS_VIEW_DISPLAY,
    SETTINGS_VIEW_XIAOZHI,
    SETTINGS_VIEW_DEVICE,
    SETTINGS_VIEW_SYSTEM,
    SETTINGS_VIEW_LANGUAGE,
} settings_view_t;

static lv_obj_t *s_root;
static lv_obj_t *s_content;
static lv_obj_t *s_header;
static lv_obj_t *s_back_button;
static lv_obj_t *s_notice;
static lv_obj_t *s_confirm_overlay;
static lv_obj_t *s_wifi_status;
static lv_obj_t *s_xiaozhi_status;
static lv_obj_t *s_sd_status;
static lv_obj_t *s_sd_capacity;
static lv_obj_t *s_firmware_version;
static lv_obj_t *s_build_time;
static lv_obj_t *s_volume_value;
static lv_obj_t *s_brightness_value;
static lv_obj_t *s_home_wifi_status;
static lv_obj_t *s_home_display_status;
static lv_obj_t *s_home_xiaozhi_status;
static lv_obj_t *s_home_battery_status;
static lv_obj_t *s_home_device_status;
static lv_obj_t *s_battery_status;
static lv_obj_t *s_battery_percent;
static lv_obj_t *s_battery_voltage;
static lv_obj_t *s_battery_input;
static lv_obj_t *s_battery_phase;
static lv_obj_t *s_battery_fault;
static lv_obj_t *s_battery_detail;
static settings_action_t s_confirm_action;
static settings_view_t s_view;
static uint32_t s_last_refresh_ms;

static void settings_render_view(void);
static void settings_back_cb(lv_event_t *event);
static void settings_card_clicked_cb(lv_event_t *event);
static void refresh_battery_status(void);
static void build_battery_detail(void);
static void build_language_detail(void);
static void language_option_cb(lv_event_t *event);

static lv_obj_t *make_label(lv_obj_t *parent, const char *text,
                            const lv_font_t *font, uint32_t color)
{
    lv_obj_t *label = lv_label_create(parent);
    if (label == NULL) {
        return NULL;
    }
    lv_label_set_text(label, app_language_text(text, NULL));
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
    return label;
}

static void set_notice(const char *text, uint32_t color)
{
    if (s_notice == NULL) {
        return;
    }
    lv_label_set_text(s_notice, text != NULL ? app_language_text(text, NULL) : "");
    lv_obj_set_style_text_color(s_notice, lv_color_hex(color), 0);
}

static void settings_card_clicked_cb(lv_event_t *event)
{
    const settings_view_t view = (settings_view_t)(intptr_t)
        lv_event_get_user_data(event);
    if (view == SETTINGS_VIEW_LANGUAGE) {
        app_sfx_play(APP_SFX_TAP);
        s_view = SETTINGS_VIEW_LANGUAGE;
        settings_render_view();
        return;
    }
    if (view <= SETTINGS_VIEW_HOME || view > SETTINGS_VIEW_SYSTEM) {
        return;
    }
    app_sfx_play(APP_SFX_TAP);
    s_view = view;
    settings_render_view();
}

static void settings_back_cb(lv_event_t *event)
{
    (void)event;
    app_sfx_play(APP_SFX_TAP);
    if (s_view == SETTINGS_VIEW_HOME) {
        ui_page_manager_switch(PAGE_LAUNCHER);
        return;
    }
    s_view = SETTINGS_VIEW_HOME;
    settings_render_view();
}

static void add_card_event_bubble(lv_obj_t *obj)
{
    if (obj != NULL) {
        lv_obj_add_flag(obj, LV_OBJ_FLAG_EVENT_BUBBLE);
    }
}

static lv_obj_t *make_navigation_card(lv_obj_t *parent,
                                      const char *title,
                                      const char *status,
                                      const char *icon,
                                      uint32_t accent,
                                      settings_view_t view,
                                      lv_obj_t **status_out)
{
    lv_obj_t *card = lv_btn_create(parent);
    if (card == NULL) {
        return NULL;
    }
    lv_obj_set_size(card, SETTINGS_CARD_WIDTH, SETTINGS_CARD_HEIGHT);
    ui_page_style_button(card, SETTINGS_CARD_BG, SETTINGS_CARD_DOWN, 13);
    lv_obj_set_style_bg_opa(card, LV_OPA_90, 0);
    lv_obj_set_style_border_color(card, lv_color_hex(0x292C32), 0);
    lv_obj_set_style_border_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_shadow_width(card, 0, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_SCROLL_CHAIN_HOR);
    lv_obj_add_event_cb(card, settings_card_clicked_cb, LV_EVENT_CLICKED,
                        (void *)(intptr_t)view);

    lv_obj_t *icon_circle = lv_obj_create(card);
    if (icon_circle != NULL) {
        lv_obj_set_size(icon_circle, 42, 42);
        lv_obj_set_pos(icon_circle, 12, 18);
        lv_obj_set_style_bg_color(icon_circle, lv_color_hex(accent), 0);
        lv_obj_set_style_bg_opa(icon_circle, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(icon_circle, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_border_width(icon_circle, 0, 0);
        lv_obj_set_style_pad_all(icon_circle, 0, 0);
        lv_obj_clear_flag(icon_circle,
                          LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_SCROLL_CHAIN_HOR);
        add_card_event_bubble(icon_circle);

        lv_obj_t *icon_label = make_label(icon_circle, icon,
                                          &font_awesome_20_4, 0xFFFFFF);
        if (icon_label != NULL) {
            lv_obj_center(icon_label);
            add_card_event_bubble(icon_label);
        }
    }

    lv_obj_t *title_label = make_label(card, title, &font_puhui_20_4,
                                       SETTINGS_TEXT);
    if (title_label != NULL) {
        lv_obj_set_width(title_label, SETTINGS_CARD_WIDTH - 110);
        lv_label_set_long_mode(title_label, LV_LABEL_LONG_DOT);
        lv_obj_set_pos(title_label, 66, 11);
        add_card_event_bubble(title_label);
    }

    lv_obj_t *status_label = make_label(card, status, &font_puhui_16_4,
                                        SETTINGS_MUTED);
    if (status_label != NULL) {
        lv_obj_set_width(status_label, SETTINGS_CARD_WIDTH - 110);
        lv_label_set_long_mode(status_label, LV_LABEL_LONG_DOT);
        lv_obj_set_pos(status_label, 66, 44);
        add_card_event_bubble(status_label);
    }
    if (status_out != NULL) {
        *status_out = status_label;
    }

    lv_obj_t *arrow = make_label(card, FONT_AWESOME_ARROW_RIGHT,
                                 &font_awesome_20_4,
                                 SETTINGS_ARROW);
    if (arrow != NULL) {
        lv_obj_set_pos(arrow, SETTINGS_CARD_WIDTH - 28, 28);
        add_card_event_bubble(arrow);
    }
    return card;
}

static void settings_back_button_clicked_cb(lv_event_t *event)
{
    settings_back_cb(event);
}

static lv_obj_t *make_settings_back_button(lv_obj_t *parent)
{
    lv_obj_t *button = lv_btn_create(parent);
    if (button == NULL) {
        return NULL;
    }
    lv_obj_set_size(button, 40, 40);
    lv_obj_set_pos(button, 4, 3);
    ui_page_style_button(button, 0x17191E, 0x292C32, 12);
    lv_obj_set_style_border_color(button, lv_color_hex(0x292C32), 0);
    lv_obj_add_event_cb(button, settings_back_button_clicked_cb,
                        LV_EVENT_CLICKED, NULL);

    lv_obj_t *icon = make_label(button, FONT_AWESOME_MICARROW_LEFT,
                                &font_awesome_20_4, SETTINGS_TEXT);
    if (icon != NULL) {
        lv_obj_center(icon);
    }
    return button;
}

static lv_obj_t *create_section(lv_obj_t *parent, const char *title,
                                int height)
{
    lv_obj_t *card = lv_obj_create(parent);
    if (card == NULL) {
        return NULL;
    }
    lv_obj_set_size(card, SETTINGS_CARD_WIDTH, height);
    ui_page_style_panel(card);
    lv_obj_set_style_bg_color(card, lv_color_hex(SETTINGS_CARD_BG), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_90, 0);
    lv_obj_set_style_radius(card, 13, 0);
    lv_obj_set_style_border_color(card, lv_color_hex(0x292C32), 0);
    lv_obj_set_style_pad_all(card, 0, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_SCROLL_CHAIN_HOR);

    lv_obj_t *heading = make_label(card, title, &font_puhui_16_4, SETTINGS_BLUE);
    if (heading != NULL) {
        lv_obj_set_pos(heading, 12, 8);
    }

    lv_obj_t *divider = lv_obj_create(card);
    if (divider != NULL) {
        lv_obj_set_size(divider, SETTINGS_CARD_WIDTH - 24, 1);
        lv_obj_set_pos(divider, 12, 32);
        lv_obj_set_style_bg_color(divider, lv_color_hex(SETTINGS_LINE), 0);
        lv_obj_set_style_bg_opa(divider, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(divider, 0, 0);
        lv_obj_set_style_pad_all(divider, 0, 0);
        lv_obj_clear_flag(divider, LV_OBJ_FLAG_SCROLLABLE);
    }
    return card;
}

static lv_obj_t *make_action_button(lv_obj_t *parent, int y,
                                    const char *text, uint32_t color,
                                    lv_event_cb_t callback)
{
    lv_obj_t *button = lv_btn_create(parent);
    if (button == NULL) {
        return NULL;
    }
    lv_obj_set_size(button, LV_HOR_RES - 64, 42);
    lv_obj_set_pos(button, 12, y);
    ui_page_style_button(button, color, 0x30343A, 9);
    lv_obj_t *label = make_label(button, text, &font_puhui_16_4,
                                 SETTINGS_TEXT);
    if (label != NULL) {
        lv_obj_center(label);
    }
    lv_obj_add_event_cb(button, callback, LV_EVENT_CLICKED, NULL);
    return button;
}

static lv_obj_t *make_language_button(lv_obj_t *parent, int y,
                                      const char *text, app_language_t language)
{
    lv_obj_t *button = lv_btn_create(parent);
    if (button == NULL) {
        return NULL;
    }
    lv_obj_set_size(button, LV_HOR_RES - 64, 44);
    lv_obj_set_pos(button, 12, y);
    ui_page_style_button(button,
                         app_language_get() == language ? SETTINGS_BLUE : SETTINGS_CARD_BG,
                         SETTINGS_CARD_DOWN, 9);
    lv_obj_t *label = make_label(button, text, &font_puhui_16_4,
                                 SETTINGS_TEXT);
    if (label != NULL) {
        lv_obj_center(label);
    }
    lv_obj_add_event_cb(button, language_option_cb, LV_EVENT_CLICKED,
                        (void *)(intptr_t)language);
    return button;
}

static void slider_update_value(settings_slider_t kind, int value)
{
    lv_obj_t *label = kind == SETTINGS_SLIDER_VOLUME
        ? s_volume_value : s_brightness_value;
    if (label != NULL) {
        lv_label_set_text_fmt(label, "%d%%", value);
    }
}

static void settings_slider_cb(lv_event_t *event)
{
    lv_obj_t *slider = lv_event_get_target_obj(event);
    const settings_slider_t kind = (settings_slider_t)(intptr_t)
        lv_event_get_user_data(event);
    const lv_event_code_t code = lv_event_get_code(event);
    if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
        esp_err_t ret = kind == SETTINGS_SLIDER_VOLUME
            ? esp_xiaozhi_chat_app_commit_volume()
            : esp_xiaozhi_chat_app_commit_brightness();
        set_notice(ret == ESP_OK ? "设置已保存" : "设置保存失败",
                   ret == ESP_OK ? SETTINGS_MUTED : SETTINGS_RED);
        return;
    }

    const int value = (int)lv_slider_get_value(slider);
    esp_err_t ret = kind == SETTINGS_SLIDER_VOLUME
        ? esp_xiaozhi_chat_app_preview_volume(value)
        : esp_xiaozhi_chat_app_preview_brightness(value);
    if (ret == ESP_OK) {
        slider_update_value(kind, value);
    }
}

static lv_obj_t *make_slider(lv_obj_t *parent, settings_slider_t kind,
                             int y, const char *title, int min, int max,
                             int value)
{
    lv_obj_t *label = make_label(parent, title, &font_puhui_16_4,
                                 SETTINGS_TEXT);
    if (label != NULL) {
        lv_obj_set_pos(label, 12, y);
    }

    lv_obj_t *value_label = make_label(parent, "", &font_puhui_16_4,
                                       SETTINGS_BLUE);
    if (value_label != NULL) {
        lv_obj_set_width(value_label, 48);
        lv_obj_set_style_text_align(value_label, LV_TEXT_ALIGN_RIGHT, 0);
        lv_obj_set_pos(value_label, SETTINGS_CARD_WIDTH - 60, y);
    }
    if (kind == SETTINGS_SLIDER_VOLUME) {
        s_volume_value = value_label;
    } else {
        s_brightness_value = value_label;
    }
    slider_update_value(kind, value);

    lv_obj_t *slider = lv_slider_create(parent);
    if (slider == NULL) {
        return NULL;
    }
    lv_obj_set_size(slider, LV_HOR_RES - 64, 14);
    lv_obj_set_pos(slider, 12, y + 22);
    lv_slider_set_range(slider, min, max);
    lv_slider_set_value(slider, value, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(slider, lv_color_hex(0x30383E), LV_PART_MAIN);
    lv_obj_set_style_bg_color(slider, lv_color_hex(SETTINGS_BLUE),
                              LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(slider, lv_color_hex(SETTINGS_TEXT), LV_PART_KNOB);
    lv_obj_add_event_cb(slider, settings_slider_cb, LV_EVENT_VALUE_CHANGED,
                        (void *)(intptr_t)kind);
    lv_obj_add_event_cb(slider, settings_slider_cb, LV_EVENT_RELEASED,
                        (void *)(intptr_t)kind);
    lv_obj_add_event_cb(slider, settings_slider_cb, LV_EVENT_PRESS_LOST,
                        (void *)(intptr_t)kind);
    return slider;
}

static void ui_sounds_changed_cb(lv_event_t *event)
{
    lv_obj_t *toggle = lv_event_get_target_obj(event);
    const bool enabled = lv_obj_has_state(toggle, LV_STATE_CHECKED);
    app_sfx_set_enabled(enabled);
    esp_err_t ret = app_settings_set_ui_sounds_enabled(enabled);
    if (ret != ESP_OK) {
        app_sfx_set_enabled(!enabled);
        if (enabled) {
            lv_obj_set_state(toggle, LV_STATE_CHECKED, false);
        } else {
            lv_obj_add_state(toggle, LV_STATE_CHECKED);
        }
        set_notice("提示音设置保存失败", SETTINGS_RED);
        return;
    }
    app_sfx_play(APP_SFX_TAP);
    set_notice(enabled ? "提示音已开启" : "提示音已关闭", SETTINGS_MUTED);
}

static void build_display_section(void)
{
    lv_obj_t *card = create_section(s_content, "显示与声音", 210);
    if (card == NULL) {
        return;
    }

    make_slider(card, SETTINGS_SLIDER_BRIGHTNESS, 40, "屏幕亮度", 5, 100,
                esp_xiaozhi_chat_app_get_brightness());
    make_slider(card, SETTINGS_SLIDER_VOLUME, 94, "音量", 0, 100,
                esp_xiaozhi_chat_app_get_volume());

    lv_obj_t *sound_label = make_label(card, "界面提示音",
                                       &font_puhui_16_4, SETTINGS_TEXT);
    if (sound_label != NULL) {
        lv_obj_set_pos(sound_label, 12, 165);
    }
    lv_obj_t *sound_hint = make_label(card, "点击与页面切换反馈",
                                      &font_puhui_16_4, SETTINGS_MUTED);
    if (sound_hint != NULL) {
        lv_obj_set_pos(sound_hint, 12, 186);
    }
    lv_obj_t *sounds_switch = lv_switch_create(card);
    if (sounds_switch != NULL) {
        lv_obj_set_size(sounds_switch, 42, 24);
        lv_obj_align(sounds_switch, LV_ALIGN_TOP_RIGHT, -14, 166);
        lv_obj_add_event_cb(sounds_switch, ui_sounds_changed_cb,
                            LV_EVENT_VALUE_CHANGED, NULL);
        if (app_sfx_is_enabled()) {
            lv_obj_add_state(sounds_switch, LV_STATE_CHECKED);
        }
    }
}

static bool wifi_is_connected(wifi_ap_record_t *record)
{
    wifi_ap_record_t local_record = {0};
    const bool connected = esp_wifi_sta_get_ap_info(&local_record) == ESP_OK;
    if (connected && record != NULL) {
        *record = local_record;
    }
    return connected;
}

static const char *xiaozhi_state_name(esp_xiaozhi_chat_app_state_t state)
{
    switch (state) {
    case ESP_XIAOZHI_CHAT_APP_STATE_ENABLED: return app_language_text("运行中", "Running");
    case ESP_XIAOZHI_CHAT_APP_STATE_STARTING: return app_language_text("启动中", "Starting");
    case ESP_XIAOZHI_CHAT_APP_STATE_STOPPING: return app_language_text("关闭中", "Stopping");
    case ESP_XIAOZHI_CHAT_APP_STATE_DISABLED: return app_language_text("已关闭", "Disabled");
    case ESP_XIAOZHI_CHAT_APP_STATE_ERROR: return app_language_text("启动失败", "Start failed");
    default: return app_language_text("准备中", "Ready");
    }
}

static void refresh_ai_status(void)
{
    if (s_xiaozhi_status != NULL) {
        lv_label_set_text_fmt(s_xiaozhi_status, "%s",
            xiaozhi_state_name(esp_xiaozhi_chat_app_get_state()));
    }
    if (s_wifi_status != NULL) {
        wifi_ap_record_t record = {0};
        if (wifi_is_connected(&record)) {
            char ssid[sizeof(record.ssid) + 1] = {0};
            memcpy(ssid, record.ssid, sizeof(record.ssid));
            lv_label_set_text_fmt(s_wifi_status,
                                  app_language_is_english() ? "WiFi connected · %.32s" :
                                                              "WiFi 已连接 · %.32s",
                                  ssid);
        } else {
            lv_label_set_text(s_wifi_status,
                              app_language_text("WiFi 未连接", "WiFi disconnected"));
        }
    }
}

static const char *battery_phase_text(const battery_monitor_state_t *state)
{
    if (state != NULL && state->charging_current_detected &&
        (state->phase == BATTERY_MONITOR_PHASE_NOT_CHARGING ||
         state->phase == BATTERY_MONITOR_PHASE_UNKNOWN)) {
        return app_language_text("充电中（电流检测）",
                                 "Charging (current detected)");
    }

    const battery_monitor_charge_phase_t phase = state != NULL
        ? state->phase : BATTERY_MONITOR_PHASE_UNKNOWN;
    switch (phase) {
    case BATTERY_MONITOR_PHASE_NOT_CHARGING:
        return app_language_text("未充电", "Not charging");
    case BATTERY_MONITOR_PHASE_TRICKLE:
        return app_language_text("涓流", "Trickle");
    case BATTERY_MONITOR_PHASE_PRECHARGE:
        return app_language_text("预充电", "Precharge");
    case BATTERY_MONITOR_PHASE_FAST_CHARGE:
        return app_language_text("快速充电", "Fast charge");
    case BATTERY_MONITOR_PHASE_TAPER:
        return app_language_text("恒压收尾", "Taper");
    case BATTERY_MONITOR_PHASE_TOP_OFF:
        return app_language_text("补充充电", "Top-off");
    case BATTERY_MONITOR_PHASE_DONE:
        return app_language_text("已完成", "Done");
    case BATTERY_MONITOR_PHASE_UNKNOWN:
    default:
        return battery_monitor_phase_name(phase);
    }
}

static const char *battery_connection_text(
    const battery_monitor_state_t *state)
{
    if (state == NULL || !state->device_present || !state->part_id_valid) {
        return app_language_text("暂不可用", "Unavailable");
    }
    if (state->data_stale) {
        return app_language_text("数据暂未更新", "Data temporarily stale");
    }
    if (state->low_voltage_warning) {
        return app_language_text("电量过低", "Battery low");
    }
    if (state->charge_done) {
        return app_language_text("已充满", "Full");
    }
    if (state->charging) {
        return app_language_text("正在充电", "Charging");
    }
    if (state->input_present || state->power_good || state->power_good_pin) {
        return app_language_text("已连接电源", "Power connected");
    }
    return app_language_text("电池供电", "Battery power");
}

static void refresh_battery_detail_labels(
    const battery_monitor_state_t *state,
    bool available)
{
    if (!available || state == NULL) {
        if (s_battery_status != NULL) {
            lv_label_set_text(s_battery_status,
                              app_language_text("检测中", "Checking..."));
            lv_obj_set_style_text_color(s_battery_status,
                                        lv_color_hex(SETTINGS_MUTED), 0);
        }
        if (s_battery_percent != NULL) {
            lv_label_set_text(s_battery_percent,
                              app_language_text("电量：--", "Battery: --"));
        }
        if (s_battery_voltage != NULL) {
            lv_label_set_text(s_battery_voltage,
                              app_language_text("电池电压：--", "Battery voltage: --"));
        }
        if (s_battery_input != NULL) {
            lv_label_set_text(s_battery_input,
                              app_language_text("输入电压：--", "Input voltage: --"));
        }
        if (s_battery_phase != NULL) {
            lv_label_set_text(s_battery_phase,
                              app_language_text("充电阶段：--", "Charge phase: --"));
        }
        if (s_battery_fault != NULL) {
            lv_label_set_text(s_battery_fault,
                              app_language_text("故障码：--", "Fault code: --"));
        }
        if (s_battery_detail != NULL) {
            lv_label_set_text(s_battery_detail,
                              state != NULL && state->detail[0] != '\0'
                                  ? state->detail :
                                    app_language_text("等待充电监测数据",
                                                      "Waiting for battery data"));
        }
        return;
    }

    const char *connection = battery_connection_text(state);
    if (s_battery_status != NULL) {
        lv_label_set_text(s_battery_status, connection);
        const uint32_t color = state->low_voltage_warning ? SETTINGS_RED
            : ((state->data_stale || state->charging) ? SETTINGS_ORANGE
               : (state->charge_done ? 0x34C759 : SETTINGS_BLUE));
        lv_obj_set_style_text_color(s_battery_status, lv_color_hex(color), 0);
    }
    if (s_battery_percent != NULL) {
        if ((state->valid || state->data_stale) && state->battery_percent >= 0) {
            lv_label_set_text_fmt(s_battery_percent,
                                  app_language_is_english() ? "Battery: %d%%" :
                                                              "电量：%d%%",
                                  state->battery_percent);
        } else {
            lv_label_set_text(s_battery_percent,
                              app_language_text("电量：--", "Battery: --"));
        }
    }
    if (s_battery_voltage != NULL) {
        if (state->valid || state->data_stale) {
            lv_label_set_text_fmt(
                s_battery_voltage,
                app_language_is_english() ? "Battery: %dmV · System: %dmV" :
                                            "电池电压：%dmV · 系统：%dmV",
                state->vbat_mv, state->vsys_mv);
        } else {
            lv_label_set_text(s_battery_voltage,
                              app_language_text("电池电压：--", "Battery voltage: --"));
        }
    }
    if (s_battery_input != NULL) {
        if (state->valid || state->data_stale) {
            lv_label_set_text_fmt(
                s_battery_input,
                app_language_is_english() ? "Input: %dmV · Charge current: %dmA" :
                                            "输入：%dmV · 充电电流：%dmA",
                state->vbus_mv, state->ichg_ma);
        } else {
            lv_label_set_text(s_battery_input,
                              app_language_text("输入电压：--", "Input voltage: --"));
        }
    }
    if (s_battery_phase != NULL) {
        lv_label_set_text_fmt(
            s_battery_phase,
            app_language_is_english() ? "Charge phase: %s" : "充电阶段：%s",
            battery_phase_text(state));
    }
    if (s_battery_fault != NULL) {
        lv_label_set_text_fmt(
            s_battery_fault,
            app_language_is_english() ? "Fault code: 0x%02X" :
                                        "故障码：0x%02X",
            state->fault_bits);
    }
    if (s_battery_detail != NULL) {
        lv_label_set_text(s_battery_detail,
                          state->data_stale
                              ? app_language_text("数据暂未更新，显示最近有效读数",
                                                  "Showing the last valid readings")
                              : (state->detail[0] != '\0' ? state->detail :
                                 app_language_text("状态正常", "Status normal")));
    }
}

static void refresh_battery_status(void)
{
    battery_monitor_state_t state = {0};
    /* Charge/status registers remain useful while ADC conversions recover
     * after USB insertion/removal. Only the numeric readings need ADC data. */
    const bool available = battery_monitor_get_state(&state) == ESP_OK &&
                           state.device_present && state.part_id_valid;

    if (s_home_battery_status != NULL) {
        if (available && (state.valid || state.data_stale) && state.battery_percent >= 0) {
            lv_label_set_text_fmt(
                s_home_battery_status,
                app_language_is_english() ? "Battery %d%% · %s" :
                                            "电量 %d%% · %s",
                state.battery_percent, battery_connection_text(&state));
        } else if (available) {
            lv_label_set_text_fmt(
                s_home_battery_status,
                app_language_is_english() ? "Battery -- · %s" :
                                            "电量 -- · %s",
                battery_connection_text(&state));
        } else {
            lv_label_set_text(s_home_battery_status,
                              app_language_text("检测中", "Checking..."));
        }
    }
    refresh_battery_detail_labels(&state, available);
}

static void build_battery_detail(void)
{
    lv_obj_t *card = create_section(s_content, "电池与充电", 258);
    if (card == NULL) {
        return;
    }

    s_battery_status = make_label(card, "检测中", &font_puhui_20_4,
                                  SETTINGS_BLUE);
    if (s_battery_status != NULL) {
        lv_obj_set_width(s_battery_status, SETTINGS_CARD_WIDTH - 24);
        lv_label_set_long_mode(s_battery_status, LV_LABEL_LONG_DOT);
        lv_obj_set_pos(s_battery_status, 12, 45);
    }
    s_battery_percent = make_label(card, "电量：--", &font_puhui_16_4,
                                   SETTINGS_TEXT);
    if (s_battery_percent != NULL) {
        lv_obj_set_pos(s_battery_percent, 12, 75);
    }
    s_battery_voltage = make_label(card, "电池电压：--", &font_puhui_16_4,
                                   SETTINGS_TEXT);
    if (s_battery_voltage != NULL) {
        lv_obj_set_pos(s_battery_voltage, 12, 102);
    }
    s_battery_input = make_label(card, "输入电压：--", &font_puhui_16_4,
                                 SETTINGS_TEXT);
    if (s_battery_input != NULL) {
        lv_obj_set_pos(s_battery_input, 12, 129);
    }
    s_battery_phase = make_label(card, "充电阶段：--", &font_puhui_16_4,
                                 SETTINGS_TEXT);
    if (s_battery_phase != NULL) {
        lv_obj_set_pos(s_battery_phase, 12, 156);
    }
    s_battery_fault = make_label(card, "故障码：--", &font_puhui_16_4,
                                 SETTINGS_TEXT);
    if (s_battery_fault != NULL) {
        lv_obj_set_pos(s_battery_fault, 12, 183);
    }
    s_battery_detail = make_label(card, "等待充电监测数据",
                                  &font_puhui_16_4, SETTINGS_MUTED);
    if (s_battery_detail != NULL) {
        lv_obj_set_width(s_battery_detail, SETTINGS_CARD_WIDTH - 24);
        lv_obj_set_height(s_battery_detail, 38);
        lv_label_set_long_mode(s_battery_detail, LV_LABEL_LONG_WRAP);
        lv_obj_set_pos(s_battery_detail, 12, 210);
    }
    refresh_battery_status();
}

static void language_option_cb(lv_event_t *event)
{
    app_language_t language = (app_language_t)(intptr_t)
        lv_event_get_user_data(event);
    app_sfx_play(APP_SFX_TAP);
    if (app_language_set(language) != ESP_OK) {
        set_notice("设置保存失败", SETTINGS_RED);
        return;
    }
    s_view = SETTINGS_VIEW_HOME;
    settings_render_view();
}

static void build_language_detail(void)
{
    lv_obj_t *card = create_section(s_content, "语言 / Language", 166);
    if (card == NULL) {
        return;
    }
    lv_obj_t *hint = make_label(card, "选择语言 / Choose language",
                                &font_puhui_16_4, SETTINGS_MUTED);
    if (hint != NULL) {
        lv_obj_set_pos(hint, 12, 40);
    }
    make_language_button(card, 66, "中文 / Chinese", APP_LANGUAGE_ZH_CN);
    make_language_button(card, 114, "English / 英文", APP_LANGUAGE_EN_US);
}

void page_settings_open_battery(void)
{
    if (s_root == NULL || ui_page_manager_current() != PAGE_SETTINGS) {
        return;
    }
    s_view = SETTINGS_VIEW_BATTERY;
    settings_render_view();
}

static void open_xiaozhi_cb(lv_event_t *event)
{
    (void)event;
    app_sfx_play(APP_SFX_TAP);
    ui_page_manager_switch(PAGE_XIAOZHI);
}

static void show_confirmation(settings_action_t action,
                              const char *title,
                              const char *detail,
                              uint32_t accent);

static void ask_wifi_reset_cb(lv_event_t *event)
{
    (void)event;
    app_sfx_play(APP_SFX_TAP);
    show_confirmation(SETTINGS_ACTION_RESET_WIFI, "重新配置 WiFi",
                      "将清除已保存的 WiFi，并重启进入配网。", SETTINGS_ORANGE);
}

static void build_wlan_detail(void)
{
    lv_obj_t *card = create_section(s_content, "无线网络", 132);
    if (card == NULL) {
        return;
    }

    s_wifi_status = make_label(card, "WiFi 检测中", &font_puhui_16_4,
                               SETTINGS_TEXT);
    if (s_wifi_status != NULL) {
        lv_obj_set_width(s_wifi_status, LV_HOR_RES - 58);
        lv_label_set_long_mode(s_wifi_status, LV_LABEL_LONG_DOT);
        lv_obj_set_pos(s_wifi_status, 12, 43);
    }
    make_action_button(card, 78, "重新配置 WiFi", SETTINGS_ORANGE,
                       ask_wifi_reset_cb);
    refresh_ai_status();
}

static void build_bluetooth_detail(void)
{
    lv_obj_t *card = create_section(s_content, "蓝牙", 142);
    if (card == NULL) {
        return;
    }

    lv_obj_t *status = make_label(card, "管理功能未接入",
                                  &font_puhui_20_4, SETTINGS_TEXT);
    if (status != NULL) {
        lv_obj_set_pos(status, 12, 47);
    }
    lv_obj_t *detail = make_label(card, "蓝牙管理功能未接入当前应用",
                                  &font_puhui_16_4, SETTINGS_MUTED);
    if (detail != NULL) {
        lv_obj_set_width(detail, SETTINGS_CARD_WIDTH - 24);
        lv_label_set_long_mode(detail, LV_LABEL_LONG_DOT);
        lv_obj_set_pos(detail, 12, 82);
    }
}

static void build_ai_detail(void)
{
    lv_obj_t *card = create_section(s_content, "小智 AI", 128);
    if (card == NULL) {
        return;
    }

    s_xiaozhi_status = make_label(card, "准备中", &font_puhui_16_4,
                                  SETTINGS_MUTED);
    if (s_xiaozhi_status != NULL) {
        lv_obj_set_width(s_xiaozhi_status, SETTINGS_CARD_WIDTH - 24);
        lv_obj_set_pos(s_xiaozhi_status, 12, 43);
    }
    make_action_button(card, 76, "打开 AI 管理", SETTINGS_BLUE,
                       open_xiaozhi_cb);
    refresh_ai_status();
}

static void refresh_sd_status(void)
{
    if (s_sd_status == NULL) {
        return;
    }
    if (!app_sdcard_mounted()) {
        lv_label_set_text(s_sd_status,
                          app_language_text("未挂载", "Not mounted"));
        if (s_sd_capacity != NULL) {
            lv_label_set_text(s_sd_capacity,
                              app_language_text("插入 SD 卡后可播放本地音乐与视频",
                                                "Insert an SD card to play local music and videos"));
        }
        return;
    }

    uint64_t total = 0;
    uint64_t free_bytes = 0;
    lv_label_set_text(s_sd_status, app_language_text("已挂载", "Mounted"));
    if (s_sd_capacity != NULL) {
        if (app_sdcard_info(&total, &free_bytes) == ESP_OK) {
            lv_label_set_text_fmt(
                s_sd_capacity,
                app_language_is_english() ? "%" PRIu64 " MB free / " PRIu64 " MB total" :
                                            "剩余 %" PRIu64 " MB / 共 %" PRIu64 " MB",
                free_bytes / (1024U * 1024U), total / (1024U * 1024U));
        } else {
            lv_label_set_text(s_sd_capacity,
                              app_language_text("容量信息暂不可用", "Capacity unavailable"));
        }
    }
}

static void build_device_section(void)
{
    lv_obj_t *card = create_section(s_content, "存储与设备", 150);
    if (card == NULL) {
        return;
    }

    lv_obj_t *sd_label = make_label(card, "SD 卡", &font_puhui_16_4,
                                    SETTINGS_TEXT);
    if (sd_label != NULL) {
        lv_obj_set_pos(sd_label, 12, 41);
    }
    s_sd_status = make_label(card, "检测中", &font_puhui_16_4, SETTINGS_BLUE);
    if (s_sd_status != NULL) {
        lv_obj_set_width(s_sd_status, 118);
        lv_obj_set_style_text_align(s_sd_status, LV_TEXT_ALIGN_RIGHT, 0);
        lv_obj_set_pos(s_sd_status, SETTINGS_CARD_WIDTH - 130, 41);
    }
    s_sd_capacity = make_label(card, "", &font_puhui_16_4, SETTINGS_MUTED);
    if (s_sd_capacity != NULL) {
        lv_obj_set_width(s_sd_capacity, LV_HOR_RES - 58);
        lv_label_set_long_mode(s_sd_capacity, LV_LABEL_LONG_DOT);
        lv_obj_set_pos(s_sd_capacity, 12, 64);
    }

    s_firmware_version = make_label(card, "固件版本：未知",
                                    &font_puhui_16_4, SETTINGS_TEXT);
    if (s_firmware_version != NULL) {
        lv_obj_set_width(s_firmware_version, LV_HOR_RES - 58);
        lv_label_set_long_mode(s_firmware_version, LV_LABEL_LONG_DOT);
        lv_obj_set_pos(s_firmware_version, 12, 94);
    }
    s_build_time = make_label(card, "", &font_puhui_16_4, SETTINGS_MUTED);
    if (s_build_time != NULL) {
        lv_obj_set_width(s_build_time, LV_HOR_RES - 58);
        lv_label_set_long_mode(s_build_time, LV_LABEL_LONG_DOT);
        lv_obj_set_pos(s_build_time, 12, 117);
    }

    const esp_app_desc_t *description = esp_app_get_description();
    if (description != NULL) {
        lv_label_set_text_fmt(
            s_firmware_version,
            app_language_is_english() ? "Firmware: %s" : "固件版本：%s",
            description->version);
        lv_label_set_text_fmt(
            s_build_time,
            app_language_is_english() ? "Build time: %s %s" : "构建时间：%s %s",
            description->date, description->time);
    }
    refresh_sd_status();
}

static void sleep_cb(lv_event_t *event)
{
    (void)event;
    app_sfx_play(APP_SFX_TAP);
    show_confirmation(SETTINGS_ACTION_SLEEP, "关机休眠",
                      "进入深度休眠，屏幕熄灭。\n若无法唤醒，请按复位键。",
                      SETTINGS_ORANGE);
}

static void restart_cb(lv_event_t *event)
{
    (void)event;
    app_sfx_play(APP_SFX_TAP);
    show_confirmation(SETTINGS_ACTION_RESTART, "重启设备",
                      "设备将立即重启。", SETTINGS_ORANGE);
}

static void reset_preferences_cb(lv_event_t *event)
{
    (void)event;
    app_sfx_play(APP_SFX_TAP);
    show_confirmation(SETTINGS_ACTION_RESET_PREFERENCES,
                      "恢复设置默认值",
                      "将恢复音量、亮度、提示音和小智开关。WiFi 信息会保留。",
                      SETTINGS_RED);
}

static void build_system_section(void)
{
    lv_obj_t *card = create_section(s_content, "系统", 184);
    if (card == NULL) {
        return;
    }
    make_action_button(card, 42, "关机休眠", SETTINGS_RED, sleep_cb);
    make_action_button(card, 88, "重启设备", SETTINGS_ORANGE, restart_cb);
    make_action_button(card, 134, "恢复设置默认值", SETTINGS_RED,
                       reset_preferences_cb);
}

static void refresh_home_status(void)
{
    if (s_home_wifi_status != NULL) {
        lv_label_set_text(s_home_wifi_status,
                          wifi_is_connected(NULL) ?
                              app_language_text("已连接", "Connected") :
                              app_language_text("未连接", "Disconnected"));
    }
    if (s_home_display_status != NULL) {
        if (app_language_is_english()) {
            lv_label_set_text_fmt(s_home_display_status,
                "Brightness %d%% · Volume %d%%",
                esp_xiaozhi_chat_app_get_brightness(),
                esp_xiaozhi_chat_app_get_volume());
        } else {
            lv_label_set_text_fmt(s_home_display_status,
                "亮度 %d%% · 音量 %d%%",
                esp_xiaozhi_chat_app_get_brightness(),
                esp_xiaozhi_chat_app_get_volume());
        }
    }
    if (s_home_xiaozhi_status != NULL) {
        lv_label_set_text(s_home_xiaozhi_status,
            xiaozhi_state_name(esp_xiaozhi_chat_app_get_state()));
    }
    if (s_home_device_status != NULL) {
        lv_label_set_text(s_home_device_status,
                          app_sdcard_mounted() ?
                              app_language_text("已挂载", "Mounted") :
                              app_language_text("未挂载", "Not mounted"));
    }
}

static void build_settings_home(void)
{
    s_home_wifi_status = NULL;
    s_home_display_status = NULL;
    s_home_xiaozhi_status = NULL;
    s_home_battery_status = NULL;
    s_home_device_status = NULL;

    make_navigation_card(s_content, "WLAN", "检测中", FONT_AWESOME_WIFI,
                         0x2EAF78, SETTINGS_VIEW_WLAN,
                         &s_home_wifi_status);
    make_navigation_card(s_content, "蓝牙", "管理功能未接入",
                         FONT_AWESOME_BLUETOOTH, 0x3288E8,
                         SETTINGS_VIEW_BLUETOOTH, NULL);
    make_navigation_card(s_content, "显示与声音", "读取中",
                         FONT_AWESOME_GEAR, 0x8B6CE8,
                         SETTINGS_VIEW_DISPLAY,
                         &s_home_display_status);
    const char *language_status = app_language_is_english()
        ? "English / 英文" : "中文 / Chinese";
    make_navigation_card(s_content, "语言 / Language", language_status,
                         FONT_AWESOME_GEAR, 0x7C83FD,
                         SETTINGS_VIEW_LANGUAGE, NULL);
    make_navigation_card(s_content, "小智 AI", "读取中", FONT_AWESOME_AI_CHIP,
                          0xE58C36, SETTINGS_VIEW_XIAOZHI,
                          &s_home_xiaozhi_status);
    make_navigation_card(s_content, "电池与充电", "检测中",
                         FONT_AWESOME_BATTERY_CHARGING, 0x34C759,
                         SETTINGS_VIEW_BATTERY, &s_home_battery_status);
    make_navigation_card(s_content, "存储与设备", "检测中",
                          FONT_AWESOME_SD_CARD, 0x20A6B2,
                          SETTINGS_VIEW_DEVICE, &s_home_device_status);
    make_navigation_card(s_content, "系统", "关机、重启与恢复",
                         FONT_AWESOME_POWER, 0xD45C62,
                          SETTINGS_VIEW_SYSTEM, NULL);
    refresh_home_status();
    refresh_battery_status();
}

static void confirm_cancel_cb(lv_event_t *event)
{
    (void)event;
    app_sfx_play(APP_SFX_TAP);
    if (s_confirm_overlay != NULL) {
        lv_obj_del(s_confirm_overlay);
        s_confirm_overlay = NULL;
    }
    s_confirm_action = SETTINGS_ACTION_NONE;
}

static void confirm_apply_cb(lv_event_t *event)
{
    (void)event;
    app_sfx_play(APP_SFX_TAP);
    const settings_action_t action = s_confirm_action;
    if (s_confirm_overlay != NULL) {
        lv_obj_del(s_confirm_overlay);
        s_confirm_overlay = NULL;
    }
    s_confirm_action = SETTINGS_ACTION_NONE;

    if (action == SETTINGS_ACTION_SLEEP) {
        if (battery_monitor_request_sleep() == ESP_OK) {
            set_notice("正在进入休眠", SETTINGS_ORANGE);
        } else {
            set_notice("关机休眠不可用，请重启设备", SETTINGS_RED);
        }
    } else if (action == SETTINGS_ACTION_RESET_WIFI) {
        wifi_creds_erase_and_reboot();
    } else if (action == SETTINGS_ACTION_RESET_PREFERENCES) {
        if (app_settings_reset() == ESP_OK) {
            esp_restart();
        } else {
            set_notice("恢复默认设置失败", SETTINGS_RED);
        }
    } else if (action == SETTINGS_ACTION_RESTART) {
        esp_restart();
    }
}

static void show_confirmation(settings_action_t action,
                              const char *title,
                              const char *detail,
                              uint32_t accent)
{
    if (s_root == NULL || s_confirm_overlay != NULL) {
        return;
    }
    s_confirm_action = action;
    s_confirm_overlay = lv_obj_create(s_root);
    if (s_confirm_overlay == NULL) {
        s_confirm_action = SETTINGS_ACTION_NONE;
        return;
    }
    lv_obj_set_size(s_confirm_overlay, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_pos(s_confirm_overlay, 0, 0);
    lv_obj_set_style_bg_color(s_confirm_overlay, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_confirm_overlay, LV_OPA_70, 0);
    lv_obj_set_style_border_width(s_confirm_overlay, 0, 0);
    lv_obj_set_style_pad_all(s_confirm_overlay, 0, 0);
    lv_obj_clear_flag(s_confirm_overlay, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *dialog = lv_obj_create(s_confirm_overlay);
    lv_obj_set_size(dialog, LV_HOR_RES - 42, 166);
    lv_obj_center(dialog);
    ui_page_style_panel(dialog);
    lv_obj_set_style_pad_all(dialog, 12, 0);

    lv_obj_t *title_label = make_label(dialog, title, &font_puhui_20_4,
                                       accent);
    if (title_label != NULL) {
        lv_obj_set_pos(title_label, 0, 0);
    }
    lv_obj_t *detail_label = make_label(dialog, detail,
                                        &font_puhui_16_4, SETTINGS_TEXT);
    if (detail_label != NULL) {
        lv_obj_set_width(detail_label, LV_HOR_RES - 68);
        lv_obj_set_height(detail_label, 60);
        lv_label_set_long_mode(detail_label, LV_LABEL_LONG_WRAP);
        lv_obj_set_pos(detail_label, 0, 32);
    }

    lv_obj_t *cancel = lv_btn_create(dialog);
    lv_obj_set_size(cancel, 104, 38);
    lv_obj_set_pos(cancel, 2, 112);
    ui_page_style_button(cancel, 0x151E24, 0x202B32, 8);
    lv_obj_t *cancel_text = make_label(cancel, "取消",
                                       &font_puhui_16_4, SETTINGS_TEXT);
    if (cancel_text != NULL) {
        lv_obj_center(cancel_text);
    }
    lv_obj_add_event_cb(cancel, confirm_cancel_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *confirm = lv_btn_create(dialog);
    lv_obj_set_size(confirm, 104, 38);
    lv_obj_align(confirm, LV_ALIGN_BOTTOM_RIGHT, -2, -2);
    ui_page_style_button(confirm, accent, 0x30343A, 8);
    lv_obj_t *confirm_text = make_label(confirm, "确认",
                                        &font_puhui_16_4, SETTINGS_TEXT);
    if (confirm_text != NULL) {
        lv_obj_center(confirm_text);
    }
    lv_obj_add_event_cb(confirm, confirm_apply_cb, LV_EVENT_CLICKED, NULL);
}

static const char *settings_view_title(settings_view_t view)
{
    switch (view) {
    case SETTINGS_VIEW_BATTERY: return "电池与充电";
    case SETTINGS_VIEW_WLAN: return "WLAN";
    case SETTINGS_VIEW_BLUETOOTH: return "蓝牙";
    case SETTINGS_VIEW_DISPLAY: return "显示与声音";
    case SETTINGS_VIEW_XIAOZHI: return "小智 AI";
    case SETTINGS_VIEW_DEVICE: return "存储与设备";
    case SETTINGS_VIEW_SYSTEM: return "系统";
    case SETTINGS_VIEW_LANGUAGE: return "语言 / Language";
    case SETTINGS_VIEW_HOME:
    default: return "设置";
    }
}

static void settings_render_view(void)
{
    if (s_root == NULL) {
        return;
    }
    if (s_content != NULL) {
        lv_obj_del(s_content);
        s_content = NULL;
    }
    if (s_header != NULL) {
        lv_obj_del(s_header);
        s_header = NULL;
    }

    s_wifi_status = NULL;
    s_xiaozhi_status = NULL;
    s_battery_status = NULL;
    s_battery_percent = NULL;
    s_battery_voltage = NULL;
    s_battery_input = NULL;
    s_battery_phase = NULL;
    s_battery_fault = NULL;
    s_battery_detail = NULL;
    s_sd_status = NULL;
    s_sd_capacity = NULL;
    s_firmware_version = NULL;
    s_build_time = NULL;
    s_volume_value = NULL;
    s_brightness_value = NULL;
    s_home_wifi_status = NULL;
    s_home_display_status = NULL;
    s_home_xiaozhi_status = NULL;
    s_home_battery_status = NULL;
    s_home_device_status = NULL;

    const bool is_home = s_view == SETTINGS_VIEW_HOME;
    s_header = ui_page_make_header(s_root, FONT_AWESOME_GEAR,
        settings_view_title(s_view),
        is_home ? "设备偏好与连接状态" : "设置详情",
        SETTINGS_BLUE);
    if (s_header != NULL && s_back_button != NULL) {
        lv_obj_move_to_index(s_back_button, -1);
    }

    s_content = lv_obj_create(s_root);
    if (s_content == NULL) {
        return;
    }
    lv_obj_set_size(s_content, LV_HOR_RES - 20, LV_VER_RES - 104);
    lv_obj_set_pos(s_content, 10, 54);
    lv_obj_set_flex_flow(s_content, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_content, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_bg_opa(s_content, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_content, 0, 0);
    lv_obj_set_style_shadow_width(s_content, 0, 0);
    lv_obj_set_style_pad_all(s_content, 3, 0);
    lv_obj_set_style_pad_row(s_content, 8, 0);
    lv_obj_set_scroll_dir(s_content, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(s_content, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_add_flag(s_content, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(s_content, LV_OBJ_FLAG_SCROLL_CHAIN);

    switch (s_view) {
    case SETTINGS_VIEW_BATTERY:
        build_battery_detail();
        break;
    case SETTINGS_VIEW_WLAN:
        build_wlan_detail();
        break;
    case SETTINGS_VIEW_BLUETOOTH:
        build_bluetooth_detail();
        break;
    case SETTINGS_VIEW_DISPLAY:
        build_display_section();
        break;
    case SETTINGS_VIEW_XIAOZHI:
        build_ai_detail();
        break;
    case SETTINGS_VIEW_DEVICE:
        build_device_section();
        break;
    case SETTINGS_VIEW_SYSTEM:
        build_system_section();
        break;
    case SETTINGS_VIEW_LANGUAGE:
        build_language_detail();
        break;
    case SETTINGS_VIEW_HOME:
    default:
        build_settings_home();
        break;
    }
}

static bool page_settings_on_gesture_at(ui_gesture_t gesture, int x, int y)
{
    (void)x;
    (void)y;
    if (gesture == UI_GESTURE_SWIPE_RIGHT && s_view != SETTINGS_VIEW_HOME) {
        s_view = SETTINGS_VIEW_HOME;
        settings_render_view();
        return true;
    }
    /* The scroll container owns vertical swipes; do not let page navigation
     * turn a scroll-down into an accidental exit to the launcher. */
    return gesture == UI_GESTURE_SWIPE_UP ||
           gesture == UI_GESTURE_SWIPE_DOWN;
}

esp_err_t page_settings_on_enter(void)
{
    s_root = esp_xiaozhi_chat_display_page_root();
    if (s_root == NULL) {
        return ESP_FAIL;
    }
    ui_page_style_root(s_root);
    s_header = NULL;
    s_back_button = NULL;
    s_content = NULL;
    s_notice = NULL;
    s_confirm_overlay = NULL;
    s_confirm_action = SETTINGS_ACTION_NONE;
    s_view = SETTINGS_VIEW_HOME;
    s_last_refresh_ms = 0;
    s_back_button = make_settings_back_button(s_root);
    settings_render_view();
    s_notice = make_label(s_root, "", &font_puhui_16_4, SETTINGS_MUTED);
    if (s_notice != NULL) {
        lv_obj_set_width(s_notice, LV_HOR_RES - 24);
        lv_obj_set_style_text_align(s_notice, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_align(s_notice, LV_ALIGN_BOTTOM_MID, 0, -24);
    }
    return ESP_OK;
}

esp_err_t page_settings_on_exit(void)
{
    s_root = NULL;
    s_content = NULL;
    s_header = NULL;
    s_back_button = NULL;
    s_notice = NULL;
    s_confirm_overlay = NULL;
    s_wifi_status = NULL;
    s_xiaozhi_status = NULL;
    s_home_battery_status = NULL;
    s_battery_status = NULL;
    s_battery_percent = NULL;
    s_battery_voltage = NULL;
    s_battery_input = NULL;
    s_battery_phase = NULL;
    s_battery_fault = NULL;
    s_battery_detail = NULL;
    s_sd_status = NULL;
    s_sd_capacity = NULL;
    s_firmware_version = NULL;
    s_build_time = NULL;
    s_volume_value = NULL;
    s_brightness_value = NULL;
    s_home_wifi_status = NULL;
    s_home_display_status = NULL;
    s_home_xiaozhi_status = NULL;
    s_home_device_status = NULL;
    s_confirm_action = SETTINGS_ACTION_NONE;
    s_view = SETTINGS_VIEW_HOME;
    return ESP_OK;
}

void page_settings_on_tick(uint32_t ms)
{
    if (ms - s_last_refresh_ms < 1000) {
        return;
    }
    s_last_refresh_ms = ms;
    refresh_ai_status();
    refresh_battery_status();
    refresh_sd_status();
    refresh_home_status();
}

const ui_page_t page_settings = {
    .id = PAGE_SETTINGS,
    .name = "settings",
    .on_enter = page_settings_on_enter,
    .on_exit = page_settings_on_exit,
    .on_gesture_at = page_settings_on_gesture_at,
    .on_tick = page_settings_on_tick,
};
