# Camera display (ESP32-S31)

This ESP-IDF 6.1 example shows the live camera image on the T-Bao-S31 ST7796S
320x320 display. `esp_cam_io_parl` detects OV2640/OV3660 over the dedicated
camera I2C bus (verify the supply requirements of a replacement module). The
camera produces 240x240 JPEG frames, which are decoded to RGB565 and displayed
in the center with a 40-pixel black border. No PSRAM is required.

## Build and flash

From an ESP-IDF **6.1** environment:

```sh
cd examples/camera_display
idf.py --preview set-target esp32s31
idf.py --preview build
idf.py --preview -p <PORT> flash monitor
```

The example uses `espressif/esp_lcd_st7796` 1.4.0 (the local ESP32-S31
compatibility copy from `display_touch_test`),
`haqqscripter/esp_cam_io_parl` 0.1.0, and `espressif/esp_jpeg` 1.3.1.

## Connections and diagnostics

- LCD: 8-bit I80, D0..D7 = GPIO44,43,42,40,39,38,37,36;
  DC/WR/CS/RST/BL = GPIO18/17/19/35/16. `BOARD_LCD_MIRROR_X` corrects the
  horizontal mirror seen on this panel; Y is unchanged.
- Camera: I2C1 SDA/SCL = GPIO3/4 at 100 kHz, RESET = GPIO45,
  D0..D7 = GPIO46..53 (module pins D2..D9), PCLK = GPIO54,
  XCLK = GPIO55, HREF = GPIO57. The module's PWDN is tied low;
  VSYNC is not used by this version of the camera driver.
- SGM38121 is at `0x28` on **main I2C0** (SDA=GPIO0, SCL=GPIO1, 100 kHz),
  separate from the camera I2C1 (SDA=GPIO3, SCL=GPIO4). Its EN pin is pulled
  low. Before starting the camera, the example programs DVDD1 -> DVDD to
  1.2V (`0x03=0x57`), AVDD1 -> DOVDD to 1.8V (`0x05=0x34`), and AVDD2 ->
  AVDD to 2.8V (`0x06=0xB1`), then enables those three rails (`0x0E` bits
  0/2/3). Each write is checked by reading the register back. DVDD2 is not
  connected to the camera. The code waits 20ms before creating camera I2C1.
  The listed voltages are for the populated OV2640; verify a future OV3660
  module's rail ratings before using it with these settings.

If power setup fails, the LCD displays `POWER ERROR / SGM38121 / I2C 0X28`
and the serial log identifies the failing step. If camera initialization
fails, the LCD displays the status and the sensor PID when its register
protocol can be read (OV2640 at 0x30, OV3660/OV5640
at 0x3c, NT99141 at 0x2a). For a sensor with an unknown register protocol,
the detected I2C address is displayed instead. Check the serial monitor for
the underlying ESP-IDF error and for the detected model/PID. Repeated capture
or decode failures also show the PID on the screen.

The JPEG decoder and LCD colors both use native RGB565 in memory; the I80
driver swaps the two bytes on output. The camera's frame queue retains the
newest frame when decoding or LCD transfers take longer than capture.
