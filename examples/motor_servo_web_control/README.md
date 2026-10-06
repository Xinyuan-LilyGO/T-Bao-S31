# Motor and servo web control (ESP32-S31)

This ESP-IDF 6.1 example tests the T-Bao-S31 DRV8833 dual motor driver and an
ES9051 servo. It scans nearby Wi-Fi networks, connects to the locally configured
target network, starts an HTTP control server, and shows the browser URL on the
320x320 ST7796S display.

The LCD uses a white LVGL theme and software clockwise 90-degree rotation. The
verified full-screen controller configuration keeps hardware XY swap disabled.

## Pin assignment

| Function | T-Bao-S31 signal | GPIO |
| --- | --- | ---: |
| ES9051 servo command | `M_PWM` | 5 |
| ES9051 5 V power enable | XL9555 `P05` / `PWR_EN` | I2C `0x22` |
| DRV8833 driver enable | XL9555 `P11` / `DRV_EN` | I2C `0x22` |
| DRV8833 motor A input 1 | `DRV_AIN1` | 15 |
| DRV8833 motor A input 2 | `DRV_AIN2` | 14 |
| DRV8833 motor B input 1 | `DRV_BIN1` | 13 |
| DRV8833 motor B input 2 | `DRV_BIN2` | 12 |

XL9555 register masks are linear: P00-P07 use bits 0-7 and P10-P17 use
bits 8-15. Therefore board signal `P11` (port 1, pin 1) uses mask bit 9,
not bit 11. Using bit 11 leaves `DRV_EN` in input mode and the DRV8833 in
sleep, even though web commands are accepted.

Motor PWM runs at 20 kHz. A positive command drives input 1 with PWM while
input 2 stays low; a negative command swaps those inputs. A zero command keeps
both inputs low so the motor coasts.

The ES9051 signal runs at 50 Hz. The web page exposes a conservative default
pulse range of 1100-1900 us with 1500 us as center. If the mechanism reaches an
endpoint or the servo strains, reduce `kServoPulseMinUs` and
`kServoPulseMaxUs` in `main/actuator_control.h` before further testing.

## Power and safety

- The program enables the board's 5 V rail through XL9555 P05 before accepting
  servo commands. GPIO5 is only the command signal.
- XL9555 P11 starts low while all four DRV8833 inputs are initialized to zero,
  then goes high and remains enabled. Motor stop and failsafe operations keep
  AIN1, AIN2, BIN1, and BIN2 low instead of turning P11 off.
- Startup reads P05 and P11 back from the XL9555 input register. An enable-line
  wiring or register-mapping fault now stops initialization with an explicit
  `actuator enable readback mismatch` log.
- Use a motor supply sized for the connected motors and the DRV8833 board.
- Connect the controller, servo supply, and motor driver grounds together.
- Keep wheels or linkages unloaded during the first test.
- All PWM outputs start disabled. A two-second browser heartbeat timeout, Wi-Fi
  disconnect, or `STOP ALL` command stops both motors and disables servo PWM.

## Local Wi-Fi credentials

Real credentials belong in `main/wifi_credentials.h`. This file is explicitly
ignored by the example's `.gitignore` and must not be committed.

```powershell
Copy-Item main/wifi_credentials.h.example main/wifi_credentials.h
```

Set `WIFI_TEST_TARGET_SSID` and `WIFI_TEST_TARGET_PASSWORD` in that local file.
The password is not logged, and the example uses RAM-backed Wi-Fi configuration
instead of persisting it in NVS.

## Build and flash

Use the ESP-IDF 6.1 environment required by ESP32-S31:

```powershell
cd examples/motor_servo_web_control
idf.py --preview --build-dir build_s31 set-target esp32s31
idf.py --preview --build-dir build_s31 build
idf.py --preview --build-dir build_s31 -p PORT flash monitor
```

After a successful connection, the screen shows a URL such as
`http://192.168.1.50`. Open that address from a phone or computer on the same
local network.

## Browser controls

- Motor A and Motor B: signed speed from -100% to +100%.
- ES9051: PWM enable and pulse width from 1100 us to 1900 us.
- `ZERO A` / `ZERO B`: stop one motor.
- `CENTER`: enable the servo at 1500 us.
- `STOP ALL`: stop both motors and disable servo PWM.

The page and REST endpoints are intended for trusted local bench testing. They
do not implement authentication or encrypted transport.
