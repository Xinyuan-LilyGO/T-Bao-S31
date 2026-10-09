#include "factory_tests.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>

#include "board_config.h"
#include "board_service.h"
#include "driver/gpio.h"
#include "driver/touch_sens.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "factory_settings.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

namespace {

constexpr char kTag[] = "factory_input";
constexpr int kTouchChannel = 5;
constexpr int kTouchInitialScanCount = 3;
constexpr int kTouchContinuousSettleMs = 20;
constexpr int kSamplePeriodMs = 20;

touch_sensor_handle_t s_touch_sensor = nullptr;
touch_channel_handle_t s_touch_channel = nullptr;
bool s_touch_enabled = false;
bool s_touch_scanning = false;
bool s_touch_pad_claimed = false;

gpio_num_t s_button_pin = GPIO_NUM_NC;
bool s_button_configured = false;

#if CONFIG_IDF_TARGET_ESP32S31
std::array<uint32_t, TOUCH_SAMPLE_CFG_NUM> s_touch_baseline = {};
std::array<uint32_t, TOUCH_SAMPLE_CFG_NUM> s_touch_latest = {};
std::array<uint32_t, TOUCH_SAMPLE_CFG_NUM> s_touch_threshold = {};
#endif

void set_error(FactoryTestRecord *record, const char *code, const char *detail)
{
    if (record == nullptr) {
        return;
    }
    std::snprintf(record->error_code, sizeof(record->error_code), "%s", code);
    std::snprintf(record->detail, sizeof(record->detail), "%s", detail);
}

void log_cleanup_error(const char *operation, esp_err_t error)
{
    if (error != ESP_OK) {
        ESP_LOGW(kTag, "%s: %s", operation, esp_err_to_name(error));
    }
}

void touch_cleanup_impl()
{
#if CONFIG_IDF_TARGET_ESP32S31
    if (s_touch_scanning && s_touch_sensor != nullptr) {
        const esp_err_t error = touch_sensor_stop_continuous_scanning(s_touch_sensor);
        log_cleanup_error("stop GPIO11 touch scanning", error);
        if (error == ESP_OK || error == ESP_ERR_INVALID_STATE) {
            s_touch_scanning = false;
        }
    }

    if (s_touch_enabled && s_touch_sensor != nullptr) {
        const esp_err_t error = touch_sensor_disable(s_touch_sensor);
        log_cleanup_error("disable GPIO11 touch sensor", error);
        if (error == ESP_OK || error == ESP_ERR_INVALID_STATE) {
            s_touch_enabled = false;
        }
    }

    if (s_touch_channel != nullptr) {
        const esp_err_t error = touch_sensor_del_channel(s_touch_channel);
        log_cleanup_error("delete GPIO11 touch channel", error);
        if (error == ESP_OK) {
            s_touch_channel = nullptr;
        }
    }

    if (s_touch_sensor != nullptr && s_touch_channel == nullptr) {
        const esp_err_t error = touch_sensor_del_controller(s_touch_sensor);
        log_cleanup_error("delete GPIO11 touch controller", error);
        if (error == ESP_OK) {
            s_touch_sensor = nullptr;
        }
    }

    if (s_touch_pad_claimed && s_touch_sensor == nullptr) {
        const esp_err_t error = gpio_reset_pin(BOARD_TOUCH_PAD);
        log_cleanup_error("reset GPIO11 touch pad", error);
        if (error == ESP_OK) {
            s_touch_pad_claimed = false;
        }
    }
#else
    (void)gpio_reset_pin(BOARD_TOUCH_PAD);
#endif
}

void button_cleanup_impl()
{
    if (s_button_configured && s_button_pin != GPIO_NUM_NC) {
        const esp_err_t error = gpio_reset_pin(s_button_pin);
        log_cleanup_error("reset button GPIO", error);
    }
    s_button_pin = GPIO_NUM_NC;
    s_button_configured = false;
}

bool service_pending_action(const FactoryTestEnvironment &environment)
{
    if (environment.wait_action == nullptr) {
        return false;
    }
    FactoryAction ignored = {};
    return environment.wait_action(&ignored, 1, environment.wait_context);
}

#if CONFIG_IDF_TARGET_ESP32S31

std::array<uint32_t, TOUCH_SAMPLE_CFG_NUM> max_delta_from_baseline()
{
    std::array<uint32_t, TOUCH_SAMPLE_CFG_NUM> deltas = {};
    for (size_t index = 0; index < deltas.size(); ++index) {
        const uint32_t sample = s_touch_latest[index];
        const uint32_t baseline = s_touch_baseline[index];
        deltas[index] = sample >= baseline ? sample - baseline : baseline - sample;
    }
    return deltas;
}

uint32_t max_delta()
{
    const auto deltas = max_delta_from_baseline();
    return *std::max_element(deltas.begin(), deltas.end());
}

void write_touch_measurements(
    FactoryTestRecord *record, uint32_t samples, uint32_t stable_samples,
    uint32_t largest_delta)
{
    if (record == nullptr) {
        return;
    }
    std::snprintf(
        record->measurements, sizeof(record->measurements),
        "{\"gpio\":11,\"channel\":5,\"baseline\":[%lu,%lu,%lu],"
        "\"sample\":[%lu,%lu,%lu],\"threshold\":[%lu,%lu,%lu],"
        "\"samples\":%lu,\"stable_samples\":%lu,\"max_delta\":%lu}",
        static_cast<unsigned long>(s_touch_baseline[0]),
        static_cast<unsigned long>(s_touch_baseline[1]),
        static_cast<unsigned long>(s_touch_baseline[2]),
        static_cast<unsigned long>(s_touch_latest[0]),
        static_cast<unsigned long>(s_touch_latest[1]),
        static_cast<unsigned long>(s_touch_latest[2]),
        static_cast<unsigned long>(s_touch_threshold[0]),
        static_cast<unsigned long>(s_touch_threshold[1]),
        static_cast<unsigned long>(s_touch_threshold[2]),
        static_cast<unsigned long>(samples),
        static_cast<unsigned long>(stable_samples),
        static_cast<unsigned long>(largest_delta));
}

esp_err_t setup_touch_sensor(FactoryTestRecord *record)
{
    static touch_sensor_sample_config_t sample_config[TOUCH_SAMPLE_CFG_NUM] = {
        TOUCH_SENSOR_V3_DEFAULT_SAMPLE_CONFIG(1, 1, 1),
        TOUCH_SENSOR_V3_DEFAULT_SAMPLE_CONFIG(2, 1, 1),
        TOUCH_SENSOR_V3_DEFAULT_SAMPLE_CONFIG(4, 1, 1),
    };

    touch_sensor_config_t sensor_config =
        TOUCH_SENSOR_DEFAULT_BASIC_CONFIG(TOUCH_SAMPLE_CFG_NUM, sample_config);
    esp_err_t error = touch_sensor_new_controller(&sensor_config, &s_touch_sensor);
    if (error != ESP_OK) {
        set_error(record, "TOUCH_SENSOR_INIT", "Unable to create touch controller");
        return error;
    }

    touch_channel_config_t channel_config = {};
    for (size_t index = 0; index < TOUCH_SAMPLE_CFG_NUM; ++index) {
        // The test uses its own calibrated delta check. Keep the driver's
        // active detector out of the way while the benchmark is collected.
        channel_config.active_thresh[index] = 0xFFFF;
    }
    error = touch_sensor_new_channel(
        s_touch_sensor, kTouchChannel, &channel_config, &s_touch_channel);
    if (error != ESP_OK) {
        set_error(record, "TOUCH_SENSOR_INIT", "Unable to create TOUCH_CH5");
        return error;
    }
    s_touch_pad_claimed = true;

    touch_chan_info_t channel_info = {};
    error = touch_sensor_get_channel_info(s_touch_channel, &channel_info);
    if (error != ESP_OK || channel_info.chan_gpio != BOARD_TOUCH_PAD) {
        set_error(
            record, "TOUCH_CHANNEL_MISMATCH",
            "TOUCH_CH5 is not mapped to GPIO11");
        return error == ESP_OK ? ESP_ERR_INVALID_STATE : error;
    }

    touch_sensor_filter_config_t filter_config =
        TOUCH_SENSOR_DEFAULT_FILTER_CONFIG();
    error = touch_sensor_config_filter(s_touch_sensor, &filter_config);
    if (error != ESP_OK) {
        set_error(record, "TOUCH_SENSOR_INIT", "Unable to configure touch filter");
        return error;
    }

    error = touch_sensor_enable(s_touch_sensor);
    if (error != ESP_OK) {
        set_error(record, "TOUCH_SENSOR_INIT", "Unable to enable touch sensor");
        return error;
    }
    s_touch_enabled = true;

    for (int scan = 0; scan < kTouchInitialScanCount; ++scan) {
        error = touch_sensor_trigger_oneshot_scanning(s_touch_sensor, 2000);
        if (error != ESP_OK) {
            set_error(record, "TOUCH_SCAN_FAILED", "Initial touch scan failed");
            return error;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    error = touch_channel_read_data(
        s_touch_channel, TOUCH_CHAN_DATA_TYPE_BENCHMARK, s_touch_baseline.data());
    if (error != ESP_OK) {
        set_error(record, "TOUCH_READ_FAILED", "Unable to read touch baseline");
        return error;
    }
    for (size_t index = 0; index < s_touch_baseline.size(); ++index) {
        if (s_touch_baseline[index] == 0) {
            set_error(record, "TOUCH_BASELINE_INVALID", "GPIO11 touch baseline is zero");
            return ESP_ERR_INVALID_STATE;
        }
        const uint32_t ratio_delta = static_cast<uint32_t>(
            static_cast<float>(s_touch_baseline[index]) * FACTORY_TOUCH_DELTA_RATIO);
        s_touch_threshold[index] = std::max<uint32_t>(
            FACTORY_TOUCH_MIN_DELTA, ratio_delta);
    }

    error = touch_sensor_disable(s_touch_sensor);
    if (error != ESP_OK) {
        set_error(record, "TOUCH_SENSOR_INIT", "Unable to prepare touch scanner");
        return error;
    }
    s_touch_enabled = false;

    error = touch_sensor_enable(s_touch_sensor);
    if (error != ESP_OK) {
        set_error(record, "TOUCH_SENSOR_INIT", "Unable to restart touch scanner");
        return error;
    }
    s_touch_enabled = true;
    error = touch_sensor_start_continuous_scanning(s_touch_sensor);
    if (error != ESP_OK) {
        set_error(record, "TOUCH_SCAN_FAILED", "Unable to start touch scanning");
        return error;
    }
    s_touch_scanning = true;
    vTaskDelay(pdMS_TO_TICKS(kTouchContinuousSettleMs));
    return ESP_OK;
}

esp_err_t run_touch_button(
    const FactoryTestEnvironment &environment,
    FactoryTestRecord *record,
    FactoryModuleOutcome *outcome)
{
    touch_button_test_cleanup();
    s_touch_baseline.fill(0);
    s_touch_latest.fill(0);
    s_touch_threshold.fill(0);

    board_ui_show_status(
        "GPIO11 TOUCH", "Touch the pad", "TOUCH_CH5 / GPIO11 / 30 s");
    esp_err_t error = setup_touch_sensor(record);
    if (error != ESP_OK) {
        return error;
    }

    const int64_t deadline = esp_timer_get_time() +
        static_cast<int64_t>(FACTORY_TOUCH_TIMEOUT_MS) * 1000;
    uint32_t samples = 0;
    uint32_t stable_samples = 0;
    uint32_t largest_delta = 0;
    uint32_t stable_count = 0;
    while (esp_timer_get_time() < deadline) {
        if (touch_channel_read_data(
                s_touch_channel, TOUCH_CHAN_DATA_TYPE_SMOOTH,
                s_touch_latest.data()) != ESP_OK) {
            write_touch_measurements(record, samples, stable_samples, largest_delta);
            set_error(record, "TOUCH_READ_FAILED", "Unable to read GPIO11 touch data");
            return ESP_FAIL;
        }
        ++samples;
        const uint32_t delta = max_delta();
        largest_delta = std::max(largest_delta, delta);
        bool above_threshold = false;
        for (size_t index = 0; index < s_touch_threshold.size(); ++index) {
            const uint32_t sample_delta =
                s_touch_latest[index] >= s_touch_baseline[index] ?
                s_touch_latest[index] - s_touch_baseline[index] :
                s_touch_baseline[index] - s_touch_latest[index];
            above_threshold |= sample_delta >= s_touch_threshold[index];
        }
        if (above_threshold) {
            ++stable_count;
            if (stable_count >= FACTORY_TOUCH_DEBOUNCE_SAMPLES) {
                stable_samples = stable_count;
                write_touch_measurements(
                    record, samples, stable_samples, largest_delta);
                std::snprintf(
                    record->detail, sizeof(record->detail),
                    "GPIO11 TOUCH_CH5 detected");
                outcome->automatic_pass = true;
                return ESP_OK;
            }
        } else {
            stable_count = 0;
        }
        stable_samples = std::max(stable_samples, stable_count);
        (void)service_pending_action(environment);
        vTaskDelay(pdMS_TO_TICKS(kSamplePeriodMs));
    }

    write_touch_measurements(record, samples, stable_samples, largest_delta);
    set_error(record, "TIMEOUT", "GPIO11 touch was not detected");
    return ESP_ERR_TIMEOUT;
}

#else

esp_err_t run_touch_button(
    const FactoryTestEnvironment &,
    FactoryTestRecord *record,
    FactoryModuleOutcome *)
{
    set_error(record, "UNSUPPORTED", "GPIO11 touch requires ESP32-S31");
    return ESP_ERR_NOT_SUPPORTED;
}

#endif

esp_err_t configure_button_gpio(gpio_num_t pin, FactoryTestRecord *record)
{
    gpio_config_t config = {};
    config.pin_bit_mask = 1ULL << pin;
    config.mode = GPIO_MODE_INPUT;
    config.pull_up_en = GPIO_PULLUP_ENABLE;
    config.pull_down_en = GPIO_PULLDOWN_DISABLE;
    config.intr_type = GPIO_INTR_DISABLE;
    const esp_err_t error = gpio_config(&config);
    if (error != ESP_OK) {
        set_error(record, "GPIO_CONFIG_FAILED", "Unable to configure button input");
        return error;
    }
    s_button_pin = pin;
    s_button_configured = true;
    return ESP_OK;
}

void write_button_measurements(
    FactoryTestRecord *record, gpio_num_t pin, int initial_level,
    uint32_t release_samples, uint32_t press_samples, int final_level)
{
    if (record == nullptr) {
        return;
    }
    std::snprintf(
        record->measurements, sizeof(record->measurements),
        "{\"gpio\":%d,\"active_level\":0,\"initial_level\":%d,"
        "\"final_level\":%d,\"release_samples\":%lu,"
        "\"press_samples\":%lu}",
        static_cast<int>(pin), initial_level, final_level,
        static_cast<unsigned long>(release_samples),
        static_cast<unsigned long>(press_samples));
}

esp_err_t run_digital_button(
    const FactoryTestEnvironment &environment,
    FactoryTestRecord *record,
    FactoryModuleOutcome *outcome,
    gpio_num_t pin,
    const char *title,
    const char *instruction,
    const char *footer)
{
    button_cleanup_impl();
    board_ui_show_status(title, instruction, footer);
    const esp_err_t config_error = configure_button_gpio(pin, record);
    if (config_error != ESP_OK) {
        return config_error;
    }

    const int initial_level = gpio_get_level(pin);
    uint32_t release_samples = 0;
    uint32_t press_samples = 0;
    uint32_t high_count = 0;
    const int64_t release_deadline = esp_timer_get_time() +
        static_cast<int64_t>(FACTORY_BUTTON_RELEASE_TIMEOUT_MS) * 1000;
    while (esp_timer_get_time() < release_deadline) {
        if (gpio_get_level(pin) == 1) {
            ++high_count;
            ++release_samples;
            if (high_count >= FACTORY_BUTTON_DEBOUNCE_SAMPLES) {
                break;
            }
        } else {
            high_count = 0;
        }
        (void)service_pending_action(environment);
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    if (high_count < FACTORY_BUTTON_DEBOUNCE_SAMPLES) {
        write_button_measurements(
            record, pin, initial_level, release_samples, press_samples,
            gpio_get_level(pin));
        set_error(record, "STUCK_LOW", "Button did not release before the test");
        return ESP_ERR_INVALID_STATE;
    }

    const int64_t press_deadline = esp_timer_get_time() +
        static_cast<int64_t>(FACTORY_BUTTON_TIMEOUT_MS) * 1000;
    uint32_t low_count = 0;
    while (esp_timer_get_time() < press_deadline) {
        if (gpio_get_level(pin) == 0) {
            ++low_count;
            ++press_samples;
            if (low_count >= FACTORY_BUTTON_DEBOUNCE_SAMPLES) {
                write_button_measurements(
                    record, pin, initial_level, release_samples, press_samples, 0);
                std::snprintf(
                    record->detail, sizeof(record->detail),
                    "%s detected on GPIO%d", title, static_cast<int>(pin));
                outcome->automatic_pass = true;
                return ESP_OK;
            }
        } else {
            low_count = 0;
        }
        (void)service_pending_action(environment);
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    write_button_measurements(
        record, pin, initial_level, release_samples, press_samples,
        gpio_get_level(pin));
    set_error(record, "TIMEOUT", "Button press was not detected");
    return ESP_ERR_TIMEOUT;
}

}  // namespace

esp_err_t touch_button_test_run(
    const FactoryTestEnvironment &environment,
    FactoryTestRecord *record,
    FactoryModuleOutcome *outcome)
{
    if (record == nullptr || outcome == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    *outcome = {};
    return run_touch_button(environment, record, outcome);
}

void touch_button_test_cleanup()
{
    touch_cleanup_impl();
}

esp_err_t gpio60_button_test_run(
    const FactoryTestEnvironment &environment,
    FactoryTestRecord *record,
    FactoryModuleOutcome *outcome)
{
    if (record == nullptr || outcome == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    *outcome = {};
    return run_digital_button(
        environment, record, outcome, BOARD_GPIO60,
        "GPIO60 BUTTON", "Press the IO60 button", "GPIO60 / active low / 30 s");
}

void gpio60_button_test_cleanup()
{
    button_cleanup_impl();
}

esp_err_t boot_button_test_run(
    const FactoryTestEnvironment &environment,
    FactoryTestRecord *record,
    FactoryModuleOutcome *outcome)
{
    if (record == nullptr || outcome == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    *outcome = {};
    return run_digital_button(
        environment, record, outcome, BOARD_BOOT_PIN,
        "BOOT BUTTON", "Press the BOOT button", "GPIO61 / ESP32_BOOT / active low");
}

void boot_button_test_cleanup()
{
    button_cleanup_impl();
}
