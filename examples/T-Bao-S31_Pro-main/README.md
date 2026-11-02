# T-Bao S31 VoCat

AI pet firmware for the **T-Bao S31 / ESP32-S31**. It combines Xiaozhi voice chat, a round touch UI, camera vision, media playback, and motor control in one ESP-IDF application.

> Status: the T-Bao S31 board definition, camera/audio/display drivers, and application features are integrated and the firmware can be built.

[中文说明](README_cn.md)

## Features

- **Xiaozhi voice chat**: offline wake words ("你好喵伴" and "你好小智") with WebSocket cloud ASR, LLM, and TTS, plus emote states and live captions.
- **Touch UI**: LVGL 9 and `esp_emote_expression` with launcher, dialogue, camera, music, video, game, timer, motion, settings, and about pages.
- **Camera and vision**: OV2640/OV3660-compatible PARLIO JPEG camera with on-screen preview, HTTP MJPEG `/stream`, JPEG `/snapshot`, recording, lightweight face detection/face following, and guard mode.
- **Media playback**: local audio/video from microSD; online music search resolves a playable URL from multiple public APIs.
- **Motion control**: dual motors and a servo with touch, Web, voice/MCP control sources, speed limits, motion sequences, and failsafe handling.
- **Connectivity and control**: Wi-Fi provisioning, volume/brightness settings, MCP tools, and an HTTP preview service.

## Hardware

The supported board name is `tbao_s31_board`; its definition is in [`boards/tbao_s31_board`](boards/tbao_s31_board).

| Module | Configuration |
| --- | --- |
| MCU | ESP32-S31, target `esp32s31`, 16 MB Flash + PSRAM |
| LCD | ST7796S, approximately 1.54 inch, 320x320, 8-bit PARLIO/I80; data GPIO36/37/38/39/40/42/43/44 |
| Touch | FT6336/FT6336U at I2C address `0x38`; reset and interrupt are provided by XL9555 |
| Audio | ES8389 playback + ES7210 dual-microphone capture; I2S GPIO6/7/8/9/10 |
| Camera | OV2640/OV3660, PARLIO JPEG; SCCB GPIO3/4, RESET GPIO45, XCLK GPIO55, data GPIO46..53, PCLK GPIO54, HREF GPIO57 |
| Storage | microSD over 4-bit SDMMC: D0..D3 = GPIO20..23, CLK = GPIO24, CMD = GPIO25, mounted at `/sdcard` |
| Expansion/actuators | XL9555 GPIO expander, dual motor driver, servo output, and LCD backlight PWM |

See [`pinmap_cn.md`](pinmap_cn.md) for the complete pin, power, and connector mapping.

## Requirements

- ESP-IDF **6.1 or newer**, configured for target `esp32s31`.
- Python, CMake, Ninja, and the `riscv32-esp-elf` toolchain supplied by ESP-IDF.

Check the environment:

```bash
idf.py --version
idf.py --list-targets | findstr esp32s31
```

## Build and flash

Run these commands from the repository root:

```bash
# Select the target on a fresh checkout
idf.py set-target esp32s31

# Optional: re-resolve or fetch dependencies
idf.py reconfigure

# Build
idf.py build

# Flash (append `-p COM12` when a specific port is needed)
idf.py flash
```

### First-time provisioning and model setup

After flashing, follow the on-screen instructions to provision Wi-Fi. Then open [xiaozhi.me](https://xiaozhi.me/), go to the console, choose "Add device" in the upper-right corner, and enter the six-digit code shown on the display or in the serial log to configure the cloud model.

### Face-follow tuning

Face-follow behavior depends on static friction, vehicle weight, and the surface material. If tracking is unstable, adjust the default value at line 26 of [`main/face_follow_controller.c`](main/face_follow_controller.c):

```c
#define FACE_FOLLOW_DEFAULT_YAW_DRIVE 0.14f
```

Make small changes around `0.14f`; keep the default or consult an AI assistant when unsure about the parameter.

After changing files under `boards/tbao_s31_board`, run:

```bash
idf.py gen-bmgr-config -b tbao_s31_board -c ./boards
idf.py reconfigure
idf.py build
```

## SD-card layout

The application mounts the card at `/sdcard`:

| Content | Location |
| --- | --- |
| Music | Playable audio files in `/sdcard` |
| Video | MP4/AVI/MJPEG files in `/sdcard` |

> Note: downloaded MP4 files must be transcoded with FFmpeg before playback. If FFmpeg is not installed locally, use the [FFmpeg online tool](https://ffmpeg.gcc.ac.cn/advanced_zh.html).
>
> Parameter 1: `-y`
>
> Parameter 2:
>
> ```text
> -map 0:v:0 -vf scale=320:320 -c:v mjpeg -q:v 5 -pix_fmt yuvj420p -an -r 10 -movflags +faststart -f mp4
> ```

## Project layout

```text
main/                         application, pages, camera, audio, motion, and HTTP services
boards/tbao_s31_board/       T-Bao S31 Board Manager definition
components/                   local components (ESP-DL, face detection, board adapters, and more)
managed_components/           ESP-IDF Component Manager dependencies
patches/                      repeatable patches for managed components
assets/                       boot splash and UI assets
docs/                         design and porting notes
tests/                        Python contract tests
```

## License

Application code is Apache-2.0 unless a file states otherwise. Components and model/asset packages retain their upstream licenses; see the corresponding component directories for details.
