#pragma once

#include "driver/gpio.h"
#include "driver/i2c_types.h"

// T-Bao-S31 main-board LCD connection: ST7796S, 1.54 inch, 320x320 RGB.
#define BOARD_LCD_H_RES                 320
#define BOARD_LCD_V_RES                 320
#define BOARD_LCD_DATA_WIDTH            8
#define BOARD_LCD_PIXEL_CLOCK_HZ        (10 * 1000 * 1000)
#define BOARD_LCD_DMA_LINES             16

#define BOARD_LCD_RS                   GPIO_NUM_18
#define BOARD_LCD_WR                   GPIO_NUM_17
#define BOARD_LCD_CS                   GPIO_NUM_19
#define BOARD_LCD_RST                  GPIO_NUM_35
#define BOARD_LCD_BL_PWM               GPIO_NUM_16

#define BOARD_LCD_D0                   GPIO_NUM_44
#define BOARD_LCD_D1                   GPIO_NUM_43
#define BOARD_LCD_D2                   GPIO_NUM_42
#define BOARD_LCD_D3                   GPIO_NUM_40
#define BOARD_LCD_D4                   GPIO_NUM_39
#define BOARD_LCD_D5                   GPIO_NUM_38
#define BOARD_LCD_D6                   GPIO_NUM_37
#define BOARD_LCD_D7                   GPIO_NUM_36

// The current board schematic uses RGB ordering. Adjust these values if the
// panel revision needs a different orientation or inversion setting.
#define BOARD_LCD_RGB_ORDER             LCD_RGB_ELEMENT_ORDER_RGB
#define BOARD_LCD_INVERT_COLOR          0
#define BOARD_LCD_SWAP_XY               0
#define BOARD_LCD_MIRROR_X              0
#define BOARD_LCD_MIRROR_Y              0
#define BOARD_LCD_X_GAP                 0
#define BOARD_LCD_Y_GAP                 0

// Main I2C bus shared by the main and power boards.
#define BOARD_I2C_PORT                  I2C_NUM_0
#define BOARD_I2C_SDA                   GPIO_NUM_0
#define BOARD_I2C_SCL                   GPIO_NUM_1
#define BOARD_I2C_FREQ_HZ               400000

// The touch controller is behind the main-board level shifter.
#define BOARD_TOUCH_I2C_ADDR            0x38
#define BOARD_TOUCH_H_RES               320
#define BOARD_TOUCH_V_RES               320
#define BOARD_TOUCH_RAW_H_RES           240
#define BOARD_TOUCH_RAW_V_RES           240
// The raw FT6336 X axis is reversed relative to the LCD. Keep Y direct and
// mirror X in the esp_lcd_touch middleware after scaling to 320x320.
#define BOARD_TOUCH_SWAP_XY             0
#define BOARD_TOUCH_MIRROR_X            1
#define BOARD_TOUCH_MIRROR_Y            0
#define BOARD_TOUCH_INT_ACTIVE_LEVEL    0

// FT6336 raw coordinate calibration. The default panel coordinate space is
// 240x240, so the valid raw index range is 0..239. Adjust these four values
// after an edge-sweep if the module reports a different active area.
#define BOARD_TOUCH_RAW_X_MIN           0
#define BOARD_TOUCH_RAW_X_MAX           (BOARD_TOUCH_RAW_H_RES - 1)
#define BOARD_TOUCH_RAW_Y_MIN           0
#define BOARD_TOUCH_RAW_Y_MAX           (BOARD_TOUCH_RAW_V_RES - 1)

// The FT6336U component defaults to a high threshold and a low report rate.
// These values match the panel's initial register values and improve tracking.
#define BOARD_TOUCH_THRESHOLD            25
#define BOARD_TOUCH_REPORT_RATE_HZ       30
// FT6336 G_MODE: 0 = interrupt polling mode, 1 = interrupt trigger mode.
#define BOARD_TOUCH_G_MODE               0

// XL9555 is on the power board at 0x22. P00 is TP_INT and P01 is TP_RST.
#define BOARD_XL9555_I2C_ADDR           0x22
#define BOARD_XL9555_P00_TOUCH_INT_MASK (1U << 0)
#define BOARD_XL9555_P01_TOUCH_RST_MASK (1U << 1)
#define BOARD_TOUCH_POLL_MS             10
#define BOARD_TOUCH_INT_DIAGNOSTIC_MS   250
#define BOARD_TOUCH_RELEASE_DEBOUNCE_SAMPLES 12
