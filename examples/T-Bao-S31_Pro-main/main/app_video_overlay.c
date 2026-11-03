/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Small fixed-geometry RGB565 drawing layer used while LVGL is paused for
 * direct video playback.
 */
#include "app_video_overlay.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include <stddef.h>
#include <string.h>

#define VIDEO_OVERLAY_BAR_HEIGHT 58
#define VIDEO_OVERLAY_BUTTON_SIZE 48
#define VIDEO_OVERLAY_BUTTON_MARGIN 8
#define VIDEO_OVERLAY_RADIUS 12
#define VIDEO_OVERLAY_MODE_BADGE_HEIGHT 34
#define VIDEO_OVERLAY_MODE_BADGE_MARGIN 8
#define VIDEO_OVERLAY_TEXT_SCALE 2

#define VIDEO_OVERLAY_WHITE 0xFFFFU
#define VIDEO_OVERLAY_DARK  0x0841U
#define VIDEO_OVERLAY_ACCENT 0x5D9BU

static SemaphoreHandle_t s_state_mutex;
static app_video_overlay_state_t s_state = {
    .mode = APP_VIDEO_OVERLAY_MODE_SINGLE_LOOP,
    .action = APP_VIDEO_OVERLAY_ACTION_NONE,
};
static int s_width;
static int s_height;

static uint16_t overlay_blend(uint16_t base, uint16_t over, uint8_t alpha)
{
    const uint32_t inv = 255U - alpha;
    const uint32_t base_red = (base >> 11) & 0x1FU;
    const uint32_t base_green = (base >> 5) & 0x3FU;
    const uint32_t base_blue = base & 0x1FU;
    const uint32_t over_red = (over >> 11) & 0x1FU;
    const uint32_t over_green = (over >> 5) & 0x3FU;
    const uint32_t over_blue = over & 0x1FU;
    return (uint16_t)((((base_red * inv + over_red * alpha) / 255U) << 11) |
                      (((base_green * inv + over_green * alpha) / 255U) << 5) |
                      ((base_blue * inv + over_blue * alpha) / 255U));
}

static bool overlay_in_bounds(int x, int y, int width, int height)
{
    return x >= 0 && y >= 0 && x < width && y < height;
}

static void overlay_pixel(uint16_t *frame, int width, int height,
                          int x, int y, uint16_t color)
{
    if (overlay_in_bounds(x, y, width, height)) {
        frame[(size_t)y * (size_t)width + (size_t)x] = color;
    }
}

static void overlay_blend_pixel(uint16_t *frame, int width, int height,
                                int x, int y, uint16_t color, uint8_t alpha)
{
    if (overlay_in_bounds(x, y, width, height)) {
        uint16_t *pixel = &frame[(size_t)y * (size_t)width + (size_t)x];
        *pixel = overlay_blend(*pixel, color, alpha);
    }
}

static void overlay_fill_rect(uint16_t *frame, int width, int height,
                              int left, int top, int right, int bottom,
                              uint16_t color)
{
    if (left < 0) {
        left = 0;
    }
    if (top < 0) {
        top = 0;
    }
    if (right > width) {
        right = width;
    }
    if (bottom > height) {
        bottom = height;
    }
    for (int y = top; y < bottom; ++y) {
        for (int x = left; x < right; ++x) {
            frame[(size_t)y * (size_t)width + (size_t)x] = color;
        }
    }
}

static bool overlay_in_round_rect(int x, int y, int left, int top,
                                  int right, int bottom, int radius)
{
    if (x < left || x >= right || y < top || y >= bottom) {
        return false;
    }
    const int corner_left = left + radius;
    const int corner_right = right - radius - 1;
    const int corner_top = top + radius;
    const int corner_bottom = bottom - radius - 1;
    if (x >= corner_left && x <= corner_right) {
        return true;
    }
    if (y >= corner_top && y <= corner_bottom) {
        return true;
    }
    const int corner_x = x < corner_left ? corner_left : corner_right;
    const int corner_y = y < corner_top ? corner_top : corner_bottom;
    const int dx = x - corner_x;
    const int dy = y - corner_y;
    return dx * dx + dy * dy <= radius * radius;
}

static void overlay_fill_round_rect(uint16_t *frame, int width, int height,
                                    int left, int top, int right, int bottom,
                                    int radius, uint16_t color)
{
    if (radius < 0) {
        radius = 0;
    }
    const int max_radius = (right - left < bottom - top ? right - left :
                            bottom - top) / 2;
    if (radius > max_radius) {
        radius = max_radius;
    }
    for (int y = top; y < bottom; ++y) {
        for (int x = left; x < right; ++x) {
            if (overlay_in_round_rect(x, y, left, top, right, bottom,
                                      radius)) {
                overlay_pixel(frame, width, height, x, y, color);
            }
        }
    }
}

static void overlay_blend_round_rect(uint16_t *frame, int width, int height,
                                     int left, int top, int right, int bottom,
                                     int radius, uint16_t color, uint8_t alpha)
{
    if (radius < 0) {
        radius = 0;
    }
    const int max_radius = (right - left < bottom - top ? right - left :
                            bottom - top) / 2;
    if (radius > max_radius) {
        radius = max_radius;
    }
    for (int y = top; y < bottom; ++y) {
        for (int x = left; x < right; ++x) {
            if (overlay_in_round_rect(x, y, left, top, right, bottom,
                                      radius)) {
                overlay_blend_pixel(frame, width, height, x, y, color,
                                    alpha);
            }
        }
    }
}

static void overlay_fill_circle(uint16_t *frame, int width, int height,
                                int center_x, int center_y, int radius,
                                uint16_t color, uint8_t alpha)
{
    const int radius_squared = radius * radius;
    for (int y = center_y - radius; y <= center_y + radius; ++y) {
        for (int x = center_x - radius; x <= center_x + radius; ++x) {
            const int dx = x - center_x;
            const int dy = y - center_y;
            if (dx * dx + dy * dy <= radius_squared) {
                if (alpha == 255U) {
                    overlay_pixel(frame, width, height, x, y, color);
                } else {
                    overlay_blend_pixel(frame, width, height, x, y,
                                        color, alpha);
                }
            }
        }
    }
}

static void overlay_draw_line(uint16_t *frame, int width, int height,
                              int x0, int y0, int x1, int y1,
                              uint16_t color)
{
    const int dx = x1 > x0 ? x1 - x0 : x0 - x1;
    const int sx = x0 < x1 ? 1 : -1;
    const int dy = -(y1 > y0 ? y1 - y0 : y0 - y1);
    const int sy = y0 < y1 ? 1 : -1;
    int error = dx + dy;
    for (;;) {
        overlay_pixel(frame, width, height, x0, y0, color);
        if (x0 == x1 && y0 == y1) {
            break;
        }
        const int error_twice = 2 * error;
        if (error_twice >= dy) {
            error += dy;
            x0 += sx;
        }
        if (error_twice <= dx) {
            error += dx;
            y0 += sy;
        }
    }
}

static void overlay_draw_play_icon(uint16_t *frame, int width, int height,
                                   int center_x, int center_y,
                                   uint16_t color)
{
    for (int y = -13; y <= 13; ++y) {
        const int span = 13 - (y < 0 ? -y : y);
        for (int x = -5; x <= span; ++x) {
            if (x >= -5 && x <= span) {
                overlay_pixel(frame, width, height,
                              center_x + x, center_y + y, color);
            }
        }
    }
}

static void overlay_draw_pause_icon(uint16_t *frame, int width, int height,
                                    int center_x, int center_y,
                                    uint16_t color)
{
    overlay_fill_rect(frame, width, height, center_x - 11, center_y - 13,
                      center_x - 3, center_y + 14, color);
    overlay_fill_rect(frame, width, height, center_x + 3, center_y - 13,
                      center_x + 11, center_y + 14, color);
}

static void overlay_draw_mode_icon(uint16_t *frame, int width, int height,
                                   int center_x, int center_y,
                                   app_video_overlay_mode_t mode)
{
    if (mode == APP_VIDEO_OVERLAY_MODE_RANDOM) {
        overlay_fill_circle(frame, width, height, center_x - 12,
                            center_y - 10, 3, VIDEO_OVERLAY_WHITE, 255);
        overlay_fill_circle(frame, width, height, center_x - 12,
                            center_y + 10, 3, VIDEO_OVERLAY_WHITE, 255);
        overlay_fill_circle(frame, width, height, center_x + 12,
                            center_y, 3, VIDEO_OVERLAY_WHITE, 255);
        overlay_draw_line(frame, width, height, center_x - 9, center_y - 10,
                          center_x + 1, center_y, VIDEO_OVERLAY_WHITE);
        overlay_draw_line(frame, width, height, center_x - 9, center_y + 10,
                          center_x + 1, center_y, VIDEO_OVERLAY_WHITE);
        overlay_draw_line(frame, width, height, center_x + 1, center_y,
                          center_x + 7, center_y - 6, VIDEO_OVERLAY_WHITE);
        overlay_draw_line(frame, width, height, center_x + 1, center_y,
                          center_x + 7, center_y + 6, VIDEO_OVERLAY_WHITE);
        return;
    }
    if (mode == APP_VIDEO_OVERLAY_MODE_SEQUENTIAL) {
        overlay_fill_rect(frame, width, height, center_x - 13, center_y - 11,
                          center_x + 7, center_y - 7, VIDEO_OVERLAY_WHITE);
        overlay_fill_rect(frame, width, height, center_x - 13, center_y - 2,
                          center_x + 7, center_y + 2, VIDEO_OVERLAY_WHITE);
        overlay_fill_rect(frame, width, height, center_x - 13, center_y + 7,
                          center_x + 7, center_y + 11, VIDEO_OVERLAY_WHITE);
        overlay_draw_line(frame, width, height, center_x + 5, center_y,
                          center_x + 13, center_y, VIDEO_OVERLAY_WHITE);
        overlay_draw_line(frame, width, height, center_x + 8, center_y - 4,
                          center_x + 13, center_y, VIDEO_OVERLAY_WHITE);
        overlay_draw_line(frame, width, height, center_x + 8, center_y + 4,
                          center_x + 13, center_y, VIDEO_OVERLAY_WHITE);
        return;
    }

    overlay_fill_circle(frame, width, height, center_x, center_y, 13,
                        VIDEO_OVERLAY_WHITE, 255);
    overlay_fill_circle(frame, width, height, center_x, center_y, 7,
                        VIDEO_OVERLAY_DARK, 255);
    overlay_draw_line(frame, width, height, center_x + 4, center_y - 12,
                      center_x + 14, center_y - 12, VIDEO_OVERLAY_WHITE);
    overlay_draw_line(frame, width, height, center_x + 14, center_y - 12,
                      center_x + 9, center_y - 17, VIDEO_OVERLAY_WHITE);
    overlay_draw_line(frame, width, height, center_x + 14, center_y - 12,
                      center_x + 9, center_y - 7, VIDEO_OVERLAY_WHITE);
}

static void overlay_draw_menu_icon(uint16_t *frame, int width, int height,
                                   int center_x, int center_y)
{
    for (int offset = -9; offset <= 9; offset += 9) {
        overlay_fill_rect(frame, width, height, center_x - 14,
                          center_y + offset - 2, center_x + 14,
                          center_y + offset + 3, VIDEO_OVERLAY_WHITE);
    }
}

static uint8_t overlay_glyph_row(char value, int row)
{
    static const uint8_t glyphs[][7] = {
        ['A'] = {0x0E, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11},
        ['C'] = {0x0F, 0x10, 0x10, 0x10, 0x10, 0x10, 0x0F},
        ['D'] = {0x1E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x1E},
        ['E'] = {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x1F},
        ['G'] = {0x0F, 0x10, 0x10, 0x17, 0x11, 0x11, 0x0F},
        ['I'] = {0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x1F},
        ['L'] = {0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1F},
        ['M'] = {0x11, 0x1B, 0x15, 0x15, 0x11, 0x11, 0x11},
        ['N'] = {0x11, 0x19, 0x15, 0x13, 0x11, 0x11, 0x11},
        ['O'] = {0x0E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E},
        ['P'] = {0x1E, 0x11, 0x11, 0x1E, 0x10, 0x10, 0x10},
        ['Q'] = {0x0E, 0x11, 0x11, 0x11, 0x15, 0x12, 0x0D},
        ['R'] = {0x1E, 0x11, 0x11, 0x1E, 0x14, 0x12, 0x11},
        ['S'] = {0x0F, 0x10, 0x10, 0x0E, 0x01, 0x01, 0x1E},
        ['U'] = {0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E},
    };
    if (row < 0 || row >= 7 || value < 'A' || value > 'Z') {
        return 0;
    }
    return glyphs[(unsigned char)value][row];
}

static int overlay_text_width(const char *text, int scale)
{
    if (text == NULL || text[0] == '\0') {
        return 0;
    }
    const int length = (int)strlen(text);
    return length * 5 * scale + (length - 1) * scale;
}

static void overlay_draw_text(uint16_t *frame, int width, int height,
                              int left, int top, const char *text, int scale,
                              uint16_t color)
{
    if (text == NULL || scale <= 0) {
        return;
    }
    int cursor_x = left;
    for (const char *cursor = text; *cursor != '\0'; ++cursor) {
        for (int row = 0; row < 7; ++row) {
            const uint8_t bits = overlay_glyph_row(*cursor, row);
            for (int column = 0; column < 5; ++column) {
                if ((bits & (1U << (4 - column))) != 0) {
                    overlay_fill_rect(frame, width, height,
                                      cursor_x + column * scale,
                                      top + row * scale,
                                      cursor_x + (column + 1) * scale,
                                      top + (row + 1) * scale, color);
                }
            }
        }
        cursor_x += 5 * scale + scale;
    }
}

static const char *overlay_mode_label(app_video_overlay_mode_t mode)
{
    switch (mode) {
    case APP_VIDEO_OVERLAY_MODE_RANDOM:
        return "RANDOM";
    case APP_VIDEO_OVERLAY_MODE_SEQUENTIAL:
        return "SEQUENCE";
    case APP_VIDEO_OVERLAY_MODE_SINGLE_LOOP:
    default:
        return "SINGLE LOOP";
    }
}

static void overlay_draw_mode_badge(uint16_t *frame, int width, int height,
                                    app_video_overlay_mode_t mode)
{
    const char *label = overlay_mode_label(mode);
    const int text_width = overlay_text_width(label, VIDEO_OVERLAY_TEXT_SCALE);
    const int badge_width = text_width + 20;
    const int bar_top = height - VIDEO_OVERLAY_BAR_HEIGHT;
    const int badge_top = bar_top - VIDEO_OVERLAY_MODE_BADGE_HEIGHT -
                          VIDEO_OVERLAY_MODE_BADGE_MARGIN;
    const int badge_left = (width - badge_width) / 2;
    overlay_blend_round_rect(frame, width, height, badge_left, badge_top,
                             badge_left + badge_width,
                             badge_top + VIDEO_OVERLAY_MODE_BADGE_HEIGHT,
                             VIDEO_OVERLAY_MODE_BADGE_HEIGHT / 2,
                             VIDEO_OVERLAY_DARK, 230);
    overlay_blend_round_rect(frame, width, height, badge_left + 2,
                             badge_top + 2, badge_left + badge_width - 2,
                             badge_top + VIDEO_OVERLAY_MODE_BADGE_HEIGHT - 2,
                             VIDEO_OVERLAY_MODE_BADGE_HEIGHT / 2 - 2,
                             VIDEO_OVERLAY_ACCENT, 85);
    overlay_draw_text(frame, width, height,
                      badge_left + (badge_width - text_width) / 2,
                      badge_top + (VIDEO_OVERLAY_MODE_BADGE_HEIGHT -
                                   7 * VIDEO_OVERLAY_TEXT_SCALE) / 2,
                      label, VIDEO_OVERLAY_TEXT_SCALE, VIDEO_OVERLAY_WHITE);
}

static void overlay_draw_button(uint16_t *frame, int width, int height,
                                int left, int top, bool mode_button,
                                app_video_overlay_mode_t mode)
{
    overlay_fill_round_rect(frame, width, height, left, top,
                            left + VIDEO_OVERLAY_BUTTON_SIZE,
                            top + VIDEO_OVERLAY_BUTTON_SIZE,
                            VIDEO_OVERLAY_RADIUS, VIDEO_OVERLAY_DARK);
    overlay_blend_round_rect(frame, width, height, left + 1, top + 1,
                             left + VIDEO_OVERLAY_BUTTON_SIZE - 1,
                             top + VIDEO_OVERLAY_BUTTON_SIZE - 1,
                             VIDEO_OVERLAY_RADIUS - 1,
                             VIDEO_OVERLAY_ACCENT, 90);
    const int center_x = left + VIDEO_OVERLAY_BUTTON_SIZE / 2;
    const int center_y = top + VIDEO_OVERLAY_BUTTON_SIZE / 2;
    if (mode_button) {
        overlay_draw_mode_icon(frame, width, height, center_x, center_y,
                               mode);
    } else {
        overlay_draw_menu_icon(frame, width, height, center_x, center_y);
    }
}

esp_err_t app_video_overlay_init(int width, int height)
{
    if (width <= 0 || height <= 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_state_mutex == NULL) {
        s_state_mutex = xSemaphoreCreateMutex();
        if (s_state_mutex == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }
    s_width = width;
    s_height = height;
    if (xSemaphoreTake(s_state_mutex, portMAX_DELAY) == pdTRUE) {
        s_state.controls_visible = false;
        s_state.paused = false;
        s_state.show_action_badge = false;
        s_state.show_mode_badge = false;
        s_state.mode = APP_VIDEO_OVERLAY_MODE_SINGLE_LOOP;
        s_state.action = APP_VIDEO_OVERLAY_ACTION_NONE;
        xSemaphoreGive(s_state_mutex);
    }
    return ESP_OK;
}

void app_video_overlay_set_state(const app_video_overlay_state_t *state)
{
    if (state == NULL || s_state_mutex == NULL) {
        return;
    }
    if (xSemaphoreTake(s_state_mutex, portMAX_DELAY) != pdTRUE) {
        return;
    }
    s_state = *state;
    if (s_state.mode > APP_VIDEO_OVERLAY_MODE_SEQUENTIAL) {
        s_state.mode = APP_VIDEO_OVERLAY_MODE_SINGLE_LOOP;
    }
    if (s_state.action > APP_VIDEO_OVERLAY_ACTION_PLAY) {
        s_state.action = APP_VIDEO_OVERLAY_ACTION_NONE;
    }
    xSemaphoreGive(s_state_mutex);
}

esp_err_t app_video_overlay_compose(const uint16_t *base, uint16_t *composed,
                                    int width, int height, uint32_t now_ms)
{
    (void)now_ms;
    if (base == NULL || composed == NULL || width <= 0 || height <= 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_state_mutex == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (width != s_width || height != s_height) {
        return ESP_ERR_INVALID_SIZE;
    }

    app_video_overlay_state_t state;
    if (xSemaphoreTake(s_state_mutex, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    state = s_state;
    xSemaphoreGive(s_state_mutex);

    const size_t pixels = (size_t)width * (size_t)height;
    memcpy(composed, base, pixels * sizeof(uint16_t));
    if (!state.controls_visible && !state.show_action_badge &&
        !state.show_mode_badge && !state.paused) {
        return ESP_OK;
    }

    if (state.controls_visible) {
        const int bar_top = height - VIDEO_OVERLAY_BAR_HEIGHT;
        overlay_blend_round_rect(composed, width, height, 4, bar_top,
                                 width - 4, height, VIDEO_OVERLAY_RADIUS,
                                 VIDEO_OVERLAY_DARK, 205);
        overlay_fill_round_rect(composed, width, height, 4, bar_top,
                                width - 4, bar_top + 2, 1,
                                VIDEO_OVERLAY_ACCENT);
        overlay_draw_button(composed, width, height,
                            VIDEO_OVERLAY_BUTTON_MARGIN, bar_top + 5, true,
                            state.mode);
        overlay_draw_button(composed, width, height,
                            width - VIDEO_OVERLAY_BUTTON_MARGIN -
                            VIDEO_OVERLAY_BUTTON_SIZE,
                            bar_top + 5, false, state.mode);
    }

    if (state.show_mode_badge) {
        overlay_draw_mode_badge(composed, width, height, state.mode);
    }

    if (state.show_action_badge || state.paused) {
        const int center_x = width / 2;
        const int center_y = height / 2;
        overlay_fill_circle(composed, width, height, center_x, center_y, 34,
                            VIDEO_OVERLAY_DARK, 190);
        if (state.show_action_badge &&
            state.action == APP_VIDEO_OVERLAY_ACTION_PAUSE) {
            overlay_draw_pause_icon(composed, width, height, center_x,
                                    center_y, VIDEO_OVERLAY_WHITE);
        } else {
            overlay_draw_play_icon(composed, width, height, center_x,
                                   center_y, VIDEO_OVERLAY_WHITE);
        }
    }
    return ESP_OK;
}

void app_video_overlay_deinit(void)
{
    if (s_state_mutex != NULL) {
        vSemaphoreDelete(s_state_mutex);
        s_state_mutex = NULL;
    }
    s_width = 0;
    s_height = 0;
    s_state.controls_visible = false;
    s_state.paused = false;
    s_state.show_action_badge = false;
    s_state.show_mode_badge = false;
    s_state.mode = APP_VIDEO_OVERLAY_MODE_SINGLE_LOOP;
    s_state.action = APP_VIDEO_OVERLAY_ACTION_NONE;
}
