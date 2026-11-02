/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Xiaozhi AI runtime control page. The page only requests state changes; the
 * chat/audio lifecycle is executed by the background runtime worker.
 */
#include "page_xiaozhi.h"

#include "app_sfx.h"
#include "app_language.h"
#include "esp_log.h"
#include "esp_xiaozhi_chat_app.h"
#include "esp_xiaozhi_chat_display.h"
#include "font_awesome_symbols.h"
#include "lvgl.h"
#include "page_launcher.h"
#include "ui_page_style.h"
#include "launcher_icons.h"

LV_FONT_DECLARE(font_awesome_30_4);
LV_FONT_DECLARE(font_puhui_16_4);
LV_FONT_DECLARE(font_puhui_20_4);

static lv_obj_t *s_status = NULL;
static lv_obj_t *s_detail = NULL;
static lv_obj_t *s_toggle = NULL;
static lv_obj_t *s_toggle_label = NULL;
static lv_obj_t *s_state_dot = NULL;
static uint32_t s_last_refresh_ms = 0;
static bool s_toggle_request_error = false;

static const char *TAG = "PAGE_XIAOZHI";

static const char *xiaozhi_state_text(esp_xiaozhi_chat_app_state_t state)
{
    switch (state) {
    case ESP_XIAOZHI_CHAT_APP_STATE_ENABLED:
        return app_language_text("已开启", "Enabled");
    case ESP_XIAOZHI_CHAT_APP_STATE_STARTING:
        return app_language_text("开启中", "Starting");
    case ESP_XIAOZHI_CHAT_APP_STATE_STOPPING:
        return app_language_text("关闭中", "Stopping");
    case ESP_XIAOZHI_CHAT_APP_STATE_DISABLED:
        return app_language_text("已关闭", "Disabled");
    case ESP_XIAOZHI_CHAT_APP_STATE_ERROR:
        return app_language_text("启动失败", "Start failed");
    default:
        return app_language_text("准备中", "Ready");
    }
}

static void page_xiaozhi_refresh(void)
{
    const esp_xiaozhi_chat_app_state_t state =
        esp_xiaozhi_chat_app_get_state();
    const bool busy = state == ESP_XIAOZHI_CHAT_APP_STATE_STARTING ||
                      state == ESP_XIAOZHI_CHAT_APP_STATE_STOPPING;

    if (s_status != NULL) {
        lv_label_set_text(s_status, xiaozhi_state_text(state));
    }
    if (s_state_dot != NULL) {
        const uint32_t color = s_toggle_request_error ||
                               state == ESP_XIAOZHI_CHAT_APP_STATE_ERROR
                                   ? 0xF28B82
                                   : busy ? 0xE8BE72
                                   : state == ESP_XIAOZHI_CHAT_APP_STATE_ENABLED
                                         ? 0x66DDB1 : 0x738491;
        lv_obj_set_style_bg_color(s_state_dot, lv_color_hex(color), 0);
    }
    if (s_detail != NULL) {
        if (s_toggle_request_error) {
            lv_label_set_text(s_detail, app_language_text("小智 AI 控制未就绪，请稍后重试", "Xiaozhi is not ready. Please try again."));
        } else if (busy) {
            lv_label_set_text(s_detail, app_language_text("正在处理，请稍候...", "Processing, please wait..."));
        } else if (state == ESP_XIAOZHI_CHAT_APP_STATE_ENABLED) {
            lv_label_set_text(s_detail, app_language_text("语音服务已就绪，可在后台常驻", "Voice service is ready and can run in the background."));
        } else if (state == ESP_XIAOZHI_CHAT_APP_STATE_ERROR) {
            lv_label_set_text(s_detail, app_language_text("启动失败，可再次点击重试", "Start failed. Tap to retry."));
        } else {
            lv_label_set_text(s_detail, app_language_text("开启语音服务，与小智聊聊天", "Enable voice service and chat with Xiaozhi."));
        }
    }
    if (s_toggle_label != NULL) {
        const char *toggle_text =
            state == ESP_XIAOZHI_CHAT_APP_STATE_ENABLED ?
                app_language_text("关闭小智 AI", "Disable Xiaozhi AI") :
            state == ESP_XIAOZHI_CHAT_APP_STATE_STARTING ?
                app_language_text("开启中", "Starting") :
            state == ESP_XIAOZHI_CHAT_APP_STATE_STOPPING ?
                app_language_text("关闭中", "Stopping") :
                app_language_text("开启小智 AI", "Enable Xiaozhi AI");
        lv_label_set_text(s_toggle_label,
                          toggle_text);
    }
    if (s_toggle != NULL) {
        lv_obj_set_style_bg_color(
            s_toggle,
            state == ESP_XIAOZHI_CHAT_APP_STATE_ENABLED
                ? lv_color_hex(0x22363F)
                : lv_color_hex(0x398DEB),
            0);
        lv_obj_set_style_bg_color(
            s_toggle,
            state == ESP_XIAOZHI_CHAT_APP_STATE_ENABLED
                ? lv_color_hex(0x304B57) : lv_color_hex(0x2674CA),
            LV_STATE_PRESSED);
        if (busy) {
            lv_obj_add_state(s_toggle, LV_STATE_DISABLED);
        } else {
            lv_obj_clear_state(s_toggle, LV_STATE_DISABLED);
        }
    }
}

static void page_xiaozhi_toggle_cb(lv_event_t *e)
{
    (void)e;
    const esp_xiaozhi_chat_app_state_t state =
        esp_xiaozhi_chat_app_get_state();
    if (state == ESP_XIAOZHI_CHAT_APP_STATE_STARTING ||
        state == ESP_XIAOZHI_CHAT_APP_STATE_STOPPING) {
        return;
    }

    app_sfx_play(APP_SFX_TAP);
    const bool enable = state != ESP_XIAOZHI_CHAT_APP_STATE_ENABLED;
    esp_err_t ret = esp_xiaozhi_chat_app_set_enabled(enable);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "failed to %s Xiaozhi: %s",
                 enable ? "enable" : "disable", esp_err_to_name(ret));
        s_toggle_request_error = true;
        page_xiaozhi_refresh();
        return;
    }
    s_toggle_request_error = false;
    page_xiaozhi_refresh();
}

esp_err_t page_xiaozhi_on_enter(void)
{
    lv_obj_t *root = esp_xiaozhi_chat_display_page_root();
    if (root == NULL) {
        return ESP_FAIL;
    }

    ui_page_style_root(root);
    ui_page_make_back_button(root);
    ui_page_make_header_image(root, &launcher_icon_xiaozhi, "小智 AI", "你的专属语音伙伴");

    lv_obj_t *card = lv_obj_create(root);
    lv_obj_set_size(card, LV_HOR_RES - 32, 96);
    lv_obj_set_pos(card, 16, 54);
    ui_page_style_panel(card);
    lv_obj_set_style_bg_color(card, lv_color_hex(0x121F2B), 0);
    lv_obj_set_style_border_color(card, lv_color_hex(0x273E50), 0);
    lv_obj_set_style_radius(card, 16, 0);
    lv_obj_set_style_pad_all(card, 0, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *icon_box = lv_obj_create(card);
    lv_obj_set_size(icon_box, 42, 42);
    lv_obj_set_pos(icon_box, 14, 12);
    lv_obj_set_style_bg_color(icon_box, lv_color_hex(0x203A52), 0);
    lv_obj_set_style_bg_opa(icon_box, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(icon_box, 0, 0);
    lv_obj_set_style_radius(icon_box, 13, 0);
    lv_obj_set_style_pad_all(icon_box, 0, 0);
    lv_obj_clear_flag(icon_box, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *icon = lv_label_create(icon_box);
    lv_label_set_text(icon, FONT_AWESOME_AI_CHIP);
    lv_obj_set_style_text_font(icon, &font_awesome_30_4, 0);
    lv_obj_set_style_text_color(icon, lv_color_hex(0x8DC7FF), 0);
    lv_obj_center(icon);

    s_status = lv_label_create(card);
    lv_obj_set_width(s_status, LV_HOR_RES - 132);
    lv_obj_set_style_text_font(s_status, &font_puhui_20_4, 0);
    lv_obj_set_style_text_color(s_status, lv_color_white(), 0);
    lv_obj_align(s_status, LV_ALIGN_TOP_LEFT, 70, 21);

    s_state_dot = lv_obj_create(card);
    lv_obj_set_size(s_state_dot, 8, 8);
    lv_obj_align(s_state_dot, LV_ALIGN_TOP_RIGHT, -16, 28);
    lv_obj_set_style_radius(s_state_dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(s_state_dot, 0, 0);
    lv_obj_set_style_bg_opa(s_state_dot, LV_OPA_COVER, 0);
    lv_obj_clear_flag(s_state_dot, LV_OBJ_FLAG_SCROLLABLE);

    s_detail = lv_label_create(card);
    lv_obj_set_width(s_detail, LV_HOR_RES - 64);
    lv_obj_set_style_text_font(s_detail, &font_puhui_16_4, 0);
    lv_obj_set_style_text_color(s_detail, lv_color_hex(0xA1B2C1), 0);
    lv_obj_align(s_detail, LV_ALIGN_TOP_LEFT, 14, 58);
    lv_label_set_long_mode(s_detail, LV_LABEL_LONG_WRAP);

    /* Keep the guide above the bottom action button on the 320x320 display. */
    const int guide_y = 156;
    const int guide_bottom_gap = 8;
    const int guide_bottom = LV_VER_RES - 18 - 46 - guide_bottom_gap;
    const int guide_height = guide_bottom > guide_y ? guide_bottom - guide_y : 92;
    lv_obj_t *guide = lv_obj_create(root);
    lv_obj_set_size(guide, LV_HOR_RES - 32, guide_height);
    lv_obj_set_pos(guide, 16, guide_y);
    ui_page_style_panel(guide);
    lv_obj_set_style_bg_color(guide, lv_color_hex(0x10171D), 0);
    lv_obj_set_style_border_color(guide, lv_color_hex(0x202D37), 0);
    lv_obj_set_style_radius(guide, 16, 0);
    lv_obj_set_style_pad_all(guide, 0, 0);
    lv_obj_clear_flag(guide, LV_OBJ_FLAG_SCROLLABLE);

    const char *steps[] = {
        app_language_text("第一次呼叫：你好小智", "First call: Hello Xiaozhi"),
        app_language_text("第二次：你好小智，讲个笑话", "Then: Ask for a joke"),
    };
    const char *hints[] = {
        app_language_text("仅唤醒小智", "Wake Xiaozhi only"),
        app_language_text("就可以开始正常对话啦", "Then start a normal convrsation"),
    };
    for (int i = 0; i < 2; ++i) {
        lv_obj_t *number = lv_label_create(guide);
        lv_label_set_text(number, i == 0 ? "1" : "2");
        lv_obj_set_size(number, 24, 24);
        lv_obj_set_pos(number, 12, 9 + i * 42);
        lv_obj_set_style_text_font(number, &font_puhui_16_4, 0);
        lv_obj_set_style_text_color(number, lv_color_hex(0x8DC7FF), 0);
        lv_obj_set_style_text_align(number, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_bg_color(number, lv_color_hex(0x203246), 0);
        lv_obj_set_style_bg_opa(number, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(number, 8, 0);
        lv_obj_set_style_pad_top(number, 3, 0);

        lv_obj_t *step = lv_label_create(guide);
        lv_label_set_text(step, steps[i]);
        lv_obj_set_width(step, LV_HOR_RES - 94);
        lv_obj_set_height(step, 22);
        lv_label_set_long_mode(step, LV_LABEL_LONG_WRAP);
        lv_obj_set_pos(step, 46, 2 + i * 42);
        lv_obj_set_style_text_font(step, &font_puhui_16_4, 0);
        lv_obj_set_style_text_color(step, lv_color_hex(0xE6EEF5), 0);

        lv_obj_t *hint = lv_label_create(guide);
        lv_label_set_text(hint, hints[i]);
        lv_obj_set_width(hint, LV_HOR_RES - 94);
        lv_obj_set_height(hint, 20);
        lv_obj_set_pos(hint, 46, 24 + i * 42);
        lv_obj_set_style_text_font(hint, &font_puhui_16_4, 0);
        lv_obj_set_style_text_color(hint, lv_color_hex(0x8C9EAD), 0);
    }

    s_toggle = lv_btn_create(root);
    lv_obj_set_size(s_toggle, LV_HOR_RES - 64, 46);
    lv_obj_align(s_toggle, LV_ALIGN_BOTTOM_MID, 0, -18);
    ui_page_style_button(s_toggle, 0x398DEB, 0x2674CA, 15);
    lv_obj_set_style_border_width(s_toggle, 0, 0);
    lv_obj_set_style_bg_color(s_toggle, lv_color_hex(0x25323E), LV_STATE_DISABLED);
    lv_obj_set_style_bg_opa(s_toggle, LV_OPA_COVER, LV_STATE_DISABLED);
    lv_obj_add_event_cb(s_toggle, page_xiaozhi_toggle_cb, LV_EVENT_CLICKED,
                        NULL);

    s_toggle_label = lv_label_create(s_toggle);
    lv_obj_set_style_text_font(s_toggle_label, &font_puhui_20_4, 0);
    lv_obj_set_style_text_color(s_toggle_label, lv_color_white(), 0);
    lv_obj_center(s_toggle_label);

    s_last_refresh_ms = 0;
    page_xiaozhi_refresh();
    return ESP_OK;
}

esp_err_t page_xiaozhi_on_exit(void)
{
    s_status = NULL;
    s_detail = NULL;
    s_toggle = NULL;
    s_toggle_label = NULL;
    s_state_dot = NULL;
    s_toggle_request_error = false;
    return ESP_OK;
}

void page_xiaozhi_on_tick(uint32_t ms)
{
    if (ms - s_last_refresh_ms < 100) {
        return;
    }
    s_last_refresh_ms = ms;
    page_xiaozhi_refresh();
}

const ui_page_t page_xiaozhi = {
    .id = PAGE_XIAOZHI,
    .name = "xiaozhi",
    .on_enter = page_xiaozhi_on_enter,
    .on_exit = page_xiaozhi_on_exit,
    .on_tick = page_xiaozhi_on_tick,
};
