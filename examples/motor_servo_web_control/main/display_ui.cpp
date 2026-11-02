#include "display_ui.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>

#include "board_config.h"
#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_lcd_io_i80.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_st7796.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lvgl.h"

namespace {

constexpr char kTag[] = "control_display";
constexpr int kLcdTransferTimeoutMs = 1000;
constexpr int kLvglTickMs = 5;
constexpr int kLabelHeight = 18;
constexpr size_t kDrawBufferPixels =
    BOARD_LCD_H_RES * BOARD_LCD_DRAW_LINES;

esp_lcd_panel_io_handle_t s_lcd_io = nullptr;
esp_lcd_panel_handle_t s_lcd_panel = nullptr;
SemaphoreHandle_t s_lcd_transfer_done = nullptr;
SemaphoreHandle_t s_lvgl_mutex = nullptr;
lv_display_t *s_display = nullptr;
esp_timer_handle_t s_lvgl_tick_timer = nullptr;
#if BOARD_LCD_SOFTWARE_ROTATE_CW_90
uint16_t *s_rotation_buffer = nullptr;
#endif

lv_obj_t *s_ap_count_label = nullptr;
lv_obj_t *s_status_band = nullptr;
lv_obj_t *s_status_icon = nullptr;
lv_obj_t *s_status_label = nullptr;
lv_obj_t *s_network_label = nullptr;
lv_obj_t *s_url_label = nullptr;
lv_obj_t *s_motor_a_value = nullptr;
lv_obj_t *s_motor_b_value = nullptr;
lv_obj_t *s_motor_a_bar = nullptr;
lv_obj_t *s_motor_b_bar = nullptr;
lv_obj_t *s_servo_value = nullptr;
lv_obj_t *s_servo_bar = nullptr;
lv_obj_t *s_failsafe_label = nullptr;

esp_err_t init_backlight()
{
    gpio_config_t config = {};
    config.pin_bit_mask = 1ULL << BOARD_LCD_BL_PWM;
    config.mode = GPIO_MODE_OUTPUT;
    ESP_RETURN_ON_ERROR(gpio_config(&config), kTag, "configure backlight GPIO");
    return gpio_set_level(BOARD_LCD_BL_PWM, 0);
}

bool IRAM_ATTR lcd_transfer_done(
    esp_lcd_panel_io_handle_t,
    esp_lcd_panel_io_event_data_t *,
    void *context)
{
    BaseType_t high_task_woken = pdFALSE;
    xSemaphoreGiveFromISR(
        static_cast<SemaphoreHandle_t>(context), &high_task_woken);
    return high_task_woken == pdTRUE;
}

esp_err_t init_lcd()
{
    s_lcd_transfer_done = xSemaphoreCreateBinary();
    if (s_lcd_transfer_done == nullptr) {
        return ESP_ERR_NO_MEM;
    }

    esp_lcd_i80_bus_handle_t i80_bus = nullptr;
    esp_lcd_i80_bus_config_t bus_config = {};
    bus_config.dc_gpio_num = BOARD_LCD_RS;
    bus_config.wr_gpio_num = BOARD_LCD_WR;
    bus_config.clk_src = LCD_CLK_SRC_PLL160M;
    std::fill_n(
        bus_config.data_gpio_nums, ESP_LCD_I80_BUS_WIDTH_MAX, GPIO_NUM_NC);
    bus_config.data_gpio_nums[0] = BOARD_LCD_D0;
    bus_config.data_gpio_nums[1] = BOARD_LCD_D1;
    bus_config.data_gpio_nums[2] = BOARD_LCD_D2;
    bus_config.data_gpio_nums[3] = BOARD_LCD_D3;
    bus_config.data_gpio_nums[4] = BOARD_LCD_D4;
    bus_config.data_gpio_nums[5] = BOARD_LCD_D5;
    bus_config.data_gpio_nums[6] = BOARD_LCD_D6;
    bus_config.data_gpio_nums[7] = BOARD_LCD_D7;
    bus_config.bus_width = BOARD_LCD_DATA_WIDTH;
    bus_config.max_transfer_bytes =
        BOARD_LCD_H_RES * BOARD_LCD_DRAW_LINES * sizeof(uint16_t);
    bus_config.dma_burst_size = 64;
    ESP_RETURN_ON_ERROR(
        esp_lcd_new_i80_bus(&bus_config, &i80_bus), kTag, "create LCD bus");

    esp_lcd_panel_io_i80_config_t io_config = {};
    io_config.cs_gpio_num = BOARD_LCD_CS;
    io_config.pclk_hz = BOARD_LCD_PIXEL_CLOCK_HZ;
    io_config.trans_queue_depth = 4;
    io_config.on_color_trans_done = lcd_transfer_done;
    io_config.user_ctx = s_lcd_transfer_done;
    io_config.lcd_cmd_bits = 8;
    io_config.lcd_param_bits = 8;
    io_config.dc_levels.dc_idle_level = 0;
    io_config.dc_levels.dc_cmd_level = 0;
    io_config.dc_levels.dc_dummy_level = 0;
    io_config.dc_levels.dc_data_level = 1;
    io_config.flags.swap_color_bytes = 1;
    ESP_RETURN_ON_ERROR(
        esp_lcd_new_panel_io_i80(i80_bus, &io_config, &s_lcd_io),
        kTag,
        "create LCD IO");

    esp_lcd_panel_dev_config_t panel_config = {};
    panel_config.reset_gpio_num = BOARD_LCD_RST;
    panel_config.rgb_ele_order = BOARD_LCD_RGB_ORDER;
    panel_config.bits_per_pixel = 16;
    ESP_RETURN_ON_ERROR(
        esp_lcd_new_panel_st7796(s_lcd_io, &panel_config, &s_lcd_panel),
        kTag,
        "create ST7796S panel");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_reset(s_lcd_panel), kTag, "reset LCD");
    ESP_RETURN_ON_ERROR(
        esp_lcd_panel_init(s_lcd_panel), kTag, "initialize LCD");

#if BOARD_LCD_INVERT_COLOR
    ESP_RETURN_ON_ERROR(
        esp_lcd_panel_invert_color(s_lcd_panel, true), kTag, "invert LCD");
#endif
#if BOARD_LCD_SWAP_XY
    ESP_RETURN_ON_ERROR(
        esp_lcd_panel_swap_xy(s_lcd_panel, true), kTag, "swap LCD axes");
#endif
    ESP_RETURN_ON_ERROR(
        esp_lcd_panel_mirror(
            s_lcd_panel, BOARD_LCD_MIRROR_X, BOARD_LCD_MIRROR_Y),
        kTag,
        "mirror LCD");
    ESP_RETURN_ON_ERROR(
        esp_lcd_panel_set_gap(s_lcd_panel, BOARD_LCD_X_GAP, BOARD_LCD_Y_GAP),
        kTag,
        "set LCD gap");
    return esp_lcd_panel_disp_on_off(s_lcd_panel, true);
}

void lvgl_flush(
    lv_display_t *display,
    const lv_area_t *area,
    uint8_t *pixel_map)
{
#if BOARD_LCD_SOFTWARE_ROTATE_CW_90
    const int source_width = lv_area_get_width(area);
    const int source_height = lv_area_get_height(area);
    const size_t pixel_count = static_cast<size_t>(lv_area_get_size(area));

    esp_err_t err = ESP_ERR_INVALID_SIZE;
    if (s_rotation_buffer != nullptr && pixel_count <= kDrawBufferPixels) {
        const auto *source = reinterpret_cast<const uint16_t *>(pixel_map);
        for (int source_y = 0; source_y < source_height; ++source_y) {
            for (int source_x = 0; source_x < source_width; ++source_x) {
                const int destination_x = source_height - 1 - source_y;
                const int destination_y = source_x;
                s_rotation_buffer[
                    destination_y * source_height + destination_x] =
                    source[source_y * source_width + source_x];
            }
        }

        err = esp_lcd_panel_draw_bitmap(
            s_lcd_panel,
            BOARD_LCD_V_RES - 1 - area->y2,
            area->x1,
            BOARD_LCD_V_RES - area->y1,
            area->x2 + 1,
            s_rotation_buffer);
    }
#else
    const esp_err_t err = esp_lcd_panel_draw_bitmap(
        s_lcd_panel,
        area->x1,
        area->y1,
        area->x2 + 1,
        area->y2 + 1,
        pixel_map);
#endif
    if (err == ESP_OK) {
        if (xSemaphoreTake(
                s_lcd_transfer_done,
                pdMS_TO_TICKS(kLcdTransferTimeoutMs)) != pdTRUE) {
            ESP_LOGE(kTag, "LCD transfer timed out");
        }
    } else {
        ESP_LOGE(kTag, "LCD draw failed: %s", esp_err_to_name(err));
    }
    lv_display_flush_ready(display);
}

void lvgl_tick(void *)
{
    lv_tick_inc(kLvglTickMs);
}

void lvgl_task(void *)
{
    while (true) {
        uint32_t delay_ms = 10;
        if (xSemaphoreTake(s_lvgl_mutex, portMAX_DELAY) == pdTRUE) {
            delay_ms = lv_timer_handler();
            xSemaphoreGive(s_lvgl_mutex);
        }
        delay_ms = std::clamp<uint32_t>(delay_ms, 5, 20);
        vTaskDelay(pdMS_TO_TICKS(delay_ms));
    }
}

lv_obj_t *create_label(
    lv_obj_t *parent,
    int x,
    int y,
    int width,
    lv_color_t color)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_obj_set_pos(label, x, y);
    lv_obj_set_size(label, width, kLabelHeight);
    lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_color(label, color, 0);
    lv_obj_set_style_text_letter_space(label, 0, 0);
    lv_obj_set_style_text_line_space(label, 0, 0);
    return label;
}

lv_obj_t *create_bar(
    lv_obj_t *parent,
    int x,
    int y,
    int width,
    int minimum,
    int maximum,
    lv_color_t indicator)
{
    lv_obj_t *bar = lv_bar_create(parent);
    lv_obj_set_pos(bar, x, y);
    lv_obj_set_size(bar, width, 8);
    lv_bar_set_range(bar, minimum, maximum);
    lv_bar_set_value(bar, minimum, LV_ANIM_OFF);
    lv_obj_set_style_radius(bar, 3, LV_PART_MAIN);
    lv_obj_set_style_radius(bar, 3, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(bar, lv_color_hex(0xE4E7EC), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(bar, indicator, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_INDICATOR);
    return bar;
}

void create_ui()
{
    const lv_color_t background = lv_color_hex(0xFFFFFF);
    const lv_color_t primary = lv_color_hex(0x101828);
    const lv_color_t secondary = lv_color_hex(0x667085);
    const lv_color_t accent = lv_color_hex(0x175CD3);

    lv_obj_t *screen = lv_screen_active();
    lv_obj_set_style_bg_color(screen, background, 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(screen, 0, 0);
    lv_obj_set_scrollable(screen, false);

    lv_obj_t *title = create_label(screen, 14, 7, 210, primary);
    lv_label_set_text(title, "MOTOR + SERVO");

    s_ap_count_label = create_label(screen, 222, 7, 84, secondary);
    lv_obj_set_style_text_align(s_ap_count_label, LV_TEXT_ALIGN_RIGHT, 0);
    lv_label_set_text(s_ap_count_label, "AP 0");

    s_status_band = lv_obj_create(screen);
    lv_obj_remove_style_all(s_status_band);
    lv_obj_set_pos(s_status_band, 0, 30);
    lv_obj_set_size(s_status_band, BOARD_LCD_H_RES, 42);
    lv_obj_set_style_bg_color(s_status_band, lv_color_hex(0xF2F4F7), 0);
    lv_obj_set_style_bg_opa(s_status_band, LV_OPA_COVER, 0);
    lv_obj_set_scrollable(s_status_band, false);

    s_status_icon = create_label(s_status_band, 14, 12, 22, accent);
    lv_label_set_text(s_status_icon, LV_SYMBOL_REFRESH);
    s_status_label = create_label(s_status_band, 42, 12, 264, primary);
    lv_label_set_text(s_status_label, "Initializing...");

    s_network_label = create_label(screen, 14, 80, 292, secondary);
    lv_label_set_text(s_network_label, "Waiting for Wi-Fi scan");

    lv_obj_t *url_title = create_label(screen, 14, 103, 110, secondary);
    lv_label_set_text(url_title, "WEB CONTROL");
    s_url_label = create_label(screen, 14, 123, 292, accent);
    lv_label_set_text(s_url_label, "Waiting for network...");

    lv_obj_t *separator = lv_obj_create(screen);
    lv_obj_remove_style_all(separator);
    lv_obj_set_pos(separator, 14, 149);
    lv_obj_set_size(separator, 292, 1);
    lv_obj_set_style_bg_color(separator, lv_color_hex(0xD0D5DD), 0);
    lv_obj_set_style_bg_opa(separator, LV_OPA_COVER, 0);

    lv_obj_t *motor_a_title = create_label(screen, 14, 160, 138, secondary);
    lv_label_set_text(motor_a_title, "MOTOR A  15/14");
    lv_obj_t *motor_b_title = create_label(screen, 168, 160, 138, secondary);
    lv_label_set_text(motor_b_title, "MOTOR B  13/12");

    s_motor_a_value = create_label(screen, 14, 181, 138, primary);
    lv_label_set_text(s_motor_a_value, "STOP | 0%");
    s_motor_b_value = create_label(screen, 168, 181, 138, primary);
    lv_label_set_text(s_motor_b_value, "STOP | 0%");
    s_motor_a_bar = create_bar(screen, 14, 204, 138, 0, 100, accent);
    s_motor_b_bar = create_bar(screen, 168, 204, 138, 0, 100, accent);

    lv_obj_t *servo_title = create_label(screen, 14, 226, 126, secondary);
    lv_label_set_text(servo_title, "SERVO ES9051");
    s_servo_value = create_label(screen, 140, 226, 166, primary);
    lv_obj_set_style_text_align(s_servo_value, LV_TEXT_ALIGN_RIGHT, 0);
    lv_label_set_text(s_servo_value, "OFF | 1500 us");
    s_servo_bar = create_bar(
        screen,
        14,
        250,
        292,
        kServoPulseMinUs,
        kServoPulseMaxUs,
        lv_color_hex(0x175CD3));
    lv_bar_set_value(s_servo_bar, kServoPulseCenterUs, LV_ANIM_OFF);

    s_failsafe_label = create_label(screen, 14, 272, 292, secondary);
    lv_label_set_text(s_failsafe_label, "SAFE | Outputs off");

    lv_obj_t *footer = create_label(screen, 14, 296, 292, secondary);
    lv_label_set_text(footer, "Failsafe 2 s | A:15/14  B:13/12  S:5");
}

struct PhaseVisual {
    const char *icon;
    lv_color_t foreground;
    lv_color_t background;
};

PhaseVisual phase_visual(WifiPhase phase)
{
    switch (phase) {
    case WifiPhase::kConnected:
        return {LV_SYMBOL_OK, lv_color_hex(0x067647), lv_color_hex(0xECFDF3)};
    case WifiPhase::kConnectionFailed:
        return {LV_SYMBOL_CLOSE, lv_color_hex(0xB42318), lv_color_hex(0xFEF3F2)};
    case WifiPhase::kTargetNotConfigured:
    case WifiPhase::kTargetNotFound:
    case WifiPhase::kDisconnected:
        return {
            LV_SYMBOL_WARNING,
            lv_color_hex(0xB54708),
            lv_color_hex(0xFFFAEB)};
    case WifiPhase::kConnecting:
        return {LV_SYMBOL_WIFI, lv_color_hex(0x175CD3), lv_color_hex(0xEFF8FF)};
    case WifiPhase::kInitializing:
    case WifiPhase::kScanning:
    default:
        return {
            LV_SYMBOL_REFRESH,
            lv_color_hex(0x175CD3),
            lv_color_hex(0xF2F4F7)};
    }
}

const char *motor_direction(int percent)
{
    if (percent > 0) {
        return "FWD";
    }
    if (percent < 0) {
        return "REV";
    }
    return "STOP";
}

lv_color_t motor_color(int percent)
{
    if (percent > 0) {
        return lv_color_hex(0x067647);
    }
    if (percent < 0) {
        return lv_color_hex(0xB54708);
    }
    return lv_color_hex(0x98A2B3);
}

esp_err_t init_lvgl()
{
    s_lvgl_mutex = xSemaphoreCreateMutex();
    if (s_lvgl_mutex == nullptr) {
        return ESP_ERR_NO_MEM;
    }

    lv_init();
    s_display = lv_display_create(BOARD_LCD_H_RES, BOARD_LCD_V_RES);
    if (s_display == nullptr) {
        return ESP_ERR_NO_MEM;
    }
    lv_display_set_color_format(s_display, LV_COLOR_FORMAT_RGB565);
    lv_display_set_flush_cb(s_display, lvgl_flush);

    void *buffer_a = esp_lcd_i80_alloc_draw_buffer(
        s_lcd_io, kDrawBufferPixels * sizeof(uint16_t), MALLOC_CAP_DMA);
    void *buffer_b = esp_lcd_i80_alloc_draw_buffer(
        s_lcd_io, kDrawBufferPixels * sizeof(uint16_t), MALLOC_CAP_DMA);
#if BOARD_LCD_SOFTWARE_ROTATE_CW_90
    s_rotation_buffer = static_cast<uint16_t *>(esp_lcd_i80_alloc_draw_buffer(
        s_lcd_io, kDrawBufferPixels * sizeof(uint16_t), MALLOC_CAP_DMA));
#endif
    if (buffer_a == nullptr || buffer_b == nullptr) {
        return ESP_ERR_NO_MEM;
    }
#if BOARD_LCD_SOFTWARE_ROTATE_CW_90
    if (s_rotation_buffer == nullptr) {
        return ESP_ERR_NO_MEM;
    }
#endif
    lv_display_set_buffers(
        s_display,
        buffer_a,
        buffer_b,
        kDrawBufferPixels * sizeof(uint16_t),
        LV_DISPLAY_RENDER_MODE_PARTIAL);

    create_ui();

    const esp_timer_create_args_t timer_args = {
        .callback = lvgl_tick,
        .arg = nullptr,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "lvgl_tick",
        .skip_unhandled_events = true,
    };
    ESP_RETURN_ON_ERROR(
        esp_timer_create(&timer_args, &s_lvgl_tick_timer),
        kTag,
        "create LVGL tick timer");
    ESP_RETURN_ON_ERROR(
        esp_timer_start_periodic(s_lvgl_tick_timer, kLvglTickMs * 1000),
        kTag,
        "start LVGL tick timer");

    if (xTaskCreate(lvgl_task, "lvgl", 6144, nullptr, 4, nullptr) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

}  // namespace

esp_err_t display_ui_init()
{
    ESP_RETURN_ON_ERROR(init_backlight(), kTag, "initialize backlight");
    ESP_RETURN_ON_ERROR(init_lcd(), kTag, "initialize LCD");
    ESP_RETURN_ON_ERROR(init_lvgl(), kTag, "initialize LVGL");
    ESP_RETURN_ON_ERROR(
        gpio_set_level(BOARD_LCD_BL_PWM, 1), kTag, "enable backlight");
    ESP_LOGI(kTag, "ST7796S control UI ready");
    return ESP_OK;
}

void display_ui_update_wifi(const WifiStatus &status)
{
    if (s_lvgl_mutex == nullptr || s_status_label == nullptr) {
        return;
    }
    if (xSemaphoreTake(s_lvgl_mutex, portMAX_DELAY) != pdTRUE) {
        return;
    }

    const PhaseVisual visual = phase_visual(status.phase);
    lv_obj_set_style_bg_color(s_status_band, visual.background, 0);
    lv_obj_set_style_text_color(s_status_icon, visual.foreground, 0);
    lv_label_set_text(s_status_icon, visual.icon);
    lv_label_set_text(s_status_label, status.detail);
    lv_label_set_text_fmt(s_ap_count_label, "AP %u", status.ap_count);

    if (status.target_found) {
        lv_label_set_text_fmt(
            s_network_label,
            "Target detected | %d dBm",
            status.target_rssi);
    } else {
        lv_label_set_text(s_network_label, "Target not found");
    }

    if (status.phase == WifiPhase::kConnected && status.web_url[0] != '\0') {
        lv_label_set_text(s_url_label, status.web_url);
        lv_obj_set_style_text_color(s_url_label, lv_color_hex(0x175CD3), 0);
    } else {
        lv_label_set_text(s_url_label, "Waiting for network...");
        lv_obj_set_style_text_color(s_url_label, lv_color_hex(0x667085), 0);
    }

    xSemaphoreGive(s_lvgl_mutex);
}

void display_ui_update_actuators(const ActuatorState &state)
{
    if (s_lvgl_mutex == nullptr || s_motor_a_value == nullptr) {
        return;
    }
    if (xSemaphoreTake(s_lvgl_mutex, portMAX_DELAY) != pdTRUE) {
        return;
    }

    lv_label_set_text_fmt(
        s_motor_a_value,
        "%s | %d%%",
        motor_direction(state.motor_a_percent),
        std::abs(state.motor_a_percent));
    lv_label_set_text_fmt(
        s_motor_b_value,
        "%s | %d%%",
        motor_direction(state.motor_b_percent),
        std::abs(state.motor_b_percent));
    lv_bar_set_value(
        s_motor_a_bar, std::abs(state.motor_a_percent), LV_ANIM_OFF);
    lv_bar_set_value(
        s_motor_b_bar, std::abs(state.motor_b_percent), LV_ANIM_OFF);
    lv_obj_set_style_bg_color(
        s_motor_a_bar, motor_color(state.motor_a_percent), LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(
        s_motor_b_bar, motor_color(state.motor_b_percent), LV_PART_INDICATOR);

    if (state.servo_enabled) {
        lv_label_set_text_fmt(
            s_servo_value, "ON | %d us", state.servo_pulse_us);
    } else {
        lv_label_set_text_fmt(
            s_servo_value, "OFF | %d us", state.servo_pulse_us);
    }
    lv_bar_set_value(s_servo_bar, state.servo_pulse_us, LV_ANIM_OFF);

    if (state.failsafe_active) {
        lv_label_set_text(
            s_failsafe_label, "FAILSAFE | Browser link lost");
        lv_obj_set_style_text_color(
            s_failsafe_label, lv_color_hex(0xB42318), 0);
    } else if (state.motor_a_percent == 0 &&
               state.motor_b_percent == 0 && !state.servo_enabled) {
        lv_label_set_text(
            s_failsafe_label, "SAFE | Outputs off");
        lv_obj_set_style_text_color(
            s_failsafe_label, lv_color_hex(0x667085), 0);
    } else {
        lv_label_set_text(
            s_failsafe_label, "ACTIVE | Browser control");
        lv_obj_set_style_text_color(
            s_failsafe_label, lv_color_hex(0x067647), 0);
    }

    xSemaphoreGive(s_lvgl_mutex);
}
