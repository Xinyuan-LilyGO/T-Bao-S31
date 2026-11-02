#include "factory_tests.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "board_config.h"
#include "board_service.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "esp_check.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace {

constexpr char kTag[] = "factory_actuator";
constexpr ledc_mode_t kSpeedMode = LEDC_LOW_SPEED_MODE;
constexpr uint32_t kMotorDutyMax = (1U << 10) - 1;
constexpr gpio_num_t kMotorPins[] = {
    BOARD_DRV_AIN1, BOARD_DRV_AIN2, BOARD_DRV_BIN1, BOARD_DRV_BIN2,
};
bool s_motor_configured = false;
bool s_servo_configured = false;

esp_err_t set_channel(ledc_channel_t channel, uint32_t duty)
{
    ESP_RETURN_ON_ERROR(
        ledc_set_duty(kSpeedMode, channel, duty), kTag, "set LEDC duty");
    return ledc_update_duty(kSpeedMode, channel);
}

esp_err_t configure_channel(
    ledc_channel_t channel, ledc_timer_t timer, gpio_num_t gpio)
{
    ledc_channel_config_t config = {};
    config.gpio_num = gpio;
    config.speed_mode = kSpeedMode;
    config.channel = channel;
    config.timer_sel = timer;
    config.duty = 0;
    return ledc_channel_config(&config);
}

void log_cleanup_error(const char *operation, esp_err_t err)
{
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "%s: %s", operation, esp_err_to_name(err));
    }
}

void deconfigure_channel(ledc_channel_t channel)
{
    ledc_channel_config_t config = {};
    config.speed_mode = kSpeedMode;
    config.channel = channel;
    config.deconfigure = true;
    log_cleanup_error("deconfigure LEDC channel", ledc_channel_config(&config));
}

void deconfigure_timer(ledc_timer_t timer)
{
    esp_err_t err = ledc_timer_pause(kSpeedMode, timer);
    if (err == ESP_OK) {
        ledc_timer_config_t config = {};
        config.speed_mode = kSpeedMode;
        config.timer_num = timer;
        config.deconfigure = true;
        err = ledc_timer_config(&config);
    }
    log_cleanup_error("deconfigure LEDC timer", err);
}

esp_err_t configure_motor_gpio_low(bool hold)
{
    for (gpio_num_t pin : kMotorPins) {
        ESP_RETURN_ON_ERROR(gpio_set_level(pin, 0), kTag, "preset motor GPIO low");
    }

    gpio_config_t config = {};
    for (gpio_num_t pin : kMotorPins) {
        config.pin_bit_mask |= 1ULL << pin;
    }
    config.mode = GPIO_MODE_OUTPUT;
    config.pull_down_en = GPIO_PULLDOWN_ENABLE;
    ESP_RETURN_ON_ERROR(gpio_config(&config), kTag, "configure motor GPIO low");

    for (gpio_num_t pin : kMotorPins) {
        ESP_RETURN_ON_ERROR(gpio_set_level(pin, 0), kTag, "drive motor GPIO low");
        // A previous factory run may have held the pad through the reset that
        // entered the ROM downloader. Prepare the internal low state before
        // releasing it, then re-latch the safe level while the motor is idle.
        ESP_RETURN_ON_ERROR(gpio_hold_dis(pin), kTag, "release motor GPIO hold");
        ESP_RETURN_ON_ERROR(gpio_set_level(pin, 0), kTag, "restore motor GPIO low");
        if (hold) {
            ESP_RETURN_ON_ERROR(gpio_hold_en(pin), kTag, "hold motor GPIO low");
        }
    }
    return ESP_OK;
}

esp_err_t configure_motor(gpio_num_t input1, gpio_num_t input2)
{
    ESP_RETURN_ON_ERROR(
        configure_motor_gpio_low(false), kTag, "prepare motor GPIOs");
    ledc_timer_config_t timer = {};
    timer.speed_mode = kSpeedMode;
    timer.duty_resolution = LEDC_TIMER_10_BIT;
    timer.timer_num = LEDC_TIMER_0;
    timer.freq_hz = 20000;
    timer.clk_cfg = LEDC_USE_PLL_DIV_CLK;
    ESP_RETURN_ON_ERROR(ledc_timer_config(&timer), kTag, "configure motor timer");
    s_motor_configured = true;
    ESP_RETURN_ON_ERROR(
        configure_channel(LEDC_CHANNEL_0, LEDC_TIMER_0, input1),
        kTag, "configure motor input 1");
    ESP_RETURN_ON_ERROR(
        configure_channel(LEDC_CHANNEL_1, LEDC_TIMER_0, input2),
        kTag, "configure motor input 2");
    return ESP_OK;
}

esp_err_t set_motor_percent(int percent)
{
    const uint32_t duty = static_cast<uint32_t>(
        std::abs(percent) * static_cast<int>(kMotorDutyMax) / 100);
    ESP_RETURN_ON_ERROR(set_channel(LEDC_CHANNEL_0, 0), kTag, "clear input 1");
    ESP_RETURN_ON_ERROR(set_channel(LEDC_CHANNEL_1, 0), kTag, "clear input 2");
    if (percent > 0) {
        return set_channel(LEDC_CHANNEL_0, duty);
    }
    if (percent < 0) {
        return set_channel(LEDC_CHANNEL_1, duty);
    }
    return ESP_OK;
}

uint32_t servo_duty(int pulse_us)
{
    return static_cast<uint32_t>(
        (static_cast<uint64_t>(pulse_us) * (1U << 16) + 10000) / 20000);
}

}  // namespace

esp_err_t motor_outputs_init_safe()
{
    return configure_motor_gpio_low(true);
}

esp_err_t motor_test_run(
    const FactoryTestEnvironment &,
    FactoryTestId id,
    FactoryTestRecord *record,
    FactoryModuleOutcome *outcome)
{
    if (record == nullptr || outcome == nullptr ||
        (id != FactoryTestId::kMotorA && id != FactoryTestId::kMotorB)) {
        return ESP_ERR_INVALID_ARG;
    }
    *outcome = {};
    outcome->manual_required = true;
    outcome->replay_supported = true;
    motor_test_cleanup();
    if (!board_service_state().expander_ready) {
        std::snprintf(record->error_code, sizeof(record->error_code), "I2C_NO_ACK");
        std::snprintf(record->detail, sizeof(record->detail), "XL9555 unavailable");
        return ESP_ERR_NOT_FOUND;
    }
    const bool motor_a = id == FactoryTestId::kMotorA;
    const gpio_num_t input1 = motor_a ? BOARD_DRV_AIN1 : BOARD_DRV_BIN1;
    const gpio_num_t input2 = motor_a ? BOARD_DRV_AIN2 : BOARD_DRV_BIN2;
    ESP_RETURN_ON_ERROR(configure_motor(input1, input2), kTag, "configure motor");
    ESP_RETURN_ON_ERROR(
        board_service_set_output(BOARD_XL_P11_DRV_EN, true),
        kTag, "enable motor driver");
    uint32_t level = 0;
    ESP_RETURN_ON_ERROR(
        board_service_get_level(BOARD_XL_P11_DRV_EN, &level),
        kTag, "read motor enable");
    if ((level & BOARD_XL_P11_DRV_EN) == 0) {
        std::snprintf(record->error_code, sizeof(record->error_code), "ENABLE_READBACK");
        std::snprintf(record->detail, sizeof(record->detail), "DRV_EN readback low");
        motor_test_cleanup();
        return ESP_FAIL;
    }

    board_ui_show_status(
        motor_a ? "MOTOR A" : "MOTOR B", "Forward 30%", "Observe direction and motion");
    ESP_RETURN_ON_ERROR(set_motor_percent(30), kTag, "motor forward");
    vTaskDelay(pdMS_TO_TICKS(600));
    ESP_RETURN_ON_ERROR(set_motor_percent(0), kTag, "motor stop");
    vTaskDelay(pdMS_TO_TICKS(200));
    board_ui_show_status(
        motor_a ? "MOTOR A" : "MOTOR B", "Reverse 30%", "Observe direction and motion");
    ESP_RETURN_ON_ERROR(set_motor_percent(-30), kTag, "motor reverse");
    vTaskDelay(pdMS_TO_TICKS(600));
    ESP_RETURN_ON_ERROR(set_motor_percent(0), kTag, "motor stop");
    board_service_set_output(BOARD_XL_P11_DRV_EN, false);
    outcome->automatic_pass = true;
    std::snprintf(record->measurements, sizeof(record->measurements),
                  "{\"duty_percent\":30,\"forward_ms\":600,\"reverse_ms\":600,"
                  "\"enable_readback\":true}");
    std::snprintf(record->detail, sizeof(record->detail),
                  "Confirm forward and reverse motion");
    return ESP_OK;
}

void motor_test_cleanup()
{
    if (s_motor_configured) {
        set_channel(LEDC_CHANNEL_0, 0);
        set_channel(LEDC_CHANNEL_1, 0);
        ledc_stop(kSpeedMode, LEDC_CHANNEL_0, 0);
        ledc_stop(kSpeedMode, LEDC_CHANNEL_1, 0);
        deconfigure_channel(LEDC_CHANNEL_0);
        deconfigure_channel(LEDC_CHANNEL_1);
        deconfigure_timer(LEDC_TIMER_0);
    }
    board_service_set_output(BOARD_XL_P11_DRV_EN, false);
    const esp_err_t gpio_err = configure_motor_gpio_low(true);
    if (gpio_err != ESP_OK) {
        ESP_LOGE(
            kTag, "Unable to hold motor GPIOs low: %s",
            esp_err_to_name(gpio_err));
    }
    s_motor_configured = false;
}

esp_err_t servo_test_run(
    const FactoryTestEnvironment &,
    FactoryTestRecord *record,
    FactoryModuleOutcome *outcome)
{
    if (record == nullptr || outcome == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    *outcome = {};
    outcome->manual_required = true;
    outcome->replay_supported = true;
    servo_test_cleanup();
    if (!board_service_state().expander_ready) {
        std::snprintf(record->error_code, sizeof(record->error_code), "I2C_NO_ACK");
        std::snprintf(record->detail, sizeof(record->detail), "XL9555 unavailable");
        return ESP_ERR_NOT_FOUND;
    }
    ledc_timer_config_t timer = {};
    timer.speed_mode = kSpeedMode;
    timer.duty_resolution = LEDC_TIMER_16_BIT;
    timer.timer_num = LEDC_TIMER_1;
    timer.freq_hz = 50;
    timer.clk_cfg = LEDC_USE_PLL_DIV_CLK;
    ESP_RETURN_ON_ERROR(ledc_timer_config(&timer), kTag, "configure servo timer");
    s_servo_configured = true;
    ESP_RETURN_ON_ERROR(
        configure_channel(LEDC_CHANNEL_4, LEDC_TIMER_1, BOARD_SERVO_PWM),
        kTag, "configure servo channel");
    ESP_RETURN_ON_ERROR(
        board_service_set_output(BOARD_XL_P05_5V_EN, true),
        kTag, "enable servo 5V");
    vTaskDelay(pdMS_TO_TICKS(30));

    constexpr int pulses[] = {1500, 1400, 1600, 1500};
    for (int pulse : pulses) {
        char detail[48] = {};
        std::snprintf(detail, sizeof(detail), "Pulse %d us", pulse);
        board_ui_show_status("SERVO", detail, "Observe smooth movement");
        ESP_RETURN_ON_ERROR(
            set_channel(LEDC_CHANNEL_4, servo_duty(pulse)),
            kTag, "set servo pulse");
        vTaskDelay(pdMS_TO_TICKS(400));
    }
    set_channel(LEDC_CHANNEL_4, 0);
    ledc_stop(kSpeedMode, LEDC_CHANNEL_4, 0);
    board_service_set_output(BOARD_XL_P05_5V_EN, false);
    outcome->automatic_pass = true;
    std::snprintf(record->measurements, sizeof(record->measurements),
                  "{\"pulses_us\":[1500,1400,1600,1500],\"hold_ms\":400}");
    std::snprintf(record->detail, sizeof(record->detail), "Confirm normal servo movement");
    return ESP_OK;
}

void servo_test_cleanup()
{
    if (s_servo_configured) {
        set_channel(LEDC_CHANNEL_4, 0);
        ledc_stop(kSpeedMode, LEDC_CHANNEL_4, 0);
        deconfigure_channel(LEDC_CHANNEL_4);
        deconfigure_timer(LEDC_TIMER_1);
    }
    gpio_config_t servo = {};
    servo.pin_bit_mask = 1ULL << BOARD_SERVO_PWM;
    servo.mode = GPIO_MODE_OUTPUT;
    servo.pull_down_en = GPIO_PULLDOWN_ENABLE;
    if (gpio_config(&servo) == ESP_OK) {
        gpio_set_level(BOARD_SERVO_PWM, 0);
    }
    board_service_set_output(BOARD_XL_P05_5V_EN, false);
    s_servo_configured = false;
}
