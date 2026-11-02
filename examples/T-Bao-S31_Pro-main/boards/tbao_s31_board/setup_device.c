#include <string.h>
#include <stdint.h>

#include "esp_board_device.h"
#include "esp_err.h"
#include "esp_check.h"
#include "esp_io_expander.h"
#include "esp_io_expander_xl9555.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_st7796.h"
#include "esp_lcd_touch_ft6x36.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "TBAO_S31_SETUP";

#define TBAO_XL_P00_TOUCH_INT (1U << 0)
#define TBAO_XL_P01_TOUCH_RST (1U << 1)

#define TBAO_TOUCH_RAW_MAX_X 239U
#define TBAO_TOUCH_RAW_MAX_Y 239U
#define TBAO_TOUCH_LOGICAL_MAX_X 319U
#define TBAO_TOUCH_LOGICAL_MAX_Y 319U

#define TBAO_TOUCH_REG_THRESHOLD 0x80U
#define TBAO_TOUCH_REG_POINT_RATE 0x88U
#define TBAO_TOUCH_REG_G_MODE 0xA4U

static uint16_t scale_touch_coordinate(uint16_t value, uint16_t source_max, uint16_t destination_max)
{
    if (value >= source_max) {
        return destination_max;
    }
    return (uint16_t)(((uint32_t)value * destination_max + source_max / 2U) / source_max);
}

static void tbao_touch_process_coordinates(esp_lcd_touch_handle_t tp,
                                            uint16_t *x,
                                            uint16_t *y,
                                            uint16_t *strength,
                                            uint8_t *point_num,
                                            uint8_t max_point_num)
{
    (void)tp;
    (void)strength;
    if (x == NULL || y == NULL || point_num == NULL) {
        return;
    }

    uint8_t count = *point_num;
    if (count > max_point_num) {
        count = max_point_num;
    }
    for (uint8_t i = 0; i < count; ++i) {
        x[i] = scale_touch_coordinate(x[i], TBAO_TOUCH_RAW_MAX_X, TBAO_TOUCH_LOGICAL_MAX_X);
        y[i] = scale_touch_coordinate(y[i], TBAO_TOUCH_RAW_MAX_Y, TBAO_TOUCH_LOGICAL_MAX_Y);
    }
}

static esp_err_t tune_touch_controller(esp_lcd_panel_io_handle_t io)
{
    static const struct {
        uint8_t reg;
        uint8_t value;
    } settings[] = {
        {TBAO_TOUCH_REG_THRESHOLD, 25},
        {TBAO_TOUCH_REG_POINT_RATE, 30},
        {TBAO_TOUCH_REG_G_MODE, 0},
    };

    for (size_t i = 0; i < sizeof(settings) / sizeof(settings[0]); ++i) {
        uint8_t value = settings[i].value;
        ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, settings[i].reg, &value, 1),
                            TAG, "configure FT6336 register 0x%02X", settings[i].reg);
    }

    ESP_LOGI(TAG, "FT6336 touch tuned: threshold=25 report_rate=30Hz G_MODE=0");
    return ESP_OK;
}

esp_err_t io_expander_factory_entry_t(i2c_master_bus_handle_t i2c_handle,
                                      const uint16_t dev_addr,
                                      esp_io_expander_handle_t *handle_ret)
{
    return esp_io_expander_new_i2c_xl9555(i2c_handle, dev_addr, handle_ret);
}

esp_err_t lcd_panel_factory_entry_t(esp_lcd_panel_io_handle_t io,
                                     const esp_lcd_panel_dev_config_t *panel_dev_config,
                                     esp_lcd_panel_handle_t *ret_panel)
{
    esp_lcd_panel_dev_config_t panel_config = {0};
    memcpy(&panel_config, panel_dev_config, sizeof(panel_config));
    esp_err_t ret = esp_lcd_new_panel_st7796(io, &panel_config, ret_panel);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "create ST7796S panel failed: %s", esp_err_to_name(ret));
    }
    return ret;
}

static esp_err_t reset_touch_from_xl9555(void)
{
    esp_io_expander_handle_t *expander = NULL;
    esp_err_t ret = esp_board_device_get_handle("gpio_expander", (void **)&expander);
    if (ret != ESP_OK || expander == NULL || *expander == NULL) {
        ESP_LOGE(TAG, "get XL9555 handle failed: %s", esp_err_to_name(ret));
        return ret == ESP_OK ? ESP_ERR_INVALID_STATE : ret;
    }

    ESP_RETURN_ON_ERROR(esp_io_expander_set_dir(*expander, TBAO_XL_P00_TOUCH_INT,
                                                IO_EXPANDER_INPUT), TAG,
                        "configure touch interrupt");
    ESP_RETURN_ON_ERROR(esp_io_expander_set_dir(*expander, TBAO_XL_P01_TOUCH_RST,
                                                IO_EXPANDER_OUTPUT), TAG,
                        "configure touch reset");
    ESP_RETURN_ON_ERROR(esp_io_expander_set_level(*expander, TBAO_XL_P01_TOUCH_RST, 1),
                        TAG, "release touch reset");
    vTaskDelay(pdMS_TO_TICKS(10));
    ESP_RETURN_ON_ERROR(esp_io_expander_set_level(*expander, TBAO_XL_P01_TOUCH_RST, 0),
                        TAG, "assert touch reset");
    vTaskDelay(pdMS_TO_TICKS(15));
    ESP_RETURN_ON_ERROR(esp_io_expander_set_level(*expander, TBAO_XL_P01_TOUCH_RST, 1),
                        TAG, "release touch reset");
    vTaskDelay(pdMS_TO_TICKS(300));
    return ESP_OK;
}

esp_err_t lcd_touch_factory_entry_t(esp_lcd_panel_io_handle_t io,
                                    const esp_lcd_touch_config_t *touch_dev_config,
                                    esp_lcd_touch_handle_t *ret_touch)
{
    esp_err_t ret = reset_touch_from_xl9555();
    if (ret != ESP_OK) {
        return ret;
    }

    esp_lcd_touch_config_t touch_config = {0};
    memcpy(&touch_config, touch_dev_config, sizeof(touch_config));
    touch_config.x_max = TBAO_TOUCH_LOGICAL_MAX_X;
    touch_config.y_max = TBAO_TOUCH_LOGICAL_MAX_Y;
    touch_config.flags.swap_xy = 0;
    touch_config.flags.mirror_x = 1;
    touch_config.flags.mirror_y = 0;
    touch_config.rst_gpio_num = GPIO_NUM_NC;
    touch_config.int_gpio_num = GPIO_NUM_NC;
    touch_config.process_coordinates = tbao_touch_process_coordinates;
    ret = esp_lcd_touch_new_i2c_ft6x36(io, &touch_config, ret_touch);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "create FT6336/FT6336U touch failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = tune_touch_controller(io);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "FT6336 touch tuning failed: %s; keeping driver defaults",
                 esp_err_to_name(ret));
        return ESP_OK;
    }
    return ESP_OK;
}
