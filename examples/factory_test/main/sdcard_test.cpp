#include "factory_tests.h"

#include <array>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <unistd.h>

#include "board_config.h"
#include "board_service.h"
#include "driver/gpio.h"
#include "driver/sdmmc_host.h"
#include "esp_vfs_fat.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdmmc_cmd.h"

namespace {

constexpr char kTestFile[] = BOARD_SD_MOUNT_POINT "/tbao_factory_test.bin";
constexpr size_t kTestBytes = 64 * 1024;
constexpr size_t kChunkBytes = 4096;
sdmmc_card_t *s_card = nullptr;
std::array<uint8_t, kChunkBytes> s_io_buffer = {};

uint8_t pattern_byte(size_t offset)
{
    return static_cast<uint8_t>((offset * 37U + 0x5AU) & 0xFFU);
}

void reset_sd_pins()
{
    constexpr gpio_num_t pins[] = {
        BOARD_SD_D0, BOARD_SD_D1, BOARD_SD_D2,
        BOARD_SD_D3, BOARD_SD_CLK, BOARD_SD_CMD,
    };
    for (gpio_num_t pin : pins) {
        gpio_reset_pin(pin);
        gpio_pullup_dis(pin);
    }
}

esp_err_t set_sd_power(bool enabled)
{
    return board_service_set_output(BOARD_XL_P10_SD_EN, enabled);
}

esp_err_t mount_card()
{
    esp_vfs_fat_sdmmc_mount_config_t mount = {};
    mount.format_if_mount_failed = false;
    mount.max_files = 5;
    mount.allocation_unit_size = 16 * 1024;
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.max_freq_khz = 20000;
    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
    slot.clk = BOARD_SD_CLK;
    slot.cmd = BOARD_SD_CMD;
    slot.d0 = BOARD_SD_D0;
    slot.d1 = BOARD_SD_D1;
    slot.d2 = BOARD_SD_D2;
    slot.d3 = BOARD_SD_D3;
    slot.width = 4;
    slot.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;
    s_card = nullptr;
    return esp_vfs_fat_sdmmc_mount(
        BOARD_SD_MOUNT_POINT, &host, &slot, &mount, &s_card);
}

void unmount_card()
{
    if (s_card != nullptr) {
        esp_vfs_fat_sdcard_unmount(BOARD_SD_MOUNT_POINT, s_card);
        s_card = nullptr;
    }
    reset_sd_pins();
}

bool write_pattern()
{
    FILE *file = std::fopen(kTestFile, "wb");
    if (file == nullptr) {
        return false;
    }
    auto &chunk = s_io_buffer;
    for (size_t offset = 0; offset < kTestBytes; offset += chunk.size()) {
        for (size_t i = 0; i < chunk.size(); ++i) {
            chunk[i] = pattern_byte(offset + i);
        }
        if (std::fwrite(chunk.data(), 1, chunk.size(), file) != chunk.size()) {
            std::fclose(file);
            return false;
        }
    }
    const bool ok = std::fflush(file) == 0 && ::fsync(::fileno(file)) == 0;
    return std::fclose(file) == 0 && ok;
}

bool verify_pattern()
{
    FILE *file = std::fopen(kTestFile, "rb");
    if (file == nullptr) {
        return false;
    }
    auto &chunk = s_io_buffer;
    bool ok = true;
    for (size_t offset = 0; offset < kTestBytes && ok; offset += chunk.size()) {
        if (std::fread(chunk.data(), 1, chunk.size(), file) != chunk.size()) {
            ok = false;
            break;
        }
        for (size_t i = 0; i < chunk.size(); ++i) {
            if (chunk[i] != pattern_byte(offset + i)) {
                ok = false;
                break;
            }
        }
    }
    if (std::fgetc(file) != EOF) {
        ok = false;
    }
    std::fclose(file);
    return ok;
}

void set_failure(FactoryTestRecord *record, const char *code, const char *stage)
{
    std::snprintf(record->error_code, sizeof(record->error_code), "%s", code);
    std::snprintf(
        record->detail, sizeof(record->detail), "%s: %s",
        stage, errno != 0 ? std::strerror(errno) : "ESP-IDF error");
}

}  // namespace

esp_err_t sdcard_test_run(
    const FactoryTestEnvironment &,
    FactoryTestRecord *record,
    FactoryModuleOutcome *outcome)
{
    if (record == nullptr || outcome == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    *outcome = {};
    sdcard_test_cleanup();
    if (!board_service_state().expander_ready) {
        set_failure(record, "I2C_NO_ACK", "XL9555 unavailable");
        return ESP_ERR_NOT_FOUND;
    }
    board_ui_show_status("SD CARD", "Powering dedicated FAT card", "No formatting is performed");
    if (set_sd_power(true) != ESP_OK) {
        set_failure(record, "POWER_CONTROL", "Enable SD power");
        return ESP_FAIL;
    }
    vTaskDelay(pdMS_TO_TICKS(40));
    if (mount_card() != ESP_OK) {
        set_failure(record, "MOUNT_FAILED", "Mount SD card");
        return ESP_FAIL;
    }
    uint64_t total = 0;
    uint64_t free = 0;
    if (esp_vfs_fat_info(BOARD_SD_MOUNT_POINT, &total, &free) != ESP_OK) {
        set_failure(record, "FS_INFO_FAILED", "Read FAT information");
        return ESP_FAIL;
    }
    board_ui_show_status("SD CARD", "Writing and verifying 64 KiB", "4-bit SDMMC at 20 MHz");
    errno = 0;
    if (!write_pattern() || !verify_pattern()) {
        set_failure(record, "DATA_MISMATCH", "Initial write/read verification");
        return ESP_FAIL;
    }
    unmount_card();
    set_sd_power(false);
    vTaskDelay(pdMS_TO_TICKS(100));
    set_sd_power(true);
    vTaskDelay(pdMS_TO_TICKS(50));
    if (mount_card() != ESP_OK || !verify_pattern()) {
        set_failure(record, "REMOUNT_FAILED", "Power-cycle remount verification");
        return ESP_FAIL;
    }
    if (std::remove(kTestFile) != 0) {
        set_failure(record, "DELETE_FAILED", "Delete factory test file");
        return ESP_FAIL;
    }
    unmount_card();
    set_sd_power(false);
    outcome->automatic_pass = true;
    std::snprintf(
        record->measurements, sizeof(record->measurements),
        "{\"bus_width\":4,\"frequency_khz\":20000,\"bytes\":65536,"
        "\"total_bytes\":%llu,\"free_bytes\":%llu,\"power_cycle_verified\":true}",
        static_cast<unsigned long long>(total),
        static_cast<unsigned long long>(free));
    std::snprintf(record->detail, sizeof(record->detail),
                  "64 KiB write/read and power-cycle verification passed");
    return ESP_OK;
}

void sdcard_test_cleanup()
{
    if (s_card != nullptr) {
        std::remove(kTestFile);
    }
    unmount_card();
    set_sd_power(false);
}
