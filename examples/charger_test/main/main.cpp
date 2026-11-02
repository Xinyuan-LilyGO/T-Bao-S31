#include "charger_test.h"
#include "display_ui.h"
#include "esp_log.h"

namespace {

constexpr char kTag[] = "charger_test";

void on_charger_state(const ChargerTestState &state, void *)
{
    display_ui_update(state);
}

}  // namespace

extern "C" void app_main(void)
{
    ESP_LOGI(kTag, "T-Bao-S31 SGM41529 charger test");

    const esp_err_t display_err = display_ui_init();
    if (display_err != ESP_OK) {
        ESP_LOGE(
            kTag,
            "Display initialization failed: %s",
            esp_err_to_name(display_err));
        return;
    }

    const esp_err_t charger_err =
        charger_test_start(on_charger_state, nullptr);
    if (charger_err != ESP_OK) {
        ESP_LOGE(
            kTag,
            "Charger initialization failed: %s",
            esp_err_to_name(charger_err));
    }
}
