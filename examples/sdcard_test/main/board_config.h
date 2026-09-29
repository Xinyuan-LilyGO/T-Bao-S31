#pragma once

#include "driver/gpio.h"
#include "driver/i2c_types.h"

#define BOARD_I2C_PORT                 I2C_NUM_0
#define BOARD_I2C_SDA                  GPIO_NUM_0
#define BOARD_I2C_SCL                  GPIO_NUM_1
#define BOARD_I2C_FREQ_HZ              100000

#define BOARD_XL9555_I2C_ADDR          0x22
#define BOARD_XL9555_P10_SD_VDD_EN     (1U << 10)

#define BOARD_SD_D0                    GPIO_NUM_20
#define BOARD_SD_D1                    GPIO_NUM_21
#define BOARD_SD_D2                    GPIO_NUM_22
#define BOARD_SD_D3                    GPIO_NUM_23
#define BOARD_SD_CLK                   GPIO_NUM_24
#define BOARD_SD_CMD                   GPIO_NUM_25
#define BOARD_SD_BUS_WIDTH             4
#define BOARD_SD_POWER_ON_LEVEL        1
#define BOARD_SD_MOUNT_POINT           "/sdcard"
