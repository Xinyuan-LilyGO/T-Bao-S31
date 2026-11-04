/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "esp_xiaozhi_chat_display.h"

#include "ui_touch.h"
#include "esp_board_manager.h"
#include "esp_board_manager_adapter.h"
#include "dev_display_lcd.h"
#include "font_awesome_symbols.h"
#include "font_emoji.h"
#include "expression_emote.h"
#include "widget/gfx_anim.h"
#include "ui_page_manager.h"
#include "app_language.h"

#include <esp_err.h>
#include <esp_check.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_lvgl_port.h>
#include <esp_pm.h>
#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>
#include <esp_partition.h>
#include <esp_random.h>
#include <esp_timer.h>
#include <inttypes.h>
#include <lvgl.h>
#include "src/display/lv_display_private.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

/* LVGL pointer input read callback (defined below) mirrors ui_touch's state. */
void lvgl_port_touch_mirror_read(lv_indev_t *indev, lv_indev_data_t *data);

#define LANG_STR_INITIALIZING           "正在初始化..."
#define LANG_STR_BATTERY_NEED_CHARGE    "电量低，请充电"

#define DARK_BACKGROUND_COLOR           lv_color_hex(0x121212)
#define DARK_TEXT_COLOR                 lv_color_white()
#define DARK_CHAT_BACKGROUND_COLOR      lv_color_hex(0x1E1E1E)
#define DARK_USER_BUBBLE_COLOR          lv_color_hex(0x1A6C37)
#define DARK_ASSISTANT_BUBBLE_COLOR     lv_color_hex(0x333333)
#define DARK_SYSTEM_BUBBLE_COLOR        lv_color_hex(0x2A2A2A)
#define DARK_SYSTEM_TEXT_COLOR          lv_color_hex(0xAAAAAA)
#define DARK_BORDER_COLOR               lv_color_hex(0x333333)
#define DARK_LOW_BATTERY_COLOR          lv_color_hex(0xFF0000)

#if CONFIG_ESP_BOARD_TBAO_S31_BOARD
#define TBAO_LCD_SW_ROTATE              1
#else
#define TBAO_LCD_SW_ROTATE              0
#endif

#define LIGHT_BACKGROUND_COLOR          lv_color_white()
#define LIGHT_TEXT_COLOR                lv_color_black()
#define LIGHT_CHAT_BACKGROUND_COLOR     lv_color_hex(0xE0E0E0)
#define LIGHT_USER_BUBBLE_COLOR         lv_color_hex(0x95EC69)
#define LIGHT_ASSISTANT_BUBBLE_COLOR    lv_color_white()
#define LIGHT_SYSTEM_BUBBLE_COLOR       lv_color_hex(0xE0E0E0)
#define LIGHT_SYSTEM_TEXT_COLOR         lv_color_hex(0x666666)
#define LIGHT_BORDER_COLOR              lv_color_hex(0xE0E0E0)
#define LIGHT_LOW_BATTERY_COLOR         lv_color_black()

LV_FONT_DECLARE(font_awesome_30_4);
LV_FONT_DECLARE(font_puhui_20_4);
LV_FONT_DECLARE(font_awesome_20_4);

typedef struct {
    const lv_font_t *text_font;
    const lv_font_t *icon_font;
    const lv_font_t *emoji_font;
} display_fonts_t;

typedef struct {
    lv_color_t background;
    lv_color_t text;
    lv_color_t chat_background;
    lv_color_t user_bubble;
    lv_color_t assistant_bubble;
    lv_color_t system_bubble;
    lv_color_t system_text;
    lv_color_t border;
    lv_color_t low_battery;
} theme_colors_t;

typedef enum {
    DISPLAY_KIND_NONE,
    DISPLAY_KIND_LCD,
} display_kind_t;

typedef struct display {
    display_kind_t kind;
    int width;
    int height;
    esp_pm_lock_handle_t pm_lock;
    lv_display_t *lv_disp;
    lv_obj_t *emotion_label;
    lv_obj_t *network_label;
    lv_obj_t *status_label;
    lv_obj_t *notification_label;
    lv_obj_t *notification_close_button;
    lv_obj_t *mute_label;
    lv_obj_t *battery_label;
    lv_obj_t *chat_message_label;
    lv_obj_t *low_battery_popup;
    lv_obj_t *low_battery_label;
    lv_obj_t *shutdown_popup;
    lv_obj_t *shutdown_countdown_label;
    lv_obj_t *shutdown_close_button;
    bool shutdown_countdown_active;
    bool shutdown_countdown_dismissed;
    const char *battery_icon;
    int battery_percent;
    bool battery_valid;
    bool battery_charging;
    bool low_battery_warning_visible;
    const char *network_icon;
    bool muted;
    char current_theme_name[32];
    esp_timer_handle_t notification_timer;
} display_t;

typedef struct {
    display_t base;
    esp_lcd_panel_io_handle_t panel_io;
    esp_lcd_panel_handle_t panel;
    lv_display_flush_cb_t port_flush_cb;
    lv_obj_t *status_bar;
    lv_obj_t *content;
    lv_obj_t *container;
    lv_obj_t *side_bar;
    lv_obj_t *preview_image;
    display_fonts_t fonts;
    theme_colors_t current_theme;
    struct {
        uint32_t refresh_count;
        uint32_t flush_count;
        uint64_t pixels;
        uint64_t wait_us;
        int64_t wait_start_us;
        int64_t window_start_us;
    } perf;
} lcd_display_t;

static const char *TAG = "ESP_XIAOZHI_CHAT_DISPLAY";

static bool s_lvgl_inited;
static bool s_lvgl_port_inited;
static display_t *s_display = NULL;

static const struct {
    const char *text;
    const char *fa_icon;
    const char *unicode_icon;
} s_emotion_table[] = {
    {"neutral", FONT_AWESOME_EMOJI_NEUTRAL, "😶"},
    {"happy", FONT_AWESOME_EMOJI_HAPPY, "🙂"},
    {"sad", FONT_AWESOME_EMOJI_SAD, "😔"},
    {"crying", FONT_AWESOME_EMOJI_CRYING, "😭"},
    {"sleepy", FONT_AWESOME_EMOJI_SLEEPY, "😴"},
    {"angry", FONT_AWESOME_EMOJI_ANGRY, "😠"},
    {"shocked", FONT_AWESOME_EMOJI_SHOCKED, "😱"},
    {"confused", FONT_AWESOME_EMOJI_CONFUSED, "🙄"},
    {"winking", FONT_AWESOME_EMOJI_WINKING, "😉"},
};
#define EMOTION_COUNT (sizeof(s_emotion_table) / sizeof(s_emotion_table[0]))

/* ===================== EMOTE (animated expression) backend =====================
 * When enabled, the animated emote face takes over the LCD panel after
 * provisioning completes (see esp_xiaozhi_chat_display_enable_emote). The LVGL
 * display is deleted so only emote drives the panel. If emote init/mount fails,
 * the LVGL display keeps working as a graceful fallback. */
static emote_handle_t s_emote = NULL;
static bool s_emote_active = false;
static bool s_emote_idle = false;
static int s_emote_current_index = 0;
static portMUX_TYPE s_emote_state_lock = portMUX_INITIALIZER_UNLOCKED;
/* Serialize high-level emote commands issued by chat, touch, timers, and
 * battery/status callbacks. The gfx layer has its own lock, but compound
 * commands (for example animation + dialog timer) still need one owner. */
static SemaphoreHandle_t s_emote_api_mutex = NULL;
/* Last eye animation requested through set_emotion(); used to resume it when
 * the face page becomes visible again after a PAGE had the panel. */
static const char *s_last_eye_anim = "neutral";
#define EMOTE_API_LOCK_TIMEOUT_MS 1000
/* True while the animated face is the active panel renderer. When false the
 * LVGL page UI owns the panel (see esp_xiaozhi_chat_display_set_face_visible). */
static bool s_face_visible = false;
static bool s_face_accept_flush = false;
typedef enum {
    PANEL_FLUSH_OWNER_NONE = 0,
    PANEL_FLUSH_OWNER_LVGL,
    PANEL_FLUSH_OWNER_EMOTE,
    PANEL_FLUSH_OWNER_VIDEO,
} panel_flush_owner_t;

static volatile panel_flush_owner_t s_panel_flush_owner = PANEL_FLUSH_OWNER_NONE;
static portMUX_TYPE s_panel_flush_lock = portMUX_INITIALIZER_UNLOCKED;
/* The sending task owns this mutex until DMA has really finished. The ISR
 * only reports completion; it must never release a task-owned mutex. */
static SemaphoreHandle_t s_panel_tx_mutex = NULL;
static lcd_display_t *s_panel_lcd = NULL;
static uint16_t *s_emote_rotate_buffer = NULL;
static size_t s_emote_rotate_buffer_pixels = 0;
static int s_emote_display_height = 0;
static uint16_t *s_video_rotate_buffer = NULL;
static size_t s_video_rotate_buffer_pixels = 0;
static SemaphoreHandle_t s_video_flush_sem = NULL;
static bool s_video_visible = false;
static bool s_video_accept_flush = false;
/* LVGL container reused by every PAGE-mode page (dialogue / camera / ...). */
static lv_obj_t *s_page_root = NULL;

typedef struct {
    const char *emotion;
    const char *anim;
} emote_map_t;

/* Maps app emotion names to entries in the bundled 360x360 asset pack. */
static const emote_map_t s_emote_map[] = {
    {"neutral",     "neutral"},
    {"happy",       "happy"},
    {"sad",         "sad"},
    {"crying",      "crying"},
    {"sleepy",      "sleepy"},
    {"angry",       "angry"},
    {"shocked",     "shocked"},
    {"confused",    "confused"},
    {"winking",     "winking"},
};
#define EMOTE_MAP_COUNT (sizeof(s_emote_map) / sizeof(s_emote_map[0]))

static size_t emote_index_for(const char *emotion)
{
    if (emotion != NULL) {
        for (size_t i = 0; i < EMOTE_MAP_COUNT; ++i) {
            if (strcmp(s_emote_map[i].emotion, emotion) == 0) {
                return i;
            }
        }
    }
    return 0;
}

static const char *emote_anim_for(const char *emotion)
{
    return s_emote_map[emote_index_for(emotion)].anim;
}

static void emote_record_current_index(size_t index)
{
    portENTER_CRITICAL(&s_emote_state_lock);
    s_emote_current_index = (int)index;
    portEXIT_CRITICAL(&s_emote_state_lock);
}

/* Forward declarations: LVGL-port lock/unlock helpers (defined further below)
 * are also used by the page-manager-facing helpers above. */
static bool esp_xiaozhi_chat_display_lock(display_t *disp, int timeout_ms);
static void esp_xiaozhi_chat_display_unlock(display_t *disp);
static esp_err_t esp_xiaozhi_chat_display_sync_panel(lcd_display_t *lcd);
static bool emote_on_flush_done(esp_lcd_panel_io_handle_t panel_io,
                                esp_lcd_panel_io_event_data_t *edata,
                                void *user_ctx);

static bool emote_api_lock(const char *operation)
{
    if (s_emote_api_mutex == NULL) {
        ESP_LOGW(TAG, "emote API mutex unavailable for %s",
                 operation != NULL ? operation : "operation");
        return false;
    }
    if (xSemaphoreTake(s_emote_api_mutex,
                       pdMS_TO_TICKS(EMOTE_API_LOCK_TIMEOUT_MS)) != pdTRUE) {
        ESP_LOGW(TAG, "timed out waiting for emote API mutex (%s)",
                 operation != NULL ? operation : "operation");
        return false;
    }
    return true;
}

static void emote_api_unlock(void)
{
    if (s_emote_api_mutex != NULL) {
        xSemaphoreGive(s_emote_api_mutex);
    }
}

/* The eye animation is the emote's only continuously running animation.  When
 * a PAGE owns the panel (camera, launcher, settings, ...) the emote's output is
 * discarded, but its render task still decodes and draws a full 360x360 frame
 * at the animation rate.  Stopping it while hidden frees that CPU for the
 * page, which is what made the camera preview (and any LVGL animation) feel
 * sluggish whenever the AI face was active in the background. */
static void emote_set_eye_anim_running(bool running)
{
    if (!s_emote_active || s_emote == NULL) {
        return;
    }
    if (!emote_api_lock(running ? "eye anim start" : "eye anim stop")) {
        return;
    }
    if (running) {
        /* Use the emote API rather than gfx_anim_start(): it takes the gfx
         * render lock before touching the animation. */
        (void)emote_set_anim_emoji(s_emote, s_last_eye_anim);
    } else {
        gfx_obj_t *eye = emote_get_obj_by_name(s_emote, EMT_DEF_ELEM_EYE_ANIM);
        if (eye != NULL) {
            (void)gfx_anim_stop(eye);
        }
    }
    emote_api_unlock();
}

static void emote_set_status_event(const char *status)
{
    if (s_emote == NULL || status == NULL) {
        return;
    }

    if (!emote_api_lock("status")) {
        return;
    }

    const bool idle = strncmp(status, "Ready", 5) == 0 ||
                      strncmp(status, "Connected", 9) == 0 ||
                      strncmp(status, "Disconnected", 12) == 0 ||
                      strncmp(status, "AI Disabled", 11) == 0;
    portENTER_CRITICAL(&s_emote_state_lock);
    s_emote_idle = idle;
    portEXIT_CRITICAL(&s_emote_state_lock);

    if (strncmp(status, "Listening", 8) == 0) {
        emote_set_event_msg(s_emote, EMOTE_MGR_EVT_LISTEN, NULL);
    } else if (strncmp(status, "Speaking", 8) == 0) {
        emote_set_event_msg(s_emote, EMOTE_MGR_EVT_SPEAK, NULL);
    } else if (strncmp(status, "Ready", 5) == 0 ||
               strncmp(status, "Connected", 9) == 0 ||
               strncmp(status, "Disconnected", 12) == 0 ||
               strncmp(status, "AI Disabled", 11) == 0) {
        emote_set_event_msg(s_emote, EMOTE_MGR_EVT_OFF, NULL);
    } else {
        emote_set_event_msg(s_emote, EMOTE_MGR_EVT_SET, NULL);
    }
    emote_api_unlock();
}

static bool emote_assets_partition_valid(void)
{
    const esp_partition_t *partition = esp_partition_find_first(
        ESP_PARTITION_TYPE_ANY, ESP_PARTITION_SUBTYPE_ANY, "assets");
    if (partition == NULL) {
        ESP_LOGW(TAG, "emote: assets partition is missing");
        return false;
    }

    uint32_t stored_files = 0;
    uint32_t stored_checksum = 0;
    uint32_t stored_len = 0;
    esp_err_t ret = esp_partition_read(partition, 0, &stored_files, sizeof(stored_files));
    ret |= esp_partition_read(partition, 4, &stored_checksum, sizeof(stored_checksum));
    ret |= esp_partition_read(partition, 8, &stored_len, sizeof(stored_len));
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "emote: cannot read assets header: %s", esp_err_to_name(ret));
        return false;
    }

    const uint64_t table_min_len = (uint64_t)stored_files * (CONFIG_MMAP_FILE_NAME_LENGTH + 12U);
    const uint64_t table_max_len = partition->size - 12U;
    if (stored_files == 0 || stored_files > 256 ||
        table_min_len > stored_len || stored_len > table_max_len) {
        ESP_LOGW(TAG, "emote: invalid assets header files=%" PRIu32 " len=%" PRIu32,
                 stored_files, stored_len);
        return false;
    }

    (void)stored_checksum;
    return true;
}

static void emote_release_rotate_buffer(void)
{
    if (s_emote_rotate_buffer != NULL) {
        heap_caps_free(s_emote_rotate_buffer);
        s_emote_rotate_buffer = NULL;
        s_emote_rotate_buffer_pixels = 0;
    }
}

static esp_err_t emote_prepare_rotate_buffer(int width, int height)
{
    const size_t pixels = (size_t)width * height;
    const size_t bytes = pixels * sizeof(uint16_t);

    if (s_emote_rotate_buffer != NULL && s_emote_rotate_buffer_pixels >= pixels) {
        return ESP_OK;
    }

    emote_release_rotate_buffer();
    s_emote_rotate_buffer = (uint16_t *)heap_caps_aligned_alloc(
        64, bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (s_emote_rotate_buffer == NULL) {
        s_emote_rotate_buffer = (uint16_t *)heap_caps_aligned_alloc(
            64, bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
    if (s_emote_rotate_buffer == NULL) {
        ESP_LOGE(TAG, "emote: rotate buffer allocation failed (%zu bytes)", bytes);
        return ESP_ERR_NO_MEM;
    }

    s_emote_rotate_buffer_pixels = pixels;
    return ESP_OK;
}

static void panel_notify_flush_finished(panel_flush_owner_t owner)
{
    switch (owner) {
    case PANEL_FLUSH_OWNER_EMOTE:
        if (s_emote != NULL) {
            (void)emote_notify_flush_finished(s_emote);
        }
        break;
    case PANEL_FLUSH_OWNER_LVGL:
        if (s_panel_lcd != NULL && s_panel_lcd->base.lv_disp != NULL) {
            lv_display_flush_ready(s_panel_lcd->base.lv_disp);
        }
        break;
    case PANEL_FLUSH_OWNER_VIDEO:
        if (s_video_flush_sem != NULL) {
            xSemaphoreGive(s_video_flush_sem);
        }
        break;
    default:
        break;
    }
}

/* Called with the TX mutex held. DISPON is a non-orientation-changing fence:
 * the panel IO waits for queued color DMA before sending the command. Only
 * after this succeeds may a missed completion be reconciled in task context. */
static esp_err_t panel_drain_locked(void)
{
    const esp_err_t ret = esp_xiaozhi_chat_display_sync_panel(s_panel_lcd);
    if (ret == ESP_OK) {
        portENTER_CRITICAL(&s_panel_flush_lock);
        const panel_flush_owner_t owner = s_panel_flush_owner;
        s_panel_flush_owner = PANEL_FLUSH_OWNER_NONE;
        portEXIT_CRITICAL(&s_panel_flush_lock);
        panel_notify_flush_finished(owner);
    }
    return ret;
}

static esp_err_t panel_wait_for_flush_idle(void)
{
    if (s_panel_tx_mutex == NULL || s_panel_lcd == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (xSemaphoreTake(s_panel_tx_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
        ESP_LOGW(TAG, "LCD renderer handoff timed out waiting for TX owner");
        return ESP_ERR_TIMEOUT;
    }
    const esp_err_t ret = panel_drain_locked();
    xSemaphoreGive(s_panel_tx_mutex);
    return ret;
}

static esp_err_t panel_begin_flush(panel_flush_owner_t owner)
{
    if (s_panel_tx_mutex == NULL || s_panel_lcd == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (xSemaphoreTake(s_panel_tx_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
        ESP_LOGW(TAG, "LCD transfer timed out acquiring TX lock (renderer=%d)", owner);
        return ESP_ERR_TIMEOUT;
    }
    portENTER_CRITICAL(&s_panel_flush_lock);
    const bool allowed =
        (owner == PANEL_FLUSH_OWNER_EMOTE && s_face_visible &&
         s_face_accept_flush && !s_video_visible) ||
        (owner == PANEL_FLUSH_OWNER_LVGL && !s_face_visible && !s_video_visible) ||
        (owner == PANEL_FLUSH_OWNER_VIDEO && s_video_visible && s_video_accept_flush);
    const bool idle = s_panel_flush_owner == PANEL_FLUSH_OWNER_NONE;
    if (allowed && idle) {
        s_panel_flush_owner = owner;
    }
    portEXIT_CRITICAL(&s_panel_flush_lock);
    if (!allowed || !idle) {
        xSemaphoreGive(s_panel_tx_mutex);
        return allowed ? ESP_ERR_TIMEOUT : ESP_ERR_INVALID_STATE;
    }
    return ESP_OK;
}

static esp_err_t panel_end_flush(panel_flush_owner_t owner, esp_err_t submit_ret)
{
    const esp_err_t fence_ret = panel_drain_locked();
    if (fence_ret == ESP_OK) {
        /* Reassert in task context after the physical fence, even if the ISR
         * event-group notification could not be queued. No DMA buffer is live. */
        panel_notify_flush_finished(owner);
    } else {
        ESP_LOGE(TAG, "LCD DMA fence failed (renderer=%d): %s", owner,
                 esp_err_to_name(fence_ret));
    }
    xSemaphoreGive(s_panel_tx_mutex);
    return submit_ret == ESP_OK ? fence_ret : submit_ret;
}

static void emote_on_flush_ready(int x_start, int y_start, int x_end, int y_end,
                                 const void *data, emote_handle_t handle)
{
    esp_lcd_panel_handle_t panel = (esp_lcd_panel_handle_t)emote_get_user_data(handle);
    bool accept_flush;
    portENTER_CRITICAL(&s_panel_flush_lock);
    accept_flush = panel != NULL && s_face_visible && s_face_accept_flush;
    portEXIT_CRITICAL(&s_panel_flush_lock);

    if (!accept_flush) {
        /* Face hidden: LVGL owns the panel, so we must not draw over it. Notify
         * completion ourselves so emote's internal flush wait never deadlocks. */
        emote_notify_flush_finished(handle);
        return;
    }

    if (data == NULL || x_end <= x_start || y_end <= y_start) {
        ESP_LOGW(TAG, "emote: invalid flush area (%d,%d)-(%d,%d)",
                 x_start, y_start, x_end, y_end);
        emote_notify_flush_finished(handle);
        return;
    }

    const int src_width = x_end - x_start;
    const int src_height = y_end - y_start;
    const size_t pixels = (size_t)src_width * src_height;
    const void *flush_data = data;
    int flush_x_start = x_start;
    int flush_y_start = y_start;
    int flush_x_end = x_end;
    int flush_y_end = y_end;

#if CONFIG_ESP_BOARD_TBAO_S31_BOARD
    /* The LCD stays in its stable page-mode orientation (MX=1). Reproduce the
     * former face-mode MADCTL transform in the frame so PAGE and FACE can share
     * the PARLIO panel without changing MADCTL while a transfer is queued. */
    if (s_emote_rotate_buffer == NULL || pixels > s_emote_rotate_buffer_pixels) {
        ESP_LOGE(TAG, "emote: rotate buffer too small for %dx%d flush", src_width, src_height);
        emote_notify_flush_finished(handle);
        return;
    }
    if (x_start != 0 || y_start != 0 || x_end != s_display->width ||
        y_end != s_display->height) {
        /* The packed gfx renderer composes whole frames. Do not put an
         * unexpected partial/unaligned frame on the PARLIO bus. */
        ESP_LOGW(TAG, "emote: rejected non-full-frame flush (%d,%d)-(%d,%d)",
                 x_start, y_start, x_end, y_end);
        emote_notify_flush_finished(handle);
        return;
    }
#endif
    if (panel_begin_flush(PANEL_FLUSH_OWNER_EMOTE) != ESP_OK) {
        emote_notify_flush_finished(handle);
        return;
    }
#if CONFIG_ESP_BOARD_TBAO_S31_BOARD
    lv_draw_sw_rotate(data, s_emote_rotate_buffer,
                      src_width, src_height,
                      src_width * (int)sizeof(uint16_t),
                      src_height * (int)sizeof(uint16_t),
                      LV_DISPLAY_ROTATION_270, LV_COLOR_FORMAT_RGB565);
    flush_data = s_emote_rotate_buffer;
    flush_x_start = s_emote_display_height - y_end;
    flush_y_start = x_start;
    flush_x_end = s_emote_display_height - y_start;
    flush_y_end = x_end;
#endif

    esp_err_t ret = esp_lcd_panel_draw_bitmap(panel, flush_x_start, flush_y_start,
                                               flush_x_end, flush_y_end, flush_data);
    ret = panel_end_flush(PANEL_FLUSH_OWNER_EMOTE, ret);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "emote: draw bitmap failed: %s", esp_err_to_name(ret));
    }
}

static bool emote_on_flush_done(const esp_lcd_panel_io_handle_t panel_io,
                                esp_lcd_panel_io_event_data_t *edata, void *user_ctx)
{
    (void)panel_io;
    (void)edata;
    /* Unified color-transfer-done dispatcher. BOTH renderers share the same
     * LCD panel IO, and only one callback slot exists. Route the completion
     * event to whichever renderer actually issued the transfer:
     *  - FACE mode (emote drives the panel): notify emote's flush wait.
     *  - PAGE mode (LVGL drives the panel): call lv_disp_flush_ready() so LVGL
     *    never blocks forever on a flush (previously emote overwrote the LVGL
     *    callback and switching back to a page only drew the first block, then
     *    everything froze). */
    panel_flush_owner_t owner;
    portENTER_CRITICAL_ISR(&s_panel_flush_lock);
    owner = s_panel_flush_owner;
    s_panel_flush_owner = PANEL_FLUSH_OWNER_NONE;
    portEXIT_CRITICAL_ISR(&s_panel_flush_lock);

    switch (owner) {
    case PANEL_FLUSH_OWNER_EMOTE: {
        if (s_emote != NULL) {
            emote_notify_flush_finished(s_emote);
        }
        break;
    }
    case PANEL_FLUSH_OWNER_LVGL: {
        lcd_display_t *lcd = (lcd_display_t *)user_ctx;
        if (lcd != NULL && lcd->base.lv_disp != NULL) {
            lv_display_flush_ready(lcd->base.lv_disp);
        }
        break;
    }
    case PANEL_FLUSH_OWNER_VIDEO:
        if (s_video_flush_sem != NULL) {
            if (xPortInIsrContext() == pdTRUE) {
                BaseType_t higher_priority_task_woken = pdFALSE;
                xSemaphoreGiveFromISR(s_video_flush_sem,
                                      &higher_priority_task_woken);
                return higher_priority_task_woken == pdTRUE;
            }
            xSemaphoreGive(s_video_flush_sem);
        }
        break;
    case PANEL_FLUSH_OWNER_NONE:
    default:
        break;
    }
    return true;
}

esp_err_t esp_xiaozhi_chat_display_enable_emote(void)
{
    if (s_emote_active) {
        return ESP_OK;
    }
    if (s_display == NULL) {
        ESP_LOGW(TAG, "emote: display not initialized yet");
        return ESP_ERR_INVALID_STATE;
    }

    void *lcd_handle = NULL;
    esp_err_t ret = esp_board_manager_get_device_handle("display_lcd", &lcd_handle);
    if (ret != ESP_OK || lcd_handle == NULL) {
        ESP_LOGW(TAG, "emote: no display_lcd handle (%s)", esp_err_to_name(ret));
        return ret;
    }
    dev_display_lcd_handles_t *lcd_handles = (dev_display_lcd_handles_t *)lcd_handle;
    if (lcd_handles->io_handle == NULL || lcd_handles->panel_handle == NULL) {
        ESP_LOGW(TAG, "emote: LCD io/panel handles NULL");
        return ESP_ERR_INVALID_STATE;
    }
    if (!emote_assets_partition_valid()) {
        ESP_LOGW(TAG, "emote: assets unavailable; keeping LVGL display");
        return ESP_ERR_NOT_FOUND;
    }

    const int w = s_display->width;
    const int h = s_display->height;
    s_emote_display_height = h;

#if CONFIG_ESP_BOARD_TBAO_S31_BOARD
    ret = emote_prepare_rotate_buffer(w, h);
    if (ret != ESP_OK) {
        return ret;
    }
#endif

    emote_config_t cfg = {
        .flags = {
            .swap = true,
            .double_buffer = true,
            .buff_dma = false,
            .buff_spiram = true,
        },
        .gfx_emote = {
            .h_res = w,
            .v_res = h,
            .fps = 25,
        },
        .buffers = {
            .buf_pixels = (size_t)(w * h),
        },
        .task = {
            .task_priority = 5,
            .task_stack = 6 * 1024,
            .task_affinity = 0,
            .task_stack_in_ext = true,   /* keep the 6KB stack out of internal SRAM */
        },
        .flush_cb = emote_on_flush_ready,
        /* Whole-scene composition now happens in gfx_render_handler for every
         * dirty refresh, including label-only updates; no try-lock callback. */
        .update_cb = NULL,
        .user_data = (void *)lcd_handles->panel_handle,
    };

    if (!emote_api_lock("init")) {
        emote_release_rotate_buffer();
        return ESP_ERR_TIMEOUT;
    }

    s_emote = emote_init(&cfg);
    if (s_emote == NULL) {
        ESP_LOGE(TAG, "emote_init failed; keeping LVGL display");
        emote_api_unlock();
        emote_release_rotate_buffer();
        return ESP_FAIL;
    }

    emote_data_t src = {
        .type = EMOTE_SOURCE_PARTITION,
        .source = { .partition_label = "assets" },
        .flags = { .mmap_enable = 1 },
    };
    ret = emote_mount_and_load_assets(s_emote, &src);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "emote mount+load failed (%s); keeping LVGL display", esp_err_to_name(ret));
        emote_deinit(s_emote);
        s_emote = NULL;
        emote_api_unlock();
        emote_release_rotate_buffer();
        return ret;
    }
    emote_api_unlock();

    s_emote_active = true;
    ESP_LOGI(TAG, "Emote display active (%dx%d)", w, h);
    if (emote_api_lock("enable animation")) {
        (void)emote_set_anim_emoji(s_emote, "neutral");
        emote_api_unlock();
    }
    emote_set_status_event("Ready");

    /* The animated face now owns the panel. Switch the renderer into FACE mode
     * after the first animation is ready. The next frame callback refreshes the
     * complete canvas without requiring an emote call from this handoff. */
    esp_xiaozhi_chat_display_set_face_visible(true);
    return ESP_OK;
}

static esp_err_t esp_xiaozhi_chat_display_sync_panel(lcd_display_t *lcd)
{
    if (lcd == NULL || lcd->panel == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    /* tx_param waits for all queued PARLIO color transfers. Sending DISPON
     * without changing the panel orientation gives the renderer handoff a
     * synchronization point without rewriting MADCTL. */
    return esp_lcd_panel_disp_on_off(lcd->panel, true);
}

esp_err_t esp_xiaozhi_chat_display_set_face_visible(bool visible)
{
    if (s_display == NULL || s_display->kind != DISPLAY_KIND_LCD) {
        return ESP_ERR_INVALID_STATE;
    }
    if (visible && (!s_emote_active || s_emote == NULL)) {
        ESP_LOGI(TAG, "Face requested before emote is ready; keep LVGL active");
        return ESP_OK;
    }
    lcd_display_t *lcd = (lcd_display_t *)s_display;
    bool was_accepting;
    bool resume_lvgl_on_failure;
    bool stopped_lvgl = false;
    portENTER_CRITICAL(&s_panel_flush_lock);
    was_accepting = s_face_accept_flush;
    resume_lvgl_on_failure = !s_face_visible && !s_video_visible;
    s_face_accept_flush = false;
    portEXIT_CRITICAL(&s_panel_flush_lock);

    esp_err_t ret = panel_wait_for_flush_idle();
    if (ret != ESP_OK) {
        goto handoff_failed;
    }
    if (!esp_xiaozhi_chat_display_lock(s_display, 1000)) {
        ret = ESP_ERR_TIMEOUT;
        goto handoff_failed;
    }
    if (lcd->container != NULL) {
        lv_obj_add_flag(lcd->container, LV_OBJ_FLAG_HIDDEN);
    }
    if (lcd->status_bar != NULL) {
        lv_obj_add_flag(lcd->status_bar, LV_OBJ_FLAG_HIDDEN);
    }
    if (s_page_root != NULL) {
        if (visible) {
            lv_obj_add_flag(s_page_root, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_clear_flag(s_page_root, LV_OBJ_FLAG_HIDDEN);
        }
    }
    if (visible) {
        lv_obj_t *screen = lv_screen_active();
        lv_obj_set_style_bg_color(screen, lv_color_black(), 0);
        lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
        lv_obj_invalidate(screen);
        if (lcd->base.lv_disp != NULL) {
            lv_refr_now(lcd->base.lv_disp);
        }
    }
    esp_xiaozhi_chat_display_unlock(s_display);

    /* Stop/resume outside the LVGL object lock. Never hand the panel to a
     * different renderer merely because a transfer wait timed out. */
    if (visible) {
        lvgl_port_stop();
        stopped_lvgl = true;
        ret = panel_wait_for_flush_idle();
        if (ret != ESP_OK) {
            goto handoff_failed;
        }
        portENTER_CRITICAL(&s_panel_flush_lock);
        s_face_visible = true;
        s_face_accept_flush = true;
        portEXIT_CRITICAL(&s_panel_flush_lock);
        emote_set_eye_anim_running(true);
    } else {
        portENTER_CRITICAL(&s_panel_flush_lock);
        s_face_visible = false;
        portEXIT_CRITICAL(&s_panel_flush_lock);
        lvgl_port_resume();
        /* Avoid rendering animation frames that would only be discarded. */
        emote_set_eye_anim_running(false);
    }
    return ESP_OK;

handoff_failed:
    portENTER_CRITICAL(&s_panel_flush_lock);
    s_face_accept_flush = was_accepting;
    portEXIT_CRITICAL(&s_panel_flush_lock);
    if (stopped_lvgl && resume_lvgl_on_failure) {
        lvgl_port_resume();
    }
    ESP_LOGW(TAG, "LCD face/page handoff aborted: %s", esp_err_to_name(ret));
    return ret;
}

static esp_err_t video_prepare_rotate_buffer(int width, int height)
{
#if CONFIG_ESP_BOARD_TBAO_S31_BOARD
    size_t pixels = (size_t)width * (size_t)height;
    if (s_video_rotate_buffer != NULL &&
        s_video_rotate_buffer_pixels >= pixels) {
        return ESP_OK;
    }
    size_t bytes = pixels * sizeof(uint16_t);
    uint16_t *buffer = (uint16_t *)heap_caps_aligned_alloc(
        64, bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (buffer == NULL) {
        buffer = (uint16_t *)heap_caps_aligned_alloc(
            64, bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
    if (buffer == NULL) {
        return ESP_ERR_NO_MEM;
    }
    heap_caps_free(s_video_rotate_buffer);
    s_video_rotate_buffer = buffer;
    s_video_rotate_buffer_pixels = pixels;
#else
    (void)width;
    (void)height;
#endif
    return ESP_OK;
}

/* LVGL performs this conversion from native RGB565 to the panel's big-endian
 * wire order when swap_bytes is enabled. Video frames bypass LVGL, so the
 * direct panel path has to do the same conversion after software rotation. */
static void video_swap_rgb565_bytes(uint16_t *buffer, int width, int height)
{
#if CONFIG_ESP_BOARD_TBAO_S31_BOARD
    if (buffer == NULL || width <= 0 || height <= 0) {
        return;
    }
    uint8_t *bytes = (uint8_t *)buffer;
    const size_t pixel_count = (size_t)width * (size_t)height;
    for (size_t i = 0; i < pixel_count; ++i) {
        const uint8_t low_byte = bytes[i * 2U];
        bytes[i * 2U] = bytes[i * 2U + 1U];
        bytes[i * 2U + 1U] = low_byte;
    }
#else
    (void)buffer;
    (void)width;
    (void)height;
#endif
}

esp_err_t esp_xiaozhi_chat_display_set_video_visible(bool visible)
{
    if (s_display == NULL || s_display->kind != DISPLAY_KIND_LCD) {
        return ESP_ERR_INVALID_STATE;
    }
    lcd_display_t *lcd = (lcd_display_t *)s_display;
    if (visible) {
        if (s_video_visible) {
            return ESP_OK;
        }
        if (s_video_flush_sem == NULL) {
            s_video_flush_sem = xSemaphoreCreateBinary();
            if (s_video_flush_sem == NULL) {
                return ESP_ERR_NO_MEM;
            }
        }
        esp_err_t ret = video_prepare_rotate_buffer(s_display->width,
                                                    s_display->height);
        if (ret != ESP_OK) {
            return ret;
        }

        portENTER_CRITICAL(&s_panel_flush_lock);
        s_video_accept_flush = false;
        portEXIT_CRITICAL(&s_panel_flush_lock);
        ret = panel_wait_for_flush_idle();
        if (ret != ESP_OK) {
            return ret;
        }
        if (!esp_xiaozhi_chat_display_lock(s_display, 1000)) {
            return ESP_ERR_TIMEOUT;
        }
        if (s_page_root != NULL) {
            lv_obj_add_flag(s_page_root, LV_OBJ_FLAG_HIDDEN);
        }
        if (lcd->container != NULL) {
            lv_obj_add_flag(lcd->container, LV_OBJ_FLAG_HIDDEN);
        }
        if (lcd->status_bar != NULL) {
            lv_obj_add_flag(lcd->status_bar, LV_OBJ_FLAG_HIDDEN);
        }
        esp_xiaozhi_chat_display_unlock(s_display);
        lvgl_port_stop();
        ret = panel_wait_for_flush_idle();
        if (ret != ESP_OK) {
            if (!s_face_visible) {
                lvgl_port_resume();
            }
            return ret;
        }
        portENTER_CRITICAL(&s_panel_flush_lock);
        s_video_visible = true;
        s_video_accept_flush = true;
        portEXIT_CRITICAL(&s_panel_flush_lock);
        return ESP_OK;
    }

    if (!s_video_visible) {
        return ESP_OK;
    }
    portENTER_CRITICAL(&s_panel_flush_lock);
    const bool was_accepting = s_video_accept_flush;
    s_video_accept_flush = false;
    portEXIT_CRITICAL(&s_panel_flush_lock);
    esp_err_t ret = panel_wait_for_flush_idle();
    if (ret == ESP_OK && !esp_xiaozhi_chat_display_lock(s_display, 1000)) {
        ret = ESP_ERR_TIMEOUT;
    }
    if (ret != ESP_OK) {
        portENTER_CRITICAL(&s_panel_flush_lock);
        s_video_accept_flush = was_accepting;
        portEXIT_CRITICAL(&s_panel_flush_lock);
        return ret;
    }
    if (lcd->container != NULL) {
        lv_obj_add_flag(lcd->container, LV_OBJ_FLAG_HIDDEN);
    }
    if (lcd->status_bar != NULL) {
        lv_obj_add_flag(lcd->status_bar, LV_OBJ_FLAG_HIDDEN);
    }
    if (s_page_root != NULL) {
        lv_obj_clear_flag(s_page_root, LV_OBJ_FLAG_HIDDEN);
    }
    portENTER_CRITICAL(&s_panel_flush_lock);
    s_video_visible = false;
    portEXIT_CRITICAL(&s_panel_flush_lock);
    esp_xiaozhi_chat_display_unlock(s_display);
    lvgl_port_resume();
    return ESP_OK;
}

esp_err_t esp_xiaozhi_chat_display_present_video_frame(const uint16_t *frame,
                                                       int width, int height)
{
    if (frame == NULL || width <= 0 || height <= 0 || s_display == NULL ||
        s_display->kind != DISPLAY_KIND_LCD) {
        return ESP_ERR_INVALID_ARG;
    }
    lcd_display_t *lcd = (lcd_display_t *)s_display;
    if (width != s_display->width || height != s_display->height ||
        s_video_flush_sem == NULL) {
        return ESP_ERR_INVALID_SIZE;
    }
    bool active;
    portENTER_CRITICAL(&s_panel_flush_lock);
    active = s_video_visible && s_video_accept_flush;
    portEXIT_CRITICAL(&s_panel_flush_lock);
    if (!active) {
        return ESP_ERR_INVALID_STATE;
    }

    while (xSemaphoreTake(s_video_flush_sem, 0) == pdTRUE) {
    }
    esp_err_t ret = panel_begin_flush(PANEL_FLUSH_OWNER_VIDEO);
    if (ret != ESP_OK) {
        return ret;
    }
    const void *draw_data = frame;
    int x_start = 0;
    int y_start = 0;
    int x_end = width;
    int y_end = height;
#if CONFIG_ESP_BOARD_TBAO_S31_BOARD
    lv_draw_sw_rotate(frame, s_video_rotate_buffer, width, height,
                      width * (int)sizeof(uint16_t),
                      height * (int)sizeof(uint16_t),
                      LV_DISPLAY_ROTATION_270, LV_COLOR_FORMAT_RGB565);
    draw_data = s_video_rotate_buffer;
#endif
    video_swap_rgb565_bytes((uint16_t *)draw_data, width, height);

    ret = esp_lcd_panel_draw_bitmap(lcd->panel, x_start, y_start,
                                    x_end, y_end, draw_data);
    return panel_end_flush(PANEL_FLUSH_OWNER_VIDEO, ret);
}

lv_obj_t *esp_xiaozhi_chat_display_page_root(void)
{
    return s_page_root;
}

esp_err_t esp_xiaozhi_chat_display_clear_page_root(void)
{
    if (s_page_root != NULL && esp_xiaozhi_chat_display_lock(s_display, 1000)) {
        lv_obj_clean(s_page_root);
        esp_xiaozhi_chat_display_unlock(s_display);
    }
    return ESP_OK;
}

static theme_colors_t esp_xiaozhi_chat_display_dark_theme(void)
{
    theme_colors_t t = {0};
    t.background = DARK_BACKGROUND_COLOR;
    t.text = DARK_TEXT_COLOR;
    t.chat_background = DARK_CHAT_BACKGROUND_COLOR;
    t.user_bubble = DARK_USER_BUBBLE_COLOR;
    t.assistant_bubble = DARK_ASSISTANT_BUBBLE_COLOR;
    t.system_bubble = DARK_SYSTEM_BUBBLE_COLOR;
    t.system_text = DARK_SYSTEM_TEXT_COLOR;
    t.border = DARK_BORDER_COLOR;
    t.low_battery = DARK_LOW_BATTERY_COLOR;
    return t;
}

static theme_colors_t esp_xiaozhi_chat_display_light_theme(void)
{
    theme_colors_t t = {0};
    t.background = LIGHT_BACKGROUND_COLOR;
    t.text = LIGHT_TEXT_COLOR;
    t.chat_background = LIGHT_CHAT_BACKGROUND_COLOR;
    t.user_bubble = LIGHT_USER_BUBBLE_COLOR;
    t.assistant_bubble = LIGHT_ASSISTANT_BUBBLE_COLOR;
    t.system_bubble = LIGHT_SYSTEM_BUBBLE_COLOR;
    t.system_text = LIGHT_SYSTEM_TEXT_COLOR;
    t.border = LIGHT_BORDER_COLOR;
    t.low_battery = LIGHT_LOW_BATTERY_COLOR;
    return t;
}

static inline lcd_display_t *esp_xiaozhi_chat_display_from_base(display_t *disp)
{
    return (lcd_display_t *)disp;
}

static bool esp_xiaozhi_chat_display_lock(display_t *disp, int timeout_ms)
{
    if (disp == NULL) {
        return false;
    }
    if (disp->kind == DISPLAY_KIND_LCD) {
        return lvgl_port_lock(timeout_ms);
    }
    (void)timeout_ms;
    return true;
}

static void esp_xiaozhi_chat_display_unlock(display_t *disp)
{
    if (disp == NULL) {
        return;
    }
    if (disp->kind == DISPLAY_KIND_LCD) {
        lvgl_port_unlock();
    }
}

static void esp_xiaozhi_chat_display_hide_notification_locked(display_t *disp)
{
    if (disp == NULL) {
        return;
    }
    if (disp->notification_label != NULL) {
        lv_obj_add_flag(disp->notification_label, LV_OBJ_FLAG_HIDDEN);
    }
    if (disp->notification_close_button != NULL) {
        lv_obj_add_flag(disp->notification_close_button, LV_OBJ_FLAG_HIDDEN);
    }
    if (disp->status_label != NULL) {
        lv_obj_clear_flag(disp->status_label, LV_OBJ_FLAG_HIDDEN);
    }
}

static void esp_xiaozhi_chat_display_notification_close_cb(lv_event_t *event)
{
    if (event == NULL) {
        return;
    }
    display_t *disp = (display_t *)lv_event_get_user_data(event);
    if (disp == NULL) {
        return;
    }
    esp_xiaozhi_chat_display_hide_notification_locked(disp);
    if (disp->notification_timer != NULL) {
        esp_timer_stop(disp->notification_timer);
    }
}

static void esp_xiaozhi_chat_display_shutdown_close_cb(lv_event_t *event)
{
    if (event == NULL) {
        return;
    }
    display_t *disp = (display_t *)lv_event_get_user_data(event);
    if (disp == NULL) {
        return;
    }
    /* Closing the dialog only hides the UI. The battery monitor continues the
     * 30-second protection countdown and will still shut down if charging does
     * not recover. */
    disp->shutdown_countdown_dismissed = true;
    if (disp->shutdown_popup != NULL) {
        lv_obj_add_flag(disp->shutdown_popup, LV_OBJ_FLAG_HIDDEN);
    }
}

static void esp_xiaozhi_chat_display_notification(void *arg)
{
    display_t *disp = (display_t *)arg;
    if (disp == NULL || !esp_xiaozhi_chat_display_lock(disp, 30000)) {
        return;
    }
    esp_xiaozhi_chat_display_hide_notification_locked(disp);
    esp_xiaozhi_chat_display_unlock(disp);
}

static esp_err_t esp_xiaozhi_chat_display_state_init(display_t *disp)
{
    ESP_RETURN_ON_FALSE(disp, ESP_ERR_INVALID_STATE, TAG, "Display not initialized");

    disp->battery_icon = NULL;
    disp->battery_percent = -1;
    disp->battery_valid = false;
    disp->battery_charging = false;
    disp->low_battery_warning_visible = false;
    disp->shutdown_countdown_active = false;
    disp->shutdown_countdown_dismissed = false;
    disp->network_icon = NULL;
    disp->muted = false;
    strlcpy(disp->current_theme_name, "light", sizeof(disp->current_theme_name));

    esp_timer_create_args_t timer_args = {
        .callback = esp_xiaozhi_chat_display_notification,
        .arg = disp,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "notification_timer",
        .skip_unhandled_events = false,
    };
    ESP_ERROR_CHECK(esp_timer_create(&timer_args, &disp->notification_timer));

    esp_err_t ret = esp_pm_lock_create(ESP_PM_APB_FREQ_MAX, 0, "display_update", &disp->pm_lock);
    if (ret == ESP_ERR_NOT_SUPPORTED) {
        ESP_LOGI(TAG, "Power management not supported");
    } else {
        ESP_ERROR_CHECK(ret);
    }

    return ESP_OK;
}

static esp_err_t esp_xiaozhi_chat_display_setup_ui(lcd_display_t *lcd)
{
    display_t *disp = &lcd->base;
    if (!esp_xiaozhi_chat_display_lock(disp, 30000)) {
        return ESP_ERR_INVALID_STATE;
    }
    lv_obj_t *screen = lv_screen_active();
    lv_obj_set_style_text_font(screen, lcd->fonts.text_font, 0);
    lv_obj_set_style_text_color(screen, lcd->current_theme.text, 0);
    lv_obj_set_style_bg_color(screen, lcd->current_theme.background, 0);

    lcd->container = lv_obj_create(screen);
    lv_obj_set_size(lcd->container, LV_HOR_RES, LV_VER_RES);
    lv_obj_add_flag(lcd->container, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_flex_flow(lcd->container, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(lcd->container, 0, 0);
    lv_obj_set_style_border_width(lcd->container, 0, 0);
    lv_obj_set_style_pad_row(lcd->container, 0, 0);
    lv_obj_set_style_bg_color(lcd->container, lcd->current_theme.background, 0);
    lv_obj_set_style_border_color(lcd->container, lcd->current_theme.border, 0);

    lcd->status_bar = lv_obj_create(lcd->container);
    lv_obj_set_size(lcd->status_bar, LV_HOR_RES, lcd->fonts.text_font->line_height);
    lv_obj_set_style_radius(lcd->status_bar, 0, 0);
    lv_obj_set_style_bg_color(lcd->status_bar, lcd->current_theme.background, 0);
    lv_obj_set_style_text_color(lcd->status_bar, lcd->current_theme.text, 0);

    lcd->content = lv_obj_create(lcd->container);
    lv_obj_set_scrollbar_mode(lcd->content, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_radius(lcd->content, 0, 0);
    lv_obj_set_width(lcd->content, LV_HOR_RES);
    lv_obj_set_flex_grow(lcd->content, 1);
    lv_obj_set_style_pad_all(lcd->content, 5, 0);
    lv_obj_set_style_bg_color(lcd->content, lcd->current_theme.chat_background, 0);
    lv_obj_set_style_border_color(lcd->content, lcd->current_theme.border, 0);

    lv_obj_set_flex_flow(lcd->content, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(lcd->content, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_SPACE_EVENLY);
    disp->emotion_label = lv_label_create(lcd->content);
    lv_obj_set_style_text_font(disp->emotion_label, &font_awesome_30_4, 0);
    lv_obj_set_style_text_color(disp->emotion_label, lcd->current_theme.text, 0);
    lv_label_set_text(disp->emotion_label, FONT_AWESOME_AI_CHIP);
    lcd->preview_image = lv_image_create(lcd->content);
    lv_obj_set_size(lcd->preview_image, disp->width * 0.5, disp->height * 0.5);
    lv_obj_align(lcd->preview_image, LV_ALIGN_CENTER, 0, 0);
    lv_obj_add_flag(lcd->preview_image, LV_OBJ_FLAG_HIDDEN);
    disp->chat_message_label = lv_label_create(lcd->content);
    lv_label_set_text(disp->chat_message_label, "");
    lv_obj_set_width(disp->chat_message_label, LV_HOR_RES * 0.9);
    lv_label_set_long_mode(disp->chat_message_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(disp->chat_message_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(disp->chat_message_label, lcd->current_theme.text, 0);

    lv_obj_set_flex_flow(lcd->status_bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_all(lcd->status_bar, 0, 0);
    lv_obj_set_style_border_width(lcd->status_bar, 0, 0);
    lv_obj_set_style_pad_column(lcd->status_bar, 0, 0);
    lv_obj_set_style_pad_left(lcd->status_bar, 2, 0);
    lv_obj_set_style_pad_right(lcd->status_bar, 2, 0);

    disp->network_label = lv_label_create(lcd->status_bar);
    lv_label_set_text(disp->network_label, "");
    lv_obj_set_style_text_font(disp->network_label, lcd->fonts.icon_font, 0);
    lv_obj_set_style_text_color(disp->network_label, lcd->current_theme.text, 0);
    disp->notification_label = lv_label_create(lcd->status_bar);
    lv_obj_set_flex_grow(disp->notification_label, 1);
    lv_obj_set_style_text_align(disp->notification_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(disp->notification_label, lcd->current_theme.text, 0);
    lv_label_set_text(disp->notification_label, "");
    lv_obj_add_flag(disp->notification_label, LV_OBJ_FLAG_HIDDEN);
    disp->notification_close_button = lv_btn_create(lcd->status_bar);
    lv_obj_set_size(disp->notification_close_button, 22, 22);
    lv_obj_set_style_radius(disp->notification_close_button, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(disp->notification_close_button,
                              lv_color_hex(0xE5484D), 0);
    lv_obj_set_style_bg_opa(disp->notification_close_button, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(disp->notification_close_button, 0, 0);
    lv_obj_set_style_pad_all(disp->notification_close_button, 0, 0);
    lv_obj_add_event_cb(disp->notification_close_button,
                        esp_xiaozhi_chat_display_notification_close_cb,
                        LV_EVENT_CLICKED, disp);
    lv_obj_t *notification_close_icon =
        lv_label_create(disp->notification_close_button);
    lv_label_set_text(notification_close_icon, LV_SYMBOL_CLOSE);
    lv_obj_set_style_text_font(notification_close_icon, lcd->fonts.icon_font, 0);
    lv_obj_set_style_text_color(notification_close_icon, lv_color_white(), 0);
    lv_obj_center(notification_close_icon);
    lv_obj_add_flag(disp->notification_close_button, LV_OBJ_FLAG_HIDDEN);
    disp->status_label = lv_label_create(lcd->status_bar);
    lv_obj_set_flex_grow(disp->status_label, 1);
    lv_label_set_long_mode(disp->status_label, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_style_text_align(disp->status_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(disp->status_label, lcd->current_theme.text, 0);
    lv_label_set_text(disp->status_label,
                      app_language_text(LANG_STR_INITIALIZING, "Initializing..."));
    disp->mute_label = lv_label_create(lcd->status_bar);
    lv_label_set_text(disp->mute_label, "");
    lv_obj_set_style_text_font(disp->mute_label, lcd->fonts.icon_font, 0);
    lv_obj_set_style_text_color(disp->mute_label, lcd->current_theme.text, 0);
    disp->battery_label = lv_label_create(lcd->status_bar);
    lv_label_set_text(disp->battery_label, "");
    lv_obj_set_style_text_font(disp->battery_label, lcd->fonts.icon_font, 0);
    lv_obj_set_style_text_color(disp->battery_label, lcd->current_theme.text, 0);
    disp->low_battery_popup = lv_obj_create(screen);
    lv_obj_set_scrollbar_mode(disp->low_battery_popup, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_size(disp->low_battery_popup, LV_HOR_RES * 0.9, lcd->fonts.text_font->line_height * 2);
    lv_obj_align(disp->low_battery_popup, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(disp->low_battery_popup, lcd->current_theme.low_battery, 0);
    lv_obj_set_style_radius(disp->low_battery_popup, 10, 0);
    disp->low_battery_label = lv_label_create(disp->low_battery_popup);
    lv_label_set_text(disp->low_battery_label,
                      app_language_text(LANG_STR_BATTERY_NEED_CHARGE,
                                        "Low battery, please charge"));
    lv_obj_set_style_text_color(disp->low_battery_label, lv_color_white(), 0);
    lv_obj_center(disp->low_battery_label);
    lv_obj_add_flag(disp->low_battery_popup, LV_OBJ_FLAG_HIDDEN);

    /* The display's top layer survives page/screen changes and always renders
     * above page containers. It does not own the backend countdown state. */
    disp->shutdown_popup = lv_obj_create(lv_layer_top());
    lv_obj_set_scrollbar_mode(disp->shutdown_popup, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_size(disp->shutdown_popup, LV_HOR_RES * 0.9,
                    lcd->fonts.text_font->line_height * 4);
    lv_obj_align(disp->shutdown_popup, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(disp->shutdown_popup, lv_color_hex(0xB91C1C), 0);
    lv_obj_set_style_bg_opa(disp->shutdown_popup, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(disp->shutdown_popup, 0, 0);
    lv_obj_set_style_radius(disp->shutdown_popup, 16, 0);
    lv_obj_set_style_pad_all(disp->shutdown_popup, 12, 0);
    disp->shutdown_close_button = lv_btn_create(disp->shutdown_popup);
    lv_obj_set_size(disp->shutdown_close_button, 24, 24);
    lv_obj_align(disp->shutdown_close_button, LV_ALIGN_TOP_RIGHT, -2, -2);
    lv_obj_set_style_radius(disp->shutdown_close_button, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(disp->shutdown_close_button,
                              lv_color_hex(0x7F1D1D), 0);
    lv_obj_set_style_bg_opa(disp->shutdown_close_button, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(disp->shutdown_close_button, 0, 0);
    lv_obj_set_style_pad_all(disp->shutdown_close_button, 0, 0);
    lv_obj_add_event_cb(disp->shutdown_close_button,
                        esp_xiaozhi_chat_display_shutdown_close_cb,
                        LV_EVENT_CLICKED, disp);
    lv_obj_t *shutdown_close_icon =
        lv_label_create(disp->shutdown_close_button);
    lv_label_set_text(shutdown_close_icon, LV_SYMBOL_CLOSE);
    lv_obj_set_style_text_font(shutdown_close_icon, lcd->fonts.icon_font, 0);
    lv_obj_set_style_text_color(shutdown_close_icon, lv_color_white(), 0);
    lv_obj_center(shutdown_close_icon);
    disp->shutdown_countdown_label = lv_label_create(disp->shutdown_popup);
    lv_obj_set_width(disp->shutdown_countdown_label, LV_HOR_RES * 0.8);
    lv_label_set_long_mode(disp->shutdown_countdown_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(disp->shutdown_countdown_label,
                                LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(disp->shutdown_countdown_label,
                                lv_color_white(), 0);
    lv_obj_set_style_text_font(disp->shutdown_countdown_label,
                               lcd->fonts.text_font, 0);
    lv_label_set_text(disp->shutdown_countdown_label,
                      "电量过低\n30秒后关机，请及时连接充电器");
    lv_obj_center(disp->shutdown_countdown_label);
    lv_obj_add_flag(disp->shutdown_popup, LV_OBJ_FLAG_HIDDEN);

    /* Reusable container for PAGE-mode pages (dialogue / camera / ...). Hidden
     * until a page switches the panel away from the animated face. */
    if (s_page_root == NULL) {
        s_page_root = lv_obj_create(lv_screen_active());
        lv_obj_set_size(s_page_root, LV_HOR_RES, LV_VER_RES);
        lv_obj_set_flex_flow(s_page_root, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(s_page_root, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_bg_color(s_page_root, lv_color_black(), 0);
        lv_obj_set_style_border_width(s_page_root, 0, 0);
        lv_obj_set_style_pad_all(s_page_root, 4, 0);
        lv_obj_add_flag(s_page_root, LV_OBJ_FLAG_HIDDEN);
    }
    esp_xiaozhi_chat_display_unlock(disp);

    return ESP_OK;
}

static esp_err_t esp_xiaozhi_chat_display_destroy(display_t *disp)
{
    ESP_RETURN_ON_FALSE(disp, ESP_ERR_INVALID_STATE, TAG, "Display not initialized");

    if (disp->notification_timer != NULL) {
        esp_timer_stop(disp->notification_timer);
        esp_timer_delete(disp->notification_timer);
        disp->notification_timer = NULL;
    }
    if (disp->kind == DISPLAY_KIND_LCD) {
        lcd_display_t *lcd = esp_xiaozhi_chat_display_from_base(disp);
        if (lcd->content != NULL) {
            lv_obj_del(lcd->content);
            lcd->content = NULL;
        }
        if (lcd->status_bar != NULL) {
            lv_obj_del(lcd->status_bar);
            lcd->status_bar = NULL;
        }
        if (lcd->side_bar != NULL) {
            lv_obj_del(lcd->side_bar);
            lcd->side_bar = NULL;
        }
        if (lcd->container != NULL) {
            lv_obj_del(lcd->container);
            lcd->container = NULL;
        }
        if (disp->low_battery_popup != NULL) {
            lv_obj_del(disp->low_battery_popup);
            disp->low_battery_popup = NULL;
        }
        if (disp->shutdown_popup != NULL) {
            lv_obj_del(disp->shutdown_popup);
            disp->shutdown_popup = NULL;
            disp->shutdown_countdown_label = NULL;
            disp->shutdown_close_button = NULL;
        }
        if (s_page_root != NULL) {
            lv_obj_del(s_page_root);
            s_page_root = NULL;
        }
        if (disp->lv_disp != NULL) {
            lv_display_delete(disp->lv_disp);
            disp->lv_disp = NULL;
        }
        if (lcd->panel != NULL) {
            esp_lcd_panel_del(lcd->panel);
            lcd->panel = NULL;
        }
        if (lcd->panel_io != NULL) {
            esp_lcd_panel_io_del(lcd->panel_io);
            lcd->panel_io = NULL;
        }
        if (s_panel_lcd == lcd) {
            s_panel_lcd = NULL;
        }
        free(esp_xiaozhi_chat_display_from_base(disp));
    } else {
        if (disp->network_label != NULL) {
            lv_obj_del(disp->network_label);
            lv_obj_del(disp->notification_label);
            lv_obj_del(disp->notification_close_button);
            lv_obj_del(disp->status_label);
            lv_obj_del(disp->mute_label);
            lv_obj_del(disp->battery_label);
            lv_obj_del(disp->emotion_label);
            disp->network_label = NULL;
        }
        if (disp->low_battery_popup != NULL) {
            lv_obj_del(disp->low_battery_popup);
            disp->low_battery_popup = NULL;
        }
        if (disp->shutdown_popup != NULL) {
            lv_obj_del(disp->shutdown_popup);
            disp->shutdown_popup = NULL;
            disp->shutdown_countdown_label = NULL;
            disp->shutdown_close_button = NULL;
        }
        if (disp->pm_lock != NULL) {
            esp_pm_lock_delete(disp->pm_lock);
            disp->pm_lock = NULL;
        }
        free(disp);
    }

    return ESP_OK;
}

/* Keep the port's rotation/byte-order conversion, but serialize the complete
 * prepare + submit + DMA-fence sequence with the other panel renderers. */
static void lcd_display_flush_cb(lv_display_t *disp, const lv_area_t *area,
                                  uint8_t *pixels)
{
    lcd_display_t *lcd = s_panel_lcd;
    if (lcd == NULL || lcd->base.lv_disp != disp || lcd->port_flush_cb == NULL) {
        lv_display_flush_ready(disp);
        return;
    }
    if (panel_begin_flush(PANEL_FLUSH_OWNER_LVGL) != ESP_OK) {
        /* No pixels were submitted. Inactive LVGL pages must not draw over
         * the face/video renderer or wait for a nonexistent DMA. */
        lv_display_flush_ready(disp);
        return;
    }

    lcd->port_flush_cb(disp, area, pixels);
    const esp_err_t ret = panel_end_flush(PANEL_FLUSH_OWNER_LVGL, ESP_OK);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "LVGL: failed to finish LCD transfer: %s", esp_err_to_name(ret));
    }
}

/* A timeout is only a reason to reconcile a missed completion after a physical
 * DMA fence. Returning while DMA is live lets LVGL reuse the transmit buffer. */
#define LVGL_FLUSH_WAIT_TIMEOUT_MS 100

static void lvgl_port_flush_wait_cb(lv_display_t *disp)
{
    if (disp == NULL) {
        return;
    }

    TickType_t start = xTaskGetTickCount();
    const TickType_t timeout = pdMS_TO_TICKS(LVGL_FLUSH_WAIT_TIMEOUT_MS);
    while (disp->flushing != 0) {
        if ((xTaskGetTickCount() - start) >= timeout) {
            const esp_err_t ret = panel_wait_for_flush_idle();
            if (ret == ESP_OK) {
                lv_display_flush_ready(disp);
            } else {
                ESP_LOGW(TAG, "LCD flush still pending; DMA fence failed: %s",
                         esp_err_to_name(ret));
            }
            start = xTaskGetTickCount();
        }
        vTaskDelay(1);
    }
}

static void lcd_display_perf_event_cb(lv_event_t *event)
{
    lcd_display_t *lcd = (lcd_display_t *)lv_event_get_user_data(event);
    if (lcd == NULL) {
        return;
    }

    const lv_event_code_t code = lv_event_get_code(event);
    const int64_t now_us = esp_timer_get_time();
    if (lcd->perf.window_start_us == 0) {
        lcd->perf.window_start_us = now_us;
    }

    switch (code) {
    case LV_EVENT_FLUSH_START: {
        const lv_area_t *area = (const lv_area_t *)lv_event_get_param(event);
        lcd->perf.flush_count++;
        if (area != NULL) {
            lcd->perf.pixels += (uint64_t)lv_area_get_width(area) * (uint64_t)lv_area_get_height(area);
        }
        break;
    }
    case LV_EVENT_FLUSH_WAIT_START:
        lcd->perf.wait_start_us = now_us;
        break;
    case LV_EVENT_FLUSH_WAIT_FINISH:
        if (lcd->perf.wait_start_us != 0) {
            lcd->perf.wait_us += (uint64_t)(now_us - lcd->perf.wait_start_us);
            lcd->perf.wait_start_us = 0;
        }
        break;
    case LV_EVENT_REFR_READY:
        lcd->perf.refresh_count++;
        break;
    default:
        break;
    }

    if (code == LV_EVENT_REFR_READY && now_us - lcd->perf.window_start_us >= 1000000) {
        const uint64_t elapsed_us = (uint64_t)(now_us - lcd->perf.window_start_us);
        const uint32_t fps_x100 = (uint32_t)((uint64_t)lcd->perf.refresh_count * 100000000ULL / elapsed_us);
        const uint64_t pixels_per_sec = lcd->perf.pixels * 1000000ULL / elapsed_us;
        ESP_LOGI(TAG,
                 "LCD perf: fps=%" PRIu32 ".%02" PRIu32 " flush=%" PRIu32
                 " pixels/s=%" PRIu64 " dma_wait=%" PRIu64 " ms",
                 fps_x100 / 100, fps_x100 % 100, lcd->perf.flush_count,
                 pixels_per_sec, lcd->perf.wait_us / 1000ULL);
        memset(&lcd->perf, 0, sizeof(lcd->perf));
        lcd->perf.window_start_us = now_us;
    }
}

static display_t *esp_xiaozhi_chat_display_create(esp_lcd_panel_io_handle_t panel_io,
                                                  esp_lcd_panel_handle_t panel,
                                                  int width, int height,
                                                  int offset_x, int offset_y,
                                                  bool mirror_x, bool mirror_y, bool swap_xy,
                                                  display_fonts_t fonts)
{
    lcd_display_t *lcd = (lcd_display_t *)calloc(1, sizeof(lcd_display_t));
    if (lcd == NULL) {
        ESP_LOGE(TAG, "Failed to allocate lcd display");
        return NULL;
    }
    if (s_panel_tx_mutex == NULL) {
        s_panel_tx_mutex = xSemaphoreCreateMutex();
        if (s_panel_tx_mutex == NULL) {
            ESP_LOGE(TAG, "Failed to allocate LCD TX mutex");
            free(lcd);
            return NULL;
        }
    }
    lcd->base.width = width;
    lcd->base.height = height;
    lcd->base.kind = DISPLAY_KIND_LCD;
    lcd->panel_io = panel_io;
    lcd->panel = panel;
    lcd->fonts = fonts;
    lcd->current_theme = esp_xiaozhi_chat_display_light_theme();
    esp_xiaozhi_chat_display_state_init(&lcd->base);

    uint16_t *buffer = (uint16_t *)heap_caps_malloc((size_t)width * sizeof(uint16_t), MALLOC_CAP_8BIT);
    if (buffer != NULL) {
        for (int i = 0; i < width; ++i) {
            buffer[i] = 0x0000;
        }
        for (int y = 0; y < height; ++y) {
            esp_lcd_panel_draw_bitmap(panel, 0, y, width, y + 1, buffer);
        }
    }
    /* The last clear row may still be read by DMA until this command drains
     * queued transfers. Do not free its source buffer before the fence. */
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel, true));
    heap_caps_free(buffer);

    if (!s_lvgl_inited) {
        lv_init();
        s_lvgl_inited = true;
    }
    if (!s_lvgl_port_inited) {
        lvgl_port_cfg_t port_cfg = ESP_LVGL_PORT_INIT_CONFIG();
        port_cfg.task_priority = 3;         /* keep UI responsive (emote=5) */
        port_cfg.timer_period_ms = 10;      /* 100Hz LVGL tick for smooth input and animation */
        /* Pin LVGL to CPU1: the emote face task runs on CPU0 at prio 5 and the
         * WiFi task on CPU0 at prio 23. With task_affinity=-1 FreeRTOS could
         * place LVGL on CPU0 too, where the boot burst (WiFi connect + emote
         * animation + camera + audio) starved BOTH LVGL and IDLE0 past the WDT
         * timeout -> task_wdt reboot right after WiFi connected. Splitting the
         * renderer onto CPU1 keeps the UI responsive during boot. */
        port_cfg.task_affinity = 1;
        /* The LVGL task stack MUST stay in internal RAM: widget event callbacks
         * (button clicks such as "enable Xiaozhi", settings toggles) run inside
         * lv_timer_handler on this task and write NVS, which enters a
         * cache-disabled section. A PSRAM stack there trips
         * esp_task_stack_is_sane_cache_disabled() in spi_flash. */
        ESP_ERROR_CHECK(lvgl_port_init(&port_cfg));
        s_lvgl_port_inited = true;
    }

    /* Full-frame, double-buffered, PSRAM-backed LVGL. A complete frame is
     * submitted as one PARLIO transaction so the ST7796 GRAM is not updated
     * in visible strips while the panel is scanning it.
     * buff_spiram = 1: the ST77916 QSPI driver needs a ~14 KB "private TX
     * buffer" per draw_bitmap; when the LVGL buffer also lived in internal DMA
     * SRAM the two competed and the SPI bounce buffer allocation failed
     * ("Failed to allocate priv TX buffer"), freezing the screen. Moving the
     * LVGL buffer to PSRAM frees internal DMA SRAM for the SPI TX buffer. */
    const lvgl_port_display_cfg_t display_cfg = {
        .io_handle = panel_io,
        .panel_handle = panel,
        .control_handle = NULL,
        .buffer_size = (uint32_t)(width * height),
        .double_buffer = true,
        .trans_size = 0,
        .hres = (uint32_t)width,
        .vres = (uint32_t)height,
        .monochrome = false,
        .rotation = { .swap_xy = swap_xy, .mirror_x = mirror_x, .mirror_y = mirror_y },
        .color_format = LV_COLOR_FORMAT_RGB565,
        .flags = { .buff_dma = 1, .buff_spiram = 1, .sw_rotate = TBAO_LCD_SW_ROTATE, .swap_bytes = 1, .full_refresh = 1, .direct_mode = 0 },
    };
    /* Install the shared dispatcher and wrapper before the LVGL task is
     * allowed to submit its first frame. The port lock is recursive. */
    if (!lvgl_port_lock(30000)) {
        ESP_LOGE(TAG, "Failed to lock LVGL while adding LCD display");
        esp_xiaozhi_chat_display_destroy(&lcd->base);
        return NULL;
    }
    lcd->base.lv_disp = lvgl_port_add_disp(&display_cfg);
    if (lcd->base.lv_disp == NULL) {
        ESP_LOGE(TAG, "Failed to add display");
        esp_xiaozhi_chat_display_destroy(&lcd->base);
        lvgl_port_unlock();
        return NULL;
    }
    s_panel_lcd = lcd;
    lcd->port_flush_cb = lcd->base.lv_disp->flush_cb;
    lv_display_set_flush_cb(lcd->base.lv_disp, lcd_display_flush_cb);
    const esp_lcd_panel_io_callbacks_t panel_callbacks = {
        .on_color_trans_done = emote_on_flush_done,
    };
    const esp_err_t callback_ret = esp_lcd_panel_io_register_event_callbacks(
        panel_io, &panel_callbacks, lcd);
    if (callback_ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register shared LCD completion callback: %s",
                 esp_err_to_name(callback_ret));
        esp_xiaozhi_chat_display_destroy(&lcd->base);
        lvgl_port_unlock();
        return NULL;
    }
#if CONFIG_ESP_BOARD_TBAO_S31_BOARD
    lv_display_set_rotation(lcd->base.lv_disp, LV_DISPLAY_ROTATION_270);
    ESP_LOGI(TAG, "T-Bao S31 LCD rotation: software clockwise 270 degrees");
#endif
    if (offset_x != 0 || offset_y != 0) {
        lv_display_set_offset(lcd->base.lv_disp, offset_x, offset_y);
    }
    /* A missed ISR completion can only be reconciled after draining DMA. */
    lv_display_set_flush_wait_cb(lcd->base.lv_disp, lvgl_port_flush_wait_cb);
    lv_display_add_event_cb(lcd->base.lv_disp, lcd_display_perf_event_cb,
                            LV_EVENT_FLUSH_START, lcd);
    lv_display_add_event_cb(lcd->base.lv_disp, lcd_display_perf_event_cb,
                            LV_EVENT_FLUSH_WAIT_START, lcd);
    lv_display_add_event_cb(lcd->base.lv_disp, lcd_display_perf_event_cb,
                            LV_EVENT_FLUSH_WAIT_FINISH, lcd);
    lv_display_add_event_cb(lcd->base.lv_disp, lcd_display_perf_event_cb,
                            LV_EVENT_REFR_READY, lcd);
    esp_xiaozhi_chat_display_setup_ui(lcd);

    /* Register a POINTER input device that reads the raw touch state from
     * ui_touch (the gesture layer), so LVGL widgets (buttons, sliders) react
     * to taps. Only ONE consumer reads the CST816S controller (ui_touch);
     * this indev just mirrors the latest position to LVGL. */
    static lv_indev_t *s_indev = NULL;
    if (s_indev == NULL && lcd->base.lv_disp != NULL) {
        s_indev = lv_indev_create();
        lv_indev_set_type(s_indev, LV_INDEV_TYPE_POINTER);
        lv_indev_set_read_cb(s_indev, lvgl_port_touch_mirror_read);
        lv_indev_set_disp(s_indev, lcd->base.lv_disp);
        ESP_LOGI(TAG, "LVGL pointer indev registered (mirrors ui_touch)");
    }

    lvgl_port_unlock();
    return &lcd->base;
}

/* LVGL pointer input read callback: mirrors ui_touch's pre-rotation state. */
void lvgl_port_touch_mirror_read(lv_indev_t *indev, lv_indev_data_t *data)
{
    (void)indev;
    int x = -1, y = -1;
    bool pressed = false;
    ui_touch_get_pos(&x, &y, &pressed);
    /* LVGL sends one final RELEASED event through the slider. Keep the last
     * valid point for that event; using (0, 0) makes every drag snap to zero. */
    data->point.x = x >= 0 ? (lv_coord_t)x : 0;
    data->point.y = y >= 0 ? (lv_coord_t)y : 0;
    data->state = pressed && x >= 0 && y >= 0
                      ? LV_INDEV_STATE_PRESSED
                      : LV_INDEV_STATE_RELEASED;
}

static esp_err_t esp_xiaozhi_chat_display_required(void)
{
    return s_display != NULL ? ESP_OK : ESP_ERR_INVALID_STATE;
}

esp_err_t esp_xiaozhi_chat_display_init(void)
{
    esp_err_t ret = ESP_OK;

    if (s_emote_api_mutex == NULL) {
        s_emote_api_mutex = xSemaphoreCreateMutex();
        if (s_emote_api_mutex == NULL) {
            ESP_LOGE(TAG, "Failed to create emote API mutex");
            return ESP_ERR_NO_MEM;
        }
    }

    void *lcd_handle = NULL;
    ret = esp_board_manager_get_device_handle("display_lcd", &lcd_handle);
    if (ret != ESP_OK || lcd_handle == NULL) {
        ESP_LOGW(TAG, "display_lcd handle not available: %s", esp_err_to_name(ret));
        esp_board_manager_adapter_set_lcd_backlight(100);
        return ESP_OK;
    }
    dev_display_lcd_handles_t *lcd_handles = (dev_display_lcd_handles_t *)lcd_handle;
    if (lcd_handles->io_handle == NULL || lcd_handles->panel_handle == NULL) {
        ESP_LOGW(TAG, "LCD io/panel handles NULL");
        esp_board_manager_adapter_set_lcd_backlight(100);
        return ESP_OK;
    }
    dev_display_lcd_config_t *lcd_cfg = NULL;
    ret = esp_board_manager_get_device_config("display_lcd", (void **)&lcd_cfg);
    if (ret != ESP_OK || lcd_cfg == NULL) {
        s_display = esp_xiaozhi_chat_display_create(
                        lcd_handles->io_handle, lcd_handles->panel_handle,
                        320, 240, 0, 0, 1, 1, 0,
        (display_fonts_t) {
            .text_font = &font_puhui_20_4,
            .icon_font = &font_awesome_20_4,
            .emoji_font = font_emoji_64_init(),
        });
    } else {
        s_display = esp_xiaozhi_chat_display_create(
                        lcd_handles->io_handle, lcd_handles->panel_handle,
                        (int)lcd_cfg->lcd_width, (int)lcd_cfg->lcd_height, 0, 0,
                        (bool)lcd_cfg->mirror_x, (bool)lcd_cfg->mirror_y, (bool)lcd_cfg->swap_xy,
        (display_fonts_t) {
            .text_font = &font_puhui_20_4,
            .icon_font = &font_awesome_20_4,
            .emoji_font = font_emoji_64_init(),
        });
    }
    if (s_display != NULL) {
        ESP_LOGI(TAG, "LCD display created");
    }
    esp_board_manager_adapter_set_lcd_backlight(100);
    return ESP_OK;
}

esp_err_t esp_xiaozhi_chat_display_get_width(int *width)
{
    if (width == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    *width = s_display ? s_display->width : 0;
    return ESP_OK;
}

esp_err_t esp_xiaozhi_chat_display_get_height(int *height)
{
    if (height == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    *height = s_display ? s_display->height : 0;
    return ESP_OK;
}

esp_err_t esp_xiaozhi_chat_display_set_status(const char *status)
{
    ESP_RETURN_ON_FALSE(status, ESP_ERR_INVALID_ARG, TAG, "Invalid status");
    ESP_RETURN_ON_ERROR(esp_xiaozhi_chat_display_required(), TAG, "Display not initialized");

    if (s_emote_active) {
        emote_set_status_event(status);
        return ESP_OK;
    }

    if (!esp_xiaozhi_chat_display_lock(s_display, 30000)) {
        return ESP_OK;
    }
    if (s_display->status_label != NULL) {
        lv_label_set_text(s_display->status_label, status);
        lv_obj_clear_flag(s_display->status_label, LV_OBJ_FLAG_HIDDEN);
    }
    if (s_display->notification_label != NULL) {
        lv_obj_add_flag(s_display->notification_label, LV_OBJ_FLAG_HIDDEN);
    }
    if (s_display->notification_close_button != NULL) {
        lv_obj_add_flag(s_display->notification_close_button, LV_OBJ_FLAG_HIDDEN);
    }
    esp_xiaozhi_chat_display_unlock(s_display);

    /* Forward status to the active page (e.g. dialogue page shows it). The face
     * page does not consume status (it drives emotion directly), so no loop. */
    ui_page_manager_notify_status(status);
    return ESP_OK;
}

esp_err_t esp_xiaozhi_chat_display_clear_status(void)
{
    ESP_RETURN_ON_ERROR(esp_xiaozhi_chat_display_required(), TAG, "Display not initialized");

    if (s_emote_active) {
        /* The emote manager's OFF event hides the status icon and toast label. */
        emote_set_status_event("Ready");
        return ESP_OK;
    }

    if (!esp_xiaozhi_chat_display_lock(s_display, 30000)) {
        return ESP_OK;
    }
    if (s_display->notification_label != NULL) {
        lv_label_set_text(s_display->notification_label, "");
        lv_obj_add_flag(s_display->notification_label, LV_OBJ_FLAG_HIDDEN);
    }
    if (s_display->notification_close_button != NULL) {
        lv_obj_add_flag(s_display->notification_close_button, LV_OBJ_FLAG_HIDDEN);
    }
    if (s_display->status_label != NULL) {
        lv_label_set_text(s_display->status_label, "");
        lv_obj_add_flag(s_display->status_label, LV_OBJ_FLAG_HIDDEN);
    }
    esp_xiaozhi_chat_display_unlock(s_display);

    if (s_display->notification_timer != NULL) {
        esp_timer_stop(s_display->notification_timer);
    }
    return ESP_OK;
}

esp_err_t esp_xiaozhi_chat_display_set_subtitle(const char *subtitle)
{
    ESP_RETURN_ON_FALSE(subtitle, ESP_ERR_INVALID_ARG, TAG, "Invalid subtitle");
    ESP_RETURN_ON_ERROR(esp_xiaozhi_chat_display_required(), TAG, "Display not initialized");

    if (s_emote_active) {
        emote_set_status_event(subtitle);
        return ESP_OK;
    }

    if (!esp_xiaozhi_chat_display_lock(s_display, 30000)) {
        return ESP_OK;
    }
    if (s_display->status_label != NULL) {
        lv_label_set_text(s_display->status_label, subtitle);
        lv_obj_clear_flag(s_display->status_label, LV_OBJ_FLAG_HIDDEN);
    }
    if (s_display->notification_label != NULL) {
        lv_obj_add_flag(s_display->notification_label, LV_OBJ_FLAG_HIDDEN);
    }
    if (s_display->notification_close_button != NULL) {
        lv_obj_add_flag(s_display->notification_close_button, LV_OBJ_FLAG_HIDDEN);
    }
    esp_xiaozhi_chat_display_unlock(s_display);

    return ESP_OK;
}

esp_err_t esp_xiaozhi_chat_display_set_volume(int volume)
{
    ESP_RETURN_ON_FALSE(volume >= 0 && volume <= 100, ESP_ERR_INVALID_ARG, TAG, "Invalid volume");
    ESP_RETURN_ON_ERROR(esp_xiaozhi_chat_display_required(), TAG, "Display not initialized");

    esp_board_manager_adapter_set_lcd_backlight(volume);

    return ESP_OK;
}

static const char *battery_icon_for_percent(int percent)
{
    if (percent >= 90) {
        return LV_SYMBOL_BATTERY_FULL;
    }
    if (percent >= 70) {
        return LV_SYMBOL_BATTERY_3;
    }
    if (percent >= 45) {
        return LV_SYMBOL_BATTERY_2;
    }
    if (percent >= 20) {
        return LV_SYMBOL_BATTERY_1;
    }
    return LV_SYMBOL_BATTERY_EMPTY;
}

esp_err_t esp_xiaozhi_chat_display_set_battery(int percent,
                                               bool charging,
                                               bool valid)
{
    ESP_RETURN_ON_ERROR(esp_xiaozhi_chat_display_required(), TAG,
                        "Display not initialized");
    if (valid && (percent < 0 || percent > 100)) {
        return ESP_ERR_INVALID_ARG;
    }

    s_display->battery_percent = valid ? percent : -1;
    s_display->battery_valid = valid;
    s_display->battery_charging = charging;

    /* The emote renderer owns the panel in FACE mode and LVGL is stopped, so
     * only send the emote event in that mode. When a PAGE owns the panel, keep
     * the LVGL status bar up to date instead. */
    if (s_emote_active && s_face_visible) {
        if (!valid || percent < 0) {
            /* Do not manufacture a 0% reading for an invalid ADC sample. The
             * next valid sample will refresh the emote battery widget. */
            return ESP_OK;
        }
        char message[24];
        snprintf(message, sizeof(message), "%d,%d", charging ? 1 : 0,
                 percent);
        if (!emote_api_lock("battery")) {
            return ESP_ERR_TIMEOUT;
        }
        esp_err_t ret = emote_set_event_msg(s_emote, EMOTE_MGR_EVT_BAT, message);
        emote_api_unlock();
        return ret;
    }

    /* Battery refresh shares a worker with the critical shutdown popup. Do
     * not spend the entire countdown waiting to update an ordinary icon. */
    if (!esp_xiaozhi_chat_display_lock(s_display, 100)) {
        return ESP_ERR_TIMEOUT;
    }

    const char *icon = valid ? battery_icon_for_percent(percent) : "";
    if (s_display->kind == DISPLAY_KIND_LCD) {
        lcd_display_t *lcd = esp_xiaozhi_chat_display_from_base(s_display);
        if (s_display->battery_label != NULL) {
            if (valid && charging) {
                lv_label_set_text_fmt(s_display->battery_label, "%s%s", icon,
                                      LV_SYMBOL_CHARGE);
            } else {
                lv_label_set_text(s_display->battery_label, icon);
            }
        }

        if (s_display->low_battery_popup != NULL) {
            if (s_display->low_battery_label != NULL) {
                lv_label_set_text(
                    s_display->low_battery_label,
                    app_language_text(LANG_STR_BATTERY_NEED_CHARGE,
                                      "Low battery, please charge"));
            }
            if (!valid) {
                s_display->low_battery_warning_visible = false;
                lv_obj_add_flag(s_display->low_battery_popup, LV_OBJ_FLAG_HIDDEN);
            } else if (!s_display->low_battery_warning_visible && percent <= 15) {
                s_display->low_battery_warning_visible = true;
                lv_obj_clear_flag(s_display->low_battery_popup, LV_OBJ_FLAG_HIDDEN);
            } else if (s_display->low_battery_warning_visible && percent >= 20) {
                s_display->low_battery_warning_visible = false;
                lv_obj_add_flag(s_display->low_battery_popup, LV_OBJ_FLAG_HIDDEN);
            }
        }
        (void)lcd;
    }
    esp_xiaozhi_chat_display_unlock(s_display);
    return ESP_OK;
}

esp_err_t esp_xiaozhi_chat_display_set_brightness(int brightness)
{
    ESP_RETURN_ON_FALSE(brightness >= 0 && brightness <= 100, ESP_ERR_INVALID_ARG, TAG, "Invalid brightness");
    ESP_RETURN_ON_ERROR(esp_xiaozhi_chat_display_required(), TAG, "Display not initialized");

    esp_board_manager_adapter_set_lcd_backlight(brightness);

    return ESP_OK;
}

esp_err_t esp_xiaozhi_chat_display_set_notification(const char *notification, int duration_ms)
{
    ESP_RETURN_ON_FALSE(notification, ESP_ERR_INVALID_ARG, TAG, "Invalid notification");
    ESP_RETURN_ON_ERROR(esp_xiaozhi_chat_display_required(), TAG, "Display not initialized");

    if (s_emote_active && s_face_visible) {
        if (!emote_api_lock("notification")) {
            return ESP_ERR_TIMEOUT;
        }
        emote_set_event_msg(s_emote, EMOTE_MGR_EVT_SYS, notification);
        emote_api_unlock();
        return ESP_OK;
    }

    if (!esp_xiaozhi_chat_display_lock(s_display, 30000)) {
        return ESP_OK;
    }
    if (s_display->notification_label != NULL) {
        lv_label_set_text(s_display->notification_label, notification);
        lv_obj_clear_flag(s_display->notification_label, LV_OBJ_FLAG_HIDDEN);
    }
    if (s_display->notification_close_button != NULL) {
        lv_obj_clear_flag(s_display->notification_close_button, LV_OBJ_FLAG_HIDDEN);
    }
    if (s_display->status_label != NULL) {
        lv_obj_add_flag(s_display->status_label, LV_OBJ_FLAG_HIDDEN);
    }
    esp_xiaozhi_chat_display_unlock(s_display);
    if (s_display->notification_timer != NULL) {
        esp_timer_stop(s_display->notification_timer);
        esp_timer_start_once(s_display->notification_timer, (uint64_t)duration_ms * 1000ULL);
    }
    return ESP_OK;
}

esp_err_t esp_xiaozhi_chat_display_show_shutdown_countdown(int seconds)
{
    ESP_RETURN_ON_FALSE(seconds >= 0, ESP_ERR_INVALID_ARG, TAG,
                        "Invalid shutdown countdown");
    ESP_RETURN_ON_ERROR(esp_xiaozhi_chat_display_required(), TAG,
                        "Display not initialized");

    char message[96];
    if (app_language_is_english()) {
        snprintf(message, sizeof(message),
                 "Battery voltage too low\nShutdown in %d seconds;\n"
                 "connect the charger",
                 seconds);
    } else {
        snprintf(message, sizeof(message),
                 "电量过低\n%d秒后关机，请及时连接充电器", seconds);
    }

    if (!esp_xiaozhi_chat_display_lock(s_display, 100)) {
        return ESP_ERR_TIMEOUT;
    }
    const bool face_visible = s_emote_active && s_face_visible;
    if (!s_display->shutdown_countdown_active) {
        /* Start a new UI session only on an inactive -> active transition.
         * Re-rendering 30 seconds or switching pages must not undo dismissal. */
        s_display->shutdown_countdown_active = true;
        s_display->shutdown_countdown_dismissed = false;
    }
    const bool dismissed = s_display->shutdown_countdown_dismissed;
    if (s_display->shutdown_popup != NULL) {
        if (s_display->shutdown_countdown_label != NULL) {
            lv_label_set_text(s_display->shutdown_countdown_label, message);
        }
        if (!dismissed && !face_visible) {
            lv_obj_clear_flag(s_display->shutdown_popup, LV_OBJ_FLAG_HIDDEN);
            lv_obj_t *parent = lv_obj_get_parent(s_display->shutdown_popup);
            if (parent != NULL) {
                lv_obj_move_to_index(s_display->shutdown_popup,
                                     (int32_t)lv_obj_get_child_count(parent) - 1);
            }
        } else {
            lv_obj_add_flag(s_display->shutdown_popup, LV_OBJ_FLAG_HIDDEN);
        }
    }
    esp_xiaozhi_chat_display_unlock(s_display);
    if (face_visible && !dismissed) {
        if (!emote_api_lock("shutdown countdown")) {
            return ESP_ERR_TIMEOUT;
        }
        esp_err_t ret = emote_set_event_msg(s_emote, EMOTE_MGR_EVT_SYS, message);
        emote_api_unlock();
        return ret;
    }
    return ESP_OK;
}

esp_err_t esp_xiaozhi_chat_display_hide_shutdown_countdown(void)
{
    ESP_RETURN_ON_ERROR(esp_xiaozhi_chat_display_required(), TAG,
                        "Display not initialized");

    if (!esp_xiaozhi_chat_display_lock(s_display, 100)) {
        return ESP_ERR_TIMEOUT;
    }
    const bool was_active = s_display->shutdown_countdown_active;
    s_display->shutdown_countdown_active = false;
    s_display->shutdown_countdown_dismissed = false;
    if (s_display->shutdown_popup != NULL) {
        lv_obj_add_flag(s_display->shutdown_popup, LV_OBJ_FLAG_HIDDEN);
    }
    esp_xiaozhi_chat_display_unlock(s_display);
    if (was_active && s_emote_active && s_face_visible) {
        emote_set_status_event("Ready");
    }
    return ESP_OK;
}

esp_err_t esp_xiaozhi_chat_display_set_emotion(const char *emotion)
{
    ESP_RETURN_ON_FALSE(emotion, ESP_ERR_INVALID_ARG, TAG, "Invalid emotion");
    ESP_RETURN_ON_ERROR(esp_xiaozhi_chat_display_required(), TAG, "Display not initialized");

    const size_t emotion_index = emote_index_for(emotion);

    if (s_emote_active) {
        if (!emote_api_lock("emotion")) {
            return ESP_ERR_TIMEOUT;
        }
        esp_err_t ret = emote_set_anim_emoji(s_emote, emote_anim_for(emotion));
        emote_api_unlock();
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "Failed to set emotion '%s': %s", emotion, esp_err_to_name(ret));
        } else {
            emote_record_current_index(emotion_index);
            s_last_eye_anim = emote_anim_for(emotion);
            /* set_anim_emoji starts playback; keep it stopped while a PAGE owns
             * the panel.  The animation name is kept, so showing the face
             * resumes this exact animation. */
            if (!s_face_visible) {
                emote_set_eye_anim_running(false);
            }
        }
        return ret;
    }
    /* LVGL fallback (no emote): show the emotion icon on the status label. */
    if (!esp_xiaozhi_chat_display_lock(s_display, 30000) || s_display->emotion_label == NULL) {
        return ESP_OK;
    }
    const char *icon = s_emotion_table[0].fa_icon;
    for (size_t i = 0; i < EMOTION_COUNT; ++i) {
        if (strcmp(s_emotion_table[i].text, emotion) == 0) {
            icon = s_emotion_table[i].fa_icon;
            break;
        }
    }
    lv_label_set_text(s_display->emotion_label, icon);
    esp_xiaozhi_chat_display_unlock(s_display);

    emote_record_current_index(emotion_index);
    return ESP_OK;
}

bool esp_xiaozhi_chat_display_emote_is_idle(void)
{
    bool idle;
    portENTER_CRITICAL(&s_emote_state_lock);
    idle = s_emote_active && s_emote_idle;
    portEXIT_CRITICAL(&s_emote_state_lock);
    return idle;
}

esp_err_t esp_xiaozhi_chat_display_set_random_emotion(void)
{
    ESP_RETURN_ON_ERROR(esp_xiaozhi_chat_display_required(), TAG, "Display not initialized");
    if (EMOTE_MAP_COUNT < 2) {
        return ESP_ERR_INVALID_STATE;
    }

    int current_index;
    portENTER_CRITICAL(&s_emote_state_lock);
    current_index = s_emote_current_index;
    portEXIT_CRITICAL(&s_emote_state_lock);
    if (current_index < 0 || current_index >= (int)EMOTE_MAP_COUNT) {
        current_index = 0;
    }

    /* Pick from all entries except the current one, so every interaction is a
     * visible change. Timed idle rotation is gated by the face page instead. */
    const size_t offset = esp_random() % (EMOTE_MAP_COUNT - 1);
    const size_t next_index = offset >= (size_t)current_index ? offset + 1 : offset;
    return esp_xiaozhi_chat_display_set_emotion(s_emote_map[next_index].emotion);
}

/* Face-page gestures use the closest available dedicated animations. */
void esp_xiaozhi_chat_display_pet(void)
{
    if (s_emote_active && s_emote != NULL) {
        const char *anim = emote_anim_for("winking");
        if (!emote_api_lock("pet")) {
            return;
        }
        if (emote_set_anim_emoji(s_emote, anim) == ESP_OK) {
            emote_record_current_index(emote_index_for("winking"));
            s_last_eye_anim = anim;
        }
        emote_insert_anim_dialog(s_emote, anim, 2500);
        emote_api_unlock();
    }
}

void esp_xiaozhi_chat_display_laugh(void)
{
    if (s_emote_active && s_emote != NULL) {
        const char *anim = emote_anim_for("happy");
        if (!emote_api_lock("laugh")) {
            return;
        }
        if (emote_set_anim_emoji(s_emote, anim) == ESP_OK) {
            emote_record_current_index(emote_index_for("happy"));
            s_last_eye_anim = anim;
        }
        emote_insert_anim_dialog(s_emote, anim, 2500);
        emote_api_unlock();
    }
}

esp_err_t esp_xiaozhi_chat_display_set_chat_message(const char *role, const char *content)
{
    ESP_RETURN_ON_FALSE(role, ESP_ERR_INVALID_ARG, TAG, "Invalid role");
    ESP_RETURN_ON_FALSE(content, ESP_ERR_INVALID_ARG, TAG, "Invalid content");
    ESP_RETURN_ON_ERROR(esp_xiaozhi_chat_display_required(), TAG, "Display not initialized");

    if (s_emote_active) {
        if (!emote_api_lock("chat message")) {
            return ESP_ERR_TIMEOUT;
        }
        if (strcmp(role, "user") == 0) {
            emote_set_event_msg(s_emote, EMOTE_MGR_EVT_SYS, content);
        } else {
            emote_set_event_msg(s_emote, EMOTE_MGR_EVT_SPEAK, content);
        }
        emote_api_unlock();
        return ESP_OK;
    }

    if (strlen(content) == 0) {
        return ESP_OK;
    }
    if (s_display->kind == DISPLAY_KIND_LCD) {
        lcd_display_t *lcd = esp_xiaozhi_chat_display_from_base(s_display);
        if (!esp_xiaozhi_chat_display_lock(s_display, 30000) || lcd->content == NULL) {
            return ESP_OK;
        }
        if (s_display->chat_message_label != NULL) {
            lv_label_set_text(s_display->chat_message_label, content);
        }
        esp_xiaozhi_chat_display_unlock(s_display);
        return ESP_OK;
    }
    if (!esp_xiaozhi_chat_display_lock(s_display, 30000) || s_display->chat_message_label == NULL) {
        return ESP_OK;
    }
    lv_label_set_text(s_display->chat_message_label, content);
    esp_xiaozhi_chat_display_unlock(s_display);
    return ESP_OK;
}

esp_err_t esp_xiaozhi_chat_display_set_icon(const char *icon)
{
    ESP_RETURN_ON_FALSE(icon, ESP_ERR_INVALID_ARG, TAG, "Invalid icon");
    ESP_RETURN_ON_ERROR(esp_xiaozhi_chat_display_required(), TAG, "Display not initialized");

    if (s_display->kind == DISPLAY_KIND_LCD) {
        if (!esp_xiaozhi_chat_display_lock(s_display, 30000) || s_display->emotion_label == NULL) {
            return ESP_OK;
        }
        lv_obj_set_style_text_font(s_display->emotion_label, &font_awesome_30_4, 0);
        lv_label_set_text(s_display->emotion_label, icon);
        lv_obj_clear_flag(s_display->emotion_label, LV_OBJ_FLAG_HIDDEN);
        lcd_display_t *lcd = esp_xiaozhi_chat_display_from_base(s_display);
        if (lcd->preview_image != NULL) {
            lv_obj_add_flag(lcd->preview_image, LV_OBJ_FLAG_HIDDEN);
        }
        esp_xiaozhi_chat_display_unlock(s_display);
        return ESP_OK;
    }
    if (!esp_xiaozhi_chat_display_lock(s_display, 30000) || s_display->emotion_label == NULL) {
        return ESP_OK;
    }
    lv_label_set_text(s_display->emotion_label, icon);
    esp_xiaozhi_chat_display_unlock(s_display);
    return ESP_OK;
}

esp_err_t esp_xiaozhi_chat_display_set_image(const void *image)
{
    ESP_RETURN_ON_ERROR(esp_xiaozhi_chat_display_required(), TAG, "Display not initialized");

    if (s_display->kind != DISPLAY_KIND_LCD) {
        return ESP_OK;
    }
    lcd_display_t *lcd = esp_xiaozhi_chat_display_from_base(s_display);
    const lv_img_dsc_t *img_dsc = (const lv_img_dsc_t *)image;
    if (!esp_xiaozhi_chat_display_lock(s_display, 30000) || lcd->content == NULL) {
        return ESP_OK;
    }
    if (lcd->preview_image != NULL) {
        if (img_dsc != NULL) {
            int scale = (img_dsc->header.w > 0) ? (128 * s_display->width / img_dsc->header.w) : 256;
            lv_image_set_scale(lcd->preview_image, scale);
            lv_image_set_src(lcd->preview_image, img_dsc);
            lv_obj_clear_flag(lcd->preview_image, LV_OBJ_FLAG_HIDDEN);
            if (s_display->emotion_label != NULL) {
                lv_obj_add_flag(s_display->emotion_label, LV_OBJ_FLAG_HIDDEN);
            }
        } else {
            lv_obj_add_flag(lcd->preview_image, LV_OBJ_FLAG_HIDDEN);
            if (s_display->emotion_label != NULL) {
                lv_obj_clear_flag(s_display->emotion_label, LV_OBJ_FLAG_HIDDEN);
            }
        }
    }
    esp_xiaozhi_chat_display_unlock(s_display);
    return ESP_OK;
}

esp_err_t esp_xiaozhi_chat_display_set_theme(const char *theme_name)
{
    ESP_RETURN_ON_FALSE(theme_name, ESP_ERR_INVALID_ARG, TAG, "Invalid theme name");
    ESP_RETURN_ON_ERROR(esp_xiaozhi_chat_display_required(), TAG, "Display not initialized");

    if (s_display->kind == DISPLAY_KIND_LCD) {
        lcd_display_t *lcd = esp_xiaozhi_chat_display_from_base(s_display);
        if (!esp_xiaozhi_chat_display_lock(s_display, 30000)) {
            return ESP_OK;
        }
        if (strcasecmp(theme_name, "dark") == 0) {
            lcd->current_theme = esp_xiaozhi_chat_display_dark_theme();
        } else if (strcasecmp(theme_name, "light") == 0) {
            lcd->current_theme = esp_xiaozhi_chat_display_light_theme();
        } else {
            ESP_LOGE(TAG, "Invalid theme name: %s", theme_name);
            esp_xiaozhi_chat_display_unlock(s_display);
            return ESP_ERR_INVALID_ARG;
        }
        lv_obj_t *screen = lv_screen_active();
        lv_obj_set_style_bg_color(screen, lcd->current_theme.background, 0);
        lv_obj_set_style_text_color(screen, lcd->current_theme.text, 0);
        if (lcd->container != NULL) {
            lv_obj_set_style_bg_color(lcd->container, lcd->current_theme.background, 0);
            lv_obj_set_style_border_color(lcd->container, lcd->current_theme.border, 0);
        }
        if (lcd->status_bar != NULL) {
            lv_obj_set_style_bg_color(lcd->status_bar, lcd->current_theme.background, 0);
            lv_obj_set_style_text_color(lcd->status_bar, lcd->current_theme.text, 0);
        }
        if (lcd->content != NULL) {
            lv_obj_set_style_bg_color(lcd->content, lcd->current_theme.chat_background, 0);
            lv_obj_set_style_border_color(lcd->content, lcd->current_theme.border, 0);
        }
        if (s_display->low_battery_popup != NULL) {
            lv_obj_set_style_bg_color(s_display->low_battery_popup, lcd->current_theme.low_battery, 0);
        }
        strlcpy(s_display->current_theme_name, theme_name, sizeof(s_display->current_theme_name));
        esp_xiaozhi_chat_display_unlock(s_display);
        return ESP_OK;
    }
    strlcpy(s_display->current_theme_name, theme_name, sizeof(s_display->current_theme_name));
    return ESP_OK;
}
