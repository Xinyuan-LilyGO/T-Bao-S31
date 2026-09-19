#include <algorithm>
#include <cstdint>

#include "board_config.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_err.h"
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
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

namespace {

constexpr char kTag[] = "display_touch_test";

constexpr int kI2cTimeoutMs = 1000;

// FT6336U registers used to align the controller with I2C polling. The
// component driver sets a higher threshold and a low report rate by default.
constexpr uint8_t kFt6x36RegGMode = 0xA4;
constexpr uint8_t kFt6x36RegThreshold = 0x80;
constexpr uint8_t kFt6x36RegPointRate = 0x88;

constexpr uint16_t kColorRed = 0xF800;
constexpr uint16_t kColorGreen = 0x07E0;
constexpr uint16_t kColorBlue = 0x001F;
constexpr uint16_t kColorWhite = 0xFFFF;
constexpr uint16_t kColorBlack = 0x0000;
constexpr uint16_t kColorYellow = 0xFFE0;
constexpr uint16_t kColorCyan = 0x07FF;
constexpr uint16_t kColorMagenta = 0xF81F;

constexpr uint16_t kPatternColors[] = {
    kColorRed,
    kColorGreen,
    kColorBlue,
    kColorWhite,
    kColorBlack,
    kColorYellow,
    kColorCyan,
    kColorMagenta,
};

i2c_master_bus_handle_t s_i2c_bus = nullptr;
esp_io_expander_handle_t s_xl9555 = nullptr;
esp_lcd_panel_io_handle_t s_lcd_io = nullptr;
esp_lcd_panel_handle_t s_lcd_panel = nullptr;
esp_lcd_panel_io_handle_t s_touch_io = nullptr;
esp_lcd_touch_handle_t s_touch = nullptr;
SemaphoreHandle_t s_lcd_flush_done = nullptr;
SemaphoreHandle_t s_lcd_mutex = nullptr;
uint16_t *s_strip_buffer = nullptr;
uint16_t *s_marker_buffer = nullptr;
uint16_t s_last_raw_touch_x = 0;
uint16_t s_last_raw_touch_y = 0;
bool s_have_raw_touch = false;

void capture_raw_touch_coordinates(
    esp_lcd_touch_handle_t tp,
    uint16_t *x,
    uint16_t *y,
    uint16_t *strength,
    uint8_t *point_num,
    uint8_t max_point_num)
{
    (void)tp;
    (void)strength;
    (void)max_point_num;

    // This callback runs before esp_lcd_touch applies swap/mirror flags.
    if (point_num != nullptr && *point_num > 0) {
        s_last_raw_touch_x = x[0];
        s_last_raw_touch_y = y[0];
        s_have_raw_touch = true;
    }
}

bool write_touch_register(uint8_t reg, uint8_t value)
{
    return esp_lcd_panel_io_tx_param(s_touch_io, reg, &value, 1) == ESP_OK;
}

bool read_touch_register(uint8_t reg, uint8_t *value)
{
    return esp_lcd_panel_io_rx_param(s_touch_io, reg, value, 1) == ESP_OK;
}

bool configure_touch_controller()
{
    struct TouchRegisterSetting {
        uint8_t reg;
        uint8_t value;
    };

    const TouchRegisterSetting settings[] = {
        {kFt6x36RegThreshold, BOARD_TOUCH_THRESHOLD},
        {kFt6x36RegPointRate, BOARD_TOUCH_REPORT_RATE_HZ},
        {kFt6x36RegGMode, BOARD_TOUCH_G_MODE},
    };

    for (const TouchRegisterSetting &setting : settings) {
        if (!write_touch_register(setting.reg, setting.value)) {
            ESP_LOGE(kTag, "FT6336 register 0x%02X write failed", setting.reg);
            return false;
        }
    }

    uint8_t threshold = 0;
    uint8_t point_rate = 0;
    uint8_t g_mode = 0;
    if (read_touch_register(kFt6x36RegThreshold, &threshold) &&
        read_touch_register(kFt6x36RegPointRate, &point_rate) &&
        read_touch_register(kFt6x36RegGMode, &g_mode)) {
        ESP_LOGI(kTag, "FT6336 tuning: threshold=%u report_rate=%uHz G_MODE=0x%02X",
                 threshold, point_rate, g_mode);
    } else {
        ESP_LOGW(kTag, "FT6336 tuning write succeeded but readback failed");
    }

    return true;
}

void init_i2c()
{
    i2c_master_bus_config_t bus_config = {};
    bus_config.i2c_port = BOARD_I2C_PORT;
    bus_config.sda_io_num = BOARD_I2C_SDA;
    bus_config.scl_io_num = BOARD_I2C_SCL;
    bus_config.clk_source = I2C_CLK_SRC_DEFAULT;
    bus_config.glitch_ignore_cnt = 7;
    bus_config.flags.enable_internal_pullup = true;

    ESP_ERROR_CHECK(i2c_new_master_bus(&bus_config, &s_i2c_bus));

    ESP_ERROR_CHECK(esp_io_expander_new_i2c_xl9555(
        s_i2c_bus, BOARD_XL9555_I2C_ADDR, &s_xl9555));

    ESP_LOGI(kTag, "I2C ready: SDA=%d SCL=%d", BOARD_I2C_SDA, BOARD_I2C_SCL);
}

void reset_touch_through_xl9555()
{
    // TP_INT is an expander input and TP_RST is an active-low expander output.
    ESP_ERROR_CHECK(esp_io_expander_set_dir(
        s_xl9555, BOARD_XL9555_P00_TOUCH_INT_MASK, IO_EXPANDER_INPUT));
    ESP_ERROR_CHECK(esp_io_expander_set_dir(
        s_xl9555, BOARD_XL9555_P01_TOUCH_RST_MASK, IO_EXPANDER_OUTPUT));

    ESP_ERROR_CHECK(esp_io_expander_set_level(
        s_xl9555, BOARD_XL9555_P01_TOUCH_RST_MASK, 1));
    vTaskDelay(pdMS_TO_TICKS(10));
    ESP_ERROR_CHECK(esp_io_expander_set_level(
        s_xl9555, BOARD_XL9555_P01_TOUCH_RST_MASK, 0));
    vTaskDelay(pdMS_TO_TICKS(15));
    ESP_ERROR_CHECK(esp_io_expander_set_level(
        s_xl9555, BOARD_XL9555_P01_TOUCH_RST_MASK, 1));
    // Allow the FT6336U to finish its post-reset panel initialization before
    // the first register read. The reset line is provided by XL9555.
    vTaskDelay(pdMS_TO_TICKS(300));

    ESP_LOGI(kTag, "Touch reset via XL9555 P01 completed");
}

bool IRAM_ATTR lcd_color_trans_done(
    esp_lcd_panel_io_handle_t panel_io,
    esp_lcd_panel_io_event_data_t *event_data,
    void *user_ctx)
{
    (void)panel_io;
    (void)event_data;

    BaseType_t high_task_woken = pdFALSE;
    xSemaphoreGiveFromISR(static_cast<SemaphoreHandle_t>(user_ctx), &high_task_woken);
    return high_task_woken == pdTRUE;
}

void wait_for_lcd_transfer()
{
    if (xSemaphoreTake(s_lcd_flush_done, pdMS_TO_TICKS(kI2cTimeoutMs)) != pdTRUE) {
        ESP_LOGE(kTag, "Timed out waiting for LCD transfer");
    }
}

void draw_bitmap(int x_start, int y_start, int x_end, int y_end, const uint16_t *pixels)
{
    xSemaphoreTake(s_lcd_mutex, portMAX_DELAY);
    const esp_err_t err = esp_lcd_panel_draw_bitmap(
        s_lcd_panel, x_start, y_start, x_end, y_end, pixels);
    if (err == ESP_OK) {
        wait_for_lcd_transfer();
    } else {
        ESP_LOGE(kTag, "LCD draw failed: %s", esp_err_to_name(err));
    }
    xSemaphoreGive(s_lcd_mutex);
}

void init_lcd()
{
    s_lcd_flush_done = xSemaphoreCreateBinary();
    s_lcd_mutex = xSemaphoreCreateMutex();
    if (s_lcd_flush_done == nullptr || s_lcd_mutex == nullptr) {
        ESP_LOGE(kTag, "Unable to allocate LCD synchronization objects");
        abort();
    }

    esp_lcd_i80_bus_handle_t i80_bus = nullptr;
    const size_t max_transfer_bytes =
        BOARD_LCD_H_RES * BOARD_LCD_DMA_LINES * sizeof(uint16_t);
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
    bus_config.max_transfer_bytes = max_transfer_bytes;
    bus_config.dma_burst_size = 64;
    ESP_ERROR_CHECK(esp_lcd_new_i80_bus(&bus_config, &i80_bus));

    esp_lcd_panel_io_i80_config_t io_config = {};
    io_config.cs_gpio_num = BOARD_LCD_CS;
    io_config.pclk_hz = BOARD_LCD_PIXEL_CLOCK_HZ;
    io_config.trans_queue_depth = 10;
    io_config.on_color_trans_done = lcd_color_trans_done;
    io_config.user_ctx = s_lcd_flush_done;
    io_config.lcd_cmd_bits = 8;
    io_config.lcd_param_bits = 8;
    io_config.dc_levels.dc_idle_level = 0;
    io_config.dc_levels.dc_cmd_level = 0;
    io_config.dc_levels.dc_dummy_level = 0;
    io_config.dc_levels.dc_data_level = 1;
    io_config.flags.swap_color_bytes = 1;
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_i80(i80_bus, &io_config, &s_lcd_io));

    esp_lcd_panel_dev_config_t panel_config = {};
    panel_config.reset_gpio_num = BOARD_LCD_RST;
    panel_config.rgb_ele_order = BOARD_LCD_RGB_ORDER;
    panel_config.bits_per_pixel = 16;
    ESP_ERROR_CHECK(esp_lcd_new_panel_st7796(s_lcd_io, &panel_config, &s_lcd_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_reset(s_lcd_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(s_lcd_panel));

#if BOARD_LCD_INVERT_COLOR
    ESP_ERROR_CHECK(esp_lcd_panel_invert_color(s_lcd_panel, true));
#endif
#if BOARD_LCD_SWAP_XY
    ESP_ERROR_CHECK(esp_lcd_panel_swap_xy(s_lcd_panel, true));
#endif
    ESP_ERROR_CHECK(esp_lcd_panel_mirror(
        s_lcd_panel, BOARD_LCD_MIRROR_X, BOARD_LCD_MIRROR_Y));
    ESP_ERROR_CHECK(esp_lcd_panel_set_gap(
        s_lcd_panel, BOARD_LCD_X_GAP, BOARD_LCD_Y_GAP));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(s_lcd_panel, true));

    s_strip_buffer = static_cast<uint16_t *>(esp_lcd_i80_alloc_draw_buffer(
        s_lcd_io,
        BOARD_LCD_H_RES * BOARD_LCD_DMA_LINES * sizeof(uint16_t),
        MALLOC_CAP_DMA));
    s_marker_buffer = static_cast<uint16_t *>(esp_lcd_i80_alloc_draw_buffer(
        s_lcd_io, 21 * 21 * sizeof(uint16_t), MALLOC_CAP_DMA));
    if (s_strip_buffer == nullptr || s_marker_buffer == nullptr) {
        ESP_LOGE(kTag, "Unable to allocate LCD DMA buffers");
        abort();
    }

    ESP_LOGI(kTag, "ST7796S ready: %dx%d, 8-bit I80", BOARD_LCD_H_RES, BOARD_LCD_V_RES);
}

void draw_color_pattern()
{
    constexpr size_t color_count = sizeof(kPatternColors) / sizeof(kPatternColors[0]);
    const int band_height = BOARD_LCD_V_RES / static_cast<int>(color_count);

    for (size_t band = 0; band < color_count; ++band) {
        const int y_start = static_cast<int>(band) * band_height;
        const int y_end = (band + 1 == color_count)
                              ? BOARD_LCD_V_RES
                              : y_start + band_height;
        for (int row = y_start; row < y_end; row += BOARD_LCD_DMA_LINES) {
            const int rows = std::min(BOARD_LCD_DMA_LINES, y_end - row);
            const size_t pixels = static_cast<size_t>(BOARD_LCD_H_RES) * rows;
            std::fill_n(s_strip_buffer, pixels, kPatternColors[band]);
            draw_bitmap(0, row, BOARD_LCD_H_RES, row + rows, s_strip_buffer);
        }
    }
    ESP_LOGI(kTag, "Color-bar pattern displayed");
}

void draw_touch_marker(int x, int y)
{
    constexpr int marker_size = 21;
    const int half_size = marker_size / 2;
    const int x_start = std::max(0, x - half_size);
    const int y_start = std::max(0, y - half_size);
    const int x_end = std::min(BOARD_LCD_H_RES, x_start + marker_size);
    const int y_end = std::min(BOARD_LCD_V_RES, y_start + marker_size);
    const int width = x_end - x_start;
    const int height = y_end - y_start;

    for (int row = 0; row < height; ++row) {
        for (int column = 0; column < width; ++column) {
            const bool cross = row == y - y_start || column == x - x_start;
            s_marker_buffer[row * width + column] = cross ? kColorWhite : kColorBlack;
        }
    }
    draw_bitmap(x_start, y_start, x_end, y_end, s_marker_buffer);
}

enum class TouchReadResult {
    kNoTouch,
    kPoint,
    kError,
};

TouchReadResult read_touch_point(uint16_t *x, uint16_t *y, uint16_t *strength)
{
    static bool read_error_logged = false;
    const esp_err_t read_err = esp_lcd_touch_read_data(s_touch);
    if (read_err != ESP_OK) {
        if (!read_error_logged) {
            ESP_LOGW(kTag, "Touch data read failed: %s", esp_err_to_name(read_err));
            read_error_logged = true;
        }
        return TouchReadResult::kError;
    }

    esp_lcd_touch_point_data_t point = {};
    uint8_t point_count = 0;
    const esp_err_t data_err = esp_lcd_touch_get_data(s_touch, &point, &point_count, 1);
    if (data_err != ESP_OK) {
        if (!read_error_logged) {
            ESP_LOGW(kTag, "Touch point decode failed: %s", esp_err_to_name(data_err));
            read_error_logged = true;
        }
        return TouchReadResult::kError;
    }

    if (read_error_logged) {
        ESP_LOGI(kTag, "Touch data read recovered");
        read_error_logged = false;
    }

    if (point_count == 0) {
        return TouchReadResult::kNoTouch;
    }

    *x = point.x;
    *y = point.y;
    *strength = point.strength;
    return TouchReadResult::kPoint;
}

bool read_touch_interrupt_level(bool *active, bool *level_high_out)
{
    uint32_t level_mask = 0;
    static bool warned = false;
    const esp_err_t err = esp_io_expander_get_level(
        s_xl9555, BOARD_XL9555_P00_TOUCH_INT_MASK, &level_mask);
    if (err != ESP_OK) {
        if (!warned) {
            ESP_LOGW(kTag, "Unable to read XL9555 P00: %s", esp_err_to_name(err));
            warned = true;
        }
        return false;
    }

    if (warned) {
        ESP_LOGI(kTag, "XL9555 P00 read recovered");
        warned = false;
    }

    const bool level_high = (level_mask & BOARD_XL9555_P00_TOUCH_INT_MASK) != 0;
    if (level_high_out != nullptr) {
        *level_high_out = level_high;
    }
    *active = BOARD_TOUCH_INT_ACTIVE_LEVEL ? level_high : !level_high;
    return true;
}

bool init_touch()
{
    // Keep the FT6x36 bus settings explicit because the component macro was
    // authored for an older esp_lcd_panel_io_i2c_config_t layout.
    esp_lcd_panel_io_i2c_config_t io_config = {};
    io_config.dev_addr = BOARD_TOUCH_I2C_ADDR;
    io_config.scl_speed_hz = BOARD_I2C_FREQ_HZ;
    io_config.control_phase_bytes = 1;
    io_config.dc_bit_offset = 0;
    io_config.lcd_cmd_bits = 8;
    io_config.lcd_param_bits = 8;
    io_config.flags.disable_control_phase = 1;
    io_config.transaction_timeout_ms = kI2cTimeoutMs;
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_i2c(
        s_i2c_bus, &io_config, &s_touch_io));

    esp_lcd_touch_config_t touch_config = {};
    // x_max/y_max are the last valid pixel indices. This keeps software
    // mirroring from producing 320 on a 0..319 panel.
    touch_config.x_max = BOARD_LCD_H_RES - 1;
    touch_config.y_max = BOARD_LCD_V_RES - 1;
    // Reset and interrupt are routed through XL9555, not native ESP32 GPIOs.
    touch_config.rst_gpio_num = GPIO_NUM_NC;
    touch_config.int_gpio_num = GPIO_NUM_NC;
    touch_config.levels.reset = 0;
    touch_config.levels.interrupt = BOARD_TOUCH_INT_ACTIVE_LEVEL;
    touch_config.flags.swap_xy = BOARD_TOUCH_SWAP_XY;
    touch_config.flags.mirror_x = BOARD_TOUCH_MIRROR_X;
    touch_config.flags.mirror_y = BOARD_TOUCH_MIRROR_Y;
    touch_config.process_coordinates = capture_raw_touch_coordinates;

    const esp_err_t err = esp_lcd_touch_new_i2c_ft6x36(
        s_touch_io, &touch_config, &s_touch);
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "Unable to initialize FT6336/FT6336U at address 0x%02X: %s",
                 BOARD_TOUCH_I2C_ADDR, esp_err_to_name(err));
        return false;
    }

    if (!configure_touch_controller()) {
        return false;
    }

    ESP_LOGI(kTag,
             "FT6336/FT6336U ready: address=0x%02X, swap_xy=%d mirror_x=%d mirror_y=%d",
             BOARD_TOUCH_I2C_ADDR,
             BOARD_TOUCH_SWAP_XY,
             BOARD_TOUCH_MIRROR_X,
             BOARD_TOUCH_MIRROR_Y);
    return true;
}

void touch_task(void *arg)
{
    (void)arg;
    uint16_t x = 0;
    uint16_t y = 0;
    uint16_t strength = 0;
    bool was_touched = false;
    uint8_t missed_samples = 0;
    bool last_interrupt_active = false;
    bool have_interrupt_level = false;
    TickType_t last_interrupt_sample =
        xTaskGetTickCount() - pdMS_TO_TICKS(BOARD_TOUCH_INT_DIAGNOSTIC_MS);
    bool have_last_marker = false;
    int last_marker_x = -1;
    int last_marker_y = -1;
    uint16_t raw_min_x = UINT16_MAX;
    uint16_t raw_max_x = 0;
    uint16_t raw_min_y = UINT16_MAX;
    uint16_t raw_max_y = 0;
    int mapped_min_x = BOARD_LCD_H_RES;
    int mapped_max_x = -1;
    int mapped_min_y = BOARD_LCD_V_RES;
    int mapped_max_y = -1;
    bool have_gesture_range = false;

    while (true) {
        bool interrupt_active = false;
        bool interrupt_level_high = true;
        const TickType_t now = xTaskGetTickCount();
        if ((now - last_interrupt_sample) >=
                pdMS_TO_TICKS(BOARD_TOUCH_INT_DIAGNOSTIC_MS)) {
            last_interrupt_sample = now;
            if (read_touch_interrupt_level(&interrupt_active, &interrupt_level_high) &&
                (!have_interrupt_level || interrupt_active != last_interrupt_active)) {
                ESP_LOGI(kTag, "TP_INT %s (raw level=%s, diagnostic only)",
                         interrupt_active ? "asserted" : "deasserted",
                         interrupt_level_high ? "high" : "low");
                last_interrupt_active = interrupt_active;
                have_interrupt_level = true;
            }
        }

        const TouchReadResult result = read_touch_point(&x, &y, &strength);
        if (result == TouchReadResult::kPoint) {
            was_touched = true;
            missed_samples = 0;
            const int touch_x = std::clamp<int>(static_cast<int>(x), 0, BOARD_LCD_H_RES - 1);
            const int touch_y = std::clamp<int>(static_cast<int>(y), 0, BOARD_LCD_V_RES - 1);

            if (s_have_raw_touch) {
                raw_min_x = std::min(raw_min_x, s_last_raw_touch_x);
                raw_max_x = std::max(raw_max_x, s_last_raw_touch_x);
                raw_min_y = std::min(raw_min_y, s_last_raw_touch_y);
                raw_max_y = std::max(raw_max_y, s_last_raw_touch_y);
                have_gesture_range = true;
            }
            mapped_min_x = std::min(mapped_min_x, touch_x);
            mapped_max_x = std::max(mapped_max_x, touch_x);
            mapped_min_y = std::min(mapped_min_y, touch_y);
            mapped_max_y = std::max(mapped_max_y, touch_y);
            if (!have_last_marker || touch_x != last_marker_x || touch_y != last_marker_y) {
                if (s_have_raw_touch) {
                    ESP_LOGI(kTag, "touch raw=(%u,%u) mapped=(%d,%d) strength=%u",
                             s_last_raw_touch_x, s_last_raw_touch_y,
                             touch_x, touch_y, strength);
                } else {
                    ESP_LOGI(kTag, "touch mapped=(%d,%d) strength=%u",
                             touch_x, touch_y, strength);
                }
                draw_touch_marker(touch_x, touch_y);
                last_marker_x = touch_x;
                last_marker_y = touch_y;
                have_last_marker = true;
            }
        } else if (result == TouchReadResult::kNoTouch && was_touched) {
            if (missed_samples < UINT8_MAX) {
                ++missed_samples;
            }
            if (missed_samples >= BOARD_TOUCH_RELEASE_DEBOUNCE_SAMPLES) {
                was_touched = false;
                missed_samples = 0;
                have_last_marker = false;
                if (have_gesture_range) {
                    ESP_LOGI(kTag,
                             "touch released; raw range x=%u..%u y=%u..%u, mapped range x=%d..%d y=%d..%d",
                             raw_min_x, raw_max_x, raw_min_y, raw_max_y,
                             mapped_min_x, mapped_max_x, mapped_min_y, mapped_max_y);
                } else {
                    ESP_LOGI(kTag, "touch released");
                }
                raw_min_x = UINT16_MAX;
                raw_max_x = 0;
                raw_min_y = UINT16_MAX;
                raw_max_y = 0;
                mapped_min_x = BOARD_LCD_H_RES;
                mapped_max_x = -1;
                mapped_min_y = BOARD_LCD_V_RES;
                mapped_max_y = -1;
                have_gesture_range = false;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(BOARD_TOUCH_POLL_MS));
    }
}

void init_backlight()
{
    gpio_config_t config = {};
    config.pin_bit_mask = 1ULL << BOARD_LCD_BL_PWM;
    config.mode = GPIO_MODE_OUTPUT;
    ESP_ERROR_CHECK(gpio_config(&config));
    ESP_ERROR_CHECK(gpio_set_level(BOARD_LCD_BL_PWM, 0));
}

}  // namespace

extern "C" void app_main(void)
{
    ESP_LOGI(kTag, "T-Bao-S31 display and touch test");

    init_backlight();
    init_i2c();
    reset_touch_through_xl9555();
    init_lcd();
    draw_color_pattern();
    ESP_ERROR_CHECK(gpio_set_level(BOARD_LCD_BL_PWM, 1));

    if (init_touch()) {
        BaseType_t task_created = xTaskCreate(
            touch_task,
            "touch_task",
            4096,
            nullptr,
            5,
            nullptr);
        if (task_created != pdPASS) {
            ESP_LOGE(kTag, "Unable to create touch task");
        }
    }
}
