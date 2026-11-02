# T-Bao-S31

[中文说明](README_CN.md)

T-Bao-S31 is an ESP-IDF board project for the Espressif **ESP32-S31**. The
repository contains board bring-up examples, production-oriented factory
tests, and focused tests for the display, camera, audio, charger, SD card,
Wi-Fi, I2C, motors, and servo. The target is **ESP32-S31, not ESP32-S3**.

The examples are maintained for **ESP-IDF 6.1**. Older IDF releases are not
supported and can fail to configure or compile the examples.

## Board at a glance

| Item | Design information |
| --- | --- |
| MCU | Espressif ESP32-S31; use the [ESP32-S31 product information](https://www.espressif.com/en/products/socs/esp32-s31) and the matching IDF 6.1 target |
| Flash | 16 MB board configuration; individual examples may select their own partition layout |
| Display | 1.54-inch ST7796S, 320(H) × 320(V), BGR panel, 8-bit Intel 8080 (I80) interface |
| Touch | FT6336/FT6336U, native 240 × 240 coordinates mapped to the 320 × 320 display |
| Camera | OV2640 is the current default; OV3660 is supported by the camera example when its power profile is selected |
| Audio | ES8389 playback codec, ES7210 microphone ADC, and NS4150B speaker amplifier |
| Power and charging | SGM38121 camera rail manager, SY8113 3.3 V/5 V regulators, and SGM41529 protected 2-cell charger |
| Expansion I/O | XL9555 at I2C address `0x22` controls power rails, touch reset, charger, speaker, SD power, and motor enable |
| Motion | DRV8833 dual motor driver and an ES9051 servo connected to the motor/servo header |
| Storage | TF card through the ESP32-S31 native SDMMC 4-bit interface |
| Wireless | Wi-Fi scan, connection, and local HTTP control examples |

Espressif specifies a dual-core 32-bit RISC-V CPU up to 320 MHz, 512 KB SRAM,
2.4 GHz Wi-Fi 6, Bluetooth and IEEE 802.15.4 support for the ESP32-S31. These
are chip capabilities; this repository currently demonstrates Wi-Fi, not all
wireless protocols. The camera example disables PSRAM and uses the hardware
JPEG decoder. Do not infer a fitted PSRAM capacity from the chip specification.

The board includes 3.3 V, 1.8 V and camera-specific voltage domains. Check the
schematic before connecting external signals. The controlled 5 V rail supplies
the servo and speaker; GPIO5 only carries the servo command signal.

## Hardware map

### I2C buses and addresses

Addresses below are **7-bit I2C addresses**, as used by ESP-IDF's I2C master
driver and the `i2c_tools` example.

| Bus | Pins | Speed used by the examples | Device | Address | Purpose |
| --- | --- | ---: | --- | ---: | --- |
| Main I2C0 | SDA `GPIO0`, SCL `GPIO1` | 100 kHz for power and diagnostics; display/audio examples may use 400 kHz | ES8389 | `0x10` | Audio playback codec |
| Main I2C0 | SDA `GPIO0`, SCL `GPIO1` | Same bus | XL9555 | `0x22` | Power, reset, charger, speaker, SD and motor control |
| Main I2C0 | SDA `GPIO0`, SCL `GPIO1` | 100 kHz | SGM38121 | `0x28` | Camera DVDD, DOVDD and AVDD rails |
| Main I2C0 | SDA `GPIO0`, SCL `GPIO1` | Same bus | FT6336/FT6336U | `0x38` | Touch controller |
| Main I2C0 | SDA `GPIO0`, SCL `GPIO1` | 100 kHz | ES7210 | `0x40` | Microphone ADC |
| Main I2C0 | SDA `GPIO0`, SCL `GPIO1` | Check sensor configuration | MPU-6050 module (if fitted) | `0x68` | 3-axis accelerometer and 3-axis gyroscope; no dedicated example currently |
| Main I2C0 | SDA `GPIO0`, SCL `GPIO1` | 100 kHz | SGM41529 | `0x6B` | 2-cell charger and battery monitor |
| Camera I2C1 | SDA `GPIO3`, SCL `GPIO4` | 100 kHz | OV2640 sensor | `0x30` | Camera identification and configuration |
| Camera I2C1 | SDA `GPIO3`, SCL `GPIO4` | 100 kHz | OV3660 sensor | `0x3C` | Camera identification and configuration |

The schematic also labels the LCD backlight controller as AW9364 at `0x15`.
The display examples control the backlight through `GPIO16` according to the
board configuration, so verify the fitted hardware before writing directly to
that address. The generic sensor connector is marked `0x68/0x28` in the
schematic; this does not mean that MPU-6050 supports `0x28`. An alternative
sensor using `0x28` would conflict with SGM38121 on the main bus.

The camera connector's `0x78` annotation is an 8-bit write address for the
OV3660's 7-bit `0x3C`. Use `0x30` for OV2640 and `0x3C` for OV3660 in software.

### ESP32-S31 signal assignment

| Function | GPIO or control | Notes |
| --- | --- | --- |
| Main I2C | `GPIO0=SDA`, `GPIO1=SCL` | Shared by the power board and most peripherals |
| Camera I2C | `GPIO3=CAM_SDA`, `GPIO4=CAM_SCL` | Separate I2C1 bus |
| Servo PWM | `GPIO5` | 50 Hz, 1100–1900 µs recommended, 1500 µs center |
| Audio I2S | `GPIO6=MCLK`, `GPIO7=DSDIN`, `GPIO8=BCLK`, `GPIO9=LRCK`, `GPIO10=SDOUT` | Shared ES8389/ES7210 audio clock and data wiring |
| DRV8833 Motor A | `GPIO15=AIN1`, `GPIO14=AIN2` | Positive command uses AIN1, negative uses AIN2 |
| DRV8833 Motor B | `GPIO13=BIN1`, `GPIO12=BIN2` | Positive command uses BIN1, negative uses BIN2 |
| LCD I80 | `D0..D7=GPIO44,43,42,40,39,38,37,36`; `RS=18`, `WR=17`, `CS=19`, `RST=35` | 320 × 320 ST7796S panel |
| LCD backlight | `GPIO16` | PWM/backlight signal; hardware rail details depend on the panel board |
| Camera parallel data | `D0..D7=GPIO46..53`, `PCLK=54`, `XCLK=55`, `VSYNC=56`, `HREF=57`, `RESET=45` | `esp_cam_io_parl` parallel camera interface |
| TF SDMMC | `D0..D3=GPIO20..23`, `CLK=GPIO24`, `CMD=GPIO25` | 4-bit native SDMMC; XL9555 P10 enables card power |
| USB | `GPIO33/34` through the board USB nets | Do not assume the ESP32-S31 package USB pin numbers are board GPIO numbers |

### XL9555 outputs and inputs

XL9555 port bits are linear: P00–P07 are bits 0–7 and P10–P17 are bits 8–15.
Therefore **P11 is bit 9 (`0x0200`)**, not bit 11.

| XL9555 pin | Board function | Direction / active behavior |
| --- | --- | --- |
| P00 | FT6336 touch interrupt | Input |
| P01 | FT6336 touch reset | Output |
| P02 | SGM41529 charge `nCE` | Output; charger must remain disabled until the battery checks pass |
| P03 | Charger interrupt | Input |
| P04 | Charger power-good | Input |
| P05 | 5 V rail / `PWR_EN` | Output; enables servo, speaker, and related 5 V loads |
| P06 | Optional sensor interrupt | Input on board revisions that populate it |
| P07 | NS4150B speaker enable | Output |
| P10 | TF/SD power enable | Output |
| P11 | DRV8833 `DRV_EN` | Output; high enables the motor bridge |

The complete physical-pin and connector mapping is documented in
[`docs/pinmap_cn.md`](docs/pinmap_cn.md), including the MAIN/PWR board harness,
camera connector, audio wiring, SD socket, USB nets, and schematic revision
notes.

## Hardware documents and component references

The repository includes the board schematics and several IC data sheets in
[`hardware/`](hardware/). The component links below point to the driver or
manufacturer reference when an exact public data sheet is not included.

| Part or module | Function | Reference |
| --- | --- | --- |
| ESP32-S31 | Main MCU | [Espressif ESP32-S31](https://www.espressif.com/en/products/socs/esp32-s31) |
| ST7796S | LCD controller | [ST7796S data sheet](https://www.displayfuture.com/Display/datasheet/controller/ST7796s.pdf) · [ESP-IDF component](https://components.espressif.com/components/espressif/esp_lcd_st7796) |
| FT6336/FT6336U | Capacitive touch controller | [FocalTech product site](https://www.focaltech-electronics.com/) · [driver component](https://components.espressif.com/components/lambage/esp_lcd_touch_ft6336u) |
| XL9555 | 16-bit I2C GPIO expander | [driver component](https://components.espressif.com/components/sheldonix/esp_io_expander_xl9555) · [PWR schematic](<hardware/T-Bao-S31_PWR V0.1 26-09-04.PDF>); exact IC data sheet not bundled |
| SGM38121 | Camera power management | [local SGM38121 data sheet](<hardware/SGM38121.pdf>) |
| OV2640 / OV3660 | Camera sensors | [OmniVision camera products](https://www.ovt.com/) · [camera component](https://components.espressif.com/components/haqqscripter/esp_cam_io_parl) |
| AW9364 | LCD backlight controller, schematic address `0x15` | [MAIN schematic](<hardware/T-Bao-S31_MAIN V0.1 26-09-04.PDF>) |
| ES8389 | Audio codec / DAC | [local ES8389 data sheet](<hardware/ES8389.pdf>) · [ESP codec component](https://components.espressif.com/components/espressif/esp_codec_dev) |
| ES7210 | Microphone ADC | [local ES7210 data sheet](<hardware/ES7210.PDF>) · [ESP codec component](https://components.espressif.com/components/espressif/esp_codec_dev) |
| NS4150B | Speaker amplifier | [board audio schematic](<hardware/T-Bao-S31_PWR V0.1 26-09-04.PDF>) |
| SY8113 / SY6280 | 3.3 V, 5 V and SD power rails | [PWR schematic](<hardware/T-Bao-S31_PWR V0.1 26-09-04.PDF>) |
| SGM41529 | 2-cell charger; use a separately protected battery pack | [local SGM41529 data sheet](<hardware/SGMICRO-SGM41529.pdf>) |
| DRV8833 | Dual H-bridge motor driver | [local DRV8833 data sheet](<hardware/DRV8833.pdf>) |
| ES9051 servo | External servo module | [motor/servo board schematic](<hardware/T-Bao-S31_Motor .PDF>) |
| MPU-6050 or board sensor | Optional motion sensor | [local MPU-6050 data sheet](<hardware/MPU-6050.pdf>) |
| ST7796S/FT6336 board and power board | Board-level wiring | [MAIN schematic](<hardware/T-Bao-S31_MAIN V0.1 26-09-04.PDF>) · [PWR schematic](<hardware/T-Bao-S31_PWR V0.1 26-09-04.PDF>) |

The exact fitted component, address strap, connector wiring, and power rail
must be checked against the schematic and the physical board revision before
using a replacement module.

## Repository layout

```text
.
├── .github/workflows/
│   └── build-examples.yml       # ESP-IDF 6.1 CI build for every example
├── examples/
│   ├── audio_test/              # ES8389 playback and ES7210 microphone checks
│   ├── camera_display/          # OV2640/OV3660 capture and LCD display
│   ├── charger_test/            # SGM41529 2S charger monitor
│   ├── display_touch_test/      # ST7796S color bars and FT6336 touch
│   ├── factory_test/             # Production-style complete board test
│   ├── i2c_tools/                # Interactive I2C console tools
│   ├── motor_servo_web_control/ # Browser motor and servo controller
│   ├── sdcard_test/              # SDMMC/FAT read-write verification
│   └── wifi_scan_display/        # Wi-Fi scan and display UI
├── hardware/                    # Schematics and component data sheets
├── docs/                        # Project documentation assets
├── other/                       # Design notes and test records
├── README.md
└── README_CN.md
```

Each example is a standalone ESP-IDF project. Its `main/` directory contains
the application code and board configuration, while `sdkconfig` and
`sdkconfig.defaults` select the ESP32-S31 target and memory/partition options.
`main/idf_component.yml` declares component version constraints; a tracked
`dependencies.lock`, where present, records resolved versions. Generated
`sdkconfig`, `managed_components/` and build directories are ignored by many examples.
Local engineering rules in `AGENTS.md` are also ignored by this repository.
For reproducibility, retain each example's tracked defaults and overrides;
do not copy a component from another example without checking its IDF 6.1
compatibility.

The main component versions currently requested by the examples are:

| Component | Version constraint | Purpose |
| --- | --- | --- |
| `espressif/esp_lcd_st7796` | `^1.4.0` | Shared local compatibility copy in `examples/display_touch_test/components/esp_lcd_st7796` |
| `lambage/esp_lcd_touch_ft6336u` | `^1.0.8` | Touch input |
| `sheldonix/esp_io_expander_xl9555` | `^0.8.0` | Expander control |
| `espressif/esp_codec_dev` | `^1.6.2` | ES8389 and ES7210 audio drivers |
| `haqqscripter/esp_cam_io_parl` | `^0.1.0` | Parallel camera; factory test has a local override |
| `lvgl/lvgl` | `^9.6.0~1` | UI in the LVGL-based examples |

The standalone `espressif/es7210` version `1.0.1~1` declares IDF `<6.0`;
audio examples use the ES7210 driver inside `esp_codec_dev` instead. Keep the
whole repository when building: several examples refer to the shared LCD
component by a relative path.

## Example overview

| Example | Main function | Hardware needed |
| --- | --- | --- |
| `audio_test` | Generates a 1 kHz ES8389 tone, samples ES7210 stereo microphone data, and displays RMS/peak meters | LCD, XL9555, ES8389, ES7210, speaker and microphones |
| `camera_display` | Powers and identifies OV2640/OV3660, captures JPEG frames, decodes them with the ESP32-S31 JPEG engine, and displays them | LCD, camera module, SGM38121 rails |
| `charger_test` | Verifies SGM41529 identity/configuration and monitors a conservative protected 2S charging profile | Protected 2S battery, USB/VBUS source, charger board |
| `display_touch_test` | Shows LCD color bars and maps FT6336 touch coordinates to the 320 × 320 screen | LCD, touch panel, XL9555 |
| `factory_test` | Runs an ordered production test with safety cleanup, operator confirmations, stable error codes, and machine-readable JSON | Complete fixture; see its README before connecting power |
| `i2c_tools` | Provides `i2cconfig`, `i2cdetect`, `i2cget`, `i2cset`, and `i2cdump` through the serial console | Any accessible I2C device |
| `motor_servo_web_control` | Serves a local web UI for DRV8833 Motor A/B and ES9051 servo control | Motors, DRV8833, servo, XL9555, Wi-Fi |
| `sdcard_test` | Enables TF power, mounts without formatting, writes/reads a deterministic file, remounts, and verifies again | FAT-formatted TF card |
| `wifi_scan_display` | Scans networks, displays RSSI/channel/security, optionally connects to a configured SSID, and shows the DHCP address | LCD and a local Wi-Fi network |

See each example's README for its exact pins, test output, credentials, and
hardware-specific limitations.

## Build, flash, and monitor

### 1. Install the toolchain

Install ESP-IDF **6.1** using the [official installation guide](https://docs.espressif.com/projects/esp-idf/en/latest/esp32s31/get-started/index.html).
On Windows, open an ESP-IDF 6.1 terminal; on Linux/macOS, install the IDF tools
and source the IDF checkout's `export.sh`. Clone this repository:

```sh
git clone https://github.com/Xinyuan-LilyGO/T-Bao-S31.git
cd T-Bao-S31
```

The commands below use PowerShell. Verify both the version and
the target name before building:

```powershell
idf.py --version
idf.py --preview --list-targets | Select-String esp32s31
```

The project target is `esp32s31`; selecting `esp32s3` builds for a different
chip and is not supported. The examples use the ESP Component Registry, so the
first configure/build may download managed components and require network
access.

### 2. Build an example

Run from the selected example directory. A separate build directory avoids
mixing outputs between examples:

```powershell
cd examples\display_touch_test
idf.py --preview --build-dir build_s31 set-target esp32s31
idf.py --preview --build-dir build_s31 build
```

Replace `display_touch_test` with any directory in `examples/`. Run `set-target`
once for a fresh project; it resets the generated configuration, so save any
intentional menuconfig changes first. The CI workflow
performs the same build for every project using the `espressif/idf:release-v6.1`
container.

### 3. Flash and monitor

Connect the board through its configured serial/USB interface, then replace
`COMx` with the actual port:

```powershell
idf.py --preview --build-dir build_s31 -p COMx flash monitor
```

On Linux/macOS, use forward slashes in paths, `cp` instead of `Copy-Item`, and
the actual device port, such as `/dev/ttyACM0` or `/dev/cu.usbmodem...`. The
`idf.py` commands are otherwise the same. Use the data-capable main USB-C
connector; the magnetic USB connector is power-only in the supplied schematic.

Exit the serial monitor with `Ctrl-]`. Keep the board connected to a suitable
power source and follow the example README before enabling motors, a servo, a
charger, or a camera rail.

### 4. Local configuration files

From the repository root, create only the files needed by your chosen example,
then configure them and rebuild before flashing. Credentials and station
settings are intentionally ignored by Git:

```powershell
Copy-Item examples\wifi_scan_display\main\wifi_credentials.h.example `
  examples\wifi_scan_display\main\wifi_credentials.h
Copy-Item examples\factory_test\main\factory_config.h.example `
  examples\factory_test\main\factory_config.h
Copy-Item examples\motor_servo_web_control\main\wifi_credentials.h.example `
  examples\motor_servo_web_control\main\wifi_credentials.h
```

Fill in only local test credentials. Passwords must not be committed or
printed to the serial log.

### 5. Run and inspect the result

| Example | After flashing |
| --- | --- |
| [Display/touch](examples/display_touch_test/README.md) | Inspect color bars, sweep all screen edges, and check the touch marker and serial coordinates. |
| [Audio](examples/audio_test/README.md) | Use `MIC`, `DAC` or `AUTO`; speak into both microphones and confirm actual speaker sound. |
| [Camera](examples/camera_display/README.md) | Select the matching power profile in `main/board_config.h` before fitting the sensor. Inspect the preview and serial diagnostics; unsupported sensors or profile mismatches report identity information. |
| [Charger](examples/charger_test/README.md) | Verify the battery before power-on: the default profile can enable charging automatically after checks. Inspect voltage/current/temperature and faults. |
| [Factory](examples/factory_test/README.md) | Set station credentials, select `CAMERA OV2640` or `CAMERA OV3660`, then `RUN` (or use the touch UI). Follow manual prompts and collect `FACTORY_RESULT` JSON. |
| [I2C tools](examples/i2c_tools/README.md) | Configure one bus, then scan it with the commands below. A camera needs its rails, clock and reset sequence before it can respond. |
| [Motor/servo](examples/motor_servo_web_control/README.md) | Open the LCD's HTTP URL from the same network. Test motors unloaded at 30–50%; center the servo at 1500 µs. Press `STOP ALL` before downloading. |
| [SD card](examples/sdcard_test/README.md) | Use a dedicated FAT card and look for `SD CARD TEST: PASS`; `/sdcard/tbao_s31_sd_test.bin` is overwritten. No automatic formatting. |
| [Wi-Fi](examples/wifi_scan_display/README.md) | Check the scan list and DHCP address. Without local credentials, this example scans but does not connect. |

```text
i2cconfig --scl=1 --sda=0 --freq=100000
i2cdetect
i2cconfig --scl=4 --sda=3 --freq=100000
i2cdetect
```

Start with display/touch and I2C diagnostics before attempting a full factory
run. For a build error, capture the IDF version, example name and first compiler
error. For a hardware failure, also record board revision, camera profile,
power source and serial log; a successful I2C register readback is not a voltage
measurement.

## CI build

[`build-examples.yml`](.github/workflows/build-examples.yml) discovers every
direct child of `examples/` containing a `CMakeLists.txt`, then builds each one
in an ESP-IDF 6.1 `esp32s31` matrix job. It runs for changes under `examples/`,
for changes to the workflow, and on manual dispatch. A failed example fails the
workflow, while `fail-fast: false` lets the other examples finish and exposes
all compile errors in one run.

## Bring-up and safety notes

- **Use the correct silicon and IDF.** ESP32-S31 and ESP32-S3 are different
  targets. Keep IDF at 6.1; do not “fix” an IDF 6.1 component by downgrading the
  project to an older release.
- **Power camera rails first.** Configure SGM38121 at `0x28` on I2C0 and enable
  the required rails before probing the sensor on I2C1. Select the OV2640 or
  OV3660 profile in the camera example; do not guess a core voltage by changing
  rails while the camera is connected.
- **Protect the charger.** `charger_test` is for a protected 2-series battery
  and an 8.40 V charge profile. It is not a 1-cell charger example. Use a
  current-limited source and stop immediately if the pack or charger heats.
- **Keep actuator outputs safe.** DRV8833 inputs are GPIO12–15 and `DRV_EN` is
  XL9555 P11 (`0x0200`). Initialize all four inputs low, release any inherited
  `gpio_hold` before LEDC, and hold them low again when idle or before reset and
  download. Enable P11 only after a nonzero motor command is ready. Servo PWM
  is GPIO5 at 50 Hz; enable XL9555 P05 and wait for the 5 V rail to settle
  before sending a pulse. Use `STOP ALL` and verify motion has stopped before
  flashing motor firmware. ROM download mode cannot run the application:
  on blank boards, after power loss, or if safe stop is unavailable, disconnect
  motor power for flashing. Hardware pull-downs on bridge inputs and enable
  are needed for protection independent of previous firmware.
- **Use the I2C tool carefully.** The diagnostic defaults are:
  `i2cconfig --scl=1 --sda=0 --freq=100000` for the main bus and
  `i2cconfig --scl=4 --sda=3 --freq=100000` for the camera bus, followed by
  `i2cdetect`. Do not write registers on an unknown device.
- **Coordinate shared resources.** Touch, audio, charger, power, and sensor
  devices share I2C0. When combining examples, share the bus/expander handle
  and coordinate GPIO, LEDC timer and I2S ownership. P05 supplies both the servo
  and speaker rail, so disabling it affects both loads.
- **Keep web control local.** The motor web server has no authentication or
  TLS. A two-second heartbeat timeout or Wi-Fi disconnect stops the outputs;
  this does not replace a physical way to remove motor power.
- **Treat replacement modules as new hardware.** Camera address, sensor ID,
  touch active level, pull-ups, voltage domains, and connector orientation must
  be checked against the fitted module and board revision.
- **Bench-test with loads disconnected.** First boot and first actuator test
  should use unloaded motors, a servo with free travel, a current-limited
  supply, and a common ground between the controller and external drivers.

This repository is a hardware validation and development project. Passing a
software build or an automated test does not replace electrical, thermal,
battery, motor, or production safety qualification.
