#include <cstdio>

#include "display_ui.h"
#include "esp_err.h"
#include "esp_log.h"
#include "wifi_test.h"

namespace {

constexpr char kTag[] = "wifi_scan_display";

void on_wifi_state(const WifiTestState &state, void *)
{
    display_ui_update(state);
}

void show_start_error(esp_err_t err)
{
    WifiTestState state = {};
    state.phase = WifiTestPhase::kConnectionFailed;
    std::snprintf(
        state.detail,
        sizeof(state.detail),
        "Wi-Fi init failed: %s",
        esp_err_to_name(err));
    display_ui_update(state);
    ESP_LOGE(kTag, "%s", state.detail);
}

}  // namespace

extern "C" void app_main(void)
{
    ESP_LOGI(kTag, "T-Bao-S31 Wi-Fi scan and connection test");
    ESP_ERROR_CHECK(display_ui_init());

    const esp_err_t err = wifi_test_start(on_wifi_state, nullptr);
    if (err != ESP_OK) {
        show_start_error(err);
    }
}
