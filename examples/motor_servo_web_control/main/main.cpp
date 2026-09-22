#include <cstdio>

#include "actuator_control.h"
#include "display_ui.h"
#include "esp_err.h"
#include "esp_log.h"
#include "web_server.h"
#include "wifi_manager.h"

namespace {

constexpr char kTag[] = "motor_servo_web";

void on_actuator_state(const ActuatorState &state, void *)
{
    display_ui_update_actuators(state);
}

void on_wifi_status(const WifiStatus &status, void *)
{
    display_ui_update_wifi(status);

    if (status.phase == WifiPhase::kConnected) {
        const esp_err_t err = web_server_start();
        if (err != ESP_OK) {
            WifiStatus failed = status;
            failed.phase = WifiPhase::kConnectionFailed;
            std::snprintf(
                failed.detail,
                sizeof(failed.detail),
                "Web server failed: %s",
                esp_err_to_name(err));
            display_ui_update_wifi(failed);
            actuator_control_stop_all(true);
            ESP_LOGE(kTag, "%s", failed.detail);
        }
        return;
    }

    web_server_stop();
    if (status.phase == WifiPhase::kDisconnected ||
        status.phase == WifiPhase::kConnectionFailed) {
        actuator_control_stop_all(true);
    }
}

void show_start_error(const char *component, esp_err_t err)
{
    WifiStatus status = {};
    status.phase = WifiPhase::kConnectionFailed;
    std::snprintf(
        status.detail,
        sizeof(status.detail),
        "%s failed: %s",
        component,
        esp_err_to_name(err));
    display_ui_update_wifi(status);
    ESP_LOGE(kTag, "%s", status.detail);
}

}  // namespace

extern "C" void app_main(void)
{
    ESP_LOGI(kTag, "T-Bao-S31 motor, servo and web control test");
    ESP_ERROR_CHECK(display_ui_init());

    esp_err_t err = actuator_control_init(on_actuator_state, nullptr);
    if (err != ESP_OK) {
        show_start_error("Actuator init", err);
        return;
    }

    err = wifi_manager_start(on_wifi_status, nullptr);
    if (err != ESP_OK) {
        actuator_control_stop_all(true);
        show_start_error("Wi-Fi init", err);
    }
}
