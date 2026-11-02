#include "page_face.h"

#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#include "esp_random.h"
#include "esp_log.h"
#include "app_sfx.h"
#include "esp_xiaozhi_chat_display.h"

#define TAG "PAGE_FACE"

#define FACE_EMOTION_INTERVAL_MIN_MS 5000U
#define FACE_EMOTION_INTERVAL_MAX_MS 12000U
#define FACE_EMOTION_REACTION_HOLD_MS 3000U

static bool s_random_timer_armed;
static uint32_t s_next_random_ms;

static uint32_t face_random_interval_ms(void)
{
    const uint32_t span = FACE_EMOTION_INTERVAL_MAX_MS - FACE_EMOTION_INTERVAL_MIN_MS + 1U;
    return FACE_EMOTION_INTERVAL_MIN_MS + (esp_random() % span);
}

static void face_hold_random_until(uint32_t now_ms)
{
    s_random_timer_armed = true;
    s_next_random_ms = now_ms + FACE_EMOTION_REACTION_HOLD_MS;
}

/* This page is the FACE mode host for the official emote engine
 * (esp_emote_expression). The emote renderer owns the LCD panel at 25fps while
 * this page is active (see esp_xiaozhi_chat_display_enable_emote / set_face_visible).
 * Taps choose a random emotion; long-presses request a laugh reaction. */

esp_err_t page_face_on_enter(void)
{
    s_random_timer_armed = false;
    s_next_random_ms = 0;
    return ESP_OK;
}

esp_err_t page_face_on_exit(void)
{
    s_random_timer_armed = false;
    return ESP_OK;
}

void page_face_on_tick(uint32_t ms)
{
    if (!esp_xiaozhi_chat_display_emote_is_idle()) {
        s_random_timer_armed = false;
        return;
    }

    if (!s_random_timer_armed) {
        s_next_random_ms = ms + face_random_interval_ms();
        s_random_timer_armed = true;
        return;
    }

    if ((int32_t)(ms - s_next_random_ms) < 0) {
        return;
    }

    if (esp_xiaozhi_chat_display_set_random_emotion() == ESP_OK) {
        s_next_random_ms = ms + face_random_interval_ms();
    } else {
        s_random_timer_armed = false;
    }
}

bool page_face_on_gesture(ui_gesture_t g)
{
    if (g == UI_GESTURE_TAP) {
        app_sfx_play(APP_SFX_TAP);
        (void)esp_xiaozhi_chat_display_set_random_emotion();
        face_hold_random_until(ui_page_manager_now_ms());
        return true;
    }
    if (g == UI_GESTURE_LONG_PRESS) {
        app_sfx_play(APP_SFX_PAGE);
        esp_xiaozhi_chat_display_laugh();
        face_hold_random_until(ui_page_manager_now_ms());
        return true;
    }
    /* Swipe up from the face opens the app-grid launcher (桌面). */
    if (g == UI_GESTURE_SWIPE_UP) {
        extern esp_err_t ui_page_manager_switch(ui_page_id_t);
        ui_page_manager_switch(PAGE_LAUNCHER);
        return true;
    }
    return false;
}

/* Keep the face-page chat callback wired for the shared chat flow. The emote
 * renderer intentionally does not display transient chat/status overlays. */
void page_face_on_chat(chat_role_t role, const char *text)
{
    if (role == CHAT_ROLE_USER) {
        esp_xiaozhi_chat_display_set_chat_message("user", text);
    } else if (role == CHAT_ROLE_ASSISTANT) {
        esp_xiaozhi_chat_display_set_chat_message("assistant", text);
    } else {
        esp_xiaozhi_chat_display_set_chat_message("system", text);
    }
}

const ui_page_t page_face = {
    .id        = PAGE_FACE,
    .name      = "face",
    .on_enter  = page_face_on_enter,
    .on_exit   = page_face_on_exit,
    .on_tick   = page_face_on_tick,
    .on_gesture= page_face_on_gesture,
    .on_chat   = page_face_on_chat,
};
