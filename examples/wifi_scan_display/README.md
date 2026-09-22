# Wi-Fi scan display (ESP32-S31)

This ESP-IDF 6.1 example scans nearby Wi-Fi networks and shows the strongest
unique SSIDs on the T-Bao-S31 320x320 ST7796S display. The list includes RSSI,
channel and security state. When the configured target SSID is present, the
example highlights it and attempts to connect. After DHCP succeeds, the screen
shows the assigned IPv4 address. A dropped link returns to the scan loop. The
UI uses a white theme and is rotated clockwise by 90 degrees.

The UI uses `lvgl/lvgl^9.6.0~1`. The LCD uses the local ESP32-S31-compatible
copy of `espressif/esp_lcd_st7796^1.4.0` from `display_touch_test`.

## Local credentials

Real test credentials belong in `main/wifi_credentials.h`. That file is
explicitly ignored by this example's `.gitignore` and must not be committed.
For a new checkout, create it from the tracked template:

```powershell
Copy-Item main/wifi_credentials.h.example main/wifi_credentials.h
```

Then set `WIFI_TEST_TARGET_SSID` and `WIFI_TEST_TARGET_PASSWORD` in the local
file. If the file is absent, the firmware still builds and scans, but it does
not attempt a connection. Credentials are passed to the Wi-Fi driver using
RAM storage. The password is never logged, and the Wi-Fi configuration is not
persisted in NVS by this example.

## Build and flash

Use an exported ESP-IDF 6.1 environment:

```powershell
cd examples/wifi_scan_display
idf.py --preview --build-dir build_s31 set-target esp32s31
idf.py --preview --build-dir build_s31 build
idf.py --preview --build-dir build_s31 -p PORT flash monitor
```

`sdkconfig.defaults` selects the IDF 1.5 MiB single-app partition layout so the
Wi-Fi and LVGL firmware fits without requiring a project-local partition CSV.

The ST7796S remains in its verified full-screen address mode with no hardware
XY swap. Each LVGL partial draw buffer is rotated clockwise by 90 degrees in
software before it is sent to the LCD. This avoids the cropped or overlapping
window seen when this 320x320 panel revision uses the controller's hardware
rotation.

The firmware rescans every 10 seconds while the target is absent or after a
failed connection. Each connection cycle makes three attempts. Full scan and
connection diagnostics are also written to the serial console.
