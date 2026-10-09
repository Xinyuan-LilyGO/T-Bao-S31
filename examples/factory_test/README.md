# T-Bao-S31 Factory Test

This is the independent production test firmware for **T-Bao-S31**. It targets
ESP32-S31 (not ESP32-S3) and requires ESP-IDF 6.1 or newer 6.1-compatible
preview tooling.

The first visible screen after power-on is kept behind a disabled backlight
until it has rendered:

- `T-Bao-S31`
- `Factory Test`
- `FW <PROJECT_VER>`

The firmware then waits for `START` on the touch screen or `RUN` on the serial
port. A camera power profile must be selected first.

The LVGL output is rotated 90 degrees clockwise in software. The
FT6336 coordinates use the inverse transform so touch targets remain aligned;
hardware ST7796S XY swapping stays disabled because this panel revision clips
or overlaps partial update windows when controller rotation is enabled.

## Safety

- XL9555 starts and returns to the audited `0xFC5F` output state: 5 V,
  amplifier, SD power and motor driver disabled; charger `nCE` disabled; touch
  reset released. If this state cannot be established or restored and read
  back, the run stops with `SAFE_OUTPUT_FAILED`/`CLEANUP_FAILED`; later
  actuator tests are not started.
- GPIO12..15 are driven low and held while the motors are idle. This keeps the
  bridge inputs stable across normal resets and when a running factory image is
  reset into the ROM downloader. For a blank device, power-cycle, or any case
  where no prior firmware established the hold, fit hardware pull-downs on the
  four bridge inputs and `DRV_EN`; application firmware cannot run in ROM
  download mode.
- Camera power is never probed by changing voltage. Select `OV2640` (DVDD
  register `0x57`) or `OV3660` (`0x7D`) before `RUN`. A detected mismatch fails
  with `PROFILE_MISMATCH` and reports the address and PID.
- Charging is blocked until the SGM41529 part ID, profile readback, VBUS, PG,
  VBAT, NTC and fault status pass. Both `nCE` and `EN_CHG` are disabled and
  read back on normal exit.
- SD cards are never formatted. Only `/sdcard/tbao_factory_test.bin` is created
  and removed.
- Test results are not stored in NVS. Only `factory_cfg/camera` is persisted.

Use a protected 2S battery, protected VBUS source, two motors, a servo, a
dedicated FAT SD card and a fixed factory Wi-Fi access point.

## Station Configuration

Create the ignored local configuration before building:

```powershell
Copy-Item main\factory_config.h.example main\factory_config.h
```

Edit `main/factory_config.h` with the station SSID/password and optional
threshold overrides. The password is used only to configure the Wi-Fi driver;
it is not logged or included in JSON output.

## Build and Flash

Activate ESP-IDF 6.1, then run from this directory:

```powershell
idf.py --preview --build-dir build_s31 set-target esp32s31
idf.py --preview --build-dir build_s31 build
idf.py --preview --build-dir build_s31 -p COMx flash monitor
```

The project uses 16 MB flash and a single 6 MiB factory application partition
without OTA slots. `dependencies.lock` is committed so managed component
versions remain reproducible.

## Serial Protocol

The console accepts one ASCII command per line:

```text
CAMERA OV2640
CAMERA OV3660
RUN
PASS <id>
FAIL <id>
RETRY <id>
RETEST ALL
STATUS
```

Test IDs are:

```text
display_touch touch_button io60_button boot_button microphone speaker camera charger motor_a motor_b servo sd_card wifi
```

`PASS`, `FAIL`, and manual `RETRY` are accepted only for the test currently in
`WAITING_MANUAL`. From the summary screen, `RETRY <id>` is accepted only for a
failed item. The camera profile is locked after `RUN`.

Progress lines start with `FACTORY_EVENT `. The final machine-readable line is:

```text
FACTORY_RESULT {"schema":1,"run_id":"...","device":"T-Bao-S31",...}
```

The result includes firmware/IDF versions, STA MAC, configured and detected
camera information, overall result, duration, and each test's status, stable
error code and measurements. It never includes the Wi-Fi password.

## Test Order

1. Display patterns and 9-point FT6336 touch grid, followed by display confirmation.
2. GPIO11 capacitive touch pad (`TOUCH_CH5`) detection.
3. GPIO60 active-low button detection.
4. GPIO61 `ESP32_BOOT` active-low button detection.
5. ES7210 stereo microphone level and clipping check.
6. ES8389 1 kHz speaker tone and operator confirmation/replay.
7. Camera identity, 30 decoded frames and frozen-image confirmation/replay.
8. SGM41529 protected 8.4 V / 500 mA short charge-path test.
9. Motor A, motor B and servo movement confirmations.
10. SDMMC 64 KiB write, verify, power-cycle remount and verify.
11. Wi-Fi scan, RSSI, DHCP and 3-packet gateway ping.

The GPIO11, GPIO60 and BOOT tests require the operator to press the indicated
physical pad/button after the screen prompts. GPIO60 and GPIO61 are sampled as
active-low inputs with internal pull-ups and debounce. GPIO11 is measured with
the ESP32-S31 capacitive-touch controller; the test records its baseline,
threshold and largest observed change in the result JSON. Each input test
releases its GPIO/touch resource during cleanup.

An individual failure does not stop later tests. Manual confirmation and touch
collection time out after 30 seconds. Safety cleanup runs after every attempt.

## Core Tests

The core tests cover command parsing, state transitions, timeout/retry
decisions, cleanup idempotence, microphone/charger thresholds, aggregation,
reset behavior and JSON escaping.

Build the ESP32-S31 Unity test image:

```powershell
cd test_apps\core
idf.py --preview --build-dir build_s31 set-target esp32s31
idf.py --preview --build-dir build_s31 build
```

Run the same cases natively without hardware:

```powershell
cd host
cmake -S . -B build -G Ninja -DCMAKE_CXX_COMPILER=g++
cmake --build build
ctest --test-dir build --output-on-failure
```

Before release, perform the hardware fault-injection matrix and at least 20
continuous full runs while checking heap, tasks, peripheral ownership and all
actuator outputs. Those checks require the production fixture and are not
replaced by the software tests.
