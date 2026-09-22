# Audio test (ESP32-S31)

This ESP-IDF 6.1 example tests the T-Bao-S31 audio path and shows live results
on the 320x320 ST7796S display.

It reuses the LCD, FT6336 and XL9555 setup from `display_touch_test`, then adds:

- ES8389 (`0x10`) I2C/configuration and a 1 kHz stereo DAC tone.
- ES7210 (`0x40`) I2C/configuration and live stereo PCM RMS/peak metering.
- I2S standard mode at 48 kHz, 16-bit stereo.
- LVGL buttons for `MIC`, `DAC` and `AUTO` tests.
- Separate `PASS`, `FAIL` and `MANUAL` states. A successful TX transfer can be
  marked `PASS`, but actual speaker sound remains `MANUAL`.

The deprecated standalone `espressif/es7210` 1.0.1~1 component declares
ESP-IDF `<6.0` support. This IDF 6.1 example therefore uses the ES7210 driver
provided by `espressif/esp_codec_dev` 1.6.2, which is the supported codec path.

## Board connections

| Function | Mapping |
| --- | --- |
| Main I2C | SDA=GPIO0, SCL=GPIO1 |
| I2S MCLK | GPIO6 |
| I2S ESP32 -> ES8389 DSDIN | GPIO7 |
| I2S BCLK | GPIO8 |
| I2S LRCK | GPIO9 |
| I2S ES7210 SDOUT -> ESP32 | GPIO10 |
| 5 V speaker rail enable | XL9555 `0x22`, P05; high enables SY8113 `VDD5V` |
| Speaker amplifier control | XL9555 `0x22`, P07; high enables NS4150B |

## Build

From an exported ESP-IDF 6.1 environment:

```powershell
cd examples/audio_test
idf.py --preview set-target esp32s31
idf.py --preview build
```
