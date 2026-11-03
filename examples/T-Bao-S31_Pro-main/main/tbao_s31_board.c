/* Board Manager accessors for the T-Bao S31 shared control signals. */
#include "tbao_s31_board.h"

#include "esp_board_manager.h"

static esp_err_t get_expander(esp_io_expander_handle_t *out_expander)
{
    if (out_expander == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    void *raw = NULL;
    esp_err_t ret = esp_board_manager_get_device_handle("gpio_expander", &raw);
    if (ret != ESP_OK) {
        return ret;
    }
    if (raw == NULL || *(esp_io_expander_handle_t *)raw == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    *out_expander = *(esp_io_expander_handle_t *)raw;
    return ESP_OK;
}

esp_err_t tbao_s31_board_get_i2c_bus(i2c_master_bus_handle_t *out_bus)
{
    if (out_bus == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    void *raw = NULL;
    esp_err_t ret = esp_board_manager_get_periph_handle("i2c_master", &raw);
    if (ret != ESP_OK) {
        return ret;
    }
    if (raw == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    *out_bus = (i2c_master_bus_handle_t)raw;
    return ESP_OK;
}

esp_err_t tbao_s31_board_get_io_expander(esp_io_expander_handle_t *out_expander)
{
    return get_expander(out_expander);
}

static esp_err_t set_expander_level(uint32_t mask, bool enabled)
{
    esp_io_expander_handle_t expander = NULL;
    esp_err_t ret = get_expander(&expander);
    if (ret != ESP_OK) {
        return ret;
    }
    return esp_io_expander_set_level(expander, mask, enabled ? 1 : 0);
}

esp_err_t tbao_s31_board_set_5v_power(bool enabled)
{
    return set_expander_level(TBAO_S31_XL9555_P05_POWER_EN, enabled);
}

esp_err_t tbao_s31_board_set_speaker(bool enabled)
{
    return set_expander_level(TBAO_S31_XL9555_P07_SPK_CTRL, enabled);
}

esp_err_t tbao_s31_board_set_sd_power(bool enabled)
{
    return set_expander_level(TBAO_S31_XL9555_P10_SD_VDD_EN, enabled);
}
