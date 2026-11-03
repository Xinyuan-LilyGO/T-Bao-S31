/*
 * SPDX-FileCopyrightText: 2025 Espressif Systems (Shanghai) CO., LTD
 * SPDX-License-Identifier: LicenseRef-Espressif-Modified-MIT
 *
 * Auto-generated device configuration file
 * DO NOT MODIFY THIS FILE MANUALLY
 *
 * See LICENSE file for details.
 */

#include <stdlib.h>
#include "esp_board_device.h"
#include "dev_audio_codec.h"
#include "dev_display_lcd.h"
#include "dev_fs_fat.h"
#include "dev_gpio_expander.h"
#include "dev_lcd_touch.h"
#include "dev_ledc_ctrl.h"
#include "driver/sdmmc_host.h"
#include "sdmmc_cmd.h"

// Device configuration structures
const static dev_io_expander_config_t esp_bmgr_gpio_expander_cfg = {
    .name = "gpio_expander",
    .type = "gpio_expander",
    .chip = "xl9555",
    .i2c_name = "i2c_master",
    .i2c_addr_count = 1,
    .i2c_addr = {68},
    .max_pins = 16,
    .output_io_mask = 930,
    .output_io_level_mask = 34,
    .enable_mode_set = false,
    .output_io_mode_mask = 0,
    .io_pullup_mask = 0,
    .io_pulldown_mask = 0,
    .input_io_mask = 1,
};

const static dev_ledc_ctrl_config_t esp_bmgr_lcd_brightness_cfg = {
    .name = "lcd_brightness",
    .type = "ledc_ctrl",
    .ledc_name = "ledc_backlight",
    .default_percent = 80,
};

const static dev_audio_codec_config_t esp_bmgr_audio_dac_cfg = {
    .name = "audio_dac",
    .chip = "es8389",
    .type = "audio_codec",
    .data_if_type = 0,
    .adc_enabled = false,
    .adc_max_channel = 0,
    .adc_channel_mask = 0x3,
    .adc_channel_labels = "",
    .adc_init_gain = 0,
    .dac_enabled = true,
    .dac_max_channel = 2,
    .dac_channel_mask = 0x3,
    .dac_init_gain = 0,
    .pa_cfg = {
        .name = NULL,
        .port = -1,
        .active_level = 0,
        .gain = 0.0,
    },
    .i2c_cfg = {
        .name = "i2c_master",
        .port = 0,
        .address = 32,
        .frequency = 400000,
    },
    .i2s_cfg = {
        .name = "i2s_audio_out",
        .port = 0,
        .clk_src = 0,
        .tx_aux_out_io = -1,
        .tx_aux_out_line = 0,
        .tx_aux_out_invert = false,
    },
    .adc_cfg = {
        .periph_name = NULL,
        .sample_rate_hz = 0,
        .max_store_buf_size = 0,
        .conv_frame_size = 0,
        .conv_mode = 0,
        .format = 0,
        .pattern_num = 0,
        .cfg_mode = 0,
        .cfg = {
            .single_unit = {
                .unit_id = 0,
                .atten = 0,
                .bit_width = 0,
                .channel_id = {},
            },
        },
    },
    .metadata = NULL,
    .metadata_size = 0,
    .mclk_enabled = true,
    .aec_enabled = false,
    .eq_enabled = false,
    .alc_enabled = false,
};

const static dev_audio_codec_config_t esp_bmgr_audio_adc_cfg = {
    .name = "audio_adc",
    .chip = "es7210",
    .type = "audio_codec",
    .data_if_type = 0,
    .adc_enabled = true,
    .adc_max_channel = 4,
    .adc_channel_mask = 0x3,
    .adc_channel_labels = "NA,NA,FR,FL",
    .adc_init_gain = 0,
    .dac_enabled = false,
    .dac_max_channel = 0,
    .dac_channel_mask = 0x0,
    .dac_init_gain = 0,
    .pa_cfg = {
        .name = NULL,
        .port = -1,
        .active_level = 0,
        .gain = 0.0,
    },
    .i2c_cfg = {
        .name = "i2c_master",
        .port = 0,
        .address = 128,
        .frequency = 400000,
    },
    .i2s_cfg = {
        .name = "i2s_audio_in",
        .port = 0,
        .clk_src = 0,
        .tx_aux_out_io = -1,
        .tx_aux_out_line = 0,
        .tx_aux_out_invert = false,
    },
    .adc_cfg = {
        .periph_name = NULL,
        .sample_rate_hz = 0,
        .max_store_buf_size = 0,
        .conv_frame_size = 0,
        .conv_mode = 0,
        .format = 0,
        .pattern_num = 0,
        .cfg_mode = 0,
        .cfg = {
            .single_unit = {
                .unit_id = 0,
                .atten = 0,
                .bit_width = 0,
                .channel_id = {},
            },
        },
    },
    .metadata = NULL,
    .metadata_size = 0,
    .mclk_enabled = true,
    .aec_enabled = false,
    .eq_enabled = false,
    .alc_enabled = false,
};

const static dev_display_lcd_config_t esp_bmgr_display_lcd_cfg = {
    .name = "display_lcd",
    .chip = "st7796",
    .sub_type = "parlio",
    .lcd_width = 320,
    .lcd_height = 320,
    .swap_xy = false,
    .mirror_x = true,
    .mirror_y = false,
    .need_reset = true,
    .invert_color = true,
    .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_BGR,
    .data_endian = LCD_RGB_DATA_ENDIAN_BIG,
    .bits_per_pixel = 16,
    .sub_cfg = {
        .parlio = {
            .io_parl = {
                .dc_gpio_num = 18,
                .clk_gpio_num = 17,
                .cs_gpio_num = 19,
                .data_gpio_nums = {44, 43, 42, 40, 39, 38, 37, 36},
                .data_width = 8,
                .pclk_hz = 15000000,
                .clk_src = PARLIO_CLK_SRC_DEFAULT,
                .max_transfer_bytes = 204800,
                .dma_burst_size = 64,
                .trans_queue_depth = 10,
                .lcd_cmd_bits = 8,
                .lcd_param_bits = 8,
                .dc_levels = {
                    .dc_cmd_level = 0,
                    .dc_data_level = 1,
                },
                .flags = {
                    .cs_active_high = false,
                },
            },
            .panel_config = {
                .reset_gpio_num = 35,
                .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_BGR,
                .data_endian = LCD_RGB_DATA_ENDIAN_BIG,
                .bits_per_pixel = 16,
                .flags = {
                    .reset_active_high = false,
                },
                .vendor_config = NULL,
            },
        },
    },
};

const static dev_lcd_touch_config_t esp_bmgr_lcd_touch_cfg = {
    .name = "lcd_touch",
    .chip = "ft6336",
    .type = "lcd_touch",
    .sub_type = "i2c",
    .touch_config = {
        .x_max = 319,
        .y_max = 319,
        .rst_gpio_num = -1,
        .int_gpio_num = -1,
        .levels = {
            .reset = 0,
            .interrupt = 0,
        },
        .flags = {
            .swap_xy = false,
            .mirror_x = true,
            .mirror_y = false,
        },
        .process_coordinates = NULL,
        .interrupt_callback = NULL,
        .user_data = NULL,
        .driver_data = NULL,
    },
    .sub_cfg = {
        .i2c = {
            .i2c_name = "i2c_master",
            .i2c_addr_count = 1,
            .i2c_addr = {0x70, 0x00, 0x00, 0x00},
            .io_i2c_config = {
                .dev_addr = 0,
                .control_phase_bytes = 1,
                .dc_bit_offset = 0,
                .lcd_cmd_bits = 8,
                .lcd_param_bits = 8,
                .scl_speed_hz = 400000,
                .flags = {
                    .dc_low_on_data = false,
                    .disable_control_phase = true,
                },
            },
        },
    },
};

const static dev_fs_fat_config_t esp_bmgr_fs_sdcard_cfg = {
    .name = "fs_sdcard",
    .mount_point = "/sdcard",
    .frequency = SDMMC_FREQ_HIGHSPEED,
    .vfs_config = {
        .format_if_mount_failed = false,
        .max_files = 5,
        .allocation_unit_size = 16384,
    },
    .sub_type = "sdmmc",
    .sub_cfg = {
        .sdmmc = {
            .slot = SDMMC_HOST_SLOT_1,
            .bus_width = 4,
            .slot_flags = SDMMC_SLOT_FLAG_INTERNAL_PULLUP,
            .pins = {
                .clk = 24,
                .cmd = 25,
                .d0 = 20,
                .d1 = 21,
                .d2 = 22,
                .d3 = 23,
                .d4 = -1,
                .d5 = -1,
                .d6 = -1,
                .d7 = -1,
                .cd = -1,
                .wp = -1,
            },
            .ldo_chan_id = -1,
        },
    },
};

// Device descriptor array
const esp_board_device_desc_t g_esp_board_devices[] = {
    {
        .next = &g_esp_board_devices[1],
        .name = "gpio_expander",
        .chip = "xl9555",
        .type = "gpio_expander",
        .sub_type = NULL,
        .cfg = &esp_bmgr_gpio_expander_cfg,
        .cfg_size = sizeof(esp_bmgr_gpio_expander_cfg),
        .init_skip = false,
    },
    {
        .next = &g_esp_board_devices[2],
        .name = "lcd_brightness",
        .chip = NULL,
        .type = "ledc_ctrl",
        .sub_type = NULL,
        .cfg = &esp_bmgr_lcd_brightness_cfg,
        .cfg_size = sizeof(esp_bmgr_lcd_brightness_cfg),
        .init_skip = false,
    },
    {
        .next = &g_esp_board_devices[3],
        .name = "audio_dac",
        .chip = "es8389",
        .type = "audio_codec",
        .sub_type = NULL,
        .cfg = &esp_bmgr_audio_dac_cfg,
        .cfg_size = sizeof(esp_bmgr_audio_dac_cfg),
        .init_skip = false,
    },
    {
        .next = &g_esp_board_devices[4],
        .name = "audio_adc",
        .chip = "es7210",
        .type = "audio_codec",
        .sub_type = NULL,
        .cfg = &esp_bmgr_audio_adc_cfg,
        .cfg_size = sizeof(esp_bmgr_audio_adc_cfg),
        .init_skip = false,
    },
    {
        .next = &g_esp_board_devices[5],
        .name = "display_lcd",
        .chip = "st7796",
        .type = "display_lcd",
        .sub_type = "parlio",
        .cfg = &esp_bmgr_display_lcd_cfg,
        .cfg_size = sizeof(esp_bmgr_display_lcd_cfg),
        .init_skip = false,
    },
    {
        .next = &g_esp_board_devices[6],
        .name = "lcd_touch",
        .chip = "ft6336",
        .type = "lcd_touch",
        .sub_type = "i2c",
        .cfg = &esp_bmgr_lcd_touch_cfg,
        .cfg_size = sizeof(esp_bmgr_lcd_touch_cfg),
        .init_skip = false,
    },
    {
        .next = NULL,
        .name = "fs_sdcard",
        .chip = NULL,
        .type = "fs_fat",
        .sub_type = "sdmmc",
        .cfg = &esp_bmgr_fs_sdcard_cfg,
        .cfg_size = sizeof(esp_bmgr_fs_sdcard_cfg),
        .init_skip = true,
    },
};
