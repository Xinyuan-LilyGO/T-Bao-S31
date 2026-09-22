# Camera display (ESP32-S31)

This ESP-IDF 6.1 example shows the live camera image on the T-Bao-S31 ST7796S
320x320 display. `esp_cam_io_parl` detects OV2640/OV3660 over the dedicated
camera I2C bus (verify the supply requirements of a replacement module). The
camera produces 320x320 JPEG frames. The ESP32-S31 hardware JPEG decoder writes
RGB565 directly into an LCD DMA buffer, and the complete frame is submitted as
one I80 transaction without application-side scaling or strip copies. JPEG
quality is set to 8 (lower is higher quality for these sensors). The example
keeps PSRAM disabled.

For OV2640, `esp_cam_io_parl` 0.1.0 selects a CIF source window for the generic
320x320 preset. That window is only 300x296 and can stop valid JPEG frames on
this module. The example replaces it with a centered 600x600 crop from the
sensor's SVGA mode and downsamples to 320x320 before arming PARLIO reception.
OV3660 continues to use the component's normal 320x320 configuration.

## Build and flash

From an ESP-IDF **6.1** environment:

```sh
cd examples/camera_display
idf.py --preview set-target esp32s31
idf.py --preview build
idf.py --preview -p <PORT> flash monitor
```

The example uses the ESP-IDF 6.1 `esp_driver_jpeg` hardware driver,
`espressif/esp_lcd_st7796` 1.4.0 (the local ESP32-S31 compatibility copy from
`display_touch_test`), and `haqqscripter/esp_cam_io_parl` 0.1.0.
The board has 16 MB of flash; the example sets its image header accordingly.

## Connections and diagnostics

- LCD: 8-bit I80, D0..D7 = GPIO44,43,42,40,39,38,37,36;
  DC/WR/CS/RST/BL = GPIO18/17/19/35/16. BGR element order and color inversion
  are enabled. The panel remains in its verified full-screen address mode
  (no XY swap, X mirror, zero gap). Each 320x320 RGB565 camera or status frame
  is rotated clockwise by 90 degrees in place before one full-screen DMA
  transfer. This avoids the cropped/overlapping address window produced by
  applying ST7796S hardware rotation to this 320x320 panel revision.
- Camera: I2C1 SDA/SCL = GPIO3/4 at 100 kHz, RESET = GPIO45,
  D0..D7 = GPIO46..53 (module pins D2..D9), PCLK = GPIO54,
  XCLK = GPIO55, HREF = GPIO57. The module's PWDN is pulled low;
  VSYNC = GPIO56 on the connector, but is not used by this camera driver.
  `esp_cam_io_parl` 0.1.0 does not arm an RX transaction on the ESP32-S31
  HREF delimiter path; this example supplies the missing transaction after
  enabling PARLIO.
- SGM38121 is at `0x28` on **main I2C0** (SDA=GPIO0, SCL=GPIO1, 100 kHz),
  separate from the camera I2C1 (SDA=GPIO3, SCL=GPIO4). Its EN pin is pulled
  low. The default OV2640 profile programs DVDD1 -> DVDD to 1.2V
  (`0x03=0x57`), AVDD1 -> DOVDD to 2.8V (`0x05=0xB1`), and AVDD2 -> AVDD
  to 2.8V (`0x06=0xB1`), then enables those three rails (`0x0E` bits 0/2/3).
  OV2640 core-voltage requirements vary by module/revision: set
  `BOARD_CAMERA_OV2640_CORE_1V3` to 1 only after confirming the module needs
  1.304V (`0x03=0x64`).
  Change `BOARD_CAMERA_POWER_PROFILE_OV3660` in `main/board_config.h` to 1
  *before fitting an OV3660*; that profile sets DVDD to 1.504V (`0x03=0x7D`).
  There is no safe automatic core-voltage selection before sensor identification.
  The earlier `0x7D/0xF0` settings actually requested 1.504V/3.304V, not
  1.2V/3.3V. With IN2 wired to 3.3V, the latter also has no LDO headroom.
  Register readback confirms settings, **not measured output voltage**.
  Verify the fitted camera's supply limits and measure the rails at J4
  before treating the example as validated on a new hardware revision.

If power setup fails, the LCD displays `POWER ERROR / SGM38121 / I2C 0X28`
and the serial log identifies the failing step. After powering the sensor,
the example starts XCLK, releases RESET, samples the idle levels of CAM_SDA
and CAM_SCL, and probes 0x30 (OV2640) and 0x3C (OV3660). `NO I2C ACK` means
no address responded, `I2C TIMEOUT` indicates a bus/pull-up fault, and
`UNSUPPORTED` displays the PID or address of a responding unsupported sensor.
If a camera responds under the wrong power profile, the display says
`POWER PROFILE` and names the model. The serial log includes the raw errors.
Repeated capture or decode failures also show the PID on the screen.

The camera I2C bus has no on-board external pull-up resistors in the supplied
schematic (R67/R68 are series resistors). If there is still no ACK, check the
flex cable orientation and J4 power rails, RESET and XCLK, and inspect SDA/SCL
with a meter or logic analyzer. GPIO-level samples alone cannot verify signal
rise time or actual supply voltages; external pull-ups to a suitable I/O rail
or level shifting may be needed on the physical board.

The hardware JPEG decoder uses its little-endian RGB565 output mode; the I80
driver swaps the two bytes once on output. The LCD is configured for BGR
element order, `INVON` and X mirroring so camera colors and generated text
match the fitted panel.
The camera's frame queue retains the newest frame when decoding or LCD
transfers take longer than capture. Every 30 displayed frames, the serial log
prints average capture, hardware-decode and LCD transfer times plus frame rate.
