#include "factory_tests.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iterator>

#include "board_config.h"
#include "board_service.h"
#include "driver/i2c_master.h"
#include "driver/jpeg_decode.h"
#include "driver/ledc.h"
#include "driver/parlio_rx.h"
extern "C" {
#include "esp_cam_io_parl.h"
}
#include "esp_heap_caps.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "factory_settings.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace {

constexpr char kTag[] = "factory_camera";
constexpr int kI2cTimeoutMs = 100;
constexpr int kProbeTimeoutMs = 10;
constexpr uint16_t kFrameWidth = 240;
constexpr uint16_t kFrameHeight = 240;
constexpr size_t kFramePixels = kFrameWidth * kFrameHeight;
constexpr size_t kFrameBytes = kFramePixels * sizeof(uint16_t);
constexpr size_t kJpegBufferAlignment = 16;
static_assert(kFrameWidth % 16 == 0 && kFrameHeight % 16 == 0);
constexpr ledc_mode_t kXclkSpeedMode = LEDC_LOW_SPEED_MODE;
constexpr ledc_timer_t kXclkTimer = LEDC_TIMER_0;
constexpr ledc_channel_t kXclkChannel = LEDC_CHANNEL_0;
constexpr uint8_t kPmicRevision = 0x00;
constexpr uint8_t kPmicDvdd = 0x03;
constexpr uint8_t kPmicDovdd = 0x05;
constexpr uint8_t kPmicAvdd = 0x06;
constexpr uint8_t kPmicDvddSequence = 0x0A;
constexpr uint8_t kPmicAvddSequence = 0x0B;
constexpr uint8_t kPmicEnable = 0x0E;
constexpr uint8_t kCameraRails = 0x0D;

i2c_master_dev_handle_t s_pmic = nullptr;
i2c_master_bus_handle_t s_camera_bus = nullptr;
esp_cam_io_parl_handle_t s_camera_io = nullptr;
esp_cam_sensor_io_parl_handle_t s_sensor = nullptr;
jpeg_decoder_handle_t s_decoder = nullptr;
uint16_t *s_frame = nullptr;
bool s_xclk_timer_configured = false;
bool s_xclk_channel_configured = false;

struct CameraIdentity {
    uint8_t address = 0;
    uint16_t pid = 0;
    bool pid_valid = false;
    esp_err_t pid_error = ESP_ERR_NOT_SUPPORTED;
};

esp_err_t read_pmic(uint8_t reg, uint8_t *value)
{
    return i2c_master_transmit_receive(
        s_pmic, &reg, 1, value, 1, kI2cTimeoutMs);
}

esp_err_t write_pmic_verified(uint8_t reg, uint8_t value)
{
    const uint8_t command[] = {reg, value};
    esp_err_t err = i2c_master_transmit(
        s_pmic, command, sizeof(command), kI2cTimeoutMs);
    uint8_t actual = 0;
    if (err == ESP_OK) {
        err = read_pmic(reg, &actual);
    }
    return err == ESP_OK && actual != value ? ESP_ERR_INVALID_RESPONSE : err;
}

esp_err_t configure_camera_power(FactoryCameraProfile profile)
{
    const FactoryBoardState &board = board_service_state();
    i2c_device_config_t config = {};
    config.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    config.device_address = BOARD_CAMERA_PMIC_ADDR;
    config.scl_speed_hz = 100000;
    ESP_RETURN_ON_ERROR(
        i2c_master_bus_add_device(board.i2c_bus, &config, &s_pmic),
        kTag, "add SGM38121");
    uint8_t revision = 0;
    ESP_RETURN_ON_ERROR(read_pmic(kPmicRevision, &revision), kTag, "read PMIC revision");
    if (revision != 0x80) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    uint8_t enabled = 0;
    ESP_RETURN_ON_ERROR(read_pmic(kPmicEnable, &enabled), kTag, "read PMIC enables");
    ESP_RETURN_ON_ERROR(
        write_pmic_verified(kPmicEnable, enabled & ~kCameraRails),
        kTag, "disable camera rails");
    uint8_t sequence = 0;
    ESP_RETURN_ON_ERROR(
        read_pmic(kPmicDvddSequence, &sequence), kTag, "read DVDD sequence");
    ESP_RETURN_ON_ERROR(
        write_pmic_verified(kPmicDvddSequence, sequence & 0xF0),
        kTag, "select DVDD register control");
    ESP_RETURN_ON_ERROR(
        write_pmic_verified(kPmicAvddSequence, 0),
        kTag, "select AVDD register control");
    const uint8_t dvdd =
        profile == FactoryCameraProfile::kOv3660 ? 0x7D : 0x57;
    ESP_RETURN_ON_ERROR(write_pmic_verified(kPmicDvdd, dvdd), kTag, "set DVDD");
    ESP_RETURN_ON_ERROR(write_pmic_verified(kPmicDovdd, 0xB1), kTag, "set DOVDD");
    ESP_RETURN_ON_ERROR(write_pmic_verified(kPmicAvdd, 0xB1), kTag, "set AVDD");
    ESP_RETURN_ON_ERROR(
        write_pmic_verified(kPmicEnable, enabled | kCameraRails),
        kTag, "enable camera rails");
    vTaskDelay(pdMS_TO_TICKS(20));
    return ESP_OK;
}

esp_err_t prepare_xclk_and_reset()
{
    ledc_timer_config_t timer = {};
    timer.speed_mode = kXclkSpeedMode;
    timer.duty_resolution = LEDC_TIMER_1_BIT;
    timer.timer_num = kXclkTimer;
    timer.freq_hz = BOARD_CAMERA_XCLK_HZ;
    // ESP32-S31 LEDC timers share a global source. Use the same PLL source as
    // the motor and servo tests so a prior test cannot block camera retries.
    timer.clk_cfg = LEDC_USE_PLL_DIV_CLK;
    ESP_RETURN_ON_ERROR(ledc_timer_config(&timer), kTag, "configure XCLK timer");
    s_xclk_timer_configured = true;
    ledc_channel_config_t channel = {};
    channel.gpio_num = BOARD_CAMERA_XCLK;
    channel.speed_mode = kXclkSpeedMode;
    channel.channel = kXclkChannel;
    channel.timer_sel = kXclkTimer;
    channel.duty = 1;
    ESP_RETURN_ON_ERROR(ledc_channel_config(&channel), kTag, "configure XCLK");
    s_xclk_channel_configured = true;
    gpio_config_t reset = {};
    reset.pin_bit_mask = 1ULL << BOARD_CAMERA_RESET;
    reset.mode = GPIO_MODE_OUTPUT;
    ESP_RETURN_ON_ERROR(gpio_config(&reset), kTag, "configure camera reset");
    gpio_set_level(BOARD_CAMERA_RESET, 0);
    vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level(BOARD_CAMERA_RESET, 1);
    vTaskDelay(pdMS_TO_TICKS(30));
    return ESP_OK;
}

esp_err_t read_sensor_register(
    i2c_master_dev_handle_t device,
    uint16_t reg,
    bool wide,
    uint8_t *value)
{
    const uint8_t address[] = {
        static_cast<uint8_t>(reg >> 8), static_cast<uint8_t>(reg),
    };
    return i2c_master_transmit_receive(
        device, wide ? address : &address[1], wide ? 2 : 1,
        value, 1, kI2cTimeoutMs);
}

esp_err_t read_camera_pid(
    i2c_master_dev_handle_t device,
    CameraIdentity *identity)
{
    uint8_t high = 0;
    uint8_t low = 0;
    esp_err_t err = ESP_ERR_NOT_SUPPORTED;
    if (identity->address == ESP_CAM_IO_PARL_OV2640_SCCB_ADDR) {
        const uint8_t bank[] = {0xFF, 0x01};
        err = i2c_master_transmit(device, bank, sizeof(bank), kI2cTimeoutMs);
        if (err == ESP_OK) {
            err = read_sensor_register(device, 0x0A, false, &high);
        }
        if (err == ESP_OK) {
            err = read_sensor_register(device, 0x0B, false, &low);
        }
    } else if (identity->address == ESP_CAM_IO_PARL_OV3660_SCCB_ADDR) {
        err = read_sensor_register(device, 0x300A, true, &high);
        if (err == ESP_OK) {
            err = read_sensor_register(device, 0x300B, true, &low);
        }
    } else if (identity->address == ESP_CAM_IO_PARL_NT99141_SCCB_ADDR) {
        const uint8_t bank[] = {0x30, 0x08, 0x01};
        err = i2c_master_transmit(device, bank, sizeof(bank), kI2cTimeoutMs);
        if (err == ESP_OK) {
            err = read_sensor_register(device, 0x3000, true, &high);
        }
        if (err == ESP_OK) {
            err = read_sensor_register(device, 0x3001, true, &low);
        }
    }
    if (err == ESP_OK) {
        identity->pid = static_cast<uint16_t>((high << 8) | low);
        identity->pid_valid = true;
    }
    identity->pid_error = err;
    return err;
}

esp_err_t identify_camera(CameraIdentity *identity)
{
    if (identity == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    *identity = {};
    constexpr std::array<uint8_t, 3> known_addresses = {
        ESP_CAM_IO_PARL_OV2640_SCCB_ADDR,
        ESP_CAM_IO_PARL_OV3660_SCCB_ADDR,
        ESP_CAM_IO_PARL_NT99141_SCCB_ADDR,
    };
    for (uint8_t address : known_addresses) {
        if (i2c_master_probe(s_camera_bus, address, kI2cTimeoutMs) == ESP_OK) {
            identity->address = address;
            break;
        }
    }
    if (identity->address == 0) {
        for (uint16_t address = 0x08; address <= 0x77; ++address) {
            if (std::find(
                    known_addresses.begin(), known_addresses.end(),
                    static_cast<uint8_t>(address)) != known_addresses.end()) {
                continue;
            }
            if (i2c_master_probe(s_camera_bus, address, kProbeTimeoutMs) == ESP_OK) {
                identity->address = static_cast<uint8_t>(address);
                break;
            }
        }
    }
    if (identity->address == 0) {
        return ESP_ERR_NOT_FOUND;
    }
    i2c_device_config_t config = {};
    config.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    config.device_address = identity->address;
    config.scl_speed_hz = 100000;
    i2c_master_dev_handle_t device = nullptr;
    ESP_RETURN_ON_ERROR(
        i2c_master_bus_add_device(s_camera_bus, &config, &device),
        kTag, "add camera sensor");
    read_camera_pid(device, identity);
    i2c_master_bus_rm_device(device);
    return ESP_OK;
}

const char *camera_model(const CameraIdentity &identity)
{
    if (!identity.pid_valid) {
        return "UNKNOWN";
    }
    if (identity.address == ESP_CAM_IO_PARL_OV2640_SCCB_ADDR &&
        (identity.pid >> 8) == ESP_CAM_IO_PARL_OV2640_PID) {
        return "OV2640";
    }
    if (identity.address == ESP_CAM_IO_PARL_OV3660_SCCB_ADDR &&
        identity.pid == ESP_CAM_IO_PARL_OV3660_PID) {
        return "OV3660";
    }
    if (identity.address == ESP_CAM_IO_PARL_OV5640_SCCB_ADDR &&
        identity.pid == ESP_CAM_IO_PARL_OV5640_PID) {
        return "OV5640";
    }
    if (identity.address == ESP_CAM_IO_PARL_NT99141_SCCB_ADDR &&
        identity.pid == ESP_CAM_IO_PARL_NT99141_PID) {
        return "NT99141";
    }
    return "UNKNOWN";
}

void format_camera_identity(
    const CameraIdentity &identity,
    char *buffer,
    size_t buffer_size)
{
    if (identity.address == 0) {
        std::snprintf(buffer, buffer_size, "No camera sensor acknowledged");
    } else if (identity.pid_valid) {
        std::snprintf(
            buffer, buffer_size, "%s ADDR 0x%02X PID 0x%04X",
            camera_model(identity), identity.address, identity.pid);
    } else {
        std::snprintf(
            buffer, buffer_size, "UNKNOWN ADDR 0x%02X PID unreadable",
            identity.address);
    }
}

bool profile_matches(
    FactoryCameraProfile profile,
    const CameraIdentity &identity)
{
    const char *model = camera_model(identity);
    return (profile == FactoryCameraProfile::kOv2640 &&
            std::strcmp(model, "OV2640") == 0) ||
           (profile == FactoryCameraProfile::kOv3660 &&
            std::strcmp(model, "OV3660") == 0);
}

esp_err_t init_camera_driver()
{
    esp_cam_sensor_io_parl_config_t sensor = {};
    sensor.pwdn_io = GPIO_NUM_NC;
    sensor.reset_io = GPIO_NUM_NC;
    sensor.xclk_io = GPIO_NUM_NC;
    sensor.xclk_hz = BOARD_CAMERA_XCLK_HZ;
    sensor.sda_io = GPIO_NUM_NC;
    sensor.scl_io = GPIO_NUM_NC;
    sensor.i2c_port = BOARD_CAMERA_I2C_PORT;
    sensor.ledc_timer = kXclkTimer;
    sensor.ledc_channel = kXclkChannel;
    sensor.pixel_format = ESP_CAM_IO_PARL_PIXFORMAT_JPEG;
    sensor.frame_size = ESP_CAM_IO_PARL_FRAMESIZE_240X240;
    sensor.jpeg_quality = BOARD_CAMERA_JPEG_QUALITY;
    ESP_RETURN_ON_ERROR(
        esp_cam_new_sensor_io_parl(&sensor, &s_sensor), kTag, "create camera sensor");

    esp_cam_io_parl_config_t io = {};
    io.data_width = 8;
    io.queue_frames = 1;
    io.fill_mode = ESP_CAM_IO_PARL_QUEUE_LATEST;
    io.frame_heap_caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
    io.pclk_io = BOARD_CAMERA_PCLK;
    io.pclk_sample_edge = ESP_CAM_IO_PARL_PCLK_POS;
    io.de_io = BOARD_CAMERA_HREF;
    io.hsync_io = GPIO_NUM_NC;
    io.vsync_io = GPIO_NUM_NC;
    std::fill_n(io.data_io, PARLIO_RX_UNIT_MAX_DATA_WIDTH, GPIO_NUM_NC);
    const gpio_num_t pins[] = {
        BOARD_CAMERA_D0, BOARD_CAMERA_D1, BOARD_CAMERA_D2, BOARD_CAMERA_D3,
        BOARD_CAMERA_D4, BOARD_CAMERA_D5, BOARD_CAMERA_D6, BOARD_CAMERA_D7,
    };
    std::copy(std::begin(pins), std::end(pins), io.data_io);
    ESP_RETURN_ON_ERROR(esp_cam_new_io_parl(&io, &s_camera_io), kTag, "create PARLIO");
    ESP_RETURN_ON_ERROR(esp_cam_io_parl_enable(s_camera_io, true), kTag, "enable PARLIO");
    ESP_RETURN_ON_ERROR(esp_cam_sensor_io_parl_connect(s_camera_io), kTag, "connect sensor");
    if (!s_camera_io->use_soft_delimiter) {
        parlio_receive_config_t receive = {};
        receive.delimiter = s_camera_io->rx_delimiter;
        receive.flags.partial_rx_en = true;
        ESP_RETURN_ON_ERROR(
            parlio_rx_unit_receive(
                s_camera_io->rx_unit, s_camera_io->payload,
                s_camera_io->payload_size, &receive),
            kTag, "arm PARLIO receive");
    }
    return ESP_OK;
}

void stop_xclk()
{
    if (s_xclk_channel_configured) {
        ledc_stop(kXclkSpeedMode, kXclkChannel, 0);
        ledc_channel_config_t channel = {};
        channel.speed_mode = kXclkSpeedMode;
        channel.channel = kXclkChannel;
        channel.deconfigure = true;
        const esp_err_t err = ledc_channel_config(&channel);
        if (err != ESP_OK) {
            ESP_LOGE(kTag, "deconfigure XCLK channel: %s", esp_err_to_name(err));
        }
        s_xclk_channel_configured = false;
    }
    if (s_xclk_timer_configured) {
        esp_err_t err = ledc_timer_pause(kXclkSpeedMode, kXclkTimer);
        if (err == ESP_OK) {
            ledc_timer_config_t timer = {};
            timer.speed_mode = kXclkSpeedMode;
            timer.timer_num = kXclkTimer;
            timer.deconfigure = true;
            err = ledc_timer_config(&timer);
        }
        if (err != ESP_OK) {
            ESP_LOGE(kTag, "deconfigure XCLK timer: %s", esp_err_to_name(err));
        }
        s_xclk_timer_configured = false;
    }
    gpio_config_t camera_control = {};
    camera_control.pin_bit_mask =
        (1ULL << BOARD_CAMERA_XCLK) | (1ULL << BOARD_CAMERA_RESET);
    camera_control.mode = GPIO_MODE_OUTPUT;
    if (gpio_config(&camera_control) == ESP_OK) {
        gpio_set_level(BOARD_CAMERA_XCLK, 0);
        gpio_set_level(BOARD_CAMERA_RESET, 0);
    }
}

void log_cleanup_error(const char *operation, esp_err_t error)
{
    if (error != ESP_OK) {
        ESP_LOGE(kTag, "%s: %s", operation, esp_err_to_name(error));
    }
}

void stop_camera_hardware()
{
    stop_xclk();
    if (s_camera_io != nullptr || s_sensor != nullptr) {
        vTaskDelay(pdMS_TO_TICKS(2));
    }

    if (s_sensor != nullptr && s_camera_io != nullptr) {
        const esp_err_t err = esp_cam_sensor_io_parl_disconnect();
        if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
            log_cleanup_error("disconnect camera sensor", err);
        }
    }
    if (s_camera_io != nullptr) {
        const esp_err_t disable_error = esp_cam_io_parl_disable(s_camera_io);
        if (disable_error != ESP_OK &&
            disable_error != ESP_ERR_INVALID_STATE) {
            log_cleanup_error("disable camera PARLIO", disable_error);
        } else {
            const esp_err_t delete_error = esp_cam_del_io_parl(s_camera_io);
            if (delete_error == ESP_OK) {
                s_camera_io = nullptr;
            } else {
                log_cleanup_error("delete camera PARLIO", delete_error);
            }
        }
    }
    if (s_sensor != nullptr) {
        const esp_err_t err = esp_cam_del_sensor_io_parl();
        if (err == ESP_OK) {
            s_sensor = nullptr;
        } else {
            log_cleanup_error("delete camera sensor", err);
        }
    }
    if (s_decoder != nullptr) {
        const esp_err_t err = jpeg_del_decoder_engine(s_decoder);
        if (err == ESP_OK) {
            s_decoder = nullptr;
        } else {
            log_cleanup_error("delete JPEG decoder", err);
        }
    }
    if (s_camera_bus != nullptr) {
        const esp_err_t err = i2c_del_master_bus(s_camera_bus);
        if (err == ESP_OK) {
            s_camera_bus = nullptr;
        } else {
            log_cleanup_error("delete camera I2C bus", err);
        }
    }
    if (s_pmic != nullptr) {
        uint8_t enabled = 0;
        esp_err_t err = read_pmic(kPmicEnable, &enabled);
        if (err == ESP_OK) {
            err = write_pmic_verified(kPmicEnable, enabled & ~kCameraRails);
        }
        if (err != ESP_OK) {
            log_cleanup_error("disable camera power rails", err);
        }
        err = i2c_master_bus_rm_device(s_pmic);
        if (err == ESP_OK) {
            s_pmic = nullptr;
        } else {
            log_cleanup_error("remove camera PMIC", err);
        }
    }
}

void fail_record(FactoryTestRecord *record, const char *code, const char *detail)
{
    std::snprintf(record->error_code, sizeof(record->error_code), "%s", code);
    std::snprintf(record->detail, sizeof(record->detail), "%s", detail);
}

}  // namespace

esp_err_t camera_test_run(
    const FactoryTestEnvironment &environment,
    FactoryTestRecord *record,
    FactoryModuleOutcome *outcome)
{
    if (record == nullptr || outcome == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    *outcome = {};
    camera_test_cleanup();
    if (environment.camera_profile == FactoryCameraProfile::kUnset) {
        fail_record(record, "PROFILE_REQUIRED", "Select OV2640 or OV3660 before RUN");
        return ESP_ERR_INVALID_STATE;
    }
    if (!board_service_state().i2c_ready) {
        fail_record(record, "I2C_NO_ACK", "Main I2C0 is unavailable");
        return ESP_ERR_INVALID_STATE;
    }
    board_ui_show_status(
        "CAMERA", "Applying selected power profile",
        factory_camera_profile_name(environment.camera_profile));
    esp_err_t err = configure_camera_power(environment.camera_profile);
    if (err != ESP_OK) {
        fail_record(record, "POWER_CONFIG_FAILED", "SGM38121 configuration failed");
        stop_camera_hardware();
        return err;
    }
    err = prepare_xclk_and_reset();
    if (err != ESP_OK) {
        fail_record(record, "CLOCK_FAILED", "Camera XCLK/reset setup failed");
        stop_camera_hardware();
        return err;
    }
    i2c_master_bus_config_t bus = {};
    bus.i2c_port = BOARD_CAMERA_I2C_PORT;
    bus.sda_io_num = BOARD_CAMERA_SDA;
    bus.scl_io_num = BOARD_CAMERA_SCL;
    bus.clk_source = I2C_CLK_SRC_DEFAULT;
    bus.glitch_ignore_cnt = 7;
    bus.flags.enable_internal_pullup = true;
    err = i2c_new_master_bus(&bus, &s_camera_bus);
    if (err != ESP_OK) {
        fail_record(record, "I2C_INIT_FAILED", "Camera I2C1 initialization failed");
        stop_camera_hardware();
        return err;
    }
    CameraIdentity identity = {};
    err = identify_camera(&identity);
    const char *detected = camera_model(identity);
    char identity_detail[96] = {};
    format_camera_identity(identity, identity_detail, sizeof(identity_detail));
    if (environment.run_result != nullptr) {
        std::snprintf(
            environment.run_result->camera_detected,
            sizeof(environment.run_result->camera_detected), "%s", detected);
        environment.run_result->camera_address = identity.address;
        environment.run_result->camera_pid = identity.pid;
    }
    if (err != ESP_OK) {
        board_ui_show_status(
            "CAMERA NOT FOUND", identity_detail, "Power profile unchanged");
        fail_record(record, "I2C_NO_ACK", identity_detail);
        std::snprintf(
            record->measurements, sizeof(record->measurements),
            "{\"configured\":\"%s\",\"detected\":\"UNKNOWN\","
            "\"address\":0,\"pid_readable\":false}",
            factory_camera_profile_name(environment.camera_profile));
        stop_camera_hardware();
        return err;
    }
    if (!identity.pid_valid) {
        board_ui_show_status(
            "CAMERA ID ERROR", identity_detail, "Power profile unchanged");
        fail_record(record, "ID_UNREADABLE", identity_detail);
        std::snprintf(
            record->measurements, sizeof(record->measurements),
            "{\"configured\":\"%s\",\"detected\":\"UNKNOWN\","
            "\"address\":%u,\"pid_readable\":false}",
            factory_camera_profile_name(environment.camera_profile),
            identity.address);
        stop_camera_hardware();
        return identity.pid_error == ESP_OK ? ESP_ERR_INVALID_RESPONSE :
                                              identity.pid_error;
    }
    if (!profile_matches(environment.camera_profile, identity)) {
        const bool supported = std::strcmp(detected, "OV2640") == 0 ||
                               std::strcmp(detected, "OV3660") == 0;
        board_ui_show_status(
            supported ? "CAMERA MISMATCH" : "CAMERA UNSUPPORTED",
            identity_detail, "Power profile unchanged");
        fail_record(
            record, supported ? "PROFILE_MISMATCH" : "UNSUPPORTED_SENSOR",
            identity_detail);
        std::snprintf(
            record->measurements, sizeof(record->measurements),
            "{\"configured\":\"%s\",\"detected\":\"%s\","
            "\"address\":%u,\"pid\":%u,\"pid_readable\":true}",
            factory_camera_profile_name(environment.camera_profile), detected,
            identity.address, identity.pid);
        stop_camera_hardware();
        return ESP_ERR_INVALID_STATE;
    }

    const uint32_t frame_caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
    ESP_LOGI(
        kTag,
        "Allocating %u-byte %ux%u BGR565 frame; free=%u largest=%u",
        static_cast<unsigned>(kFrameBytes), kFrameWidth, kFrameHeight,
        static_cast<unsigned>(heap_caps_get_free_size(frame_caps)),
        static_cast<unsigned>(heap_caps_get_largest_free_block(frame_caps)));
    s_frame = static_cast<uint16_t *>(
        heap_caps_aligned_calloc(
            kJpegBufferAlignment, 1, kFrameBytes, frame_caps));
    if (s_frame == nullptr) {
        char detail[128] = {};
        std::snprintf(
            detail, sizeof(detail), "%s; no %u-byte frame buffer",
            identity_detail, static_cast<unsigned>(kFrameBytes));
        board_ui_show_status("CAMERA MEMORY", detail, "BGR565 240x240");
        fail_record(record, "NO_MEMORY", detail);
        stop_camera_hardware();
        return ESP_ERR_NO_MEM;
    }
    jpeg_decode_engine_cfg_t engine = {};
    engine.timeout_ms = 80;
    err = jpeg_new_decoder_engine(&engine, &s_decoder);
    if (err != ESP_OK) {
        char detail[128] = {};
        std::snprintf(
            detail, sizeof(detail), "%s; JPEG decoder unavailable",
            identity_detail);
        fail_record(record, "JPEG_INIT_FAILED", detail);
        stop_camera_hardware();
        return err;
    }
    err = init_camera_driver();
    if (err != ESP_OK) {
        char detail[128] = {};
        std::snprintf(
            detail, sizeof(detail), "%s; driver init failed: %s",
            identity_detail, esp_err_to_name(err));
        board_ui_show_status(
            "CAMERA INIT FAILED", detail, "Power profile unchanged");
        fail_record(record, "CAMERA_INIT_FAILED", detail);
        stop_camera_hardware();
        return err;
    }

    jpeg_decode_cfg_t decode = {};
    decode.output_format = JPEG_DECODE_OUT_FORMAT_RGB565;
    decode.rgb_order = JPEG_DEC_RGB_ELEMENT_ORDER_BGR;
    decode.conv_std = JPEG_YUV_RGB_CONV_STD_BT601;
    uint32_t frames = 0;
    uint32_t failures = 0;
    uint64_t jpeg_bytes = 0;
    const int64_t start_us = esp_timer_get_time();
    while (frames < FACTORY_CAMERA_FRAME_TARGET && failures < 10 &&
           esp_timer_get_time() - start_us < 45000000) {
        esp_cam_io_parl_trans_t frame = {};
        err = esp_cam_io_parl_receive(s_camera_io, &frame, 5000);
        jpeg_decode_picture_info_t info = {};
        uint32_t output_size = 0;
        if (err == ESP_OK && frame.buffer != nullptr && frame.length > 0) {
            err = jpeg_decoder_get_info(frame.buffer, frame.length, &info);
            if (err == ESP_OK &&
                (info.width != kFrameWidth || info.height != kFrameHeight)) {
                err = ESP_ERR_INVALID_SIZE;
            }
            if (err == ESP_OK) {
                err = jpeg_decoder_process(
                    s_decoder, &decode, frame.buffer, frame.length,
                    reinterpret_cast<uint8_t *>(s_frame), kFrameBytes, &output_size);
            }
            jpeg_bytes += frame.length;
            esp_cam_io_parl_free_buffer(&frame);
        } else if (frame.buffer != nullptr) {
            esp_cam_io_parl_free_buffer(&frame);
        }
        if (err != ESP_OK || output_size != kFrameBytes) {
            ++failures;
            continue;
        }
        failures = 0;
        ++frames;
        board_ui_show_camera_frame(
            s_frame, kFrameWidth, kFrameHeight,
            FactoryTestId::kCamera, false);
        vTaskDelay(1);
    }
    const uint32_t duration_ms = static_cast<uint32_t>(
        (esp_timer_get_time() - start_us) / 1000);
    stop_camera_hardware();
    if (frames < FACTORY_CAMERA_FRAME_TARGET) {
        char detail[128] = {};
        std::snprintf(
            detail, sizeof(detail), "%s; %lu/%u valid frames",
            identity_detail, static_cast<unsigned long>(frames),
            FACTORY_CAMERA_FRAME_TARGET);
        board_ui_show_status("CAMERA FRAME FAILED", detail, "BGR565 240x240");
        fail_record(record, "FRAME_TIMEOUT", detail);
        std::snprintf(
            record->measurements, sizeof(record->measurements),
            "{\"configured\":\"%s\",\"detected\":\"%s\","
            "\"address\":%u,\"pid\":%u,\"width\":%u,\"height\":%u,"
            "\"format\":\"BGR565\",\"frames\":%lu,"
            "\"decode_failures\":%lu}",
            factory_camera_profile_name(environment.camera_profile), detected,
            identity.address, identity.pid, kFrameWidth, kFrameHeight,
            static_cast<unsigned long>(frames),
            static_cast<unsigned long>(failures));
        return ESP_ERR_TIMEOUT;
    }
    board_ui_show_camera_frame(
        s_frame, kFrameWidth, kFrameHeight,
        FactoryTestId::kCamera, true);
    outcome->automatic_pass = true;
    outcome->manual_required = true;
    outcome->replay_supported = true;
    const uint32_t fps_x10 = duration_ms == 0 ? 0 :
        static_cast<uint32_t>(frames * 10000ULL / duration_ms);
    std::snprintf(
        record->measurements, sizeof(record->measurements),
        "{\"configured\":\"%s\",\"detected\":\"%s\",\"address\":%u,"
        "\"pid\":%u,\"width\":%u,\"height\":%u,"
        "\"format\":\"BGR565\",\"frames\":%lu,\"fps\":%lu.%lu,"
        "\"avg_jpeg_bytes\":%llu}",
        factory_camera_profile_name(environment.camera_profile), detected,
        identity.address, identity.pid, kFrameWidth, kFrameHeight,
        static_cast<unsigned long>(frames),
        static_cast<unsigned long>(fps_x10 / 10),
        static_cast<unsigned long>(fps_x10 % 10),
        static_cast<unsigned long long>(jpeg_bytes / frames));
    std::snprintf(record->detail, sizeof(record->detail), "Confirm the frozen image is clear");
    return ESP_OK;
}

void camera_test_cleanup()
{
    stop_camera_hardware();
    if (s_frame != nullptr) {
        board_ui_clear_camera_frame();
        heap_caps_free(s_frame);
        s_frame = nullptr;
    }
    const uint32_t frame_caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
    ESP_LOGI(
        kTag, "Cleanup complete; free=%u largest=%u",
        static_cast<unsigned>(heap_caps_get_free_size(frame_caps)),
        static_cast<unsigned>(heap_caps_get_largest_free_block(frame_caps)));
}
