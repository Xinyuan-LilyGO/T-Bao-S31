#pragma once

#include "driver/gpio.h"
#include "driver/i2c_types.h"
#include "esp_lcd_panel_vendor.h"

// ST7796S: 1.54 inch, 320x320 BGR, 8-bit I80.
#define BOARD_LCD_H_RES                  320
#define BOARD_LCD_V_RES                  320
#define BOARD_LCD_DATA_WIDTH             8
#define BOARD_LCD_PIXEL_CLOCK_HZ         (10 * 1000 * 1000)
#define BOARD_LCD_DRAW_LINES             32

#define BOARD_LCD_RS                     GPIO_NUM_18
#define BOARD_LCD_WR                     GPIO_NUM_17
#define BOARD_LCD_CS                     GPIO_NUM_19
#define BOARD_LCD_RST                    GPIO_NUM_35
#define BOARD_LCD_BL_PWM                 GPIO_NUM_16

#define BOARD_LCD_D0                     GPIO_NUM_44
#define BOARD_LCD_D1                     GPIO_NUM_43
#define BOARD_LCD_D2                     GPIO_NUM_42
#define BOARD_LCD_D3                     GPIO_NUM_40
#define BOARD_LCD_D4                     GPIO_NUM_39
#define BOARD_LCD_D5                     GPIO_NUM_38
#define BOARD_LCD_D6                     GPIO_NUM_37
#define BOARD_LCD_D7                     GPIO_NUM_36

#define BOARD_LCD_RGB_ORDER              LCD_RGB_ELEMENT_ORDER_BGR
#define BOARD_LCD_INVERT_COLOR           1
#define BOARD_LCD_SWAP_XY                0
#define BOARD_LCD_MIRROR_X               1
#define BOARD_LCD_MIRROR_Y               0
#define BOARD_LCD_X_GAP                  0
#define BOARD_LCD_Y_GAP                  0
#define BOARD_LCD_SOFTWARE_ROTATE_CW_90  1

// Main I2C bus and XL9555 power-board expander.
#define BOARD_I2C_PORT                   I2C_NUM_0
#define BOARD_I2C_SDA                    GPIO_NUM_0
#define BOARD_I2C_SCL                    GPIO_NUM_1
#define BOARD_XL9555_I2C_ADDR            0x22
#define BOARD_XL9555_PORT_PIN_MASK(port, pin) \
    (1U << ((port) * 8 + (pin)))
#define BOARD_XL9555_P05_POWER_EN        BOARD_XL9555_PORT_PIN_MASK(0, 5)
#define BOARD_XL9555_P11_DRV_EN          BOARD_XL9555_PORT_PIN_MASK(1, 1)

// T-Bao-S31 motor/servo control signals.
#define BOARD_SERVO_PWM                  GPIO_NUM_5
#define BOARD_DRV_BIN2                   GPIO_NUM_12
#define BOARD_DRV_BIN1                   GPIO_NUM_13
#define BOARD_DRV_AIN2                   GPIO_NUM_14
#define BOARD_DRV_AIN1                   GPIO_NUM_15
