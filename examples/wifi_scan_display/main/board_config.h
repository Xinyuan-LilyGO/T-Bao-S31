#pragma once

#include "driver/gpio.h"
#include "esp_lcd_panel_vendor.h"

// ST7796S: 1.54 inch, 320x320 BGR, 8-bit I80.
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

#define BOARD_LCD_RGB_ORDER             LCD_RGB_ELEMENT_ORDER_BGR
#define BOARD_LCD_INVERT_COLOR          1
#define BOARD_LCD_SWAP_XY               0
#define BOARD_LCD_MIRROR_X              1
#define BOARD_LCD_MIRROR_Y              0
#define BOARD_LCD_X_GAP                 0
#define BOARD_LCD_Y_GAP                 0

// Keep the verified full-screen address window and rotate each LVGL partial
// buffer in software instead of changing the ST7796S scan orientation.
#define BOARD_LCD_SOFTWARE_ROTATE_CW_90  1
