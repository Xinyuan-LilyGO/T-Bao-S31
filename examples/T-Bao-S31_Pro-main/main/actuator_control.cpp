#include "actuator_control.h"

#include <algorithm>
#include <cstdlib>
#include <inttypes.h>

#include "driver/gpio.h"
#include "driver/ledc.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_io_expander.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "tbao_s31_board.h"

namespace {

constexpr char kTag[] = "actuator_control";
constexpr ledc_mode_t kSpeedMode = LEDC_LOW_SPEED_MODE;
/* LEDC timer0/channel0 belong to the Board Manager LCD backlight. */
constexpr ledc_timer_t kMotorTimer = LEDC_TIMER_2;
constexpr ledc_timer_t kServoTimer = LEDC_TIMER_3;
constexpr ledc_timer_bit_t kMotorResolution = LEDC_TIMER_10_BIT;
constexpr ledc_timer_bit_t kServoResolution = LEDC_TIMER_16_BIT;
constexpr uint32_t kMotorFrequencyHz = 20000;
constexpr uint32_t kServoFrequencyHz = 50;
constexpr uint32_t kMotorDutyMax = (1U << 10) - 1U;
constexpr uint32_t kServoDutyScale = 1U << 16;
constexpr uint32_t kServoPeriodUs = 1000000U / kServoFrequencyHz;
constexpr int kPowerSettleMs = 30;
constexpr int kFailsafeTimeoutMs = 2000;
constexpr int kFailsafeCheckMs = 250;

constexpr gpio_num_t kMotorPins[] = {
    TBAO_S31_MOTOR_AIN1,
    TBAO_S31_MOTOR_AIN2,
    TBAO_S31_MOTOR_BIN1,
    TBAO_S31_MOTOR_BIN2,
};

constexpr ledc_channel_t kMotorAIn1Channel = LEDC_CHANNEL_1;
constexpr ledc_channel_t kMotorAIn2Channel = LEDC_CHANNEL_2;
constexpr ledc_channel_t kMotorBIn1Channel = LEDC_CHANNEL_3;
constexpr ledc_channel_t kMotorBIn2Channel = LEDC_CHANNEL_4;
constexpr ledc_channel_t kServoChannel = LEDC_CHANNEL_5;

SemaphoreHandle_t s_mutex = nullptr;
esp_io_expander_handle_t s_xl9555 = nullptr;
actuator_state_t s_state = {};
actuator_state_callback_t s_callback = nullptr;
void *s_callback_context = nullptr;
int64_t s_last_command_us = 0;
bool s_initialized = false;

esp_err_t set_duty(ledc_channel_t channel, uint32_t duty)
{
    ESP_RETURN_ON_ERROR(ledc_set_duty(kSpeedMode, channel, duty),
                        kTag, "set LEDC duty");
    return ledc_update_duty(kSpeedMode, channel);
}

esp_err_t set_motor_gpio_hold(bool held)
{
    for (gpio_num_t pin : kMotorPins) {
        if (held) {
            ESP_RETURN_ON_ERROR(gpio_set_level(pin, 0), kTag,
                                "clear motor GPIO before hold");
            ESP_RETURN_ON_ERROR(gpio_hold_en(pin), kTag,
                                "hold motor GPIO low");
        } else {
            ESP_RETURN_ON_ERROR(gpio_hold_dis(pin), kTag,
                                "release motor GPIO hold");
        }
    }
    return ESP_OK;
}

esp_err_t set_motor(ledc_channel_t input_1,
                    ledc_channel_t input_2,
                    int percent)
{
    const uint32_t duty = static_cast<uint32_t>(
        std::abs(percent) * static_cast<int>(kMotorDutyMax) / 100);

    /* Clear both bridge inputs before every direction update. */
    ESP_RETURN_ON_ERROR(set_duty(input_1, 0), kTag,
                        "clear motor input 1");
    ESP_RETURN_ON_ERROR(set_duty(input_2, 0), kTag,
                        "clear motor input 2");
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
         kServoPeriodUs / 2U) /
        kServoPeriodUs);
}

esp_err_t set_expander_level(uint32_t mask, bool enabled, const char *what)
{
    if (s_xl9555 == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }

    /* The XL9555 shares the board I2C bus with the audio codecs, the camera
     * PMIC and the battery gauge.  Camera rail switching can brown out the
     * expander/gauge for several hundred milliseconds; a single readback
     * failure used to permanently lock motion as a safety reaction.  The
     * set+readback is idempotent, so retry across that whole window (each
     * failed read itself costs the XL9555's 100 ms timeout). */
    esp_err_t last = ESP_OK;
    for (int attempt = 0; attempt < 5; ++attempt) {
        last = esp_io_expander_set_level(s_xl9555, mask, enabled ? 1 : 0);
        if (last == ESP_OK) {
            uint32_t level = 0;
            last = esp_io_expander_get_level(s_xl9555, mask, &level);
            if (last == ESP_OK) {
                if ((level & mask) == (enabled ? mask : 0)) {
                    return ESP_OK;
                }
                last = ESP_ERR_INVALID_STATE;
                ESP_LOGW(kTag, "%s readback mismatch (attempt %d): 0x%04" PRIX32,
                         what, attempt + 1, level);
            }
        } else {
            ESP_LOGW(kTag, "set %s failed (attempt %d): %s", what, attempt + 1,
                     esp_err_to_name(last));
        }
        vTaskDelay(pdMS_TO_TICKS(30));
    }
    ESP_LOGE(kTag, "set %s failed after retries: %s", what,
             esp_err_to_name(last));
    return last;
}

esp_err_t set_motor_driver_enabled(bool enabled)
{
    return set_expander_level(TBAO_S31_XL9555_P11_MOTOR_EN, enabled,
                               "motor driver enable");
}

esp_err_t ensure_servo_power_enabled(bool enabled)
{
    /* P05 is the board-wide 5 V/audio rail on the integrated firmware. The
     * standalone motor demo owns that rail, but this application must leave it
     * unchanged while the servo is idle or during a motor stop. */
    if (!enabled) {
        return ESP_OK;
    }
    return set_expander_level(TBAO_S31_XL9555_P05_POWER_EN, enabled,
                              "servo power");
}

esp_err_t apply_outputs_locked(const actuator_state_t &next)
{
    const bool motor_active = next.motor_a_percent != 0 ||
                              next.motor_b_percent != 0;

    ESP_RETURN_ON_ERROR(set_motor_gpio_hold(false), kTag,
                        "release motor GPIO hold");
    ESP_RETURN_ON_ERROR(set_motor(kMotorAIn1Channel, kMotorAIn2Channel,
                                  next.motor_a_percent),
                        kTag, "set motor A");
    ESP_RETURN_ON_ERROR(set_motor(kMotorBIn1Channel, kMotorBIn2Channel,
                                  next.motor_b_percent),
                        kTag, "set motor B");
    if (!motor_active) {
        ESP_RETURN_ON_ERROR(set_motor_gpio_hold(true), kTag,
                            "hold motor GPIOs low");
    }
    ESP_RETURN_ON_ERROR(set_motor_driver_enabled(motor_active), kTag,
                        "set motor driver enable");

    if (next.servo_enabled) {
        const bool was_disabled = !s_state.servo_enabled;
        ESP_RETURN_ON_ERROR(ensure_servo_power_enabled(true), kTag,
                            "enable servo power");
        if (was_disabled) {
            vTaskDelay(pdMS_TO_TICKS(kPowerSettleMs));
        }
        return set_duty(kServoChannel,
                        servo_duty_from_pulse(next.servo_pulse_us));
    }

    ESP_RETURN_ON_ERROR(set_duty(kServoChannel, 0), kTag,
                        "disable servo PWM");
    return ensure_servo_power_enabled(false);
}

esp_err_t prepare_motor_gpio()
{
    gpio_config_t config = {};
    for (gpio_num_t pin : kMotorPins) {
        config.pin_bit_mask |= 1ULL << pin;
    }
    config.mode = GPIO_MODE_OUTPUT;
    config.pull_down_en = GPIO_PULLDOWN_ENABLE;
    ESP_RETURN_ON_ERROR(gpio_config(&config), kTag,
                        "configure motor GPIOs");
    for (gpio_num_t pin : kMotorPins) {
        ESP_RETURN_ON_ERROR(gpio_hold_dis(pin), kTag,
                            "release inherited motor hold");
        ESP_RETURN_ON_ERROR(gpio_set_level(pin, 0), kTag,
                            "clear motor GPIO");
        ESP_RETURN_ON_ERROR(gpio_hold_en(pin), kTag,
                            "hold initial motor GPIO low");
    }
    return ESP_OK;
}

esp_err_t configure_timer(ledc_timer_t timer,
                          ledc_timer_bit_t resolution,
                          uint32_t frequency_hz)
{
    ledc_timer_config_t config = {};
    config.speed_mode = kSpeedMode;
    config.duty_resolution = resolution;
    config.timer_num = timer;
    config.freq_hz = frequency_hz;
    /* The Board Manager LCD backlight already owns the low-speed LEDC group.
     * ESP32-S31 exposes one shared global clock for that group, and its
     * AUTO_CLK selection resolves to XTAL for the backlight configuration. */
    config.clk_cfg = LEDC_USE_XTAL_CLK;
    return ledc_timer_config(&config);
}

esp_err_t configure_channel(ledc_channel_t channel,
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
    ESP_RETURN_ON_ERROR(tbao_s31_board_get_io_expander(&s_xl9555), kTag,
                        "get Board Manager XL9555 handle");

    const uint32_t enable_mask = TBAO_S31_XL9555_P05_POWER_EN |
                                 TBAO_S31_XL9555_P11_MOTOR_EN;
    ESP_RETURN_ON_ERROR(esp_io_expander_set_dir(s_xl9555, enable_mask,
                                                IO_EXPANDER_OUTPUT),
                        kTag, "configure actuator expander outputs");
    ESP_RETURN_ON_ERROR(esp_io_expander_set_level(
                            s_xl9555, TBAO_S31_XL9555_P11_MOTOR_EN, 0),
                        kTag, "disable actuator power at startup");

    vTaskDelay(pdMS_TO_TICKS(kPowerSettleMs));
    uint32_t levels = 0;
    ESP_RETURN_ON_ERROR(esp_io_expander_get_level(s_xl9555, enable_mask,
                                                  &levels),
                        kTag, "read actuator startup levels");
    ESP_RETURN_ON_FALSE((levels & TBAO_S31_XL9555_P11_MOTOR_EN) == 0,
                        ESP_ERR_INVALID_STATE, kTag,
                        "motor enable startup readback is high: 0x%04" PRIX32,
                        levels);
    ESP_LOGI(kTag, "XL9555 runtime address=0x%02X P05 rail preserved (level=0x%04" PRIX32
             ") and P11 motor enable low",
             TBAO_S31_XL9555_ADDRESS, levels & TBAO_S31_XL9555_P05_POWER_EN);
    return ESP_OK;
}

void publish_state(const actuator_state_t &state)
{
    if (s_callback != nullptr) {
        s_callback(&state, s_callback_context);
    }
}

void failsafe_task(void *)
{
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(kFailsafeCheckMs));

        actuator_state_t publish = {};
        bool changed = false;
        if (xSemaphoreTake(s_mutex, portMAX_DELAY) == pdTRUE) {
            const int64_t elapsed_us = esp_timer_get_time() - s_last_command_us;
            const bool output_active = s_state.motor_a_percent != 0 ||
                                       s_state.motor_b_percent != 0 ||
                                       s_state.servo_enabled;
            if (output_active &&
                elapsed_us > static_cast<int64_t>(kFailsafeTimeoutMs) * 1000) {
                actuator_state_t stopped = s_state;
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

extern "C" esp_err_t actuator_control_init(actuator_state_callback_t callback,
                                             void *context)
{
    if (s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    s_callback = callback;
    s_callback_context = context;
    s_mutex = xSemaphoreCreateMutex();
    if (s_mutex == nullptr) {
        return ESP_ERR_NO_MEM;
    }

    ESP_RETURN_ON_ERROR(prepare_motor_gpio(), kTag,
                        "prepare motor GPIO");
    ESP_RETURN_ON_ERROR(configure_timer(kMotorTimer, kMotorResolution,
                                        kMotorFrequencyHz),
                        kTag, "configure motor PWM timer");
    ESP_RETURN_ON_ERROR(configure_timer(kServoTimer, kServoResolution,
                                        kServoFrequencyHz),
                        kTag, "configure servo PWM timer");
    ESP_RETURN_ON_ERROR(configure_channel(kMotorAIn1Channel, kMotorTimer,
                                          TBAO_S31_MOTOR_AIN1),
                        kTag, "configure AIN1");
    ESP_RETURN_ON_ERROR(configure_channel(kMotorAIn2Channel, kMotorTimer,
                                          TBAO_S31_MOTOR_AIN2),
                        kTag, "configure AIN2");
    ESP_RETURN_ON_ERROR(configure_channel(kMotorBIn1Channel, kMotorTimer,
                                          TBAO_S31_MOTOR_BIN1),
                        kTag, "configure BIN1");
    ESP_RETURN_ON_ERROR(configure_channel(kMotorBIn2Channel, kMotorTimer,
                                          TBAO_S31_MOTOR_BIN2),
                        kTag, "configure BIN2");
    ESP_RETURN_ON_ERROR(configure_channel(kServoChannel, kServoTimer,
                                          TBAO_S31_SERVO_PWM),
                        kTag, "configure servo PWM");
    ESP_RETURN_ON_ERROR(initialize_expander_controls(), kTag,
                        "initialize XL9555 controls");

    s_state = {};
    s_state.servo_pulse_us = ACTUATOR_SERVO_PULSE_CENTER_US;
    s_last_command_us = esp_timer_get_time();
    ESP_RETURN_ON_ERROR(apply_outputs_locked(s_state), kTag,
                        "initialize outputs low");

    /* LEDC/GPIO only; no flash/NVS on this path, so the stack can live in
     * PSRAM and keep internal SRAM for Wi-Fi/AFE. */
    BaseType_t task_ok = xTaskCreatePinnedToCoreWithCaps(
        failsafe_task, "actuator_failsafe", 3072, nullptr, 5, nullptr,
        tskNO_AFFINITY, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (task_ok != pdPASS) {
        task_ok = xTaskCreatePinnedToCore(failsafe_task, "actuator_failsafe",
                                          3072, nullptr, 5, nullptr,
                                          tskNO_AFFINITY);
    }
    if (task_ok != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    s_initialized = true;
    ESP_LOGI(kTag, "motor PWM=%" PRIu32 " Hz, servo PWM=%" PRIu32
             " Hz, pins A=%d/%d B=%d/%d servo=%d",
             kMotorFrequencyHz, kServoFrequencyHz,
             TBAO_S31_MOTOR_AIN1, TBAO_S31_MOTOR_AIN2,
             TBAO_S31_MOTOR_BIN1, TBAO_S31_MOTOR_BIN2,
             TBAO_S31_SERVO_PWM);
    publish_state(s_state);
    return ESP_OK;
}

extern "C" esp_err_t actuator_control_apply(int motor_a_percent,
                                              int motor_b_percent,
                                              int servo_pulse_us,
                                              bool servo_enabled)
{
    if (!s_initialized || s_mutex == nullptr ||
        motor_a_percent < ACTUATOR_MOTOR_PERCENT_MIN ||
        motor_a_percent > ACTUATOR_MOTOR_PERCENT_MAX ||
        motor_b_percent < ACTUATOR_MOTOR_PERCENT_MIN ||
        motor_b_percent > ACTUATOR_MOTOR_PERCENT_MAX ||
        servo_pulse_us < ACTUATOR_SERVO_PULSE_MIN_US ||
        servo_pulse_us > ACTUATOR_SERVO_PULSE_MAX_US) {
        return ESP_ERR_INVALID_ARG;
    }

    actuator_state_t publish = {};
    esp_err_t err = ESP_FAIL;
    if (xSemaphoreTake(s_mutex, portMAX_DELAY) == pdTRUE) {
        actuator_state_t next = {};
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

extern "C" void actuator_control_stop_all(bool failsafe_active)
{
    if (!s_initialized || s_mutex == nullptr) {
        return;
    }

    actuator_state_t publish = {};
    bool changed = false;
    if (xSemaphoreTake(s_mutex, portMAX_DELAY) == pdTRUE) {
        actuator_state_t stopped = s_state;
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

extern "C" esp_err_t actuator_control_get_state(actuator_state_t *out_state)
{
    if (out_state == nullptr || s_mutex == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    if (xSemaphoreTake(s_mutex, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    *out_state = s_state;
    xSemaphoreGive(s_mutex);
    return ESP_OK;
}
