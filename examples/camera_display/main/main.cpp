#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "board_config.h"
#include "driver/i2c_master.h"
#include "driver/jpeg_decode.h"
#include "driver/ledc.h"
#include "driver/parlio_rx.h"
extern "C" {
#include "esp_cam_io_parl.h"
}
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_lcd_io_i80.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_st7796.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

namespace {

constexpr char kTag[] = "camera_display";
constexpr int kFrameWidth = BOARD_CAMERA_FRAME_WIDTH;
constexpr int kFrameHeight = BOARD_CAMERA_FRAME_HEIGHT;
constexpr int kFrameX = (BOARD_LCD_H_RES - kFrameWidth) / 2;
constexpr int kFrameY = (BOARD_LCD_V_RES - kFrameHeight) / 2;
constexpr size_t kFramePixels = kFrameWidth * kFrameHeight;
constexpr size_t kFrameBytes = kFramePixels * sizeof(uint16_t);
constexpr size_t kStripPixels = BOARD_LCD_H_RES * BOARD_LCD_DMA_LINES;
static_assert(kFrameWidth == BOARD_LCD_H_RES && kFrameHeight == BOARD_LCD_V_RES);
static_assert(kFrameWidth == kFrameHeight);
static_assert(kFrameWidth % 16 == 0 && kFrameHeight % 16 == 0);
constexpr int kI2cTimeoutMs = 100;
constexpr uint8_t kPmicRevisionReg = 0x00;
constexpr uint8_t kPmicDvdd1VoutReg = 0x03;
constexpr uint8_t kPmicAvdd1VoutReg = 0x05;
constexpr uint8_t kPmicAvdd2VoutReg = 0x06;
constexpr uint8_t kPmicDvddSequenceReg = 0x0A;
constexpr uint8_t kPmicAvddSequenceReg = 0x0B;
constexpr uint8_t kPmicEnableReg = 0x0E;
constexpr int kDvddTargetMv = 504 + 8 * BOARD_PMIC_DVDD1_VOUT;
constexpr int kDovddTargetMv = 1384 + 8 * BOARD_PMIC_AVDD1_VOUT;
constexpr int kAvddTargetMv = 1384 + 8 * BOARD_PMIC_AVDD2_VOUT;
static_assert(kDvddTargetMv >= (BOARD_CAMERA_POWER_PROFILE_OV3660 ? 1425 : 1200) &&
              kDvddTargetMv <= (BOARD_CAMERA_POWER_PROFILE_OV3660 ? 1575 : 1360));
static_assert(kDovddTargetMv >= 2475 && kDovddTargetMv <= 3000);
static_assert(kAvddTargetMv >= 2600 && kAvddTargetMv <= 3000);

esp_lcd_panel_handle_t s_panel = nullptr;
SemaphoreHandle_t s_flush_done = nullptr;
uint16_t *s_strip = nullptr;
uint16_t *s_frame = nullptr;

// Five-pixel-wide digits 0-9 and uppercase letters A-Z, seven rows each.
constexpr uint8_t kFont[36][7] = {
    {14, 17, 19, 21, 25, 17, 14}, {4, 12, 4, 4, 4, 4, 14},
    {14, 17, 1, 2, 4, 8, 31}, {30, 1, 1, 14, 1, 1, 30},
    {2, 6, 10, 18, 31, 2, 2}, {31, 16, 30, 1, 1, 17, 14},
    {6, 8, 16, 30, 17, 17, 14}, {31, 1, 2, 4, 8, 8, 8},
    {14, 17, 17, 14, 17, 17, 14}, {14, 17, 17, 15, 1, 2, 12},
    {14, 17, 17, 31, 17, 17, 17}, {30, 17, 17, 30, 17, 17, 30},
    {14, 17, 16, 16, 16, 17, 14}, {30, 17, 17, 17, 17, 17, 30},
    {31, 16, 16, 30, 16, 16, 31}, {31, 16, 16, 30, 16, 16, 16},
    {14, 17, 16, 23, 17, 17, 14}, {17, 17, 17, 31, 17, 17, 17},
    {14, 4, 4, 4, 4, 4, 14}, {7, 2, 2, 2, 18, 18, 12},
    {17, 18, 20, 24, 20, 18, 17}, {16, 16, 16, 16, 16, 16, 31},
    {17, 27, 21, 21, 17, 17, 17}, {17, 25, 21, 19, 17, 17, 17},
    {14, 17, 17, 17, 17, 17, 14}, {30, 17, 17, 30, 16, 16, 16},
    {14, 17, 17, 17, 21, 18, 13}, {30, 17, 17, 30, 20, 18, 17},
    {15, 16, 16, 14, 1, 1, 30}, {31, 4, 4, 4, 4, 4, 4},
    {17, 17, 17, 17, 17, 17, 14}, {17, 17, 17, 17, 17, 10, 4},
    {17, 17, 17, 21, 21, 21, 10}, {17, 17, 10, 4, 10, 17, 17},
    {17, 17, 10, 4, 4, 4, 4}, {31, 1, 2, 4, 8, 16, 31},
};

bool IRAM_ATTR lcd_transfer_done(esp_lcd_panel_io_handle_t, esp_lcd_panel_io_event_data_t *, void *context)
{
    BaseType_t task_woken = pdFALSE;
    xSemaphoreGiveFromISR(static_cast<SemaphoreHandle_t>(context), &task_woken);
    return task_woken == pdTRUE;
}

void flush_bitmap(int x, int y, int width, int height, const void *pixels)
{
    ESP_ERROR_CHECK(esp_lcd_panel_draw_bitmap(s_panel, x, y, x + width, y + height, pixels));
    if (xSemaphoreTake(s_flush_done, pdMS_TO_TICKS(3000)) != pdTRUE) {
        ESP_LOGE(kTag, "LCD transfer timed out");
        abort();
    }
}

void clear_screen()
{
    std::fill_n(s_strip, kStripPixels, uint16_t{0});
    for (int y = 0; y < BOARD_LCD_V_RES; y += BOARD_LCD_DMA_LINES) {
        flush_bitmap(0, y, BOARD_LCD_H_RES,
                     std::min(BOARD_LCD_DMA_LINES, BOARD_LCD_V_RES - y), s_strip);
    }
}

void rotate_frame_clockwise(uint16_t *pixels)
{
    // Transpose, then reverse every row: source (x, y) -> (N - 1 - y, x).
    for (int y = 0; y < kFrameHeight; ++y) {
        for (int x = y + 1; x < kFrameWidth; ++x) {
            std::swap(pixels[y * kFrameWidth + x],
                      pixels[x * kFrameWidth + y]);
        }
    }
    for (int y = 0; y < kFrameHeight; ++y) {
        uint16_t *row = pixels + y * kFrameWidth;
        std::reverse(row, row + kFrameWidth);
    }
}

void display_frame(uint16_t *pixels)
{
    rotate_frame_clockwise(pixels);
    flush_bitmap(kFrameX, kFrameY, kFrameWidth, kFrameHeight, pixels);
}

const uint8_t *glyph(char c)
{
    if (c >= '0' && c <= '9') {
        return kFont[c - '0'];
    }
    if (c >= 'A' && c <= 'Z') {
        return kFont[c - 'A' + 10];
    }
    return nullptr;
}

void draw_text(uint16_t *canvas, int y, const char *text, uint16_t color)
{
    constexpr int char_width = 12;
    const int length = std::min<int>(std::strlen(text), BOARD_LCD_H_RES / char_width);
    const int start_x = (BOARD_LCD_H_RES - length * char_width) / 2;

    for (int i = 0; i < length; ++i) {
        const uint8_t *rows = glyph(text[i]);
        if (rows == nullptr) {
            continue;
        }
        for (int row = 0; row < 7; ++row) {
            for (int column = 0; column < 5; ++column) {
                if (rows[row] & (1U << (4 - column))) {
                    const int pixel = start_x + i * char_width + column * 2;
                    const int pixel_y = y + row * 2;
                    canvas[pixel_y * BOARD_LCD_H_RES + pixel] = color;
                    canvas[pixel_y * BOARD_LCD_H_RES + pixel + 1] = color;
                    canvas[(pixel_y + 1) * BOARD_LCD_H_RES + pixel] = color;
                    canvas[(pixel_y + 1) * BOARD_LCD_H_RES + pixel + 1] = color;
                }
            }
        }
    }
}

void show_status(const char *heading, const char *detail, const char *identity = "")
{
    std::fill_n(s_frame, kFramePixels, uint16_t{0});
    draw_text(s_frame, 112, heading, 0xFFFF);
    draw_text(s_frame, 144, detail, 0xFFE0);
    draw_text(s_frame, 176, identity, 0xFFFF);
    display_frame(s_frame);
}

void init_lcd()
{
    gpio_config_t bl_config = {};
    bl_config.pin_bit_mask = 1ULL << BOARD_LCD_BL;
    bl_config.mode = GPIO_MODE_OUTPUT;
    ESP_ERROR_CHECK(gpio_config(&bl_config));
    ESP_ERROR_CHECK(gpio_set_level(BOARD_LCD_BL, 0));

    s_flush_done = xSemaphoreCreateBinary();
    ESP_ERROR_CHECK(s_flush_done == nullptr ? ESP_ERR_NO_MEM : ESP_OK);

    esp_lcd_i80_bus_config_t bus_config = {};
    bus_config.dc_gpio_num = BOARD_LCD_RS;
    bus_config.wr_gpio_num = BOARD_LCD_WR;
    bus_config.clk_src = LCD_CLK_SRC_PLL160M;
    std::fill_n(bus_config.data_gpio_nums, ESP_LCD_I80_BUS_WIDTH_MAX, GPIO_NUM_NC);
    bus_config.data_gpio_nums[0] = BOARD_LCD_D0;
    bus_config.data_gpio_nums[1] = BOARD_LCD_D1;
    bus_config.data_gpio_nums[2] = BOARD_LCD_D2;
    bus_config.data_gpio_nums[3] = BOARD_LCD_D3;
    bus_config.data_gpio_nums[4] = BOARD_LCD_D4;
    bus_config.data_gpio_nums[5] = BOARD_LCD_D5;
    bus_config.data_gpio_nums[6] = BOARD_LCD_D6;
    bus_config.data_gpio_nums[7] = BOARD_LCD_D7;
    bus_config.bus_width = BOARD_LCD_DATA_WIDTH;
    bus_config.max_transfer_bytes = kFrameBytes;
    bus_config.dma_burst_size = 64;

    esp_lcd_i80_bus_handle_t bus = nullptr;
    ESP_ERROR_CHECK(esp_lcd_new_i80_bus(&bus_config, &bus));

    esp_lcd_panel_io_i80_config_t io_config = {};
    io_config.cs_gpio_num = BOARD_LCD_CS;
    io_config.pclk_hz = BOARD_LCD_PIXEL_CLOCK_HZ;
    io_config.trans_queue_depth = 10;
    io_config.on_color_trans_done = lcd_transfer_done;
    io_config.user_ctx = s_flush_done;
    io_config.lcd_cmd_bits = 8;
    io_config.lcd_param_bits = 8;
    io_config.dc_levels.dc_data_level = 1;
    io_config.flags.swap_color_bytes = 1;

    esp_lcd_panel_io_handle_t io = nullptr;
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_i80(bus, &io_config, &io));

    esp_lcd_panel_dev_config_t panel_config = {};
    panel_config.reset_gpio_num = BOARD_LCD_RST;
    panel_config.rgb_ele_order = BOARD_LCD_RGB_ORDER;
    panel_config.bits_per_pixel = 16;
    ESP_ERROR_CHECK(esp_lcd_new_panel_st7796(io, &panel_config, &s_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_reset(s_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(s_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_invert_color(s_panel, BOARD_LCD_INVERT_COLOR));
    ESP_ERROR_CHECK(esp_lcd_panel_swap_xy(s_panel, BOARD_LCD_SWAP_XY));
    ESP_ERROR_CHECK(esp_lcd_panel_mirror(s_panel, BOARD_LCD_MIRROR_X, BOARD_LCD_MIRROR_Y));
    ESP_ERROR_CHECK(esp_lcd_panel_set_gap(s_panel, BOARD_LCD_X_GAP, BOARD_LCD_Y_GAP));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(s_panel, true));

    s_strip = static_cast<uint16_t *>(esp_lcd_i80_alloc_draw_buffer(
        io, kStripPixels * sizeof(uint16_t), MALLOC_CAP_DMA));
    s_frame = static_cast<uint16_t *>(esp_lcd_i80_alloc_draw_buffer(
        io, kFrameBytes, MALLOC_CAP_DMA));
    ESP_ERROR_CHECK((s_strip == nullptr || s_frame == nullptr) ? ESP_ERR_NO_MEM : ESP_OK);
    clear_screen();
    ESP_ERROR_CHECK(gpio_set_level(BOARD_LCD_BL, 1));
}

esp_err_t read_pmic_reg(i2c_master_dev_handle_t pmic, uint8_t reg, uint8_t *value)
{
    return i2c_master_transmit_receive(pmic, &reg, 1, value, 1, kI2cTimeoutMs);
}

esp_err_t write_pmic_verified(i2c_master_dev_handle_t pmic, uint8_t reg, uint8_t value)
{
    const uint8_t command[] = {reg, value};
    esp_err_t err = i2c_master_transmit(pmic, command, sizeof(command), kI2cTimeoutMs);
    uint8_t actual = 0;
    if (err == ESP_OK) {
        err = read_pmic_reg(pmic, reg, &actual);
    }
    if (err == ESP_OK && actual != value) {
        ESP_LOGE(kTag, "SGM38121 reg 0x%02X: wrote 0x%02X, read 0x%02X", reg, value, actual);
        return ESP_ERR_INVALID_RESPONSE;
    }
    return err;
}

esp_err_t init_camera_power()
{
    i2c_master_bus_config_t bus_config = {};
    bus_config.i2c_port = BOARD_PMIC_I2C_PORT;
    bus_config.sda_io_num = BOARD_PMIC_SDA;
    bus_config.scl_io_num = BOARD_PMIC_SCL;
    bus_config.clk_source = I2C_CLK_SRC_DEFAULT;
    bus_config.glitch_ignore_cnt = 7;
    bus_config.flags.enable_internal_pullup = true;

    i2c_master_bus_handle_t bus = nullptr;
    esp_err_t err = i2c_new_master_bus(&bus_config, &bus);
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "SGM38121 I2C0: %s", esp_err_to_name(err));
        return err;
    }

    i2c_device_config_t config = {};
    config.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    config.device_address = BOARD_PMIC_ADDR;
    config.scl_speed_hz = BOARD_PMIC_I2C_HZ;
    i2c_master_dev_handle_t pmic = nullptr;
    err = i2c_master_bus_add_device(bus, &config, &pmic);
    const char *stage = "add device";
    if (err == ESP_OK) {
        uint8_t revision = 0;
        stage = "read revision";
        err = read_pmic_reg(pmic, kPmicRevisionReg, &revision);
        if (err == ESP_OK && revision != 0x80) {
            ESP_LOGE(kTag, "Unexpected SGM38121 revision: 0x%02X", revision);
            err = ESP_ERR_INVALID_RESPONSE;
        }
        uint8_t enabled = 0;
        if (err == ESP_OK) {
            stage = "read enabled rails";
            err = read_pmic_reg(pmic, kPmicEnableReg, &enabled);
        }
        // Stop previously enabled rails before changing the DOVDD voltage.
        if (err == ESP_OK && (enabled & BOARD_PMIC_CAMERA_RAILS)) {
            stage = "disable camera rails";
            err = write_pmic_verified(pmic, kPmicEnableReg,
                                      enabled & ~BOARD_PMIC_CAMERA_RAILS);
        }
        uint8_t sequence = 0;
        if (err == ESP_OK) {
            stage = "read DVDD sequence";
            err = read_pmic_reg(pmic, kPmicDvddSequenceReg, &sequence);
        }
        if (err == ESP_OK && (sequence & 0x0F)) {
            stage = "set DVDD1 register control";
            err = write_pmic_verified(pmic, kPmicDvddSequenceReg, sequence & 0xF0);
        }
        if (err == ESP_OK) {
            stage = "read AVDD sequence";
            err = read_pmic_reg(pmic, kPmicAvddSequenceReg, &sequence);
        }
        if (err == ESP_OK && sequence != 0) {
            stage = "set AVDD register control";
            err = write_pmic_verified(pmic, kPmicAvddSequenceReg, 0);
        }
        if (err == ESP_OK) {
            stage = "set DVDD1";
            err = write_pmic_verified(pmic, kPmicDvdd1VoutReg, BOARD_PMIC_DVDD1_VOUT);
        }
        if (err == ESP_OK) {
            stage = "set DOVDD";
            err = write_pmic_verified(pmic, kPmicAvdd1VoutReg, BOARD_PMIC_AVDD1_VOUT);
        }
        if (err == ESP_OK) {
            stage = "set AVDD";
            err = write_pmic_verified(pmic, kPmicAvdd2VoutReg, BOARD_PMIC_AVDD2_VOUT);
        }
        if (err == ESP_OK) {
            stage = "enable camera rails";
            err = write_pmic_verified(pmic, kPmicEnableReg,
                                      enabled | BOARD_PMIC_CAMERA_RAILS);
        }
        if (err == ESP_OK) {
            vTaskDelay(pdMS_TO_TICKS(20));
            ESP_LOGI(kTag, "SGM38121 revision=0x%02X enabled=0x%02X; voltage registers "
                     "DVDD=0x%02X (%dmV), DOVDD=0x%02X (%dmV), AVDD=0x%02X (%dmV)",
                     revision, enabled | BOARD_PMIC_CAMERA_RAILS,
                     BOARD_PMIC_DVDD1_VOUT, kDvddTargetMv,
                     BOARD_PMIC_AVDD1_VOUT, kDovddTargetMv,
                     BOARD_PMIC_AVDD2_VOUT, kAvddTargetMv);
            ESP_LOGI(kTag, "PMIC register readback is not a voltage measurement; measure the camera rails at J4");
        }
        const esp_err_t remove_err = i2c_master_bus_rm_device(pmic);
        if (err == ESP_OK) {
            err = remove_err;
        }
    }
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "SGM38121 %s failed: %s", stage, esp_err_to_name(err));
    }
    const esp_err_t del_err = i2c_del_master_bus(bus);
    return err == ESP_OK ? del_err : err;
}

esp_err_t prepare_camera_io()
{
    ledc_timer_config_t timer = {};
    timer.speed_mode = LEDC_LOW_SPEED_MODE;
    timer.duty_resolution = LEDC_TIMER_1_BIT;
    timer.timer_num = LEDC_TIMER_0;
    timer.freq_hz = BOARD_CAMERA_XCLK_HZ;
    timer.clk_cfg = LEDC_AUTO_CLK;
    esp_err_t err = ledc_timer_config(&timer);
    if (err != ESP_OK) {
        return err;
    }

    ledc_channel_config_t channel = {};
    channel.gpio_num = BOARD_CAMERA_XCLK;
    channel.speed_mode = LEDC_LOW_SPEED_MODE;
    channel.channel = LEDC_CHANNEL_0;
    channel.timer_sel = LEDC_TIMER_0;
    channel.duty = 1;
    err = ledc_channel_config(&channel);
    if (err != ESP_OK) {
        return err;
    }

    gpio_config_t reset = {};
    reset.pin_bit_mask = 1ULL << BOARD_CAMERA_RESET;
    reset.mode = GPIO_MODE_OUTPUT;
    ESP_ERROR_CHECK(gpio_set_level(BOARD_CAMERA_RESET, 0));
    err = gpio_config(&reset);
    if (err != ESP_OK) {
        return err;
    }
    vTaskDelay(pdMS_TO_TICKS(10));
    err = gpio_set_level(BOARD_CAMERA_RESET, 1);
    vTaskDelay(pdMS_TO_TICKS(30));
    return err;
}

void log_camera_bus_levels()
{
    ESP_LOGI(kTag, "Camera I2C%d idle sample: SDA GPIO%d=%d, SCL GPIO%d=%d",
             BOARD_CAMERA_I2C_PORT, BOARD_CAMERA_SDA, gpio_get_level(BOARD_CAMERA_SDA),
             BOARD_CAMERA_SCL, gpio_get_level(BOARD_CAMERA_SCL));
}

struct CameraIdentity {
    uint8_t address = 0;
    uint16_t pid = 0;
    bool has_pid = false;
    esp_err_t probe_error = ESP_ERR_NOT_FOUND;
    esp_err_t id_error = ESP_OK;
};

esp_err_t read_camera_reg(i2c_master_dev_handle_t device, uint16_t reg, bool wide, uint8_t *value)
{
    const uint8_t address[2] = {static_cast<uint8_t>(reg >> 8), static_cast<uint8_t>(reg)};
    return i2c_master_transmit_receive(device, wide ? address : &address[1],
                                       wide ? 2 : 1, value, 1, kI2cTimeoutMs);
}

CameraIdentity identify_camera(i2c_master_bus_handle_t bus)
{
    CameraIdentity identity;
    // Prefer the addresses supported by esp_cam_io_parl; scan for other sensors if none responds.
    constexpr uint8_t known_addresses[] = {0x30, 0x3C, 0x2A};
    for (uint8_t address : known_addresses) {
        const esp_err_t err = i2c_master_probe(bus, address, kI2cTimeoutMs);
        ESP_LOGI(kTag, "Camera probe 0x%02X: %s", address, esp_err_to_name(err));
        if (err == ESP_OK) {
            identity.address = address;
            identity.probe_error = ESP_OK;
            break;
        }
        if (err != ESP_ERR_NOT_FOUND) {
            identity.probe_error = err;
            return identity;
        }
    }
    if (identity.address == 0) {
        for (int address = 0x08; address <= 0x77; ++address) {
            if (address == 0x30 || address == 0x3C || address == 0x2A) {
                continue;
            }
            const esp_err_t err = i2c_master_probe(bus, address, 20);
            if (err == ESP_OK) {
                identity.address = static_cast<uint8_t>(address);
                identity.probe_error = ESP_OK;
                ESP_LOGW(kTag, "Camera bus ACK at unexpected address 0x%02X", address);
                break;
            }
            if (err != ESP_ERR_NOT_FOUND) {
                identity.probe_error = err;
                ESP_LOGE(kTag, "Camera bus probe 0x%02X: %s", address, esp_err_to_name(err));
                return identity;
            }
        }
    }
    if (identity.address == 0) {
        ESP_LOGE(kTag, "No camera I2C ACK at 0x30 (OV2640) or 0x3C (OV3660); "
                 "check J4 rails, RESET, XCLK, cable and SDA/SCL pull-ups");
        return identity;
    }

    i2c_device_config_t config = {};
    config.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    config.device_address = identity.address;
    config.scl_speed_hz = BOARD_CAMERA_I2C_HZ;
    i2c_master_dev_handle_t device = nullptr;
    identity.id_error = i2c_master_bus_add_device(bus, &config, &device);
    if (identity.id_error != ESP_OK) {
        return identity;
    }

    uint8_t high = 0;
    uint8_t low = 0;
    if (identity.address == 0x30) {
        const uint8_t bank[] = {0xFF, 0x01};
        identity.id_error = i2c_master_transmit(device, bank, sizeof(bank), kI2cTimeoutMs);
        if (identity.id_error == ESP_OK) {
            identity.id_error = read_camera_reg(device, 0x0A, false, &high);
        }
        if (identity.id_error == ESP_OK) {
            identity.id_error = read_camera_reg(device, 0x0B, false, &low);
        }
    } else if (identity.address == 0x3C || identity.address == 0x2A) {
        if (identity.address == 0x2A) {
            const uint8_t bank[] = {0x30, 0x08, 0x01};
            identity.id_error = i2c_master_transmit(device, bank, sizeof(bank), kI2cTimeoutMs);
        }
        const uint16_t reg = identity.address == 0x2A ? 0x3000 : 0x300A;
        if (identity.id_error == ESP_OK) {
            identity.id_error = read_camera_reg(device, reg, true, &high);
        }
        if (identity.id_error == ESP_OK) {
            identity.id_error = read_camera_reg(device, reg + 1, true, &low);
        }
    }
    if (identity.id_error == ESP_OK &&
        (identity.address == 0x30 || identity.address == 0x3C || identity.address == 0x2A)) {
        identity.pid = (static_cast<uint16_t>(high) << 8) | low;
        identity.has_pid = true;
        ESP_LOGI(kTag, "Camera ID at 0x%02X: PID/VER=0x%04X", identity.address, identity.pid);
    } else if (identity.id_error != ESP_OK) {
        ESP_LOGE(kTag, "Camera ID read at 0x%02X: %s", identity.address,
                 esp_err_to_name(identity.id_error));
    }
    const esp_err_t remove_err = i2c_master_bus_rm_device(device);
    if (identity.id_error == ESP_OK) {
        identity.id_error = remove_err;
    }
    return identity;
}

void report_camera_failure(const CameraIdentity &identity, esp_err_t error)
{
    char label[24] = {};
    if (identity.has_pid) {
        std::snprintf(label, sizeof(label), "PID 0X%04X", identity.pid);
    } else if (identity.address != 0) {
        std::snprintf(label, sizeof(label), "ADDR 0X%02X", identity.address);
    } else {
        std::snprintf(label, sizeof(label), "CHECK CABLE");
    }
    ESP_LOGE(kTag, "Camera init: %s, I2C=0x%02X, PID=%s0x%04X",
             esp_err_to_name(error), identity.address, identity.has_pid ? "" : "unknown ",
             identity.pid);
    show_status("CAMERA ERROR",
                error == ESP_ERR_NOT_SUPPORTED ? "UNSUPPORTED" :
                error == ESP_ERR_NOT_FOUND ? "NO I2C ACK" :
                error == ESP_ERR_TIMEOUT ? "I2C TIMEOUT" : "INIT FAILED",
                label);
}

bool init_camera(const CameraIdentity &identity, esp_cam_io_parl_handle_t *out_io,
                 esp_cam_sensor_io_parl_handle_t *out_sensor)
{
    esp_cam_sensor_io_parl_config_t sensor_config = {};
    sensor_config.pwdn_io = GPIO_NUM_NC; // PWDN is pulled low on the board.
    sensor_config.reset_io = GPIO_NUM_NC; // Already pulsed by prepare_camera_io().
    sensor_config.xclk_io = GPIO_NUM_NC; // Keep the single XCLK owner across probes.
    sensor_config.xclk_hz = BOARD_CAMERA_XCLK_HZ;
    sensor_config.sda_io = GPIO_NUM_NC;
    sensor_config.scl_io = GPIO_NUM_NC;
    sensor_config.i2c_port = BOARD_CAMERA_I2C_PORT;
    sensor_config.ledc_timer = LEDC_TIMER_0;
    sensor_config.ledc_channel = LEDC_CHANNEL_0;
    sensor_config.pixel_format = ESP_CAM_IO_PARL_PIXFORMAT_JPEG;
    sensor_config.frame_size = ESP_CAM_IO_PARL_FRAMESIZE_320X320;
    sensor_config.jpeg_quality = BOARD_CAMERA_JPEG_QUALITY;

    esp_err_t err = esp_cam_new_sensor_io_parl(&sensor_config, out_sensor);
    if (err != ESP_OK) {
        report_camera_failure(identity, err);
        return false;
    }

    const uint16_t pid = (*out_sensor)->id.PID;
    const char *model = pid == ESP_CAM_IO_PARL_OV2640_PID ? "OV2640" :
                        pid == ESP_CAM_IO_PARL_OV3660_PID ? "OV3660" :
                        pid == ESP_CAM_IO_PARL_OV5640_PID ? "OV5640" :
                        pid == ESP_CAM_IO_PARL_NT99141_PID ? "NT99141" : "UNKNOWN";
    ESP_LOGI(kTag, "Camera: %s PID=0x%04X, JPEG %dx%d", model, pid,
             kFrameWidth, kFrameHeight);

    esp_cam_io_parl_config_t io_config = {};
    io_config.data_width = 8;
    io_config.queue_frames = 1;
    io_config.fill_mode = ESP_CAM_IO_PARL_QUEUE_LATEST;
    io_config.frame_heap_caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
    io_config.pclk_io = BOARD_CAMERA_PCLK;
    io_config.pclk_sample_edge = ESP_CAM_IO_PARL_PCLK_POS;
    io_config.de_io = BOARD_CAMERA_HREF;
    io_config.hsync_io = GPIO_NUM_NC;
    io_config.vsync_io = GPIO_NUM_NC; // Not implemented by esp_cam_io_parl 0.1.0.
    std::fill_n(io_config.data_io, PARLIO_RX_UNIT_MAX_DATA_WIDTH, GPIO_NUM_NC);
    io_config.data_io[0] = BOARD_CAMERA_D0;
    io_config.data_io[1] = BOARD_CAMERA_D1;
    io_config.data_io[2] = BOARD_CAMERA_D2;
    io_config.data_io[3] = BOARD_CAMERA_D3;
    io_config.data_io[4] = BOARD_CAMERA_D4;
    io_config.data_io[5] = BOARD_CAMERA_D5;
    io_config.data_io[6] = BOARD_CAMERA_D6;
    io_config.data_io[7] = BOARD_CAMERA_D7;

    err = esp_cam_new_io_parl(&io_config, out_io);
    if (err == ESP_OK) {
        err = esp_cam_io_parl_enable(*out_io, true);
    }
    if (err == ESP_OK) {
        err = esp_cam_sensor_io_parl_connect(*out_io);
    }
    if (err == ESP_OK && pid == ESP_CAM_IO_PARL_OV2640_PID) {
        // The component selects the OV2640 CIF source for 320x320. Its 1:1
        // CIF window is only 300x296, so configure a real 600x600 crop from
        // the SVGA source and let the sensor downscale it to the LCD size.
        constexpr int kOv2640SvgaMode = 1;
        constexpr int kOv2640SquareOffsetX = 100;
        constexpr int kOv2640SquareSourceSize = 600;
        const int raw_err = (*out_sensor)->set_res_raw(
            *out_sensor, kOv2640SvgaMode, 0, 0, 0,
            kOv2640SquareOffsetX, 0,
            kOv2640SquareSourceSize, kOv2640SquareSourceSize,
            kFrameWidth, kFrameHeight, true, false);
        if (raw_err != 0) {
            ESP_LOGE(kTag, "OV2640 320x320 SVGA crop failed: %d", raw_err);
            err = ESP_ERR_INVALID_STATE;
        } else {
            ESP_LOGI(kTag, "OV2640 source: centered 600x600 SVGA crop -> 320x320");
        }
    }
    if (err == ESP_OK && !(*out_io)->use_soft_delimiter) {
        // esp_cam_io_parl 0.1.0 only submits this transaction in soft-delimiter
        // mode. Arm S31 HREF capture after the final sensor configuration.
        parlio_receive_config_t receive_config = {};
        receive_config.delimiter = (*out_io)->rx_delimiter;
        receive_config.flags.partial_rx_en = true;
        err = parlio_rx_unit_receive((*out_io)->rx_unit, (*out_io)->payload,
                                     (*out_io)->payload_size, &receive_config);
    }
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "Camera PARLIO: %s, sensor PID=0x%04X", esp_err_to_name(err), pid);
        char label[24];
        std::snprintf(label, sizeof(label), "PID 0X%04X", pid);
        show_status("CAMERA ERROR", "PARLIO FAILED", label);
        return false;
    }
    return true;
}

} // namespace

extern "C" void app_main(void)
{
    init_lcd();
    show_status("CAMERA", "STARTING");

    const esp_err_t power_err = init_camera_power();
    if (power_err != ESP_OK) {
        show_status("POWER ERROR", "SGM38121", "I2C 0X28");
        return;
    }

    i2c_master_bus_config_t bus_config = {};
    bus_config.i2c_port = BOARD_CAMERA_I2C_PORT;
    bus_config.sda_io_num = BOARD_CAMERA_SDA;
    bus_config.scl_io_num = BOARD_CAMERA_SCL;
    bus_config.clk_source = I2C_CLK_SRC_DEFAULT;
    bus_config.glitch_ignore_cnt = 7;
    bus_config.flags.enable_internal_pullup = true;

    i2c_master_bus_handle_t bus = nullptr;
    const esp_err_t i2c_err = i2c_new_master_bus(&bus_config, &bus);
    if (i2c_err != ESP_OK) {
        ESP_LOGE(kTag, "Camera I2C: %s", esp_err_to_name(i2c_err));
        show_status("CAMERA ERROR", "I2C FAILED");
        return;
    }
    const esp_err_t io_err = prepare_camera_io();
    if (io_err != ESP_OK) {
        ESP_LOGE(kTag, "Camera XCLK/RESET setup: %s", esp_err_to_name(io_err));
        show_status("CAMERA ERROR", "CLOCK FAILED");
        ESP_ERROR_CHECK(i2c_del_master_bus(bus));
        return;
    }

    log_camera_bus_levels();
    const CameraIdentity identity = identify_camera(bus);
    if (identity.probe_error != ESP_OK) {
        log_camera_bus_levels();
        report_camera_failure(identity, identity.probe_error);
        ESP_ERROR_CHECK(i2c_del_master_bus(bus));
        return;
    }
    if (identity.id_error != ESP_OK) {
        report_camera_failure(identity, identity.id_error);
        ESP_ERROR_CHECK(i2c_del_master_bus(bus));
        return;
    }
    const bool ov2640 = identity.address == 0x30 &&
                        (identity.pid >> 8) == ESP_CAM_IO_PARL_OV2640_PID;
    const bool ov3660 = identity.address == 0x3C &&
                        identity.pid == ESP_CAM_IO_PARL_OV3660_PID;
    if (!ov2640 && !ov3660) {
        report_camera_failure(identity, ESP_ERR_NOT_SUPPORTED);
        ESP_ERROR_CHECK(i2c_del_master_bus(bus));
        return;
    }
    if ((ov3660 && !BOARD_CAMERA_POWER_PROFILE_OV3660) ||
        (ov2640 && BOARD_CAMERA_POWER_PROFILE_OV3660)) {
        const char *model = ov3660 ? "OV3660" : "OV2640";
        ESP_LOGE(kTag, "%s requires a different DVDD profile; set "
                 "BOARD_CAMERA_POWER_PROFILE_OV3660 for the fitted module", model);
        show_status("CAMERA ERROR", "POWER PROFILE", model);
        ESP_ERROR_CHECK(i2c_del_master_bus(bus));
        return;
    }

    jpeg_decoder_handle_t jpeg_decoder = nullptr;
    jpeg_decode_engine_cfg_t engine_config = {};
    engine_config.intr_priority = 0;
    engine_config.timeout_ms = 80;
    const esp_err_t decoder_err = jpeg_new_decoder_engine(&engine_config, &jpeg_decoder);
    if (decoder_err != ESP_OK) {
        ESP_LOGE(kTag, "Hardware JPEG init: %s", esp_err_to_name(decoder_err));
        show_status("CAMERA ERROR", "JPEG ENGINE");
        return;
    }

    jpeg_decode_cfg_t decode_config = {};
    decode_config.output_format = JPEG_DECODE_OUT_FORMAT_RGB565;
    // BGR selects the driver's little-endian RGB565 layout. I80 swaps the
    // two bytes once when sending each pixel to the ST7796S.
    decode_config.rgb_order = JPEG_DEC_RGB_ELEMENT_ORDER_BGR;
    decode_config.conv_std = JPEG_YUV_RGB_CONV_STD_BT601;

    esp_cam_io_parl_handle_t camera_io = nullptr;
    esp_cam_sensor_io_parl_handle_t sensor = nullptr;
    if (!init_camera(identity, &camera_io, &sensor)) {
        ESP_ERROR_CHECK(jpeg_del_decoder_engine(jpeg_decoder));
        return;
    }
    ESP_LOGI(kTag, "ESP32-S31 hardware JPEG decoder -> RGB565, software CW90, full-frame LCD DMA");

    char camera_label[24];
    std::snprintf(camera_label, sizeof(camera_label), "PID 0X%04X", sensor->id.PID);
    uint32_t frames = 0;
    uint32_t failures = 0;
    bool error_visible = false;
    constexpr uint32_t kPerfWindowFrames = 30;
    uint32_t perf_frames = 0;
    uint64_t capture_time_us = 0;
    uint64_t decode_time_us = 0;
    uint64_t display_time_us = 0;
    uint64_t jpeg_bytes = 0;
    int64_t perf_window_start_us = esp_timer_get_time();
    while (true) {
        const int64_t capture_start_us = esp_timer_get_time();
        esp_cam_io_parl_trans_t frame = {};
        const esp_err_t receive_err = esp_cam_io_parl_receive(camera_io, &frame, 5000);
        const int64_t capture_done_us = esp_timer_get_time();
        if (receive_err != ESP_OK) {
            ESP_LOGW(kTag, "Camera capture: %s", esp_err_to_name(receive_err));
        }

        esp_err_t decode_err = ESP_FAIL;
        jpeg_decode_picture_info_t picture_info = {};
        uint32_t output_size = 0;
        const size_t frame_length = frame.length;
        if (receive_err == ESP_OK && frame.buffer != nullptr && frame.length > 0) {
            decode_err = jpeg_decoder_get_info(frame.buffer, frame.length, &picture_info);
            if (decode_err == ESP_OK &&
                (picture_info.width != kFrameWidth || picture_info.height != kFrameHeight)) {
                decode_err = ESP_ERR_INVALID_SIZE;
            }
            if (decode_err == ESP_OK) {
                decode_err = jpeg_decoder_process(
                    jpeg_decoder, &decode_config, frame.buffer, frame.length,
                    reinterpret_cast<uint8_t *>(s_frame), kFrameBytes, &output_size);
            }
            ESP_ERROR_CHECK(esp_cam_io_parl_free_buffer(&frame));
        } else if (frame.buffer != nullptr) {
            ESP_ERROR_CHECK(esp_cam_io_parl_free_buffer(&frame));
        }
        const int64_t decode_done_us = esp_timer_get_time();

        if (decode_err != ESP_OK || output_size != kFrameBytes) {
            if (receive_err == ESP_OK) {
                ESP_LOGW(kTag, "JPEG decode: %s, %lux%lu, output=%lu",
                         esp_err_to_name(decode_err),
                         static_cast<unsigned long>(picture_info.width),
                         static_cast<unsigned long>(picture_info.height),
                         static_cast<unsigned long>(output_size));
            }
            if (++failures == 3) {
                show_status("CAMERA ERROR",
                            receive_err == ESP_OK ? "DECODE FAILED" : "FRAME TIMEOUT",
                            camera_label);
                error_visible = true;
            }
        } else {
            if (error_visible) {
                clear_screen();
                error_visible = false;
            }
            display_frame(s_frame);
            const int64_t display_done_us = esp_timer_get_time();
            failures = 0;
            ++frames;
            ++perf_frames;
            capture_time_us += capture_done_us - capture_start_us;
            decode_time_us += decode_done_us - capture_done_us;
            display_time_us += display_done_us - decode_done_us;
            jpeg_bytes += frame_length;
            if (perf_frames == kPerfWindowFrames) {
                const uint64_t elapsed_us = display_done_us - perf_window_start_us;
                const uint32_t fps_x10 = static_cast<uint32_t>(
                    perf_frames * 10000000ULL / elapsed_us);
                const uint32_t capture_ms_x10 = static_cast<uint32_t>(
                    capture_time_us / (perf_frames * 100ULL));
                const uint32_t decode_ms_x10 = static_cast<uint32_t>(
                    decode_time_us / (perf_frames * 100ULL));
                const uint32_t display_ms_x10 = static_cast<uint32_t>(
                    display_time_us / (perf_frames * 100ULL));
                ESP_LOGI(kTag,
                         "Displayed %lu frames: %lu.%lu fps, avg capture=%lu.%lums "
                         "decode=%lu.%lums lcd=%lu.%lums jpeg=%lu bytes",
                         static_cast<unsigned long>(frames),
                         static_cast<unsigned long>(fps_x10 / 10),
                         static_cast<unsigned long>(fps_x10 % 10),
                         static_cast<unsigned long>(capture_ms_x10 / 10),
                         static_cast<unsigned long>(capture_ms_x10 % 10),
                         static_cast<unsigned long>(decode_ms_x10 / 10),
                         static_cast<unsigned long>(decode_ms_x10 % 10),
                         static_cast<unsigned long>(display_ms_x10 / 10),
                         static_cast<unsigned long>(display_ms_x10 % 10),
                         static_cast<unsigned long>(jpeg_bytes / perf_frames));
                perf_frames = 0;
                capture_time_us = 0;
                decode_time_us = 0;
                display_time_us = 0;
                jpeg_bytes = 0;
                perf_window_start_us = display_done_us;
            }
        }
        vTaskDelay(1);
    }
}
