#include "factory_controller.h"
#include "factory_tests.h"

#include "esp_log.h"

extern "C" void app_main(void)
{
    const esp_err_t motor_err = motor_outputs_init_safe();
    if (motor_err != ESP_OK) {
        ESP_LOGE(
            "factory_main", "Motor safe initialization failed: %s",
            esp_err_to_name(motor_err));
    }
    factory_controller_run();
}
