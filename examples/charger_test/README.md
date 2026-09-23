# SGM41529 charger test (ESP32-S31)

This ESP-IDF 6.1 example exercises the T-Bao-S31 SGM41529 two-cell charger
and shows the charging process on the 320x320 ST7796S display.

The firmware verifies the part ID at I2C address `0x6B` before it enables the
active-low `nCE` signal through XL9555 P02. It then enables the continuous ADC
and refreshes the screen every 500 ms.

## Displayed information

- Charging phase: idle, trickle, pre-charge, constant-current, constant-voltage,
  top-off, or charge complete.
- ADC measurements: VBUS, IBUS, VBAT, ICHG, VSYS, and charger die temperature.
- Programmed limits: charge voltage, charge current, input current,
  pre-charge current, and termination current.
- Input source, NTC state and TS percentage, IINDPM/VINDPM, thermal regulation,
  register/pin power-good state, and current charger faults.
- SGM41529 part ID and revision. An unexpected part ID is shown on screen and
  charging remains disabled.

If three consecutive monitoring reads fail, the firmware deasserts `nCE` and
retries initialization. The serial console also records state transitions and
a full measurement line every five seconds.

Peripheral initialization errors are logged and left on the display instead
of being passed to `ESP_ERROR_CHECK`, so a missing I2C device does not create a
software-reset loop.

## Board connections

| Function | Mapping |
| --- | --- |
| Main I2C | SDA=GPIO0, SCL=GPIO1 |
| SGM41529 | `0x6B` |
| XL9555 | `0x22` |
| Charge enable | XL9555 P02 / `CRG_EN` / SGM41529 `nCE` |
| Charge interrupt | XL9555 P03 / `CRG_INT` / SGM41529 `nINT` |
| Power good | XL9555 P04 / `CRG_PWR_GOOD` / SGM41529 `nPG` |

## Test profile and safety

The example programs this conservative bench profile in `main/board_config.h`:

| Parameter | Default |
| --- | ---: |
| Battery regulation voltage | 8.40 V |
| Fast-charge current | 0.50 A |
| Input current limit | 1.00 A |
| Pre-charge current | 0.10 A |
| Termination current | 0.10 A |
| Die thermal regulation threshold | 80 C |

USB source detection is left enabled. If BC1.2 detection selects more than
1.00 A, the monitor restores the configured 1.00 A cap; lower source limits
such as a 500 mA SDP result are retained.

The SGM41529 is a two-cell Li-ion/Li-polymer charger. Before flashing, verify
that the connected pack is a protected 2-series pack rated for an 8.40 V charge
voltage and the configured currents. Reduce the constants in `board_config.h`
when the battery data sheet requires lower limits. Do not use this profile for
single-cell, LiFePO4, damaged, swollen, or unidentified batteries.

Run the first test on a current-limited bench supply or a known USB power
source. Monitor battery temperature and disconnect power immediately if the
pack or charger heats abnormally. This example is for bench validation; a
production battery-management design still needs independent hardware safety
review and system-level charge qualification.

## Build and flash

Use the ESP-IDF 6.1 environment required by ESP32-S31:

```powershell
cd examples/charger_test
idf.py --preview --build-dir build_s31 set-target esp32s31
idf.py --preview --build-dir build_s31 build
idf.py --preview --build-dir build_s31 -p PORT flash monitor
```

The LCD uses `lvgl/lvgl^9.6.0~1` and the local ESP32-S31-compatible copy of
`espressif/esp_lcd_st7796^1.4.0` from `display_touch_test`.
