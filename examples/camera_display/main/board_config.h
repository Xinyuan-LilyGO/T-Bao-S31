#pragma once

#include "driver/gpio.h"
#include "driver/i2c_types.h"
#include "esp_lcd_panel_vendor.h"

#define BOARD_LCD_H_RES             320
#define BOARD_LCD_V_RES             320
#define BOARD_LCD_DATA_WIDTH        8
#define BOARD_LCD_PIXEL_CLOCK_HZ    (10 * 1000 * 1000)
#define BOARD_LCD_DMA_LINES         16

#define BOARD_LCD_RS                GPIO_NUM_18
#define BOARD_LCD_WR                GPIO_NUM_17
#define BOARD_LCD_CS                GPIO_NUM_19
#define BOARD_LCD_RST               GPIO_NUM_35
#define BOARD_LCD_BL                GPIO_NUM_16
#define BOARD_LCD_D0                GPIO_NUM_44
#define BOARD_LCD_D1                GPIO_NUM_43
#define BOARD_LCD_D2                GPIO_NUM_42
#define BOARD_LCD_D3                GPIO_NUM_40
#define BOARD_LCD_D4                GPIO_NUM_39
#define BOARD_LCD_D5                GPIO_NUM_38
#define BOARD_LCD_D6                GPIO_NUM_37
#define BOARD_LCD_D7                GPIO_NUM_36

#define BOARD_LCD_RGB_ORDER         LCD_RGB_ELEMENT_ORDER_BGR
#define BOARD_LCD_INVERT_COLOR      1
#define BOARD_LCD_SWAP_XY           false
// Keep the panel in the same full-screen address orientation used by the
// working audio UI. Camera/status frames are rotated in the RGB565 buffer.
#define BOARD_LCD_MIRROR_X          true
#define BOARD_LCD_MIRROR_Y          false
#define BOARD_LCD_X_GAP             0
#define BOARD_LCD_Y_GAP             0

#define BOARD_PMIC_I2C_PORT         I2C_NUM_0
#define BOARD_PMIC_SDA              GPIO_NUM_0
#define BOARD_PMIC_SCL              GPIO_NUM_1
#define BOARD_PMIC_I2C_HZ           100000
#define BOARD_PMIC_ADDR             0x28
// Select the fitted sensor before applying power. OV2640 core voltage varies
// by sensor revision; confirm the module's rating before selecting 1.3V.
#define BOARD_CAMERA_POWER_PROFILE_OV3660 0
#define BOARD_CAMERA_OV2640_CORE_1V3     0
// DVDD1 -> DVDD (OV2640: 1.2V or 1.304V; OV3660: 1.504V).
#if BOARD_CAMERA_POWER_PROFILE_OV3660
#define BOARD_PMIC_DVDD1_VOUT       0x7D
#elif BOARD_CAMERA_OV2640_CORE_1V3
#define BOARD_PMIC_DVDD1_VOUT       0x64
#else
#define BOARD_PMIC_DVDD1_VOUT       0x57
#endif
// AVDD1 -> DOVDD (2.8V), AVDD2 -> AVDD (2.8V).
#define BOARD_PMIC_AVDD1_VOUT       0xB1
#define BOARD_PMIC_AVDD2_VOUT       0xB1
#define BOARD_PMIC_CAMERA_RAILS     0x0D

#define BOARD_CAMERA_I2C_PORT       I2C_NUM_1
#define BOARD_CAMERA_SDA            GPIO_NUM_3
#define BOARD_CAMERA_SCL            GPIO_NUM_4
#define BOARD_CAMERA_I2C_HZ         100000
#define BOARD_CAMERA_RESET          GPIO_NUM_45
#define BOARD_CAMERA_D0             GPIO_NUM_46
#define BOARD_CAMERA_D1             GPIO_NUM_47
#define BOARD_CAMERA_D2             GPIO_NUM_48
#define BOARD_CAMERA_D3             GPIO_NUM_49
#define BOARD_CAMERA_D4             GPIO_NUM_50
#define BOARD_CAMERA_D5             GPIO_NUM_51
#define BOARD_CAMERA_D6             GPIO_NUM_52
#define BOARD_CAMERA_D7             GPIO_NUM_53
#define BOARD_CAMERA_PCLK           GPIO_NUM_54
#define BOARD_CAMERA_XCLK           GPIO_NUM_55
#define BOARD_CAMERA_VSYNC          GPIO_NUM_56
#define BOARD_CAMERA_HREF           GPIO_NUM_57
#define BOARD_CAMERA_XCLK_HZ        (20 * 1000 * 1000)
#define BOARD_CAMERA_FRAME_WIDTH    320
#define BOARD_CAMERA_FRAME_HEIGHT   320
// OV sensor JPEG quality: lower values produce higher quality images. 0~63, default 8.
#define BOARD_CAMERA_JPEG_QUALITY   8
