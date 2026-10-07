#include "factory_controller.h"
#include "factory_tests.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace {

constexpr char kTag[] = "factory_main";
constexpr uint32_t kControllerStackBytes = 16 * 1024;

void factory_controller_task(void *)
{
    factory_controller_run();

    ESP_LOGE(kTag, "Factory controller exited unexpectedly");
    const esp_err_t motor_err = motor_outputs_init_safe();
    if (motor_err != ESP_OK) {
        ESP_LOGE(
            kTag, "Unable to restore safe motor outputs: %s",
            esp_err_to_name(motor_err));
    }
    vTaskDelete(nullptr);
}

}  // namespace

extern "C" void app_main(void)
{
    const esp_err_t motor_err = motor_outputs_init_safe();
    if (motor_err != ESP_OK) {
        ESP_LOGE(
            kTag, "Motor safe initialization failed: %s",
            esp_err_to_name(motor_err));
    }

    if (xTaskCreate(
            factory_controller_task,
            "factory_ctrl",
            kControllerStackBytes,
            nullptr,
            5,
            nullptr) != pdPASS) {
        ESP_LOGE(
            kTag, "Unable to create factory controller task (%lu-byte stack)",
            static_cast<unsigned long>(kControllerStackBytes));
        const esp_err_t safe_err = motor_outputs_init_safe();
        if (safe_err != ESP_OK) {
            ESP_LOGE(
                kTag, "Unable to keep motor outputs safe: %s",
                esp_err_to_name(safe_err));
        }
    }
}
