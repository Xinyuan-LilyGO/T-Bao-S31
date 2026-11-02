# T-Bao-S31 SD Card Test

This example targets the T-Bao-S31 and ESP-IDF 6.1 or newer. It uses the
ESP32-S31 native SDMMC peripheral in 4-bit mode and reports every test step to
the serial console.

The test performs the following operations:

- Enables TF card power through XL9555 P10.
- Mounts the FAT filesystem without formatting on mount failure.
- Prints the card identification, capacity, bus width, and working frequency.
- Prints the mounted filesystem usage and root directory.
- Writes a deterministic 4 KiB test file and calls `fsync()` before closing it.
- Reads the file back and verifies every byte.
- Unmounts and mounts the card again, then verifies the file again.
- Prints `SD CARD TEST: PASS` or `SD CARD TEST: FAIL`.

The test file is `/sdcard/tbao_s31_sd_test.bin`. It is overwritten on each run;
no other files are removed and the example never formats the card.

## Pin mapping

| Function | GPIO or device |
| --- | --- |
| SDMMC D0 | GPIO20 |
| SDMMC D1 | GPIO21 |
| SDMMC D2 | GPIO22 |
| SDMMC D3 | GPIO23 |
| SDMMC CLK | GPIO24 |
| SDMMC CMD | GPIO25 |
| TF card power enable | XL9555 P10, I2C address `0x22` |
| XL9555 I2C | SDA=GPIO0, SCL=GPIO1, 100 kHz |

The board schematic provides the TF card power rail through the XL9555 P10
enable output. The example assumes the SY6280 enable input is active high.
The SD bus also needs the board's external pull-up resistors; the driver
internal pull-ups are enabled only as a diagnostic fallback.

## Build and monitor

Run these commands from this directory with ESP-IDF 6.1 exported:

```powershell
idf.py --preview --build-dir build_s31 set-target esp32s31
idf.py --preview --build-dir build_s31 build
idf.py --preview --build-dir build_s31 -p PORT flash monitor
```

Use a FAT-formatted TF card. Formatting is intentionally disabled so a mount
failure cannot destroy an existing card's contents.
