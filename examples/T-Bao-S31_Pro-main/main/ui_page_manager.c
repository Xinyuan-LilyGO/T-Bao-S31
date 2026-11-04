/*
 * SPDX-License-Identifier: Apache-2.0
 */
#include "ui_page_manager.h"

#include "esp_log.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_lvgl_port.h"
#include "esp_xiaozhi_chat_app.h"
#include "esp_xiaozhi_chat_display.h"
#include "app_face_follow.h"
#include "app_language.h"
#include "ui_control_center.h"
#include "ui_page_style.h"
#include "lvgl.h"

#include <stdlib.h>

static const char *TAG = "UI_PAGE_MGR";

/* Feature pages use their content area for scrolling or game controls. Only
 * a gesture that starts at the matching screen edge is treated as the
 * hierarchical back gesture, so an ordinary content swipe cannot leave the
 * page accidentally. */
#define UI_NAV_EDGE_PX 28

LV_FONT_DECLARE(font_puhui_16_4);

static const ui_page_t **s_pages = NULL;
static int s_count = 0;
static int s_cur = -1;
static uint32_t s_tick_ms = 0;
static esp_timer_handle_t s_tick_timer = NULL;
static bool s_allow_face_follow_once = false;
static lv_obj_t *s_face_follow_guard_popup = NULL;
static lv_timer_t *s_face_follow_guard_timer = NULL;

static void tick_timer_cb(void *arg)
{
    (void)arg;
    ui_page_manager_tick((uint32_t)(esp_timer_get_time() / 1000));
}

/* Persistent page indicator (dots) drawn on the LVGL screen, independent of the
 * page root so it survives page-root clears. */
static lv_obj_t *s_indicator = NULL;

typedef struct {
    ui_gesture_t gesture;
    int x;
    int y;
} pending_gesture_t;

static void indicator_rebuild(void)
{
    if (!lvgl_port_lock(1000)) {
        return;
    }
    if (s_indicator) {
        lv_obj_del(s_indicator);
        s_indicator = NULL;
    }
    if (s_count <= 0 || s_cur < 0) {
        lvgl_port_unlock();
        return;
    }
    s_indicator = lv_obj_create(lv_screen_active());
    if (!s_indicator) {
        lvgl_port_unlock();
        return;
    }
    lv_obj_set_size(s_indicator, LV_HOR_RES, 14);
    lv_obj_align(s_indicator, LV_ALIGN_BOTTOM_MID, 0, -2);
    lv_obj_set_flex_flow(s_indicator, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(s_indicator, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_bg_opa(s_indicator, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_indicator, 0, 0);
    lv_obj_set_style_pad_column(s_indicator, 6, 0);
    lv_obj_clear_flag(s_indicator, LV_OBJ_FLAG_CLICKABLE);

    for (int i = 0; i < s_count; ++i) {
        lv_obj_t *dot = lv_obj_create(s_indicator);
        lv_obj_set_size(dot, 8, 8);
        lv_obj_set_style_radius(dot, 4, 0);
        lv_obj_set_style_border_width(dot, 0, 0);
        if (i == s_cur) {
            lv_obj_set_style_bg_color(dot, lv_color_white(), 0);
            lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
        } else {
            lv_obj_set_style_bg_color(dot, lv_color_white(), 0);
            lv_obj_set_style_bg_opa(dot, LV_OPA_30, 0);
        }
    }
    lvgl_port_unlock();
}

static void indicator_hide(void)
{
    if (s_indicator != NULL) {
        lv_obj_add_flag(s_indicator, LV_OBJ_FLAG_HIDDEN);
    }
}

static bool gesture_started_at_back_edge(ui_gesture_t gesture, int x, int y)
{
    if (gesture == UI_GESTURE_SWIPE_RIGHT) {
        return x >= 0 && x <= UI_NAV_EDGE_PX;
    }
    if (gesture == UI_GESTURE_SWIPE_DOWN) {
        return y >= 0 && y <= UI_NAV_EDGE_PX;
    }
    return false;
}

static bool xiaozhi_blocks_face_follow(void)
{
    const esp_xiaozhi_chat_app_state_t state =
        esp_xiaozhi_chat_app_get_state();

    /* The chat runtime still owns audio/network resources until it is fully
     * disabled. Keep face-follow from starting during any transition that can
     * overlap with that resource usage. */
    return state == ESP_XIAOZHI_CHAT_APP_STATE_STARTING ||
           state == ESP_XIAOZHI_CHAT_APP_STATE_ENABLED ||
           state == ESP_XIAOZHI_CHAT_APP_STATE_STOPPING;
}

static void hide_face_follow_guard_popup(void)
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

static void face_follow_guard_timer_cb(lv_timer_t *timer)
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

static void face_follow_guard_ok_cb(lv_event_t *event)
{
    (void)event;
    hide_face_follow_guard_popup();
}

static void show_face_follow_guard_popup(ui_page_id_t blocked_page)
{
    lv_obj_t *root = esp_xiaozhi_chat_display_page_root();
    if (root == NULL) {
        return;
    }

    hide_face_follow_guard_popup();
    s_face_follow_guard_popup = lv_obj_create(root);
    if (s_face_follow_guard_popup == NULL) {
        return;
    }
    lv_obj_set_size(s_face_follow_guard_popup, LV_HOR_RES - 36, 146);
    lv_obj_center(s_face_follow_guard_popup);
    ui_page_style_panel(s_face_follow_guard_popup);
    lv_obj_set_style_pad_all(s_face_follow_guard_popup, 12, 0);
    lv_obj_clear_flag(s_face_follow_guard_popup, LV_OBJ_FLAG_SCROLLABLE |
                                                     LV_OBJ_FLAG_SCROLL_CHAIN);

    lv_obj_t *label = lv_label_create(s_face_follow_guard_popup);
    if (label == NULL) {
        hide_face_follow_guard_popup();
        return;
    }
    lv_obj_set_width(label, LV_HOR_RES - 60);
    lv_obj_set_height(label, 76);
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    const char *page_name = blocked_page == PAGE_CAMERA
                                ? app_language_text("摄像头", "Camera")
                                : app_language_text("小智 AI", "Xiaozhi AI");
    lv_label_set_text_fmt(
        label,
        app_language_is_english()
            ? "Background face follow is active. Stop it before opening %s."
            : "人脸跟随正在后台运行，请先停止跟随后再打开%s。",
        page_name);
    lv_obj_set_style_text_font(label, &font_puhui_16_4, 0);
    lv_obj_set_style_text_color(label, lv_color_white(), 0);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(label, LV_ALIGN_TOP_MID, 0, 4);

    lv_obj_t *button = lv_btn_create(s_face_follow_guard_popup);
    if (button == NULL) {
        hide_face_follow_guard_popup();
        return;
    }
    lv_obj_set_size(button, 92, 34);
    lv_obj_align(button, LV_ALIGN_BOTTOM_MID, 0, -8);
    ui_page_style_button(button, 0x398DEB, 0x2674CA, 16);
    lv_obj_add_event_cb(button, face_follow_guard_ok_cb, LV_EVENT_CLICKED,
                        NULL);
    lv_obj_t *button_label = lv_label_create(button);
    lv_label_set_text(button_label, app_language_text("知道了", "OK"));
    lv_obj_set_style_text_font(button_label, &font_puhui_16_4, 0);
    lv_obj_set_style_text_color(button_label, lv_color_white(), 0);
    lv_obj_center(button_label);

    s_face_follow_guard_timer = lv_timer_create(
        face_follow_guard_timer_cb, 4000, NULL);
    if (s_face_follow_guard_timer == NULL) {
        hide_face_follow_guard_popup();
    }
}

static bool face_follow_blocks_page(ui_page_id_t id)
{
    return app_face_follow_is_enabled() &&
           (id == PAGE_XIAOZHI || id == PAGE_CAMERA);
}

esp_err_t ui_page_manager_init(const ui_page_t *pages[], int count)
{
    ESP_RETURN_ON_FALSE(pages && count > 0, ESP_ERR_INVALID_ARG, TAG, "bad pages");
    s_pages = pages;
    s_count = count;
    s_cur = -1;

    if (s_tick_timer == NULL) {
        esp_timer_create_args_t t = {
            .callback = tick_timer_cb,
            .name = "ui_page_tick",
        };
        esp_timer_create(&t, &s_tick_timer);
        esp_timer_start_periodic(s_tick_timer, 50 * 1000);
    }
    return ESP_OK;
}

esp_err_t ui_page_manager_switch(ui_page_id_t id)
{
    if (s_pages == NULL || (int)id < 0 || (int)id >= s_count) {
        return ESP_ERR_INVALID_ARG;
    }
    if ((int)id == s_cur) {
        return ESP_OK;
    }

    const bool allow_face_follow = s_allow_face_follow_once;
    s_allow_face_follow_once = false;
    if (id == PAGE_FACE_FOLLOW && xiaozhi_blocks_face_follow() &&
        !allow_face_follow) {
        ESP_LOGW(TAG, "blocked face follow while Xiaozhi AI is active");
        return ESP_ERR_INVALID_STATE;
    }
    if (face_follow_blocks_page(id)) {
        ESP_LOGW(TAG, "blocked page %s while background face follow is active",
                 s_pages[id]->name);
        bool guard_locked = lvgl_port_lock(1000);
        show_face_follow_guard_popup(id);
        if (guard_locked) {
            lvgl_port_unlock();
        }
        return ESP_ERR_INVALID_STATE;
    }

    /* on_enter/on_exit build and tear down LVGL widgets. When this is called
     * from the touch task (gesture dispatch) we are already inside the port
     * lock; when called from the app task at startup we lock here. The lock is
     * recursive, so nesting is safe. */
    bool locked = lvgl_port_lock(1000);

    hide_face_follow_guard_popup();

    if (s_cur >= 0 && s_pages[s_cur]->on_exit) {
        s_pages[s_cur]->on_exit();
    }
    /* Clear any LVGL widgets the previous page built. */
    esp_xiaozhi_chat_display_clear_page_root();

    bool face = (s_pages[id]->id == PAGE_FACE);
    esp_xiaozhi_chat_display_set_face_visible(face);

    s_cur = (int)id;
    if (s_pages[id]->on_enter) {
        s_pages[id]->on_enter();
    }
    if (face) {
        indicator_hide();
    } else {
        indicator_rebuild();
    }
    ESP_LOGI(TAG, "switched to page %d (%s)", s_cur, s_pages[id]->name);

    /* Page builds are the heaviest ordinary LVGL allocation burst. Logging the
     * internal heap here makes a leak or a page that permanently eats SRAM
     * visible in the serial log: internal free/largest should return to a
     * stable plateau after leaving the page, and the largest block must stay
     * well above the ~100 bytes needed by esp_timer_create() in the Wi-Fi
     * power-save path. */
    const size_t internal_free =
        heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    const size_t internal_largest =
        heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (internal_largest < 2048) {
        ESP_LOGW(TAG,
                 "internal SRAM low after %s: free=%u largest=%u - Wi-Fi/TLS "
                 "allocations may fail",
                 s_pages[id]->name, (unsigned)internal_free,
                 (unsigned)internal_largest);
    } else {
        ESP_LOGI(TAG, "internal SRAM after %s: free=%u largest=%u",
                 s_pages[id]->name, (unsigned)internal_free,
                 (unsigned)internal_largest);
    }

    if (locked) {
        lvgl_port_unlock();
    }
    return ESP_OK;
}

void ui_page_manager_allow_face_follow_once(void)
{
    s_allow_face_follow_once = true;
}

esp_err_t ui_page_manager_reload_current(void)
{
    if (s_pages == NULL || s_cur < 0 || s_cur >= s_count) {
        return ESP_ERR_INVALID_STATE;
    }
    bool locked = lvgl_port_lock(1000);
    if (s_pages[s_cur]->on_exit) {
        s_pages[s_cur]->on_exit();
    }
    esp_xiaozhi_chat_display_clear_page_root();
    const bool face = s_pages[s_cur]->id == PAGE_FACE;
    esp_xiaozhi_chat_display_set_face_visible(face);
    if (s_pages[s_cur]->on_enter) {
        s_pages[s_cur]->on_enter();
    }
    if (face) {
        indicator_hide();
    } else {
        indicator_rebuild();
    }
    if (locked) {
        lvgl_port_unlock();
    }
    return ESP_OK;
}

ui_page_id_t ui_page_manager_current(void)
{
    return (s_cur >= 0) ? s_pages[s_cur]->id : PAGE_FACE;
}

void ui_page_manager_tick(uint32_t ms)
{
    s_tick_ms = ms;
    if (!lvgl_port_lock(1000)) {
        return;
    }

    /* Most on_tick callbacks manipulate LVGL objects and run from an esp_timer
     * callback, NOT the LVGL task, so those callbacks require the port lock.
     * PAGE_FACE is the emote-only exception: capture its callback while the
     * page state is stable, then release LVGL before invoking it. */
    const ui_page_t *page = (s_cur >= 0) ? s_pages[s_cur] : NULL;
    if (page != NULL && page->id == PAGE_FACE) {
        void (*face_tick)(uint32_t) = page->on_tick;
        lvgl_port_unlock();
        if (face_tick != NULL) {
            face_tick(ms);
        }
        return;
    }
    if (page != NULL && page->on_tick != NULL) {
        page->on_tick(ms);
    }
    lvgl_port_unlock();
}

/* Execute a gesture while the caller owns the LVGL port lock. This function is
 * also used by the LVGL async callback below, so page callbacks and renderer
 * handoff always run on the LVGL task rather than ui_touch_disp. */
static void dispatch_gesture_now(ui_gesture_t g, int x, int y)
{
    /* The pull-down control center intercepts gestures first: while it is open
     * it consumes everything; a swipe-down when closed opens it. */
    if (ui_control_center_on_gesture(g, x, y)) {
        return;
    }
    if (s_cur < 0) {
        return;
    }
    bool consumed = false;
    if (s_pages[s_cur]->on_gesture_at) {
        consumed = s_pages[s_cur]->on_gesture_at(g, x, y);
    } else if (s_pages[s_cur]->on_gesture) {
        consumed = s_pages[s_cur]->on_gesture(g);
    }
    if (consumed) {
        return;
    }
    if (g == UI_GESTURE_SWIPE_LEFT || g == UI_GESTURE_SWIPE_RIGHT) {
        /* Home screens remain horizontally reachable. On a feature page only
         * the right swipe is a back gesture; left stays available to the page. */
        if (s_cur == PAGE_FACE) {
            ui_page_manager_switch(PAGE_LAUNCHER);
        } else if (s_cur == PAGE_LAUNCHER) {
            ui_page_manager_switch(PAGE_FACE);
        } else if (g == UI_GESTURE_SWIPE_RIGHT &&
                   gesture_started_at_back_edge(g, x, y)) {
            ui_page_manager_switch(PAGE_LAUNCHER);
        }
    } else if (g == UI_GESTURE_SWIPE_DOWN) {
        /* The launcher consumes its own down swipe and returns to FACE. A
         * feature page uses down as the same one-level back operation as the
         * top-left home button, but only when the gesture starts at the top
         * edge; the rest of the page is content. */
        if (s_cur != PAGE_FACE && s_cur != PAGE_LAUNCHER &&
            gesture_started_at_back_edge(g, x, y)) {
            ui_page_manager_switch(PAGE_LAUNCHER);
        }
    }
}

static void dispatch_gesture_async(void *arg)
{
    pending_gesture_t *pending = (pending_gesture_t *)arg;
    if (pending == NULL) {
        return;
    }

    dispatch_gesture_now(pending->gesture, pending->x, pending->y);
    free(pending);
}

void ui_page_manager_dispatch_gesture(ui_gesture_t g, int x, int y)
{
    if (s_cur >= 0 && s_pages[s_cur]->is_direct_mode != NULL &&
        s_pages[s_cur]->is_direct_mode() &&
        s_pages[s_cur]->on_direct_gesture_at != NULL) {
        (void)s_pages[s_cur]->on_direct_gesture_at(g, x, y);
        return;
    }

    const bool face_reaction = s_cur == PAGE_FACE &&
                               (g == UI_GESTURE_TAP ||
                                g == UI_GESTURE_LONG_PRESS);
    if (face_reaction) {
        /* Keep control-center interception semantics, but invoke the face
         * callback without LVGL's lock: it calls emote APIs directly. */
        if (ui_control_center_on_gesture(g, x, y)) {
            return;
        }
        if (s_cur >= 0 && s_pages[s_cur]->on_gesture_at != NULL) {
            (void)s_pages[s_cur]->on_gesture_at(g, x, y);
        } else if (s_cur >= 0 && s_pages[s_cur]->on_gesture != NULL) {
            (void)s_pages[s_cur]->on_gesture(g);
        }
        return;
    }

    if (!lvgl_port_lock(1000)) {
        return;
    }

    pending_gesture_t *pending = malloc(sizeof(*pending));
    if (pending == NULL) {
        ESP_LOGW(TAG, "dropping gesture: no memory for LVGL handoff");
        lvgl_port_unlock();
        return;
    }
    pending->gesture = g;
    pending->x = x;
    pending->y = y;

    if (lv_async_call(dispatch_gesture_async, pending) != LV_RESULT_OK) {
        free(pending);
        ESP_LOGW(TAG, "dropping gesture: LVGL async handoff failed");
        lvgl_port_unlock();
        return;
    }

    /* FACE mode stops LVGL timer processing while emote owns the panel. An
     * async gesture must briefly re-enable it so the queued page transition
     * can run; switching to PAGE mode resumes the normal LVGL tick. */
    if (s_cur == PAGE_FACE) {
        lv_timer_enable(true);
    }
    lvgl_port_task_wake(LVGL_PORT_EVENT_USER, NULL);
    lvgl_port_unlock();
}

void ui_page_manager_notify_chat(chat_role_t role, const char *text)
{
    if (s_cur < 0) {
        /* Manager not started yet: fall back to the emote toast path. */
        esp_xiaozhi_chat_display_set_chat_message(
            role == CHAT_ROLE_USER ? "user" : (role == CHAT_ROLE_ASSISTANT ? "assistant" : "system"),
            text);
        return;
    }
    if (s_pages[s_cur]->on_chat) {
        s_pages[s_cur]->on_chat(role, text);
    }
}

void ui_page_manager_notify_status(const char *status)
{
    if (s_cur < 0 || status == NULL) {
        return;
    }
    if (s_pages[s_cur]->on_status) {
        s_pages[s_cur]->on_status(status);
    }
}

uint32_t ui_page_manager_now_ms(void)
{
    return s_tick_ms;
}
