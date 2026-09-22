#include "actuator_control.h"

#include <algorithm>
#include <cinttypes>
#include <cstdlib>

#include "board_config.h"
#include "driver/i2c_master.h"
#include "driver/ledc.h"
#include "esp_check.h"
#include "esp_io_expander.h"
#include "esp_io_expander_xl9555.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

namespace {

constexpr char kTag[] = "actuator_control";
constexpr ledc_mode_t kSpeedMode = LEDC_LOW_SPEED_MODE;
constexpr ledc_timer_t kMotorTimer = LEDC_TIMER_0;
constexpr ledc_timer_t kServoTimer = LEDC_TIMER_1;
constexpr ledc_timer_bit_t kMotorResolution = LEDC_TIMER_10_BIT;
constexpr ledc_timer_bit_t kServoResolution = LEDC_TIMER_16_BIT;
constexpr uint32_t kMotorFrequencyHz = 20000;
constexpr uint32_t kServoFrequencyHz = 50;
constexpr uint32_t kMotorDutyMax = (1U << 10) - 1;
constexpr uint32_t kServoDutyScale = 1U << 16;
constexpr uint32_t kServoPeriodUs = 1000000U / kServoFrequencyHz;
constexpr int kActuatorPowerSettleMs = 30;
constexpr int kFailsafeTimeoutMs = 2000;
constexpr int kFailsafeCheckMs = 250;

constexpr ledc_channel_t kMotorAIn1Channel = LEDC_CHANNEL_0;
constexpr ledc_channel_t kMotorAIn2Channel = LEDC_CHANNEL_1;
constexpr ledc_channel_t kMotorBIn1Channel = LEDC_CHANNEL_2;
constexpr ledc_channel_t kMotorBIn2Channel = LEDC_CHANNEL_3;
constexpr ledc_channel_t kServoChannel = LEDC_CHANNEL_4;

SemaphoreHandle_t s_mutex = nullptr;
i2c_master_bus_handle_t s_i2c_bus = nullptr;
esp_io_expander_handle_t s_xl9555 = nullptr;
ActuatorState s_state = {};
ActuatorStateCallback s_callback = nullptr;
void *s_callback_context = nullptr;
int64_t s_last_command_us = 0;

esp_err_t set_duty(ledc_channel_t channel, uint32_t duty)
{
    ESP_RETURN_ON_ERROR(
        ledc_set_duty(kSpeedMode, channel, duty),
        kTag,
        "set LEDC duty");
    return ledc_update_duty(kSpeedMode, channel);
}

esp_err_t set_motor(
    ledc_channel_t input_1,
    ledc_channel_t input_2,
    int percent)
{
    const uint32_t duty = static_cast<uint32_t>(
        std::abs(percent) * static_cast<int>(kMotorDutyMax) / 100);

    // Make both bridge inputs low before changing direction.
    ESP_RETURN_ON_ERROR(set_duty(input_1, 0), kTag, "clear motor input 1");
    ESP_RETURN_ON_ERROR(set_duty(input_2, 0), kTag, "clear motor input 2");
    if (percent > 0) {
        return set_duty(input_1, duty);
    }
    if (percent < 0) {
        return set_duty(input_2, duty);
    }
    return ESP_OK;
}

uint32_t servo_duty_from_pulse(int pulse_us)
{
    return static_cast<uint32_t>(
        (static_cast<uint64_t>(pulse_us) * kServoDutyScale +
         kServoPeriodUs / 2) /
        kServoPeriodUs);
}

esp_err_t apply_outputs_locked(const ActuatorState &next)
{
    ESP_RETURN_ON_ERROR(
        set_motor(
            kMotorAIn1Channel,
            kMotorAIn2Channel,
            next.motor_a_percent),
        kTag,
        "set motor A");
    ESP_RETURN_ON_ERROR(
        set_motor(
            kMotorBIn1Channel,
            kMotorBIn2Channel,
            next.motor_b_percent),
        kTag,
        "set motor B");
    return set_duty(
        kServoChannel,
        next.servo_enabled ? servo_duty_from_pulse(next.servo_pulse_us) : 0);
}

void publish_state(const ActuatorState &state)
{
    if (s_callback != nullptr) {
        s_callback(state, s_callback_context);
    }
}

esp_err_t configure_timer(
    ledc_timer_t timer,
    ledc_timer_bit_t resolution,
    uint32_t frequency_hz)
{
    ledc_timer_config_t config = {};
    config.speed_mode = kSpeedMode;
    config.duty_resolution = resolution;
    config.timer_num = timer;
    config.freq_hz = frequency_hz;
    config.clk_cfg = LEDC_USE_PLL_DIV_CLK;
    return ledc_timer_config(&config);
}

esp_err_t configure_channel(
    ledc_channel_t channel,
    ledc_timer_t timer,
    gpio_num_t gpio)
{
    ledc_channel_config_t config = {};
    config.gpio_num = gpio;
    config.speed_mode = kSpeedMode;
    config.channel = channel;
    config.timer_sel = timer;
    config.duty = 0;
    config.hpoint = 0;
    return ledc_channel_config(&config);
}

esp_err_t initialize_expander_controls()
{
    constexpr uint32_t kEnableMask =
        BOARD_XL9555_P05_POWER_EN | BOARD_XL9555_P11_DRV_EN;

    i2c_master_bus_config_t bus_config = {};
    bus_config.i2c_port = BOARD_I2C_PORT;
    bus_config.sda_io_num = BOARD_I2C_SDA;
    bus_config.scl_io_num = BOARD_I2C_SCL;
    bus_config.clk_source = I2C_CLK_SRC_DEFAULT;
    bus_config.glitch_ignore_cnt = 7;
    bus_config.flags.enable_internal_pullup = true;

    ESP_RETURN_ON_ERROR(
        i2c_new_master_bus(&bus_config, &s_i2c_bus),
        kTag,
        "create actuator I2C bus");
    ESP_RETURN_ON_ERROR(
        esp_io_expander_new_i2c_xl9555(
            s_i2c_bus, BOARD_XL9555_I2C_ADDR, &s_xl9555),
        kTag,
        "create XL9555");
    ESP_RETURN_ON_ERROR(
        esp_io_expander_set_dir(
            s_xl9555,
            kEnableMask,
            IO_EXPANDER_OUTPUT),
        kTag,
        "configure actuator enables");
    ESP_RETURN_ON_ERROR(
        esp_io_expander_set_level(
            s_xl9555, kEnableMask, 1),
        kTag,
        "enable servo power and motor driver");

    vTaskDelay(pdMS_TO_TICKS(kActuatorPowerSettleMs));
    ESP_LOGI(
        kTag,
        "Servo power and motor driver enabled through XL9555 P05/P11 (0x%02X)",
        BOARD_XL9555_I2C_ADDR);
    return ESP_OK;
}

void failsafe_task(void *)
{
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(kFailsafeCheckMs));

        ActuatorState publish = {};
        bool changed = false;
        if (xSemaphoreTake(s_mutex, portMAX_DELAY) == pdTRUE) {
            const int64_t elapsed_us = esp_timer_get_time() - s_last_command_us;
            const bool output_active =
                s_state.motor_a_percent != 0 ||
                s_state.motor_b_percent != 0 || s_state.servo_enabled;
            if (output_active &&
                elapsed_us > static_cast<int64_t>(kFailsafeTimeoutMs) * 1000) {
                ActuatorState stopped = s_state;
                stopped.motor_a_percent = 0;
                stopped.motor_b_percent = 0;
                stopped.servo_enabled = false;
                stopped.failsafe_active = true;
                if (apply_outputs_locked(stopped) == ESP_OK) {
                    s_state = stopped;
                    publish = s_state;
                    changed = true;
                    ESP_LOGW(kTag, "Control heartbeat timed out; outputs disabled");
                }
            }
            xSemaphoreGive(s_mutex);
        }
        if (changed) {
            publish_state(publish);
        }
    }
}

}  // namespace

esp_err_t actuator_control_init(
    ActuatorStateCallback callback,
    void *context)
{
    s_callback = callback;
    s_callback_context = context;
    s_mutex = xSemaphoreCreateMutex();
    if (s_mutex == nullptr) {
        return ESP_ERR_NO_MEM;
    }

    ESP_RETURN_ON_ERROR(
        configure_timer(kMotorTimer, kMotorResolution, kMotorFrequencyHz),
        kTag,
        "configure motor PWM timer");
    ESP_RETURN_ON_ERROR(
        configure_timer(kServoTimer, kServoResolution, kServoFrequencyHz),
        kTag,
        "configure servo PWM timer");
    ESP_RETURN_ON_ERROR(
        configure_channel(
            kMotorAIn1Channel, kMotorTimer, BOARD_DRV_AIN1),
        kTag,
        "configure AIN1");
    ESP_RETURN_ON_ERROR(
        configure_channel(
            kMotorAIn2Channel, kMotorTimer, BOARD_DRV_AIN2),
        kTag,
        "configure AIN2");
    ESP_RETURN_ON_ERROR(
        configure_channel(
            kMotorBIn1Channel, kMotorTimer, BOARD_DRV_BIN1),
        kTag,
        "configure BIN1");
    ESP_RETURN_ON_ERROR(
        configure_channel(
            kMotorBIn2Channel, kMotorTimer, BOARD_DRV_BIN2),
        kTag,
        "configure BIN2");
    ESP_RETURN_ON_ERROR(
        configure_channel(kServoChannel, kServoTimer, BOARD_SERVO_PWM),
        kTag,
        "configure servo PWM");

    s_state = {};
    s_state.servo_pulse_us = kServoPulseCenterUs;
    s_last_command_us = esp_timer_get_time();
    ESP_RETURN_ON_ERROR(
        apply_outputs_locked(s_state), kTag, "initialize outputs low");
    ESP_RETURN_ON_ERROR(
        initialize_expander_controls(), kTag, "initialize XL9555 controls");

    if (xTaskCreate(
            failsafe_task,
            "actuator_failsafe",
            3072,
            nullptr,
            5,
            nullptr) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(
        kTag,
        "Motor PWM=%" PRIu32 " Hz, servo PWM=%" PRIu32 " Hz",
        kMotorFrequencyHz,
        kServoFrequencyHz);
    publish_state(s_state);
    return ESP_OK;
}

esp_err_t actuator_control_apply(
    int motor_a_percent,
    int motor_b_percent,
    int servo_pulse_us,
    bool servo_enabled)
{
    if (motor_a_percent < kMotorPercentMin ||
        motor_a_percent > kMotorPercentMax ||
        motor_b_percent < kMotorPercentMin ||
        motor_b_percent > kMotorPercentMax ||
        servo_pulse_us < kServoPulseMinUs ||
        servo_pulse_us > kServoPulseMaxUs || s_mutex == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }

    ActuatorState publish = {};
    esp_err_t err = ESP_FAIL;
    if (xSemaphoreTake(s_mutex, portMAX_DELAY) == pdTRUE) {
        ActuatorState next = {};
        next.motor_a_percent = motor_a_percent;
        next.motor_b_percent = motor_b_percent;
        next.servo_pulse_us = servo_pulse_us;
        next.servo_enabled = servo_enabled;
        next.failsafe_active = false;
        err = apply_outputs_locked(next);
        if (err == ESP_OK) {
            s_state = next;
            s_last_command_us = esp_timer_get_time();
            publish = s_state;
        }
        xSemaphoreGive(s_mutex);
    }
    if (err == ESP_OK) {
        publish_state(publish);
    }
    return err;
}

void actuator_control_stop_all(bool failsafe_active)
{
    if (s_mutex == nullptr) {
        return;
    }

    ActuatorState publish = {};
    bool changed = false;
    if (xSemaphoreTake(s_mutex, portMAX_DELAY) == pdTRUE) {
        ActuatorState stopped = s_state;
        stopped.motor_a_percent = 0;
        stopped.motor_b_percent = 0;
        stopped.servo_enabled = false;
        stopped.failsafe_active = failsafe_active;
        if (apply_outputs_locked(stopped) == ESP_OK) {
            s_state = stopped;
            s_last_command_us = esp_timer_get_time();
            publish = s_state;
            changed = true;
        }
        xSemaphoreGive(s_mutex);
    }
    if (changed) {
        publish_state(publish);
    }
}

ActuatorState actuator_control_get_state()
{
    ActuatorState result = {};
    if (s_mutex != nullptr &&
        xSemaphoreTake(s_mutex, portMAX_DELAY) == pdTRUE) {
        result = s_state;
        xSemaphoreGive(s_mutex);
    }
    return result;
}
