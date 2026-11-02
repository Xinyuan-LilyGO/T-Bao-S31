#include "display_ui.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <iterator>

#include "board_config.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_io_expander.h"
#include "esp_io_expander_xl9555.h"
#include "esp_lcd_io_i2c.h"
#include "esp_lcd_io_i80.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_st7796.h"
#include "esp_lcd_touch.h"
#include "esp_lcd_touch_ft6x36.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lvgl.h"

namespace {

constexpr char kTag[] = "audio_ui";
constexpr int kI2cTimeoutMs = 1000;
constexpr int kLvglTickMs = 5;

constexpr uint8_t kFt6x36RegGMode = 0xA4;
constexpr uint8_t kFt6x36RegThreshold = 0x80;
constexpr uint8_t kFt6x36RegPointRate = 0x88;

i2c_master_bus_handle_t s_i2c_bus = nullptr;
esp_io_expander_handle_t s_xl9555 = nullptr;
esp_lcd_panel_io_handle_t s_lcd_io = nullptr;
esp_lcd_panel_handle_t s_lcd_panel = nullptr;
esp_lcd_panel_io_handle_t s_touch_io = nullptr;
esp_lcd_touch_handle_t s_touch = nullptr;
SemaphoreHandle_t s_lcd_transfer_done = nullptr;
SemaphoreHandle_t s_lvgl_mutex = nullptr;
lv_display_t *s_display = nullptr;
lv_indev_t *s_touch_indev = nullptr;
esp_timer_handle_t s_lvgl_tick_timer = nullptr;

lv_obj_t *s_status_es8389 = nullptr;
lv_obj_t *s_status_es7210 = nullptr;
lv_obj_t *s_status_i2s_rx = nullptr;
lv_obj_t *s_status_i2s_tx = nullptr;
lv_obj_t *s_status_dac_sound = nullptr;
lv_obj_t *s_mode_label = nullptr;
lv_obj_t *s_rms_label = nullptr;
lv_obj_t *s_peak_label = nullptr;
lv_obj_t *s_meter_bar = nullptr;
lv_obj_t *s_mic_button = nullptr;
lv_obj_t *s_dac_button = nullptr;
lv_obj_t *s_auto_button = nullptr;

UiActionCallback s_action_callback = nullptr;
void *s_action_context = nullptr;

uint16_t map_touch_axis(
    uint16_t value,
    uint16_t source_min,
    uint16_t source_max,
    uint16_t destination_max)
{
    if (source_max <= source_min) {
        return 0;
    }

    uint32_t clamped = value;
    if (clamped < source_min) {
        clamped = source_min;
    } else if (clamped > source_max) {
        clamped = source_max;
    }

    const uint32_t source_span = source_max - source_min;
    const uint32_t numerator =
        (clamped - source_min) * destination_max + source_span / 2;
    return static_cast<uint16_t>(numerator / source_span);
}

void scale_touch_coordinates(
    esp_lcd_touch_handle_t,
    uint16_t *x,
    uint16_t *y,
    uint16_t *,
    uint8_t *point_num,
    uint8_t max_point_num)
{
    if (point_num == nullptr || *point_num == 0 || x == nullptr || y == nullptr) {
        return;
    }

    const uint8_t count = std::min(*point_num, max_point_num);
    for (uint8_t i = 0; i < count; ++i) {
        x[i] = map_touch_axis(
            x[i], BOARD_TOUCH_RAW_X_MIN, BOARD_TOUCH_RAW_X_MAX,
            BOARD_TOUCH_H_RES - 1);
        y[i] = map_touch_axis(
            y[i], BOARD_TOUCH_RAW_Y_MIN, BOARD_TOUCH_RAW_Y_MAX,
            BOARD_TOUCH_V_RES - 1);
    }
}

esp_err_t init_backlight()
{
    gpio_config_t config = {};
    config.pin_bit_mask = 1ULL << BOARD_LCD_BL_PWM;
    config.mode = GPIO_MODE_OUTPUT;
    ESP_RETURN_ON_ERROR(gpio_config(&config), kTag, "configure backlight GPIO");
    return gpio_set_level(BOARD_LCD_BL_PWM, 0);
}

esp_err_t init_i2c()
{
    i2c_master_bus_config_t bus_config = {};
    bus_config.i2c_port = BOARD_I2C_PORT;
    bus_config.sda_io_num = BOARD_I2C_SDA;
    bus_config.scl_io_num = BOARD_I2C_SCL;
    bus_config.clk_source = I2C_CLK_SRC_DEFAULT;
    bus_config.glitch_ignore_cnt = 7;
    bus_config.flags.enable_internal_pullup = true;

    ESP_RETURN_ON_ERROR(
        i2c_new_master_bus(&bus_config, &s_i2c_bus), kTag, "create I2C bus");
    ESP_RETURN_ON_ERROR(
        esp_io_expander_new_i2c_xl9555(
            s_i2c_bus, BOARD_XL9555_I2C_ADDR, &s_xl9555),
        kTag, "create XL9555");

    ESP_LOGI(kTag, "I2C ready: SDA=%d SCL=%d", BOARD_I2C_SDA, BOARD_I2C_SCL);
    return ESP_OK;
}

esp_err_t reset_touch_through_xl9555()
{
    ESP_RETURN_ON_ERROR(
        esp_io_expander_set_dir(
            s_xl9555, BOARD_XL9555_P00_TOUCH_INT_MASK, IO_EXPANDER_INPUT),
        kTag, "configure TP_INT");
    ESP_RETURN_ON_ERROR(
        esp_io_expander_set_dir(
            s_xl9555, BOARD_XL9555_P01_TOUCH_RST_MASK, IO_EXPANDER_OUTPUT),
        kTag, "configure TP_RST");

    ESP_RETURN_ON_ERROR(
        esp_io_expander_set_level(
            s_xl9555, BOARD_XL9555_P01_TOUCH_RST_MASK, 1),
        kTag, "release touch reset");
    vTaskDelay(pdMS_TO_TICKS(10));
    ESP_RETURN_ON_ERROR(
        esp_io_expander_set_level(
            s_xl9555, BOARD_XL9555_P01_TOUCH_RST_MASK, 0),
        kTag, "assert touch reset");
    vTaskDelay(pdMS_TO_TICKS(15));
    ESP_RETURN_ON_ERROR(
        esp_io_expander_set_level(
            s_xl9555, BOARD_XL9555_P01_TOUCH_RST_MASK, 1),
        kTag, "release touch reset");
    vTaskDelay(pdMS_TO_TICKS(300));
    return ESP_OK;
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
        kTag, "create LCD IO");

    esp_lcd_panel_dev_config_t panel_config = {};
    panel_config.reset_gpio_num = BOARD_LCD_RST;
    panel_config.rgb_ele_order = BOARD_LCD_RGB_ORDER;
    panel_config.bits_per_pixel = 16;
    ESP_RETURN_ON_ERROR(
        esp_lcd_new_panel_st7796(s_lcd_io, &panel_config, &s_lcd_panel),
        kTag, "create ST7796S panel");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_reset(s_lcd_panel), kTag, "reset LCD");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(s_lcd_panel), kTag, "initialize LCD");

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
        kTag, "mirror LCD");
    ESP_RETURN_ON_ERROR(
        esp_lcd_panel_set_gap(s_lcd_panel, BOARD_LCD_X_GAP, BOARD_LCD_Y_GAP),
        kTag, "set LCD gap");
    ESP_RETURN_ON_ERROR(
        esp_lcd_panel_disp_on_off(s_lcd_panel, true), kTag, "enable LCD");
    return ESP_OK;
}

bool write_touch_register(uint8_t reg, uint8_t value)
{
    return esp_lcd_panel_io_tx_param(s_touch_io, reg, &value, 1) == ESP_OK;
}

esp_err_t configure_touch_controller()
{
    if (!write_touch_register(kFt6x36RegThreshold, BOARD_TOUCH_THRESHOLD) ||
        !write_touch_register(kFt6x36RegPointRate, BOARD_TOUCH_REPORT_RATE_HZ) ||
        !write_touch_register(kFt6x36RegGMode, BOARD_TOUCH_G_MODE)) {
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t init_touch()
{
    esp_lcd_panel_io_i2c_config_t io_config = {};
    io_config.dev_addr = BOARD_TOUCH_I2C_ADDR;
    io_config.scl_speed_hz = BOARD_TOUCH_I2C_FREQ_HZ;
    io_config.control_phase_bytes = 1;
    io_config.dc_bit_offset = 0;
    io_config.lcd_cmd_bits = 8;
    io_config.lcd_param_bits = 8;
    io_config.flags.disable_control_phase = 1;
    io_config.transaction_timeout_ms = kI2cTimeoutMs;
    ESP_RETURN_ON_ERROR(
        esp_lcd_new_panel_io_i2c(s_i2c_bus, &io_config, &s_touch_io),
        kTag, "create touch IO");

    esp_lcd_touch_config_t touch_config = {};
    touch_config.x_max = BOARD_TOUCH_H_RES - 1;
    touch_config.y_max = BOARD_TOUCH_V_RES - 1;
    touch_config.rst_gpio_num = GPIO_NUM_NC;
    touch_config.int_gpio_num = GPIO_NUM_NC;
    touch_config.levels.reset = 0;
    touch_config.levels.interrupt = BOARD_TOUCH_INT_ACTIVE_LEVEL;
    touch_config.flags.swap_xy = BOARD_TOUCH_SWAP_XY;
    touch_config.flags.mirror_x = BOARD_TOUCH_MIRROR_X;
    touch_config.flags.mirror_y = BOARD_TOUCH_MIRROR_Y;
    touch_config.process_coordinates = scale_touch_coordinates;

    ESP_RETURN_ON_ERROR(
        esp_lcd_touch_new_i2c_ft6x36(s_touch_io, &touch_config, &s_touch),
        kTag, "initialize FT6336");
    ESP_RETURN_ON_ERROR(
        configure_touch_controller(), kTag, "configure FT6336");
    return ESP_OK;
}

void lvgl_flush(
    lv_display_t *display,
    const lv_area_t *area,
    uint8_t *pixel_map)
{
    const esp_err_t err = esp_lcd_panel_draw_bitmap(
        s_lcd_panel,
        area->x1,
        area->y1,
        area->x2 + 1,
        area->y2 + 1,
        pixel_map);
    if (err == ESP_OK) {
        if (xSemaphoreTake(
                s_lcd_transfer_done, pdMS_TO_TICKS(kI2cTimeoutMs)) != pdTRUE) {
            ESP_LOGE(kTag, "LCD transfer timed out");
        }
    } else {
        ESP_LOGE(kTag, "LCD draw failed: %s", esp_err_to_name(err));
    }
    lv_display_flush_ready(display);
}

void lvgl_touch_read(lv_indev_t *, lv_indev_data_t *data)
{
    if (esp_lcd_touch_read_data(s_touch) != ESP_OK) {
        data->state = LV_INDEV_STATE_RELEASED;
        return;
    }

    esp_lcd_touch_point_data_t point = {};
    uint8_t point_count = 0;
    if (esp_lcd_touch_get_data(s_touch, &point, &point_count, 1) != ESP_OK ||
        point_count == 0) {
        data->state = LV_INDEV_STATE_RELEASED;
        return;
    }

    data->point.x = std::clamp<int32_t>(point.x, 0, BOARD_TOUCH_H_RES - 1);
    data->point.y = std::clamp<int32_t>(point.y, 0, BOARD_TOUCH_V_RES - 1);
    data->state = LV_INDEV_STATE_PRESSED;
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

const char *result_text(TestResult result)
{
    switch (result) {
    case TestResult::kPass:
        return "PASS";
    case TestResult::kFail:
        return "FAIL";
    case TestResult::kManual:
        return "MANUAL";
    case TestResult::kPending:
    default:
        return "--";
    }
}

lv_color_t result_color(TestResult result)
{
    switch (result) {
    case TestResult::kPass:
        return lv_color_hex(0x37D67A);
    case TestResult::kFail:
        return lv_color_hex(0xFF5D5D);
    case TestResult::kManual:
        return lv_color_hex(0xFFBE55);
    case TestResult::kPending:
    default:
        return lv_color_hex(0x8C96A3);
    }
}

void update_result_label(lv_obj_t *label, const char *name, TestResult result)
{
    lv_label_set_text_fmt(label, "%s  %s", name, result_text(result));
    lv_obj_set_style_text_color(label, result_color(result), 0);
}

void button_event(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_CLICKED || s_action_callback == nullptr) {
        return;
    }
    const auto value = reinterpret_cast<uintptr_t>(lv_event_get_user_data(event));
    s_action_callback(static_cast<TestMode>(value), s_action_context);
}

lv_obj_t *create_button(
    lv_obj_t *parent,
    const char *text,
    TestMode mode,
    lv_color_t color)
{
    lv_obj_t *button = lv_button_create(parent);
    lv_obj_set_size(button, 96, 44);
    lv_obj_set_style_radius(button, 6, 0);
    lv_obj_set_style_bg_color(button, lv_color_hex(0x28303A), 0);
    lv_obj_set_style_bg_opa(button, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(button, 1, 0);
    lv_obj_set_style_border_color(button, color, 0);
    lv_obj_set_style_shadow_width(button, 0, 0);
    lv_obj_add_event_cb(
        button,
        button_event,
        LV_EVENT_CLICKED,
        reinterpret_cast<void *>(static_cast<uintptr_t>(mode)));

    lv_obj_t *label = lv_label_create(button);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_color(label, lv_color_hex(0xF5F7FA), 0);
    lv_obj_set_style_text_letter_space(label, 0, 0);
    lv_obj_center(label);
    return button;
}

void set_button_active(lv_obj_t *button, bool active, lv_color_t color)
{
    lv_obj_set_style_bg_color(
        button, active ? color : lv_color_hex(0x28303A), 0);
    lv_obj_set_style_border_width(button, active ? 2 : 1, 0);
}

void create_ui()
{
    lv_obj_t *screen = lv_screen_active();
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x101419), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(screen, 0, 0);
    lv_obj_set_scrollable(screen, false);

    lv_obj_t *title = lv_label_create(screen);
    lv_label_set_text(title, "AUDIO TEST");
    lv_obj_set_width(title, 288);
    lv_obj_set_pos(title, 16, 8);
    lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(0xF5F7FA), 0);
    lv_obj_set_style_text_letter_space(title, 0, 0);

    lv_obj_t *status_labels[] = {
        s_status_es8389 = lv_label_create(screen),
        s_status_es7210 = lv_label_create(screen),
        s_status_i2s_rx = lv_label_create(screen),
        s_status_i2s_tx = lv_label_create(screen),
        s_status_dac_sound = lv_label_create(screen),
    };
    for (size_t i = 0; i < std::size(status_labels); ++i) {
        lv_obj_set_width(status_labels[i], 288);
        lv_obj_set_pos(status_labels[i], 18, 34 + static_cast<int>(i) * 19);
        lv_obj_set_style_text_letter_space(status_labels[i], 0, 0);
    }

    s_mode_label = lv_label_create(screen);
    lv_obj_set_width(s_mode_label, 288);
    lv_obj_set_pos(s_mode_label, 16, 133);
    lv_obj_set_style_text_color(s_mode_label, lv_color_hex(0xB8C1CC), 0);
    lv_obj_set_style_text_letter_space(s_mode_label, 0, 0);
    lv_label_set_long_mode(s_mode_label, LV_LABEL_LONG_DOT);

    s_rms_label = lv_label_create(screen);
    lv_obj_set_pos(s_rms_label, 16, 158);
    lv_obj_set_style_text_color(s_rms_label, lv_color_hex(0xF5F7FA), 0);
    lv_obj_set_style_text_letter_space(s_rms_label, 0, 0);

    s_peak_label = lv_label_create(screen);
    lv_obj_set_pos(s_peak_label, 16, 181);
    lv_obj_set_style_text_color(s_peak_label, lv_color_hex(0xF5F7FA), 0);
    lv_obj_set_style_text_letter_space(s_peak_label, 0, 0);

    s_meter_bar = lv_bar_create(screen);
    lv_obj_set_size(s_meter_bar, 288, 24);
    lv_obj_set_pos(s_meter_bar, 16, 211);
    lv_bar_set_range(s_meter_bar, 0, 100);
    lv_bar_set_value(s_meter_bar, 0, LV_ANIM_OFF);
    lv_obj_set_style_radius(s_meter_bar, 3, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_meter_bar, lv_color_hex(0x29313A), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_meter_bar, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(s_meter_bar, 3, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(s_meter_bar, lv_color_hex(0x37D67A), LV_PART_INDICATOR);

    lv_obj_t *button_row = lv_obj_create(screen);
    lv_obj_remove_style_all(button_row);
    lv_obj_set_size(button_row, 304, 46);
    lv_obj_set_pos(button_row, 8, 265);
    lv_obj_set_flex_flow(button_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(
        button_row,
        LV_FLEX_ALIGN_SPACE_BETWEEN,
        LV_FLEX_ALIGN_CENTER,
        LV_FLEX_ALIGN_CENTER);

    s_mic_button = create_button(
        button_row, "MIC", TestMode::kMic, lv_color_hex(0x37D67A));
    s_dac_button = create_button(
        button_row, "DAC", TestMode::kDac, lv_color_hex(0x4EA1FF));
    s_auto_button = create_button(
        button_row, "AUTO", TestMode::kAuto, lv_color_hex(0xFFBE55));

    AudioTestState initial = {};
    display_ui_update(initial);
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

    const size_t buffer_pixels = BOARD_LCD_H_RES * BOARD_LCD_DRAW_LINES;
    void *buffer_a = esp_lcd_i80_alloc_draw_buffer(
        s_lcd_io, buffer_pixels * sizeof(uint16_t), MALLOC_CAP_DMA);
    void *buffer_b = esp_lcd_i80_alloc_draw_buffer(
        s_lcd_io, buffer_pixels * sizeof(uint16_t), MALLOC_CAP_DMA);
    if (buffer_a == nullptr || buffer_b == nullptr) {
        return ESP_ERR_NO_MEM;
    }
    lv_display_set_buffers(
        s_display,
        buffer_a,
        buffer_b,
        buffer_pixels * sizeof(uint16_t),
        LV_DISPLAY_RENDER_MODE_PARTIAL);

    s_touch_indev = lv_indev_create();
    if (s_touch_indev == nullptr) {
        return ESP_ERR_NO_MEM;
    }
    lv_indev_set_type(s_touch_indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(s_touch_indev, lvgl_touch_read);
    lv_indev_set_display(s_touch_indev, s_display);

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
        kTag, "create LVGL tick timer");
    ESP_RETURN_ON_ERROR(
        esp_timer_start_periodic(s_lvgl_tick_timer, kLvglTickMs * 1000),
        kTag, "start LVGL tick timer");

    if (xTaskCreate(lvgl_task, "lvgl", 6144, nullptr, 4, nullptr) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

}  // namespace

esp_err_t display_ui_init(
    UiActionCallback callback,
    void *callback_context,
    DisplayUiHandles *handles_out)
{
    ESP_RETURN_ON_FALSE(handles_out != nullptr, ESP_ERR_INVALID_ARG, kTag, "null handles");
    s_action_callback = callback;
    s_action_context = callback_context;

    ESP_RETURN_ON_ERROR(init_backlight(), kTag, "initialize backlight");
    ESP_RETURN_ON_ERROR(init_i2c(), kTag, "initialize I2C");
    ESP_RETURN_ON_ERROR(
        reset_touch_through_xl9555(), kTag, "reset touch controller");
    ESP_RETURN_ON_ERROR(init_lcd(), kTag, "initialize LCD");
    ESP_RETURN_ON_ERROR(init_touch(), kTag, "initialize touch");
    ESP_RETURN_ON_ERROR(init_lvgl(), kTag, "initialize LVGL");
    ESP_RETURN_ON_ERROR(
        gpio_set_level(BOARD_LCD_BL_PWM, 1), kTag, "enable backlight");

    handles_out->i2c_bus = s_i2c_bus;
    handles_out->io_expander = s_xl9555;
    ESP_LOGI(kTag, "ST7796S, FT6336 and LVGL ready");
    return ESP_OK;
}

void display_ui_update(const AudioTestState &state)
{
    if (s_lvgl_mutex == nullptr || s_status_es8389 == nullptr) {
        return;
    }
    if (xSemaphoreTake(s_lvgl_mutex, portMAX_DELAY) != pdTRUE) {
        return;
    }

    update_result_label(s_status_es8389, "ES8389", state.es8389);
    update_result_label(s_status_es7210, "ES7210", state.es7210);
    update_result_label(s_status_i2s_rx, "I2S RX", state.i2s_rx);
    update_result_label(s_status_i2s_tx, "I2S TX", state.i2s_tx);
    update_result_label(s_status_dac_sound, "DAC SOUND", state.dac_sound);

    lv_label_set_text(s_mode_label, state.detail);
    lv_label_set_text_fmt(s_rms_label, "MIC  RMS  %.1f dBFS", state.rms_dbfs);
    lv_label_set_text_fmt(s_peak_label, "     PEAK %.1f dBFS", state.peak_dbfs);
    lv_bar_set_value(s_meter_bar, state.level_percent, LV_ANIM_OFF);

    set_button_active(
        s_mic_button,
        state.mode == TestMode::kMic,
        lv_color_hex(0x1F8A52));
    set_button_active(
        s_dac_button,
        state.mode == TestMode::kDac,
        lv_color_hex(0x236DB2));
    set_button_active(
        s_auto_button,
        state.mode == TestMode::kAuto,
        lv_color_hex(0x9A6B18));

    xSemaphoreGive(s_lvgl_mutex);
}
