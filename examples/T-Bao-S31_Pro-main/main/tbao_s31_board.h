/* T-Bao S31 board-level controls shared by the application. */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_err.h"
#include "esp_io_expander.h"

#ifdef __cplusplus
extern "C" {
#endif

#define TBAO_S31_I2C_PORT                  I2C_NUM_0
#define TBAO_S31_I2C_SDA                   GPIO_NUM_0
#define TBAO_S31_I2C_SCL                   GPIO_NUM_1

#define TBAO_S31_CAMERA_I2C_PORT           I2C_NUM_1
#define TBAO_S31_CAMERA_SDA                GPIO_NUM_3
#define TBAO_S31_CAMERA_SCL                GPIO_NUM_4
#define TBAO_S31_CAMERA_RESET              GPIO_NUM_45
#define TBAO_S31_CAMERA_XCLK               GPIO_NUM_55
#define TBAO_S31_CAMERA_PCLK               GPIO_NUM_54
#define TBAO_S31_CAMERA_HREF               GPIO_NUM_57
#define TBAO_S31_CAMERA_VSYNC              GPIO_NUM_56

#define TBAO_S31_CAMERA_FRAME_WIDTH        320
#define TBAO_S31_CAMERA_FRAME_HEIGHT       320
#define TBAO_S31_CAMERA_XCLK_HZ             20000000
#define TBAO_S31_CAMERA_JPEG_QUALITY        8

#define TBAO_S31_CAMERA_DATA0              GPIO_NUM_46
#define TBAO_S31_CAMERA_DATA1              GPIO_NUM_47
#define TBAO_S31_CAMERA_DATA2              GPIO_NUM_48
#define TBAO_S31_CAMERA_DATA3              GPIO_NUM_49
#define TBAO_S31_CAMERA_DATA4              GPIO_NUM_50
#define TBAO_S31_CAMERA_DATA5              GPIO_NUM_51
#define TBAO_S31_CAMERA_DATA6              GPIO_NUM_52
#define TBAO_S31_CAMERA_DATA7              GPIO_NUM_53

#define TBAO_S31_PMIC_ADDRESS              0x28
#define TBAO_S31_XL9555_ADDRESS             0x22 /* 7-bit runtime address */
#define TBAO_S31_XL9555_P05_POWER_EN       (1U << 5)
#define TBAO_S31_XL9555_P07_SPK_CTRL       (1U << 7)
#define TBAO_S31_XL9555_P10_SD_VDD_EN      (1U << 8)
#define TBAO_S31_XL9555_P11_MOTOR_EN       (1U << 9)

#define TBAO_S31_MOTOR_AIN1                GPIO_NUM_15
#define TBAO_S31_MOTOR_AIN2                GPIO_NUM_14
#define TBAO_S31_MOTOR_BIN1                GPIO_NUM_13
#define TBAO_S31_MOTOR_BIN2                GPIO_NUM_12
#define TBAO_S31_SERVO_PWM                 GPIO_NUM_5

esp_err_t tbao_s31_board_get_i2c_bus(i2c_master_bus_handle_t *out_bus);
esp_err_t tbao_s31_board_get_io_expander(esp_io_expander_handle_t *out_expander);
esp_err_t tbao_s31_board_set_5v_power(bool enabled);
esp_err_t tbao_s31_board_set_speaker(bool enabled);
esp_err_t tbao_s31_board_set_sd_power(bool enabled);

#ifdef __cplusplus
}
#endif
