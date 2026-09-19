# Display and Touch Test

This example targets the T-Bao-S31 board and is intended for ESP-IDF 6.1.

The display is a 1.54-inch `ST7796S` panel with `320(H) x 320(V)` RGB pixels.

It performs the following checks:

- Initializes the ST7796S LCD through the 8-bit Intel 8080 interface.
- Shows a full-screen RGB565 color-bar pattern.
- Initializes the FT6336/FT6336U touch controller at I2C address `0x38`.
- Resets the touch controller through XL9555 P01 at address `0x22`.
- Prints touch coordinates to the serial console and draws a marker at each point.
- Exposes a logical touch coordinate space of `320 x 320` (`0..319` on each axis).
- Scales the FT6336 native `240 x 240` coordinate range (`0..239`) to `320 x 320`.
- Corrects the panel orientation after scaling:
  `display_x = 319 - scaled_x`, `display_y = scaled_y`.

The example uses the following ESP Component Registry drivers:

- `espressif/esp_lcd_st7796^1.4.0`
- `lambage/esp_lcd_touch_ft6336u^1.0.8`
- `sheldonix/esp_io_expander_xl9555^0.8.0`

The touch interrupt and reset signals are not ESP32 GPIOs. They are connected to
XL9555 P00 and P01 respectively. P00 is configured as an expander input and
sampled at a low rate for diagnostics; it is not used as the touch state because
the FT6336 interrupt signal can be a data-ready pulse. Coordinates are polled
over I2C because the FT6336 driver accepts only a native ESP32 GPIO for its
interrupt callback. The example configures the controller for polling mode,
restores the panel threshold (`25`), requests a `60 Hz` report rate, polls every
`10 ms`, and requires twelve consecutive empty samples before reporting a
release. Each gesture also logs raw and mapped coordinate ranges.

The LCD backlight controller is a separate device labeled `0x15` in the
schematic; the touch controller uses `0x38`, so these addresses are distinct.

The touch resolution is a logical coordinate range, not a setting that changes
the FT6336 sensor hardware. `main/board_config.h` maps the native raw range
`0..239` into `0..319` on both axes, then mirrors only X to correct the
observed orientation: a left-to-right gesture must move left-to-right on the
LCD, while Y remains direct. If an edge sweep shows that the actual raw active
area is different, update `BOARD_TOUCH_RAW_X_MIN/MAX` and
`BOARD_TOUCH_RAW_Y_MIN/MAX` with the measured values. `x_max` and `y_max`
remain `319` because they are maximum valid indices used by the ESP touch
middleware for mirroring.

The local `components/esp_lcd_st7796` directory supplies the `1.4.0` LCD
component used by this example and keeps the project buildable with ESP-IDF
6.1.

## Pin mapping

| Function | Mapping |
| --- | --- |
| LCD 8-bit I80 | `D0..D7=GPIO44,43,42,40,39,38,37,36`, `RS=GPIO18`, `WR=GPIO17`, `CS=GPIO19`, `RST=GPIO35` |
| LCD backlight | `BL_PWM=GPIO16` |
| Main I2C | `SDA=GPIO0`, `SCL=GPIO1`, 400 kHz |
| Touch controller | FT6336/FT6336U, 7-bit address `0x38` |
| Touch reset | XL9555 `0x22`, P01 |
| Touch interrupt | XL9555 P00, sampled by this example; touch data is I2C-polled |

## Build

Run the following commands from this directory with ESP-IDF 6.1 exported:

```powershell
idf.py --preview --build-dir build_s31 set-target esp32s31
idf.py --preview --build-dir build_s31 build
idf.py --preview --build-dir build_s31 -p PORT flash monitor
```

The pin definitions and panel orientation options are in
`main/board_config.h`.
