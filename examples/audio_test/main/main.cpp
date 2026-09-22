#include "audio_test.h"
#include "display_ui.h"
#include "esp_log.h"

namespace {

constexpr char kTag[] = "audio_test";

void on_ui_action(TestMode mode, void *)
{
    audio_test_request(mode);
}

void on_audio_state(const AudioTestState &state, void *)
{
    display_ui_update(state);
}

}  // namespace

extern "C" void app_main(void)
{
    ESP_LOGI(kTag, "T-Bao-S31 ES8389 / ES7210 audio test");

    DisplayUiHandles handles = {};
    ESP_ERROR_CHECK(display_ui_init(on_ui_action, nullptr, &handles));
    ESP_ERROR_CHECK(audio_test_start(
        handles.i2c_bus,
        handles.io_expander,
        on_audio_state,
        nullptr));
}

