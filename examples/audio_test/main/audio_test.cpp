#include "audio_test.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>

#include "board_config.h"
#include "driver/gpio.h"
#include "driver/i2s_std.h"
#include "esp_check.h"
#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"
#include "esp_codec_dev_vol.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

namespace {

constexpr char kTag[] = "audio_hw";
constexpr int kProbeTimeoutMs = 1000;
constexpr int kMicFramesPerRead = 960;
constexpr int kMicFrameDurationMs =
    kMicFramesPerRead * 1000 / BOARD_AUDIO_SAMPLE_RATE;
constexpr int kMicPublishInterval = 5;
constexpr int kMicStartupDiscardFrames = 5;
constexpr int kZeroFrameFailCount = 25;
constexpr int kToneFrequencyHz = 1000;
constexpr int kToneFrames = 480;
constexpr int kToneBufferDurationMs =
    kToneFrames * 1000 / BOARD_AUDIO_SAMPLE_RATE;
constexpr float kToneAmplitude = 0.20f;
constexpr float kPi = 3.14159265358979323846f;

// esp_codec_dev keeps codec I2C addresses in 8-bit write-address form, while
// i2c_master_probe() and the board configuration use 7-bit bus addresses.
static_assert(
    ES8389_CODEC_DEFAULT_ADDR == (BOARD_ES8389_I2C_ADDR << 1),
    "ES8389 codec address does not match the board address");
static_assert(
    ES7210_CODEC_DEFAULT_ADDR == (BOARD_ES7210_I2C_ADDR << 1),
    "ES7210 codec address does not match the board address");

i2c_master_bus_handle_t s_i2c_bus = nullptr;
esp_io_expander_handle_t s_io_expander = nullptr;
i2s_chan_handle_t s_i2s_tx = nullptr;
i2s_chan_handle_t s_i2s_rx = nullptr;

const audio_codec_gpio_if_t *s_codec_gpio_if = nullptr;
const audio_codec_ctrl_if_t *s_es8389_ctrl_if = nullptr;
const audio_codec_ctrl_if_t *s_es7210_ctrl_if = nullptr;
const audio_codec_data_if_t *s_tx_data_if = nullptr;
const audio_codec_data_if_t *s_rx_data_if = nullptr;
const audio_codec_if_t *s_es8389_codec_if = nullptr;
const audio_codec_if_t *s_es7210_codec_if = nullptr;
esp_codec_dev_handle_t s_es8389_device = nullptr;
esp_codec_dev_handle_t s_es7210_device = nullptr;

QueueHandle_t s_mode_queue = nullptr;
AudioStateCallback s_state_callback = nullptr;
void *s_state_context = nullptr;
AudioTestState s_state = {};
int s_zero_frame_count = 0;

void publish_state()
{
    if (s_state_callback != nullptr) {
        s_state_callback(s_state, s_state_context);
    }
}

void set_detail(const char *detail)
{
    std::snprintf(s_state.detail, sizeof(s_state.detail), "%s", detail);
}

bool probe_device(uint8_t address, const char *name)
{
    const esp_err_t err = i2c_master_probe(s_i2c_bus, address, kProbeTimeoutMs);
    if (err == ESP_OK) {
        ESP_LOGI(kTag, "%s I2C ACK at 0x%02X", name, address);
        return true;
    }
    ESP_LOGE(
        kTag, "%s I2C probe failed at 0x%02X: %s",
        name, address, esp_err_to_name(err));
    return false;
}

esp_err_t set_speaker_enabled(bool enabled)
{
    return esp_io_expander_set_level(
        s_io_expander,
        BOARD_XL9555_P07_SPK_CTRL_MASK,
        enabled ? 1 : 0);
}

esp_err_t init_speaker_control()
{
    ESP_RETURN_ON_ERROR(
        esp_io_expander_set_dir(
            s_io_expander,
            BOARD_XL9555_P07_SPK_CTRL_MASK,
            IO_EXPANDER_OUTPUT),
        kTag, "configure speaker control");
    return set_speaker_enabled(false);
}

esp_err_t init_i2s()
{
    i2s_chan_config_t channel_config =
        I2S_CHANNEL_DEFAULT_CONFIG(BOARD_I2S_PORT, I2S_ROLE_MASTER);
    channel_config.auto_clear = true;
    channel_config.dma_desc_num = 6;
    channel_config.dma_frame_num = 240;
    ESP_RETURN_ON_ERROR(
        i2s_new_channel(&channel_config, &s_i2s_tx, &s_i2s_rx),
        kTag, "create I2S channels");

    i2s_std_config_t standard_config = {};
    standard_config.clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(BOARD_AUDIO_SAMPLE_RATE);
    standard_config.clk_cfg.mclk_multiple = BOARD_AUDIO_MCLK_MULTIPLE;
    standard_config.slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
        I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO);
    standard_config.gpio_cfg.mclk = BOARD_I2S_MCLK;
    standard_config.gpio_cfg.bclk = BOARD_I2S_BCLK;
    standard_config.gpio_cfg.ws = BOARD_I2S_LRCK;
    standard_config.gpio_cfg.dout = BOARD_I2S_DOUT;
    standard_config.gpio_cfg.din = BOARD_I2S_DIN;
    standard_config.gpio_cfg.invert_flags.mclk_inv = false;
    standard_config.gpio_cfg.invert_flags.bclk_inv = false;
    standard_config.gpio_cfg.invert_flags.ws_inv = false;

    // Initialize RX first and TX second. The paired TX channel then owns the
    // shared clocks while auto-clear keeps the idle output at zero.
    ESP_RETURN_ON_ERROR(
        i2s_channel_init_std_mode(s_i2s_rx, &standard_config),
        kTag, "initialize I2S RX");
    ESP_RETURN_ON_ERROR(
        i2s_channel_init_std_mode(s_i2s_tx, &standard_config),
        kTag, "initialize I2S TX");
    ESP_RETURN_ON_ERROR(
        i2s_channel_enable(s_i2s_rx), kTag, "enable I2S RX");
    ESP_RETURN_ON_ERROR(
        i2s_channel_enable(s_i2s_tx), kTag, "enable I2S TX");

    ESP_LOGI(
        kTag,
        "I2S ready: MCLK=%d DOUT=%d BCLK=%d LRCK=%d DIN=%d",
        BOARD_I2S_MCLK,
        BOARD_I2S_DOUT,
        BOARD_I2S_BCLK,
        BOARD_I2S_LRCK,
        BOARD_I2S_DIN);
    return ESP_OK;
}

bool init_es8389()
{
    audio_codec_i2c_cfg_t i2c_config = {};
    i2c_config.port = BOARD_I2C_PORT;
    i2c_config.addr = ES8389_CODEC_DEFAULT_ADDR;
    i2c_config.bus_handle = s_i2c_bus;
    i2c_config.clock_speed_hz = BOARD_AUDIO_I2C_FREQ_HZ;
    s_es8389_ctrl_if = audio_codec_new_i2c_ctrl(&i2c_config);
    if (s_es8389_ctrl_if == nullptr) {
        ESP_LOGE(kTag, "Unable to create ES8389 I2C control interface");
        return false;
    }

    audio_codec_i2s_cfg_t data_config = {};
    data_config.port = BOARD_I2S_PORT;
    data_config.tx_handle = s_i2s_tx;
    s_tx_data_if = audio_codec_new_i2s_data(&data_config);
    if (s_tx_data_if == nullptr) {
        ESP_LOGE(kTag, "Unable to create ES8389 I2S data interface");
        return false;
    }

    if (s_codec_gpio_if == nullptr) {
        s_codec_gpio_if = audio_codec_new_gpio();
    }
    if (s_codec_gpio_if == nullptr) {
        ESP_LOGE(kTag, "Unable to create codec GPIO interface");
        return false;
    }

    es8389_codec_cfg_t codec_config = {};
    codec_config.ctrl_if = s_es8389_ctrl_if;
    codec_config.gpio_if = s_codec_gpio_if;
    codec_config.codec_mode = ESP_CODEC_DEV_WORK_MODE_DAC;
    codec_config.pa_pin = GPIO_NUM_NC;
    codec_config.master_mode = false;
    codec_config.use_mclk = true;
    codec_config.digital_mic = false;
    codec_config.invert_mclk = false;
    codec_config.invert_sclk = false;
    codec_config.hw_gain.pa_voltage = 5.0f;
    codec_config.hw_gain.codec_dac_voltage = 3.3f;
    codec_config.no_dac_ref = false;
    codec_config.mclk_div = BOARD_AUDIO_MCLK_MULTIPLE;
    s_es8389_codec_if = es8389_codec_new(&codec_config);
    if (s_es8389_codec_if == nullptr) {
        ESP_LOGE(kTag, "Unable to create ES8389 codec interface");
        return false;
    }

    esp_codec_dev_cfg_t device_config = {};
    device_config.dev_type = ESP_CODEC_DEV_TYPE_OUT;
    device_config.codec_if = s_es8389_codec_if;
    device_config.data_if = s_tx_data_if;
    s_es8389_device = esp_codec_dev_new(&device_config);
    if (s_es8389_device == nullptr) {
        ESP_LOGE(kTag, "Unable to create ES8389 device");
        return false;
    }

    esp_codec_dev_sample_info_t sample_config = {};
    sample_config.bits_per_sample = I2S_DATA_BIT_WIDTH_16BIT;
    sample_config.channel = BOARD_AUDIO_CHANNELS;
    sample_config.channel_mask = 0x03;
    sample_config.sample_rate = BOARD_AUDIO_SAMPLE_RATE;
    sample_config.mclk_multiple = BOARD_AUDIO_MCLK_MULTIPLE;
    if (esp_codec_dev_open(s_es8389_device, &sample_config) != ESP_CODEC_DEV_OK) {
        ESP_LOGE(kTag, "ES8389 open failed");
        return false;
    }
    if (esp_codec_dev_set_out_vol(s_es8389_device, 55) != ESP_CODEC_DEV_OK) {
        ESP_LOGE(kTag, "ES8389 volume setup failed");
        return false;
    }

    ESP_LOGI(kTag, "ES8389 DAC initialized");
    return true;
}

bool init_es7210()
{
    audio_codec_i2c_cfg_t i2c_config = {};
    i2c_config.port = BOARD_I2C_PORT;
    i2c_config.addr = ES7210_CODEC_DEFAULT_ADDR;
    i2c_config.bus_handle = s_i2c_bus;
    i2c_config.clock_speed_hz = BOARD_AUDIO_I2C_FREQ_HZ;
    s_es7210_ctrl_if = audio_codec_new_i2c_ctrl(&i2c_config);
    if (s_es7210_ctrl_if == nullptr) {
        ESP_LOGE(kTag, "Unable to create ES7210 I2C control interface");
        return false;
    }

    audio_codec_i2s_cfg_t data_config = {};
    data_config.port = BOARD_I2S_PORT;
    data_config.rx_handle = s_i2s_rx;
    s_rx_data_if = audio_codec_new_i2s_data(&data_config);
    if (s_rx_data_if == nullptr) {
        ESP_LOGE(kTag, "Unable to create ES7210 I2S data interface");
        return false;
    }

    constexpr uint32_t selected_mics = ES7210_SEL_MIC1 | ES7210_SEL_MIC2;
    es7210_codec_cfg_t codec_config = {};
    codec_config.ctrl_if = s_es7210_ctrl_if;
    codec_config.master_mode = false;
    codec_config.mic_selected = selected_mics;
    codec_config.mclk_src = ES7210_MCLK_FROM_PAD;
    codec_config.mclk_div = BOARD_AUDIO_MCLK_MULTIPLE;
    s_es7210_codec_if = es7210_codec_new(&codec_config);
    if (s_es7210_codec_if == nullptr) {
        ESP_LOGE(kTag, "Unable to create ES7210 codec interface");
        return false;
    }

    esp_codec_dev_cfg_t device_config = {};
    device_config.dev_type = ESP_CODEC_DEV_TYPE_IN;
    device_config.codec_if = s_es7210_codec_if;
    device_config.data_if = s_rx_data_if;
    s_es7210_device = esp_codec_dev_new(&device_config);
    if (s_es7210_device == nullptr) {
        ESP_LOGE(kTag, "Unable to create ES7210 device");
        return false;
    }

    esp_codec_dev_sample_info_t sample_config = {};
    sample_config.bits_per_sample = I2S_DATA_BIT_WIDTH_16BIT;
    sample_config.channel = BOARD_AUDIO_CHANNELS;
    sample_config.channel_mask = selected_mics;
    sample_config.sample_rate = BOARD_AUDIO_SAMPLE_RATE;
    sample_config.mclk_multiple = BOARD_AUDIO_MCLK_MULTIPLE;
    if (esp_codec_dev_open(s_es7210_device, &sample_config) != ESP_CODEC_DEV_OK) {
        ESP_LOGE(kTag, "ES7210 open failed");
        return false;
    }
    if (esp_codec_dev_set_in_gain(s_es7210_device, 30.0f) != ESP_CODEC_DEV_OK) {
        ESP_LOGE(kTag, "ES7210 gain setup failed");
        return false;
    }

    ESP_LOGI(kTag, "ES7210 MIC1/MIC2 initialized");
    return true;
}

float amplitude_to_dbfs(float amplitude)
{
    if (amplitude <= (1.0f / 32768.0f)) {
        return -96.0f;
    }
    return std::max(-96.0f, 20.0f * std::log10(amplitude));
}

bool capture_mic_frame()
{
    std::array<int16_t, kMicFramesPerRead * BOARD_AUDIO_CHANNELS> samples = {};
    if (esp_codec_dev_read(
            s_es7210_device, samples.data(), samples.size() * sizeof(int16_t)) !=
        ESP_CODEC_DEV_OK) {
        s_state.i2s_rx = TestResult::kFail;
        set_detail("MIC READ FAILED");
        ESP_LOGE(kTag, "ES7210 PCM read failed");
        return false;
    }

    long double square_sum = 0.0;
    int32_t peak = 0;
    for (int16_t sample : samples) {
        const int32_t value = sample;
        const int32_t magnitude = value < 0 ? -value : value;
        peak = std::max(peak, magnitude);
        square_sum += static_cast<long double>(value) * value;
    }

    const long double mean_square = square_sum / samples.size();
    const float rms = static_cast<float>(std::sqrt(mean_square) / 32768.0L);
    const float peak_normalized = static_cast<float>(peak) / 32768.0f;
    s_state.rms_dbfs = amplitude_to_dbfs(rms);
    s_state.peak_dbfs = amplitude_to_dbfs(peak_normalized);
    const float scaled_level = (s_state.rms_dbfs + 60.0f) * (100.0f / 60.0f);
    s_state.level_percent = static_cast<uint8_t>(
        std::clamp(scaled_level, 0.0f, 100.0f));

    if (peak > 0) {
        s_zero_frame_count = 0;
        s_state.i2s_rx = TestResult::kPass;
    } else if (++s_zero_frame_count >= kZeroFrameFailCount) {
        s_state.i2s_rx = TestResult::kFail;
        set_detail("MIC PCM IS ALL ZERO");
    }
    return true;
}

bool poll_mode_request(TestMode *requested_mode)
{
    return xQueueReceive(s_mode_queue, requested_mode, 0) == pdTRUE;
}

bool discard_mic_startup(TestMode *requested_mode)
{
    for (int i = 0; i < kMicStartupDiscardFrames; ++i) {
        if (poll_mode_request(requested_mode)) {
            return false;
        }
        if (!capture_mic_frame()) {
            return true;
        }
    }
    return true;
}

bool run_mic_mode(TestMode *requested_mode)
{
    s_state.mode = TestMode::kMic;
    set_detail("MIC - LIVE PCM");
    publish_state();

    if (s_es7210_device == nullptr) {
        s_state.es7210 = TestResult::kFail;
        s_state.i2s_rx = TestResult::kFail;
        set_detail("MIC UNAVAILABLE");
        publish_state();
        return false;
    }

    if (!discard_mic_startup(requested_mode)) {
        return true;
    }

    int publish_countdown = 0;
    while (true) {
        if (poll_mode_request(requested_mode)) {
            return true;
        }
        if (!capture_mic_frame()) {
            publish_state();
            return false;
        }
        if (++publish_countdown >= kMicPublishInterval) {
            publish_countdown = 0;
            if (s_state.i2s_rx == TestResult::kPass) {
                set_detail("MIC - LIVE PCM");
            }
            publish_state();
        }
    }
}

bool run_mic_auto_stage(int duration_ms, TestMode *requested_mode)
{
    s_state.mode = TestMode::kAuto;
    set_detail("AUTO 1/2 - MIC");
    publish_state();

    if (s_es7210_device == nullptr) {
        s_state.es7210 = TestResult::kFail;
        s_state.i2s_rx = TestResult::kFail;
        publish_state();
        return false;
    }

    if (!discard_mic_startup(requested_mode)) {
        return true;
    }

    const int frame_count = std::max(1, duration_ms / kMicFrameDurationMs);
    for (int i = 0; i < frame_count; ++i) {
        if (poll_mode_request(requested_mode)) {
            return true;
        }
        if (!capture_mic_frame()) {
            publish_state();
            return false;
        }
        if ((i + 1) % kMicPublishInterval == 0) {
            publish_state();
        }
    }
    if (s_state.i2s_rx == TestResult::kPending) {
        s_state.i2s_rx = TestResult::kFail;
    }
    publish_state();
    return false;
}

std::array<int16_t, kToneFrames * BOARD_AUDIO_CHANNELS> make_tone_buffer()
{
    std::array<int16_t, kToneFrames * BOARD_AUDIO_CHANNELS> samples = {};
    for (int frame = 0; frame < kToneFrames; ++frame) {
        const float phase =
            2.0f * kPi * kToneFrequencyHz * frame / BOARD_AUDIO_SAMPLE_RATE;
        const int16_t sample = static_cast<int16_t>(
            std::sin(phase) * kToneAmplitude * 32767.0f);
        samples[frame * 2] = sample;
        samples[frame * 2 + 1] = sample;
    }
    return samples;
}

bool run_dac_stage(
    int duration_ms,
    TestMode visible_mode,
    const char *running_detail,
    TestMode *requested_mode)
{
    s_state.mode = visible_mode;
    set_detail(running_detail);
    publish_state();

    if (s_es8389_device == nullptr) {
        s_state.es8389 = TestResult::kFail;
        s_state.i2s_tx = TestResult::kFail;
        s_state.dac_sound = TestResult::kFail;
        set_detail("DAC UNAVAILABLE");
        publish_state();
        return false;
    }

    const esp_err_t amp_enable_err = set_speaker_enabled(true);
    if (amp_enable_err != ESP_OK) {
        s_state.dac_sound = TestResult::kFail;
        ESP_LOGE(
            kTag, "Unable to enable NS4150B: %s",
            esp_err_to_name(amp_enable_err));
    } else {
        s_state.dac_sound = TestResult::kManual;
        vTaskDelay(pdMS_TO_TICKS(20));
    }

    const auto tone = make_tone_buffer();
    const int write_count = std::max(1, duration_ms / kToneBufferDurationMs);
    for (int i = 0; i < write_count; ++i) {
        if (poll_mode_request(requested_mode)) {
            set_speaker_enabled(false);
            return true;
        }
        if (esp_codec_dev_write(
                s_es8389_device,
                const_cast<int16_t *>(tone.data()),
                tone.size() * sizeof(int16_t)) != ESP_CODEC_DEV_OK) {
            s_state.i2s_tx = TestResult::kFail;
            s_state.dac_sound = TestResult::kFail;
            set_detail("DAC WRITE FAILED");
            ESP_LOGE(kTag, "ES8389 PCM write failed");
            set_speaker_enabled(false);
            publish_state();
            return false;
        }
        s_state.i2s_tx = TestResult::kPass;
    }

    const esp_err_t amp_disable_err = set_speaker_enabled(false);
    if (amp_disable_err != ESP_OK) {
        ESP_LOGE(
            kTag, "Unable to disable NS4150B: %s",
            esp_err_to_name(amp_disable_err));
    }
    if (s_state.dac_sound != TestResult::kFail) {
        s_state.dac_sound = TestResult::kManual;
    }
    set_detail(
        visible_mode == TestMode::kAuto
            ? "AUTO COMPLETE - VERIFY SOUND"
            : "DAC COMPLETE - VERIFY SOUND");
    publish_state();
    return false;
}

bool run_auto_mode(TestMode *requested_mode)
{
    const bool interrupted = run_mic_auto_stage(2000, requested_mode);
    if (interrupted) {
        return true;
    }
    return run_dac_stage(
        2000,
        TestMode::kAuto,
        "AUTO 2/2 - 1 KHZ TONE",
        requested_mode);
}

void audio_worker(void *)
{
    TestMode mode = TestMode::kIdle;
    while (true) {
        if (mode == TestMode::kIdle) {
            xQueueReceive(s_mode_queue, &mode, portMAX_DELAY);
        }

        TestMode requested_mode = TestMode::kIdle;
        bool interrupted = false;
        switch (mode) {
        case TestMode::kMic:
            interrupted = run_mic_mode(&requested_mode);
            break;
        case TestMode::kDac:
            interrupted = run_dac_stage(
                2000,
                TestMode::kDac,
                "DAC - PLAYING 1 KHZ",
                &requested_mode);
            break;
        case TestMode::kAuto:
            interrupted = run_auto_mode(&requested_mode);
            break;
        case TestMode::kIdle:
        default:
            break;
        }
        mode = interrupted ? requested_mode : TestMode::kIdle;
    }
}

}  // namespace

esp_err_t audio_test_start(
    i2c_master_bus_handle_t i2c_bus,
    esp_io_expander_handle_t io_expander,
    AudioStateCallback callback,
    void *callback_context)
{
    ESP_RETURN_ON_FALSE(i2c_bus != nullptr, ESP_ERR_INVALID_ARG, kTag, "null I2C bus");
    ESP_RETURN_ON_FALSE(
        io_expander != nullptr, ESP_ERR_INVALID_ARG, kTag, "null IO expander");

    s_i2c_bus = i2c_bus;
    s_io_expander = io_expander;
    s_state_callback = callback;
    s_state_context = callback_context;
    s_state = {};
    publish_state();

    s_mode_queue = xQueueCreate(1, sizeof(TestMode));
    if (s_mode_queue == nullptr) {
        return ESP_ERR_NO_MEM;
    }

    const esp_err_t speaker_err = init_speaker_control();
    if (speaker_err != ESP_OK) {
        ESP_LOGE(
            kTag, "Speaker control init failed: %s", esp_err_to_name(speaker_err));
    }

    const bool es8389_ack = probe_device(BOARD_ES8389_I2C_ADDR, "ES8389");
    const bool es7210_ack = probe_device(BOARD_ES7210_I2C_ADDR, "ES7210");
    s_state.es8389 = es8389_ack ? TestResult::kPass : TestResult::kFail;
    s_state.es7210 = es7210_ack ? TestResult::kPass : TestResult::kFail;
    publish_state();

    const esp_err_t i2s_err = init_i2s();
    if (i2s_err != ESP_OK) {
        s_state.i2s_rx = TestResult::kFail;
        s_state.i2s_tx = TestResult::kFail;
        set_detail("I2S INIT FAILED");
        ESP_LOGE(kTag, "I2S init failed: %s", esp_err_to_name(i2s_err));
    } else {
        if (es7210_ack && !init_es7210()) {
            s_state.es7210 = TestResult::kFail;
        }
        if (es8389_ack && !init_es8389()) {
            s_state.es8389 = TestResult::kFail;
        }
        set_detail("READY - SELECT TEST");
    }
    publish_state();

    if (xTaskCreate(audio_worker, "audio_test", 8192, nullptr, 6, nullptr) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }

    // Start with the non-audible live microphone test. DAC playback remains a
    // deliberate touch action so unexpected tones are not emitted at boot.
    audio_test_request(TestMode::kMic);
    return ESP_OK;
}

void audio_test_request(TestMode mode)
{
    if (s_mode_queue != nullptr && mode != TestMode::kIdle) {
        xQueueOverwrite(s_mode_queue, &mode);
    }
}
