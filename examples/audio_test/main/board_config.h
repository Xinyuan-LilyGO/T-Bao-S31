#pragma once

#include "driver/gpio.h"
#include "driver/i2c_types.h"
#include "driver/i2s_types.h"
#include "esp_lcd_panel_vendor.h"

// ST7796S: 1.54 inch, 320x320, 8-bit I80.
#define BOARD_LCD_H_RES                 320
#define BOARD_LCD_V_RES                 320
#define BOARD_LCD_DATA_WIDTH            8
#define BOARD_LCD_PIXEL_CLOCK_HZ        (10 * 1000 * 1000)
#define BOARD_LCD_DRAW_LINES            32

#define BOARD_LCD_RS                    GPIO_NUM_18
#define BOARD_LCD_WR                    GPIO_NUM_17
#define BOARD_LCD_CS                    GPIO_NUM_19
#define BOARD_LCD_RST                   GPIO_NUM_35
#define BOARD_LCD_BL_PWM                GPIO_NUM_16

#define BOARD_LCD_D0                    GPIO_NUM_44
#define BOARD_LCD_D1                    GPIO_NUM_43
#define BOARD_LCD_D2                    GPIO_NUM_42
#define BOARD_LCD_D3                    GPIO_NUM_40
#define BOARD_LCD_D4                    GPIO_NUM_39
#define BOARD_LCD_D5                    GPIO_NUM_38
#define BOARD_LCD_D6                    GPIO_NUM_37
#define BOARD_LCD_D7                    GPIO_NUM_36

#define BOARD_LCD_RGB_ORDER             LCD_RGB_ELEMENT_ORDER_RGB
#define BOARD_LCD_INVERT_COLOR          0
#define BOARD_LCD_SWAP_XY               0
#define BOARD_LCD_MIRROR_X              0
#define BOARD_LCD_MIRROR_Y              0
#define BOARD_LCD_X_GAP                 0
#define BOARD_LCD_Y_GAP                 0

// Main I2C bus shared by the main board and power board.
#define BOARD_I2C_PORT                  I2C_NUM_0
#define BOARD_I2C_SDA                   GPIO_NUM_0
#define BOARD_I2C_SCL                   GPIO_NUM_1
#define BOARD_TOUCH_I2C_FREQ_HZ         400000
#define BOARD_AUDIO_I2C_FREQ_HZ         100000

#define BOARD_XL9555_I2C_ADDR           0x22
#define BOARD_XL9555_P00_TOUCH_INT_MASK (1U << 0)
#define BOARD_XL9555_P01_TOUCH_RST_MASK (1U << 1)
#define BOARD_XL9555_P07_SPK_CTRL_MASK  (1U << 7)

#define BOARD_TOUCH_I2C_ADDR            0x38
#define BOARD_TOUCH_H_RES               320
#define BOARD_TOUCH_V_RES               320
#define BOARD_TOUCH_RAW_H_RES           240
#define BOARD_TOUCH_RAW_V_RES           240
#define BOARD_TOUCH_RAW_X_MIN           0
#define BOARD_TOUCH_RAW_X_MAX           (BOARD_TOUCH_RAW_H_RES - 1)
#define BOARD_TOUCH_RAW_Y_MIN           0
#define BOARD_TOUCH_RAW_Y_MAX           (BOARD_TOUCH_RAW_V_RES - 1)
#define BOARD_TOUCH_SWAP_XY             0
#define BOARD_TOUCH_MIRROR_X            1
#define BOARD_TOUCH_MIRROR_Y            0
#define BOARD_TOUCH_INT_ACTIVE_LEVEL    0
#define BOARD_TOUCH_THRESHOLD           25
#define BOARD_TOUCH_REPORT_RATE_HZ      30
#define BOARD_TOUCH_G_MODE              0

// ES8389 and ES7210 share the clocks. Their data pins are separate directions.
#define BOARD_I2S_PORT                  I2S_NUM_0
#define BOARD_I2S_MCLK                  GPIO_NUM_6
#define BOARD_I2S_DOUT                  GPIO_NUM_7
#define BOARD_I2S_BCLK                  GPIO_NUM_8
#define BOARD_I2S_LRCK                  GPIO_NUM_9
#define BOARD_I2S_DIN                   GPIO_NUM_10
#define BOARD_AUDIO_SAMPLE_RATE         48000
#define BOARD_AUDIO_MCLK_MULTIPLE       I2S_MCLK_MULTIPLE_256
#define BOARD_AUDIO_CHANNELS            2
#define BOARD_AUDIO_BITS                16

// 7-bit addresses used by the ESP-IDF I2C master bus and device probes.
#define BOARD_ES8389_I2C_ADDR           0x10
#define BOARD_ES7210_I2C_ADDR           0x40
