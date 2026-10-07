#include "board_service.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iterator>

#include "board_config.h"
#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_io_expander_xl9555.h"
#include "esp_lcd_io_i2c.h"
#include "esp_lcd_io_i80.h"
#include "esp_lcd_st7796.h"
#include "esp_lcd_touch.h"
#include "esp_lcd_touch_ft6x36.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "sdkconfig.h"

namespace {

constexpr char kTag[] = "factory_board";
constexpr int kI2cTimeoutMs = 1000;
constexpr int kLvglTickMs = 5;
constexpr size_t kDrawBufferPixels =
    BOARD_LCD_H_RES * BOARD_LCD_DRAW_LINES;
constexpr uint8_t kFtRegGMode = 0xA4;
constexpr uint8_t kFtRegThreshold = 0x80;
constexpr uint8_t kFtRegPointRate = 0x88;

static_assert(CONFIG_XL9555_DEFAULT_OUTPUT == 0xFC5F,
              "Factory test requires the audited XL9555 safe latch value");
static_assert(
    BOARD_LCD_H_RES == BOARD_LCD_V_RES,
    "Factory display software rotation requires a square panel");

FactoryBoardState s_board = {};
esp_lcd_panel_io_handle_t s_touch_io = nullptr;
esp_lcd_touch_handle_t s_touch = nullptr;
SemaphoreHandle_t s_lcd_done = nullptr;
SemaphoreHandle_t s_lvgl_mutex = nullptr;
lv_display_t *s_display = nullptr;
lv_indev_t *s_touch_indev = nullptr;
esp_timer_handle_t s_tick_timer = nullptr;
#if BOARD_LCD_SOFTWARE_ROTATE_CW_90
uint16_t *s_rotation_buffer = nullptr;
#endif
FactoryUiActionCallback s_action_callback = nullptr;
void *s_action_context = nullptr;
std::atomic<uint32_t> s_touch_errors{0};
FactoryTestId s_manual_id = FactoryTestId::kCount;
FactoryTestId s_summary_retry_id = FactoryTestId::kCount;
lv_obj_t *s_camera_obj = nullptr;
bool s_camera_buttons_created = false;
lv_image_dsc_t s_camera_image = {};

uint16_t map_touch_axis(uint16_t value)
{
    const uint32_t clamped = std::min<uint32_t>(value, BOARD_TOUCH_RAW_RES - 1);
    return static_cast<uint16_t>(
        (clamped * (BOARD_LCD_H_RES - 1) + (BOARD_TOUCH_RAW_RES - 1) / 2) /
        (BOARD_TOUCH_RAW_RES - 1));
}

void scale_touch_coordinates(
    esp_lcd_touch_handle_t,
    uint16_t *x,
    uint16_t *y,
    uint16_t *,
    uint8_t *point_count,
    uint8_t max_points)
{
    if (x == nullptr || y == nullptr || point_count == nullptr) {
        return;
    }
    const uint8_t count = std::min(*point_count, max_points);
    for (uint8_t i = 0; i < count; ++i) {
        x[i] = map_touch_axis(x[i]);
        y[i] = map_touch_axis(y[i]);
    }
}

bool IRAM_ATTR lcd_transfer_done(
    esp_lcd_panel_io_handle_t,
    esp_lcd_panel_io_event_data_t *,
    void *context)
{
    BaseType_t task_woken = pdFALSE;
    xSemaphoreGiveFromISR(
        static_cast<SemaphoreHandle_t>(context), &task_woken);
    return task_woken == pdTRUE;
}

esp_err_t init_backlight()
{
    gpio_config_t config = {};
    config.pin_bit_mask = 1ULL << BOARD_LCD_BL;
    config.mode = GPIO_MODE_OUTPUT;
    ESP_RETURN_ON_ERROR(gpio_config(&config), kTag, "configure backlight");
    return gpio_set_level(BOARD_LCD_BL, 0);
}

esp_err_t init_main_i2c()
{
    i2c_master_bus_config_t config = {};
    config.i2c_port = BOARD_I2C_PORT;
    config.sda_io_num = BOARD_I2C_SDA;
    config.scl_io_num = BOARD_I2C_SCL;
    config.clk_source = I2C_CLK_SRC_DEFAULT;
    config.glitch_ignore_cnt = 7;
    config.flags.enable_internal_pullup = true;
    ESP_RETURN_ON_ERROR(
        i2c_new_master_bus(&config, &s_board.i2c_bus), kTag, "create I2C0");
    s_board.i2c_ready = true;
    return ESP_OK;
}

esp_err_t init_expander()
{
    ESP_RETURN_ON_ERROR(
        esp_io_expander_new_i2c_xl9555(
            s_board.i2c_bus, BOARD_XL9555_ADDR, &s_board.io_expander),
        kTag, "create XL9555");
    s_board.expander_ready = true;
    return board_service_apply_safe_outputs();
}

esp_err_t reset_touch()
{
    ESP_RETURN_ON_ERROR(
        board_service_set_output(BOARD_XL_P01_TOUCH_RST, true),
        kTag, "release touch reset");
    vTaskDelay(pdMS_TO_TICKS(10));
    ESP_RETURN_ON_ERROR(
        board_service_set_output(BOARD_XL_P01_TOUCH_RST, false),
        kTag, "assert touch reset");
    vTaskDelay(pdMS_TO_TICKS(15));
    ESP_RETURN_ON_ERROR(
        board_service_set_output(BOARD_XL_P01_TOUCH_RST, true),
        kTag, "release touch reset");
    vTaskDelay(pdMS_TO_TICKS(300));
    return ESP_OK;
}

esp_err_t init_lcd()
{
    s_lcd_done = xSemaphoreCreateBinary();
    if (s_lcd_done == nullptr) {
        return ESP_ERR_NO_MEM;
    }
    esp_lcd_i80_bus_handle_t i80_bus = nullptr;
    esp_lcd_i80_bus_config_t bus = {};
    bus.dc_gpio_num = BOARD_LCD_RS;
    bus.wr_gpio_num = BOARD_LCD_WR;
    bus.clk_src = LCD_CLK_SRC_PLL160M;
    std::fill_n(bus.data_gpio_nums, ESP_LCD_I80_BUS_WIDTH_MAX, GPIO_NUM_NC);
    const gpio_num_t data_pins[] = {
        BOARD_LCD_D0, BOARD_LCD_D1, BOARD_LCD_D2, BOARD_LCD_D3,
        BOARD_LCD_D4, BOARD_LCD_D5, BOARD_LCD_D6, BOARD_LCD_D7,
    };
    std::copy(std::begin(data_pins), std::end(data_pins), bus.data_gpio_nums);
    bus.bus_width = BOARD_LCD_DATA_WIDTH;
    bus.max_transfer_bytes =
        BOARD_LCD_H_RES * BOARD_LCD_DRAW_LINES * sizeof(uint16_t);
    bus.dma_burst_size = 64;
    ESP_RETURN_ON_ERROR(esp_lcd_new_i80_bus(&bus, &i80_bus), kTag, "create LCD bus");

    esp_lcd_panel_io_i80_config_t io = {};
    io.cs_gpio_num = BOARD_LCD_CS;
    io.pclk_hz = BOARD_LCD_PIXEL_CLOCK_HZ;
    io.trans_queue_depth = 4;
    io.on_color_trans_done = lcd_transfer_done;
    io.user_ctx = s_lcd_done;
    io.lcd_cmd_bits = 8;
    io.lcd_param_bits = 8;
    io.dc_levels.dc_data_level = 1;
    io.flags.swap_color_bytes = 1;
    ESP_RETURN_ON_ERROR(
        esp_lcd_new_panel_io_i80(i80_bus, &io, &s_board.lcd_io),
        kTag, "create LCD IO");

    esp_lcd_panel_dev_config_t panel = {};
    panel.reset_gpio_num = BOARD_LCD_RST;
    panel.rgb_ele_order = BOARD_LCD_RGB_ORDER;
    panel.bits_per_pixel = 16;
    ESP_RETURN_ON_ERROR(
        esp_lcd_new_panel_st7796(s_board.lcd_io, &panel, &s_board.lcd_panel),
        kTag, "create ST7796S");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_reset(s_board.lcd_panel), kTag, "reset LCD");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(s_board.lcd_panel), kTag, "init LCD");
    ESP_RETURN_ON_ERROR(
        esp_lcd_panel_invert_color(s_board.lcd_panel, BOARD_LCD_INVERT_COLOR),
        kTag, "invert LCD");
#if BOARD_LCD_SWAP_XY
    ESP_RETURN_ON_ERROR(
        esp_lcd_panel_swap_xy(s_board.lcd_panel, true),
        kTag, "swap LCD axes");
#endif
    ESP_RETURN_ON_ERROR(
        esp_lcd_panel_mirror(
            s_board.lcd_panel, BOARD_LCD_MIRROR_X, BOARD_LCD_MIRROR_Y),
        kTag, "mirror LCD");
    ESP_RETURN_ON_ERROR(
        esp_lcd_panel_disp_on_off(s_board.lcd_panel, true), kTag, "enable LCD");
    s_board.display_ready = true;
    return ESP_OK;
}

esp_err_t init_touch()
{
    esp_lcd_panel_io_i2c_config_t io = {};
    io.dev_addr = BOARD_TOUCH_ADDR;
    io.scl_speed_hz = BOARD_I2C_FREQ_HZ;
    io.control_phase_bytes = 1;
    io.lcd_cmd_bits = 8;
    io.lcd_param_bits = 8;
    io.flags.disable_control_phase = 1;
    io.transaction_timeout_ms = kI2cTimeoutMs;
    ESP_RETURN_ON_ERROR(
        esp_lcd_new_panel_io_i2c(s_board.i2c_bus, &io, &s_touch_io),
        kTag, "create touch IO");

    esp_lcd_touch_config_t touch = {};
    touch.x_max = BOARD_LCD_H_RES - 1;
    touch.y_max = BOARD_LCD_V_RES - 1;
    touch.rst_gpio_num = GPIO_NUM_NC;
    touch.int_gpio_num = GPIO_NUM_NC;
    touch.levels.reset = 0;
    touch.levels.interrupt = 0;
    touch.flags.swap_xy = BOARD_TOUCH_SWAP_XY;
    touch.flags.mirror_x = BOARD_TOUCH_MIRROR_X;
    touch.flags.mirror_y = BOARD_TOUCH_MIRROR_Y;
    touch.process_coordinates = scale_touch_coordinates;
    ESP_RETURN_ON_ERROR(
        esp_lcd_touch_new_i2c_ft6x36(s_touch_io, &touch, &s_touch),
        kTag, "create FT6336");

    const uint8_t threshold = BOARD_TOUCH_THRESHOLD;
    const uint8_t rate = BOARD_TOUCH_REPORT_RATE_HZ;
    const uint8_t mode = 0;
    ESP_RETURN_ON_ERROR(
        esp_lcd_panel_io_tx_param(s_touch_io, kFtRegThreshold, &threshold, 1),
        kTag, "set touch threshold");
    ESP_RETURN_ON_ERROR(
        esp_lcd_panel_io_tx_param(s_touch_io, kFtRegPointRate, &rate, 1),
        kTag, "set touch rate");
    ESP_RETURN_ON_ERROR(
        esp_lcd_panel_io_tx_param(s_touch_io, kFtRegGMode, &mode, 1),
        kTag, "set touch mode");
    ESP_LOGI(
        kTag,
        "FT6336 ready: raw=%dx%d logical=%dx%d swap_xy=%d mirror_x=%d mirror_y=%d",
        BOARD_TOUCH_RAW_RES, BOARD_TOUCH_RAW_RES,
        BOARD_LCD_H_RES, BOARD_LCD_V_RES,
        BOARD_TOUCH_SWAP_XY, BOARD_TOUCH_MIRROR_X, BOARD_TOUCH_MIRROR_Y);
    s_board.touch_ready = true;
    return ESP_OK;
}

void lvgl_flush(lv_display_t *display, const lv_area_t *area, uint8_t *pixels)
{
#if BOARD_LCD_SOFTWARE_ROTATE_CW_90
    const int source_width = lv_area_get_width(area);
    const int source_height = lv_area_get_height(area);
    const size_t pixel_count = static_cast<size_t>(lv_area_get_size(area));

    esp_err_t err = ESP_ERR_INVALID_SIZE;
    if (s_rotation_buffer != nullptr && pixel_count <= kDrawBufferPixels) {
        const auto *source = reinterpret_cast<const uint16_t *>(pixels);
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
            s_board.lcd_panel,
            BOARD_LCD_V_RES - 1 - area->y2,
            area->x1,
            BOARD_LCD_V_RES - area->y1,
            area->x2 + 1,
            s_rotation_buffer);
    }
#else
    const esp_err_t err = esp_lcd_panel_draw_bitmap(
        s_board.lcd_panel,
        area->x1,
        area->y1,
        area->x2 + 1,
        area->y2 + 1,
        pixels);
#endif
    if (err == ESP_OK) {
        if (xSemaphoreTake(s_lcd_done, pdMS_TO_TICKS(2000)) != pdTRUE) {
            ESP_LOGE(kTag, "LCD transfer timed out");
        }
    } else {
        ESP_LOGE(kTag, "LCD draw failed: %s", esp_err_to_name(err));
    }
    lv_display_flush_ready(display);
}

void lvgl_touch_read(lv_indev_t *, lv_indev_data_t *data)
{
    if (!s_board.touch_ready || esp_lcd_touch_read_data(s_touch) != ESP_OK) {
        if (s_board.touch_ready) {
            ++s_touch_errors;
        }
        data->state = LV_INDEV_STATE_RELEASED;
        return;
    }
    esp_lcd_touch_point_data_t point = {};
    uint8_t count = 0;
    if (esp_lcd_touch_get_data(s_touch, &point, &count, 1) != ESP_OK) {
        ++s_touch_errors;
        data->state = LV_INDEV_STATE_RELEASED;
        return;
    }
    if (count == 0) {
        data->state = LV_INDEV_STATE_RELEASED;
        return;
    }
    data->point.x = std::clamp<int32_t>(point.x, 0, BOARD_LCD_H_RES - 1);
    data->point.y = std::clamp<int32_t>(point.y, 0, BOARD_LCD_V_RES - 1);
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
        vTaskDelay(pdMS_TO_TICKS(std::clamp<uint32_t>(delay_ms, 5, 20)));
    }
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
    const size_t bytes = kDrawBufferPixels * sizeof(uint16_t);
    void *buffer_a = esp_lcd_i80_alloc_draw_buffer(
        s_board.lcd_io, bytes, MALLOC_CAP_DMA);
    void *buffer_b = esp_lcd_i80_alloc_draw_buffer(
        s_board.lcd_io, bytes, MALLOC_CAP_DMA);
#if BOARD_LCD_SOFTWARE_ROTATE_CW_90
    s_rotation_buffer = static_cast<uint16_t *>(
        esp_lcd_i80_alloc_draw_buffer(s_board.lcd_io, bytes, MALLOC_CAP_DMA));
#endif
    if (buffer_a == nullptr || buffer_b == nullptr
#if BOARD_LCD_SOFTWARE_ROTATE_CW_90
        || s_rotation_buffer == nullptr
#endif
    ) {
        return ESP_ERR_NO_MEM;
    }
    lv_display_set_buffers(
        s_display, buffer_a, buffer_b, bytes, LV_DISPLAY_RENDER_MODE_PARTIAL);
    if (s_board.touch_ready) {
        s_touch_indev = lv_indev_create();
        if (s_touch_indev == nullptr) {
            return ESP_ERR_NO_MEM;
        }
        lv_indev_set_type(s_touch_indev, LV_INDEV_TYPE_POINTER);
        lv_indev_set_read_cb(s_touch_indev, lvgl_touch_read);
        lv_indev_set_display(s_touch_indev, s_display);
    }
    const esp_timer_create_args_t timer = {
        .callback = lvgl_tick,
        .arg = nullptr,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "factory_lvgl",
        .skip_unhandled_events = true,
    };
    ESP_RETURN_ON_ERROR(
        esp_timer_create(&timer, &s_tick_timer), kTag, "create LVGL tick");
    ESP_RETURN_ON_ERROR(
        esp_timer_start_periodic(s_tick_timer, kLvglTickMs * 1000),
        kTag, "start LVGL tick");
    if (xTaskCreate(lvgl_task, "factory_lvgl", 6144, nullptr, 4, nullptr) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void emit_action(FactoryActionType type, FactoryTestId id, uint8_t value = 0)
{
    if (s_action_callback != nullptr) {
        FactoryAction action = {};
        action.type = type;
        action.test_id = id;
        action.value = value;
        s_action_callback(action, s_action_context);
    }
}

}  // namespace

namespace {

lv_color_t color(uint32_t value)
{
    return lv_color_hex(value);
}

bool ui_lock()
{
    return s_board.display_ready && s_display != nullptr &&
           s_lvgl_mutex != nullptr &&
           xSemaphoreTake(s_lvgl_mutex, portMAX_DELAY) == pdTRUE;
}

void ui_refresh_unlock()
{
    lv_refr_now(s_display);
    xSemaphoreGive(s_lvgl_mutex);
}

lv_obj_t *reset_screen(uint32_t background = 0x101419)
{
    lv_obj_t *screen = lv_screen_active();
    lv_obj_clean(screen);
    lv_obj_set_style_bg_color(screen, color(background), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(screen, 0, 0);
    lv_obj_set_scrollable(screen, false);
    s_camera_obj = nullptr;
    s_camera_buttons_created = false;
    return screen;
}

lv_obj_t *add_label(
    lv_obj_t *parent,
    const char *text,
    int x,
    int y,
    int width,
    uint32_t text_color = 0xF5F7FA,
    const lv_font_t *font = LV_FONT_DEFAULT,
    lv_text_align_t align = LV_TEXT_ALIGN_CENTER)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_label_set_text(label, text != nullptr ? text : "");
    lv_obj_set_pos(label, x, y);
    lv_obj_set_width(label, width);
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(label, align, 0);
    lv_obj_set_style_text_color(label, color(text_color), 0);
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_letter_space(label, 0, 0);
    return label;
}

void button_event(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_CLICKED) {
        return;
    }
    const uintptr_t code = reinterpret_cast<uintptr_t>(lv_event_get_user_data(event));
    if (code >= 0x100 && code < 0x109) {
        emit_action(
            FactoryActionType::kTouchHit,
            FactoryTestId::kDisplayTouch,
            static_cast<uint8_t>(code - 0x100));
        return;
    }
    const FactoryActionType type = static_cast<FactoryActionType>(code);
    FactoryTestId id = FactoryTestId::kCount;
    if (type == FactoryActionType::kManualPass ||
        type == FactoryActionType::kManualFail ||
        type == FactoryActionType::kRetry) {
        id = s_manual_id != FactoryTestId::kCount ? s_manual_id : s_summary_retry_id;
    }
    emit_action(type, id);
}

lv_obj_t *add_button(
    lv_obj_t *parent,
    const char *text,
    int x,
    int y,
    int width,
    int height,
    uint32_t accent,
    uintptr_t action_code)
{
    lv_obj_t *button = lv_button_create(parent);
    lv_obj_set_pos(button, x, y);
    lv_obj_set_size(button, width, height);
    lv_obj_set_style_radius(button, 6, 0);
    lv_obj_set_style_bg_color(button, color(0x20262E), 0);
    lv_obj_set_style_bg_opa(button, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(button, 2, 0);
    lv_obj_set_style_border_color(button, color(accent), 0);
    lv_obj_set_style_shadow_width(button, 0, 0);
    lv_obj_add_event_cb(
        button, button_event, LV_EVENT_CLICKED,
        reinterpret_cast<void *>(action_code));
    lv_obj_t *label = lv_label_create(button);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_color(label, color(0xF5F7FA), 0);
    lv_obj_set_style_text_letter_space(label, 0, 0);
    lv_obj_center(label);
    return button;
}

void add_manual_buttons(lv_obj_t *screen, bool allow_replay)
{
    add_button(
        screen, "PASS", 8, 266, allow_replay ? 96 : 146, 46,
        0x37D67A, static_cast<uintptr_t>(FactoryActionType::kManualPass));
    add_button(
        screen, "FAIL", allow_replay ? 112 : 166, 266,
        allow_replay ? 96 : 146, 46,
        0xFF5D5D, static_cast<uintptr_t>(FactoryActionType::kManualFail));
    if (allow_replay) {
        add_button(
            screen, "REPLAY", 216, 266, 96, 46,
            0xFFBE55, static_cast<uintptr_t>(FactoryActionType::kRetry));
    }
}

}  // namespace

esp_err_t board_service_init(FactoryUiActionCallback callback, void *context)
{
    s_action_callback = callback;
    s_action_context = context;
    esp_err_t first_error = ESP_OK;
    auto remember = [&first_error](esp_err_t err, const char *stage) {
        if (err != ESP_OK) {
            ESP_LOGE(kTag, "%s: %s", stage, esp_err_to_name(err));
            if (first_error == ESP_OK) {
                first_error = err;
            }
        }
    };

    remember(init_backlight(), "backlight init");
    remember(init_main_i2c(), "I2C0 init");
    bool expander_safe = false;
    if (s_board.i2c_ready) {
        const esp_err_t expander_err = init_expander();
        remember(expander_err, "XL9555 init");
        expander_safe = expander_err == ESP_OK;
    }
    if (expander_safe) {
        remember(reset_touch(), "touch reset");
    }
    remember(init_lcd(), "LCD init");
    if (s_board.i2c_ready && expander_safe) {
        remember(init_touch(), "touch init");
    }
    if (s_board.display_ready) {
        const esp_err_t lvgl_err = init_lvgl();
        remember(lvgl_err, "LVGL init");
        if (lvgl_err != ESP_OK) {
            s_board.display_ready = false;
        }
    }
    return first_error;
}

const FactoryBoardState &board_service_state()
{
    return s_board;
}

esp_err_t board_service_enable_backlight()
{
    if (!s_board.display_ready) {
        return ESP_ERR_INVALID_STATE;
    }
    vTaskDelay(pdMS_TO_TICKS(30));
    return gpio_set_level(BOARD_LCD_BL, 1);
}

esp_err_t board_service_set_output(uint32_t mask, bool high)
{
    if (!s_board.expander_ready) {
        return ESP_ERR_INVALID_STATE;
    }
    return esp_io_expander_set_level(s_board.io_expander, mask, high ? 1 : 0);
}

esp_err_t board_service_get_level(uint32_t mask, uint32_t *levels)
{
    if (!s_board.expander_ready || levels == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }
    return esp_io_expander_get_level(s_board.io_expander, mask, levels);
}

esp_err_t board_service_apply_safe_outputs()
{
    if (!s_board.expander_ready) {
        return ESP_ERR_INVALID_STATE;
    }
    constexpr uint32_t input_mask =
        BOARD_XL_P00_TOUCH_INT | BOARD_XL_P03_CHARGE_INT |
        BOARD_XL_P04_CHARGE_PG;

    // The driver loads CONFIG_XL9555_DEFAULT_OUTPUT into the output latch
    // before returning the handle. Make that safe latch live by changing the
    // direction first; set_level() intentionally rejects pins still in input mode.
    esp_err_t err = esp_io_expander_set_dir(
        s_board.io_expander, input_mask, IO_EXPANDER_INPUT);
    if (err == ESP_OK) {
        err = esp_io_expander_set_dir(
            s_board.io_expander, BOARD_XL_OUTPUT_MASK, IO_EXPANDER_OUTPUT);
    }
    if (err == ESP_OK) {
        err = esp_io_expander_set_level(
            s_board.io_expander, BOARD_XL_SAFE_LOW_MASK, 0);
    }
    if (err == ESP_OK) {
        err = esp_io_expander_set_level(
            s_board.io_expander, BOARD_XL_SAFE_HIGH_MASK, 1);
    }
    if (err != ESP_OK) {
        return err;
    }
    uint32_t levels = 0;
    err = esp_io_expander_get_level(
        s_board.io_expander, BOARD_XL_OUTPUT_MASK, &levels);
    if (err != ESP_OK) {
        return err;
    }
    return (levels & BOARD_XL_OUTPUT_MASK) == BOARD_XL_SAFE_HIGH_MASK ?
        ESP_OK : ESP_ERR_INVALID_STATE;
}

uint32_t board_service_touch_error_count()
{
    return s_touch_errors.load();
}

void board_service_reset_touch_error_count()
{
    s_touch_errors.store(0);
}

void board_ui_show_boot(
    const char *firmware_version,
    FactoryCameraProfile profile,
    const char *message)
{
    if (!ui_lock()) {
        return;
    }
    s_manual_id = FactoryTestId::kCount;
    lv_obj_t *screen = reset_screen();
    add_label(screen, "T-Bao-S31", 16, 16, 288, 0xF5F7FA, &lv_font_montserrat_24);
    add_label(screen, "Factory Test", 16, 50, 288, 0x4EA1FF, &lv_font_montserrat_20);
    char version[48] = {};
    std::snprintf(version, sizeof(version), "FW %s", firmware_version);
    add_label(screen, version, 16, 82, 288, 0xB8C1CC);
    char camera[48] = {};
    std::snprintf(
        camera, sizeof(camera), "Camera: %s", factory_camera_profile_name(profile));
    add_label(screen, camera, 16, 112, 288, 0xF5F7FA);
    lv_obj_t *ov2640 = add_button(
        screen, "OV2640", 20, 140, 132, 44,
        profile == FactoryCameraProfile::kOv2640 ? 0x37D67A : 0x637083,
        static_cast<uintptr_t>(FactoryActionType::kCameraOv2640));
    lv_obj_t *ov3660 = add_button(
        screen, "OV3660", 168, 140, 132, 44,
        profile == FactoryCameraProfile::kOv3660 ? 0x37D67A : 0x637083,
        static_cast<uintptr_t>(FactoryActionType::kCameraOv3660));
    (void)ov2640;
    (void)ov3660;
    add_button(
        screen, "START", 20, 202, 280, 54, 0x4EA1FF,
        static_cast<uintptr_t>(FactoryActionType::kRun));
    add_label(
        screen,
        message != nullptr ? message :
            (s_board.touch_ready ? "Touch START or send RUN" : "Send RUN on serial"),
        16, 272, 288,
        profile == FactoryCameraProfile::kUnset ? 0xFFBE55 : 0x8C96A3);
    ui_refresh_unlock();
}

void board_ui_show_status(
    const char *title,
    const char *detail,
    const char *footer)
{
    if (!ui_lock()) {
        return;
    }
    s_manual_id = FactoryTestId::kCount;
    lv_obj_t *screen = reset_screen();
    add_label(screen, title, 16, 34, 288, 0xF5F7FA, &lv_font_montserrat_24);
    add_label(
        screen, detail, 20, 104, 280, 0xB8C1CC,
        &lv_font_montserrat_20);
    if (footer != nullptr) {
        add_label(screen, footer, 16, 260, 288, 0x8C96A3);
    }
    ui_refresh_unlock();
}

void board_ui_show_meter(
    const char *title,
    const char *detail,
    float left_dbfs,
    float right_dbfs)
{
    if (!ui_lock()) {
        return;
    }
    lv_obj_t *screen = reset_screen();
    add_label(screen, title, 16, 18, 288, 0xF5F7FA, &lv_font_montserrat_20);
    add_label(screen, detail, 16, 52, 288, 0xB8C1CC);
    char left[32] = {};
    char right[32] = {};
    std::snprintf(left, sizeof(left), "LEFT  %.1f dBFS", left_dbfs);
    std::snprintf(right, sizeof(right), "RIGHT %.1f dBFS", right_dbfs);
    add_label(screen, left, 16, 92, 288, 0xF5F7FA, LV_FONT_DEFAULT, LV_TEXT_ALIGN_LEFT);
    lv_obj_t *left_bar = lv_bar_create(screen);
    lv_obj_set_pos(left_bar, 16, 118);
    lv_obj_set_size(left_bar, 288, 30);
    lv_bar_set_range(left_bar, 0, 60);
    lv_bar_set_value(left_bar, std::clamp<int>(static_cast<int>(left_dbfs + 60), 0, 60), LV_ANIM_OFF);
    add_label(screen, right, 16, 170, 288, 0xF5F7FA, LV_FONT_DEFAULT, LV_TEXT_ALIGN_LEFT);
    lv_obj_t *right_bar = lv_bar_create(screen);
    lv_obj_set_pos(right_bar, 16, 196);
    lv_obj_set_size(right_bar, 288, 30);
    lv_bar_set_range(right_bar, 0, 60);
    lv_bar_set_value(right_bar, std::clamp<int>(static_cast<int>(right_dbfs + 60), 0, 60), LV_ANIM_OFF);
    for (lv_obj_t *bar : {left_bar, right_bar}) {
        lv_obj_set_style_radius(bar, 3, LV_PART_MAIN);
        lv_obj_set_style_bg_color(bar, color(0x29313A), LV_PART_MAIN);
        lv_obj_set_style_bg_color(bar, color(0x37D67A), LV_PART_INDICATOR);
    }
    add_label(screen, "Speak or tap the enclosure", 16, 260, 288, 0x8C96A3);
    ui_refresh_unlock();
}

void board_ui_show_display_pattern(unsigned pattern)
{
    if (!ui_lock()) {
        return;
    }
    static constexpr uint32_t solids[] = {
        0xF80000, 0x00D060, 0x206CFF, 0xFFFFFF,
    };
    lv_obj_t *screen = reset_screen(pattern < 4 ? solids[pattern] : 0x000000);
    if (pattern == 4) {
        for (int i = 0; i < 8; ++i) {
            const uint32_t gray = static_cast<uint32_t>(i * 255 / 7);
            lv_obj_t *band = lv_obj_create(screen);
            lv_obj_remove_style_all(band);
            lv_obj_set_pos(band, i * 40, 0);
            lv_obj_set_size(band, 40, 320);
            lv_obj_set_style_bg_color(
                band, color((gray << 16) | (gray << 8) | gray), 0);
            lv_obj_set_style_bg_opa(band, LV_OPA_COVER, 0);
        }
    } else if (pattern == 5) {
        for (int y = 0; y < 8; ++y) {
            for (int x = 0; x < 8; ++x) {
                lv_obj_t *cell = lv_obj_create(screen);
                lv_obj_remove_style_all(cell);
                lv_obj_set_pos(cell, x * 40, y * 40);
                lv_obj_set_size(cell, 40, 40);
                lv_obj_set_style_bg_color(
                    cell, color(((x + y) & 1) ? 0xFFFFFF : 0x000000), 0);
                lv_obj_set_style_bg_opa(cell, LV_OPA_COVER, 0);
            }
        }
    }
    ui_refresh_unlock();
}

void board_ui_show_touch_grid(uint16_t hit_mask)
{
    if (!ui_lock()) {
        return;
    }
    lv_obj_t *screen = reset_screen();
    add_label(screen, "Touch all 9 targets", 10, 4, 300, 0xF5F7FA);
    for (int index = 0; index < 9; ++index) {
        const int column = index % 3;
        const int row = index / 3;
        const bool hit = (hit_mask & (1U << index)) != 0;
        lv_obj_t *button = add_button(
            screen, hit ? "OK" : "+",
            14 + column * 103, 31 + row * 94, 86, 78,
            hit ? 0x37D67A : 0x4EA1FF,
            static_cast<uintptr_t>(0x100 + index));
        lv_obj_set_style_radius(button, 39, 0);
        if (hit) {
            lv_obj_set_style_bg_color(button, color(0x1F6A46), 0);
        }
    }
    ui_refresh_unlock();
}

void board_ui_show_manual(
    FactoryTestId id,
    const char *title,
    const char *detail,
    bool allow_replay)
{
    if (!ui_lock()) {
        return;
    }
    s_manual_id = id;
    lv_obj_t *screen = reset_screen();
    add_label(screen, title, 16, 30, 288, 0xF5F7FA, &lv_font_montserrat_24);
    add_label(screen, detail, 20, 102, 280, 0xB8C1CC, &lv_font_montserrat_20);
    add_label(screen, "Operator confirmation", 16, 230, 288, 0xFFBE55);
    add_manual_buttons(screen, allow_replay);
    ui_refresh_unlock();
}

void board_ui_show_camera_frame(
    const uint16_t *pixels,
    FactoryTestId id,
    bool show_manual_buttons)
{
    if (pixels == nullptr || !ui_lock()) {
        return;
    }
    s_manual_id = show_manual_buttons ? id : FactoryTestId::kCount;
    if (s_camera_obj == nullptr || s_camera_image.data !=
            reinterpret_cast<const uint8_t *>(pixels)) {
        lv_obj_t *screen = reset_screen(0x000000);
        s_camera_image = {};
        s_camera_image.header.magic = LV_IMAGE_HEADER_MAGIC;
        s_camera_image.header.cf = LV_COLOR_FORMAT_RGB565;
        s_camera_image.header.w = BOARD_LCD_H_RES;
        s_camera_image.header.h = BOARD_LCD_V_RES;
        s_camera_image.header.stride = BOARD_LCD_H_RES * sizeof(uint16_t);
        s_camera_image.data_size =
            BOARD_LCD_H_RES * BOARD_LCD_V_RES * sizeof(uint16_t);
        s_camera_image.data = reinterpret_cast<const uint8_t *>(pixels);
        s_camera_obj = lv_image_create(screen);
        lv_image_set_src(s_camera_obj, &s_camera_image);
        lv_obj_set_pos(s_camera_obj, 0, 0);
    }
    lv_obj_invalidate(s_camera_obj);
    if (show_manual_buttons && !s_camera_buttons_created) {
        lv_obj_t *screen = lv_screen_active();
        lv_obj_t *shade = lv_obj_create(screen);
        lv_obj_remove_style_all(shade);
        lv_obj_set_pos(shade, 0, 258);
        lv_obj_set_size(shade, 320, 62);
        lv_obj_set_style_bg_color(shade, color(0x000000), 0);
        lv_obj_set_style_bg_opa(shade, LV_OPA_70, 0);
        add_manual_buttons(screen, true);
        s_camera_buttons_created = true;
    }
    ui_refresh_unlock();
}

void board_ui_clear_camera_frame()
{
    if (!ui_lock()) {
        return;
    }
    reset_screen();
    s_camera_image = {};
    s_camera_obj = nullptr;
    s_camera_buttons_created = false;
    ui_refresh_unlock();
}

void board_ui_show_summary(const FactoryRunResult &result)
{
    if (!ui_lock()) {
        return;
    }
    s_manual_id = FactoryTestId::kCount;
    s_summary_retry_id = FactoryTestId::kCount;
    lv_obj_t *screen = reset_screen();
    add_label(
        screen,
        result.overall_pass ? "FACTORY PASS" : "FACTORY FAIL",
        12, 8, 296,
        result.overall_pass ? 0x37D67A : 0xFF5D5D,
        &lv_font_montserrat_24);
    for (size_t i = 0; i < result.tests.size(); ++i) {
        const FactoryTestRecord &record = result.tests[i];
        if (record.status == FactoryTestStatus::kFail &&
            s_summary_retry_id == FactoryTestId::kCount) {
            s_summary_retry_id = record.id;
        }
        char line[48] = {};
        std::snprintf(
            line, sizeof(line), "%s  %s",
            factory_test_id_name(record.id),
            factory_test_status_name(record.status));
        const int column = static_cast<int>(i / 5);
        const int row = static_cast<int>(i % 5);
        add_label(
            screen, line, 10 + column * 154, 50 + row * 33, 148,
            record.status == FactoryTestStatus::kPass ? 0x37D67A : 0xFF5D5D,
            LV_FONT_DEFAULT, LV_TEXT_ALIGN_LEFT);
    }
    if (s_summary_retry_id != FactoryTestId::kCount) {
        add_button(
            screen, "RETRY FAIL", 8, 258, 146, 54, 0xFFBE55,
            static_cast<uintptr_t>(FactoryActionType::kRetry));
    }
    add_button(
        screen, "RETEST ALL",
        s_summary_retry_id != FactoryTestId::kCount ? 166 : 40,
        258,
        s_summary_retry_id != FactoryTestId::kCount ? 146 : 240,
        54, 0x4EA1FF,
        static_cast<uintptr_t>(FactoryActionType::kRetestAll));
    ui_refresh_unlock();
}
