#include <array>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

#include "board_config.h"
#include "driver/i2c_master.h"
#include "driver/sdmmc_host.h"
#include "esp_err.h"
#include "esp_io_expander.h"
#include "esp_io_expander_xl9555.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdmmc_cmd.h"

namespace {

constexpr char kTag[] = "sdcard_test";
constexpr char kTestFile[] = BOARD_SD_MOUNT_POINT "/tbao_s31_sd_test.bin";
constexpr size_t kTestSize = 4096;

i2c_master_bus_handle_t s_i2c_bus = nullptr;
esp_io_expander_handle_t s_xl9555 = nullptr;
sdmmc_card_t *s_card = nullptr;
char s_failure[160] = {};
std::array<uint8_t, kTestSize> s_expected = {};
std::array<uint8_t, kTestSize> s_actual = {};

bool fail_with_error(const char *stage, esp_err_t err)
{
    std::snprintf(s_failure, sizeof(s_failure), "%s: %s", stage, esp_err_to_name(err));
    ESP_LOGE(kTag, "%s (%s)", stage, esp_err_to_name(err));
    return false;
}

bool init_i2c_and_card_power()
{
    i2c_master_bus_config_t bus_config = {};
    bus_config.i2c_port = BOARD_I2C_PORT;
    bus_config.sda_io_num = BOARD_I2C_SDA;
    bus_config.scl_io_num = BOARD_I2C_SCL;
    bus_config.clk_source = I2C_CLK_SRC_DEFAULT;
    bus_config.glitch_ignore_cnt = 7;
    bus_config.flags.enable_internal_pullup = true;

    esp_err_t err = i2c_new_master_bus(&bus_config, &s_i2c_bus);
    if (err != ESP_OK) {
        return fail_with_error("create I2C bus", err);
    }

    err = esp_io_expander_new_i2c_xl9555(
        s_i2c_bus, BOARD_XL9555_I2C_ADDR, &s_xl9555);
    if (err != ESP_OK) {
        return fail_with_error("create XL9555", err);
    }

    err = esp_io_expander_set_dir(
        s_xl9555, BOARD_XL9555_P10_SD_VDD_EN, IO_EXPANDER_OUTPUT);
    if (err != ESP_OK) {
        return fail_with_error("configure TF power output", err);
    }

    err = esp_io_expander_set_level(
        s_xl9555, BOARD_XL9555_P10_SD_VDD_EN, BOARD_SD_POWER_ON_LEVEL);
    if (err != ESP_OK) {
        return fail_with_error("enable TF card power", err);
    }

    vTaskDelay(pdMS_TO_TICKS(20));
    ESP_LOGI(kTag, "I2C ready and TF card power enabled");
    return true;
}

void make_test_pattern()
{
    for (size_t i = 0; i < s_expected.size(); ++i) {
        s_expected[i] = static_cast<uint8_t>((i * 37U + 0x5AU) & 0xFFU);
    }
}

bool mount_card()
{
    esp_vfs_fat_sdmmc_mount_config_t mount_config = {};
    mount_config.format_if_mount_failed = false;
    mount_config.max_files = 5;
    mount_config.allocation_unit_size = 16 * 1024;

    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.max_freq_khz = 20000;

    sdmmc_slot_config_t slot_config = SDMMC_SLOT_CONFIG_DEFAULT();
    slot_config.clk = BOARD_SD_CLK;
    slot_config.cmd = BOARD_SD_CMD;
    slot_config.d0 = BOARD_SD_D0;
    slot_config.d1 = BOARD_SD_D1;
    slot_config.d2 = BOARD_SD_D2;
    slot_config.d3 = BOARD_SD_D3;
    slot_config.width = BOARD_SD_BUS_WIDTH;
    slot_config.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

    ESP_LOGI(kTag, "Mounting SDMMC at 20 MHz in 4-bit mode");
    s_card = nullptr;
    const esp_err_t err = esp_vfs_fat_sdmmc_mount(
        BOARD_SD_MOUNT_POINT,
        &host,
        &slot_config,
        &mount_config,
        &s_card);
    if (err != ESP_OK) {
        return fail_with_error("mount SD card", err);
    }

    ESP_LOGI(kTag, "Filesystem mounted at %s", BOARD_SD_MOUNT_POINT);
    sdmmc_card_print_info(stdout, s_card);
    return true;
}
bool fail_with_errno(const char *stage);

bool write_test_file()
{
    FILE *file = std::fopen(kTestFile, "wb");
    if (file == nullptr) {
        return fail_with_errno("open test file for writing");
    }

    const size_t written = std::fwrite(
        s_expected.data(), 1, s_expected.size(), file);
    if (written != s_expected.size()) {
        const bool result = fail_with_errno("write test file");
        std::fclose(file);
        return result;
    }
    if (std::fflush(file) != 0) {
        const bool result = fail_with_errno("flush test file");
        std::fclose(file);
        return result;
    }
    if (::fsync(::fileno(file)) != 0) {
        const bool result = fail_with_errno("sync test file");
        std::fclose(file);
        return result;
    }
    if (std::fclose(file) != 0) {
        return fail_with_errno("close test file after writing");
    }

    ESP_LOGI(kTag, "Wrote and synchronized %u bytes: %s", static_cast<unsigned>(written), kTestFile);
    return true;
}

bool read_and_verify_test_file()
{
    FILE *file = std::fopen(kTestFile, "rb");
    if (file == nullptr) {
        return fail_with_errno("open test file for reading");
    }

    s_actual.fill(0);
    const size_t read = std::fread(s_actual.data(), 1, s_actual.size(), file);
    if (read != s_actual.size()) {
        const bool result = std::ferror(file) != 0
            ? fail_with_errno("read test file")
            : fail_with_error("short test file", ESP_ERR_INVALID_SIZE);
        std::fclose(file);
        return result;
    }
    if (std::fclose(file) != 0) {
        return fail_with_errno("close test file after reading");
    }

    for (size_t i = 0; i < s_expected.size(); ++i) {
        if (s_actual[i] != s_expected[i]) {
            std::snprintf(
                s_failure, sizeof(s_failure),
                "data mismatch at offset %u (expected 0x%02X, got 0x%02X)",
                static_cast<unsigned>(i), s_expected[i], s_actual[i]);
            ESP_LOGE(kTag, "%s", s_failure);
            return false;
        }
    }

    ESP_LOGI(kTag, "Readback verified: %u bytes", static_cast<unsigned>(read));
    return true;
}

bool print_filesystem_info()
{
    uint64_t total_bytes = 0;
    uint64_t free_bytes = 0;
    const esp_err_t err = esp_vfs_fat_info(
        BOARD_SD_MOUNT_POINT, &total_bytes, &free_bytes);
    if (err != ESP_OK) {
        return err == ESP_FAIL
            ? fail_with_errno("query FAT filesystem")
            : fail_with_error("query FAT filesystem", err);
    }

    std::printf(
        "Filesystem: total=%llu MiB free=%llu MiB\n",
        static_cast<unsigned long long>(total_bytes / (1024ULL * 1024ULL)),
        static_cast<unsigned long long>(free_bytes / (1024ULL * 1024ULL)));

    DIR *directory = opendir(BOARD_SD_MOUNT_POINT);
    if (directory == nullptr) {
        return fail_with_errno("open root directory");
    }
    std::printf("Root directory:\n");
    while (const dirent *entry = readdir(directory)) {
        std::printf("  %s\n", entry->d_name);
    }
    closedir(directory);
    return true;
}

bool fail_with_errno(const char *stage)
{
    std::snprintf(
        s_failure, sizeof(s_failure), "%s: %s", stage, std::strerror(errno));
    ESP_LOGE(kTag, "%s: errno=%d (%s)", stage, errno, std::strerror(errno));
    return false;
}

bool reset_sd_bus_pins()
{
    constexpr std::array<gpio_num_t, 6> pins = {
        BOARD_SD_D0,
        BOARD_SD_D1,
        BOARD_SD_D2,
        BOARD_SD_D3,
        BOARD_SD_CLK,
        BOARD_SD_CMD,
    };

    for (const gpio_num_t pin : pins) {
        esp_err_t err = gpio_reset_pin(pin);
        if (err != ESP_OK) {
            return fail_with_error("reset SD bus GPIO", err);
        }
        err = gpio_pullup_dis(pin);
        if (err != ESP_OK) {
            return fail_with_error("disable SD bus GPIO pull-up", err);
        }
    }
    return true;
}

bool unmount_card()
{
    if (s_card == nullptr) {
        return true;
    }
    const esp_err_t err = esp_vfs_fat_sdcard_unmount(
        BOARD_SD_MOUNT_POINT, s_card);
    s_card = nullptr;
    if (err != ESP_OK) {
        return fail_with_error("unmount SD card", err);
    }
    if (!reset_sd_bus_pins()) {
        return false;
    }
    ESP_LOGI(kTag, "Filesystem unmounted");
    return true;
}

bool run_sdcard_test()
{
    make_test_pattern();
    if (!init_i2c_and_card_power() || !mount_card()) {
        return false;
    }
    if (!print_filesystem_info() || !write_test_file() ||
        !read_and_verify_test_file()) {
        return false;
    }
    if (!unmount_card() || !mount_card() ||
        !read_and_verify_test_file()) {
        return false;
    }
    return unmount_card();
}

}  // namespace

extern "C" void app_main()
{
    ESP_LOGI(kTag, "T-Bao-S31 SD card test start");
    const bool passed = run_sdcard_test();
    if (s_card != nullptr) {
        unmount_card();
    }
    if (passed) {
        std::printf("SD CARD TEST: PASS\n");
    } else {
        std::printf("SD CARD TEST: FAIL (%s)\n", s_failure);
    }
    std::fflush(stdout);
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
