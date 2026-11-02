#include "factory_tests.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>

#include "board_config.h"
#include "board_service.h"
#include "driver/i2s_std.h"
#include "esp_check.h"
#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"
#include "esp_log.h"
#include "factory_settings.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace {

constexpr char kTag[] = "factory_audio";
constexpr int kFramesPerBuffer = 960;
constexpr int kToneFrames = 480;
constexpr float kPi = 3.14159265358979323846f;

static_assert(kToneFrames <= kFramesPerBuffer);

static_assert(
    ES8389_CODEC_DEFAULT_ADDR == (BOARD_ES8389_ADDR << 1),
    "ES8389 address mismatch");
static_assert(
    ES7210_CODEC_DEFAULT_ADDR == (BOARD_ES7210_ADDR << 1),
    "ES7210 address mismatch");

struct AudioResources {
    i2s_chan_handle_t tx = nullptr;
    i2s_chan_handle_t rx = nullptr;
    const audio_codec_gpio_if_t *gpio = nullptr;
    const audio_codec_ctrl_if_t *ctrl = nullptr;
    const audio_codec_data_if_t *data = nullptr;
    const audio_codec_if_t *codec = nullptr;
    esp_codec_dev_handle_t device = nullptr;
};

AudioResources s_audio = {};
std::array<int16_t, kFramesPerBuffer * 2> s_pcm_buffer = {};

float amplitude_to_dbfs(long double amplitude)
{
    if (amplitude <= 1.0L / 32768.0L) {
        return -96.0f;
    }
    return std::max(-96.0f, 20.0f * std::log10(static_cast<float>(amplitude)));
}

esp_err_t init_i2s()
{
    i2s_chan_config_t channel =
        I2S_CHANNEL_DEFAULT_CONFIG(BOARD_I2S_PORT, I2S_ROLE_MASTER);
    channel.auto_clear = true;
    channel.dma_desc_num = 6;
    channel.dma_frame_num = 240;
    esp_err_t err = i2s_new_channel(&channel, &s_audio.tx, &s_audio.rx);
    if (err != ESP_OK) {
        return err;
    }
    i2s_std_config_t standard = {};
    standard.clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(BOARD_AUDIO_SAMPLE_RATE);
    standard.clk_cfg.mclk_multiple = BOARD_AUDIO_MCLK_MULTIPLE;
    standard.slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
        I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO);
    standard.gpio_cfg.mclk = BOARD_I2S_MCLK;
    standard.gpio_cfg.bclk = BOARD_I2S_BCLK;
    standard.gpio_cfg.ws = BOARD_I2S_LRCK;
    standard.gpio_cfg.dout = BOARD_I2S_DOUT;
    standard.gpio_cfg.din = BOARD_I2S_DIN;
    err = i2s_channel_init_std_mode(s_audio.rx, &standard);
    if (err == ESP_OK) {
        err = i2s_channel_init_std_mode(s_audio.tx, &standard);
    }
    if (err == ESP_OK) {
        err = i2s_channel_enable(s_audio.rx);
    }
    if (err == ESP_OK) {
        err = i2s_channel_enable(s_audio.tx);
    }
    return err;
}

bool init_microphone_codec()
{
    audio_codec_i2c_cfg_t control = {};
    control.port = BOARD_I2C_PORT;
    control.addr = ES7210_CODEC_DEFAULT_ADDR;
    control.bus_handle = board_service_state().i2c_bus;
    control.clock_speed_hz = 100000;
    s_audio.ctrl = audio_codec_new_i2c_ctrl(&control);

    audio_codec_i2s_cfg_t data = {};
    data.port = BOARD_I2S_PORT;
    data.rx_handle = s_audio.rx;
    s_audio.data = audio_codec_new_i2s_data(&data);

    es7210_codec_cfg_t codec = {};
    codec.ctrl_if = s_audio.ctrl;
    codec.master_mode = false;
    codec.mic_selected = ES7210_SEL_MIC1 | ES7210_SEL_MIC2;
    codec.mclk_src = ES7210_MCLK_FROM_PAD;
    codec.mclk_div = BOARD_AUDIO_MCLK_MULTIPLE;
    s_audio.codec = es7210_codec_new(&codec);
    if (s_audio.ctrl == nullptr || s_audio.data == nullptr || s_audio.codec == nullptr) {
        return false;
    }

    esp_codec_dev_cfg_t device = {};
    device.dev_type = ESP_CODEC_DEV_TYPE_IN;
    device.codec_if = s_audio.codec;
    device.data_if = s_audio.data;
    s_audio.device = esp_codec_dev_new(&device);
    if (s_audio.device == nullptr) {
        return false;
    }
    esp_codec_dev_sample_info_t sample = {};
    sample.bits_per_sample = I2S_DATA_BIT_WIDTH_16BIT;
    sample.channel = BOARD_AUDIO_CHANNELS;
    sample.channel_mask = ES7210_SEL_MIC1 | ES7210_SEL_MIC2;
    sample.sample_rate = BOARD_AUDIO_SAMPLE_RATE;
    sample.mclk_multiple = BOARD_AUDIO_MCLK_MULTIPLE;
    return esp_codec_dev_open(s_audio.device, &sample) == ESP_CODEC_DEV_OK &&
           esp_codec_dev_set_in_gain(s_audio.device, 30.0f) == ESP_CODEC_DEV_OK;
}

bool init_speaker_codec()
{
    audio_codec_i2c_cfg_t control = {};
    control.port = BOARD_I2C_PORT;
    control.addr = ES8389_CODEC_DEFAULT_ADDR;
    control.bus_handle = board_service_state().i2c_bus;
    control.clock_speed_hz = 100000;
    s_audio.ctrl = audio_codec_new_i2c_ctrl(&control);

    audio_codec_i2s_cfg_t data = {};
    data.port = BOARD_I2S_PORT;
    data.tx_handle = s_audio.tx;
    s_audio.data = audio_codec_new_i2s_data(&data);
    s_audio.gpio = audio_codec_new_gpio();

    es8389_codec_cfg_t codec = {};
    codec.ctrl_if = s_audio.ctrl;
    codec.gpio_if = s_audio.gpio;
    codec.codec_mode = ESP_CODEC_DEV_WORK_MODE_DAC;
    codec.pa_pin = GPIO_NUM_NC;
    codec.master_mode = false;
    codec.use_mclk = true;
    codec.hw_gain.pa_voltage = 5.0f;
    codec.hw_gain.codec_dac_voltage = 3.3f;
    codec.mclk_div = BOARD_AUDIO_MCLK_MULTIPLE;
    s_audio.codec = es8389_codec_new(&codec);
    if (s_audio.ctrl == nullptr || s_audio.data == nullptr ||
        s_audio.gpio == nullptr || s_audio.codec == nullptr) {
        return false;
    }

    esp_codec_dev_cfg_t device = {};
    device.dev_type = ESP_CODEC_DEV_TYPE_OUT;
    device.codec_if = s_audio.codec;
    device.data_if = s_audio.data;
    s_audio.device = esp_codec_dev_new(&device);
    if (s_audio.device == nullptr) {
        return false;
    }
    esp_codec_dev_sample_info_t sample = {};
    sample.bits_per_sample = I2S_DATA_BIT_WIDTH_16BIT;
    sample.channel = BOARD_AUDIO_CHANNELS;
    sample.channel_mask = 0x03;
    sample.sample_rate = BOARD_AUDIO_SAMPLE_RATE;
    sample.mclk_multiple = BOARD_AUDIO_MCLK_MULTIPLE;
    return esp_codec_dev_open(s_audio.device, &sample) == ESP_CODEC_DEV_OK &&
           esp_codec_dev_set_out_mute(s_audio.device, false) == ESP_CODEC_DEV_OK &&
           esp_codec_dev_set_out_vol(s_audio.device, 90) == ESP_CODEC_DEV_OK;
}

void audio_cleanup()
{
    board_service_set_output(BOARD_XL_P07_SPK_EN, false);
    vTaskDelay(pdMS_TO_TICKS(10));
    board_service_set_output(BOARD_XL_P05_5V_EN, false);
    if (s_audio.device != nullptr) {
        esp_codec_dev_close(s_audio.device);
        esp_codec_dev_delete(s_audio.device);
    }
    if (s_audio.codec != nullptr) {
        audio_codec_delete_codec_if(s_audio.codec);
    }
    if (s_audio.ctrl != nullptr) {
        audio_codec_delete_ctrl_if(s_audio.ctrl);
    }
    if (s_audio.data != nullptr) {
        audio_codec_delete_data_if(s_audio.data);
    }
    if (s_audio.gpio != nullptr) {
        audio_codec_delete_gpio_if(s_audio.gpio);
    }
    if (s_audio.rx != nullptr) {
        i2s_channel_disable(s_audio.rx);
        i2s_del_channel(s_audio.rx);
    }
    if (s_audio.tx != nullptr) {
        i2s_channel_disable(s_audio.tx);
        i2s_del_channel(s_audio.tx);
    }
    s_audio = {};
}

}  // namespace

esp_err_t microphone_test_run(
    const FactoryTestEnvironment &,
    FactoryTestRecord *record,
    FactoryModuleOutcome *outcome)
{
    if (record == nullptr || outcome == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    *outcome = {};
    audio_cleanup();
    const FactoryBoardState &board = board_service_state();
    if (!board.i2c_ready ||
        i2c_master_probe(board.i2c_bus, BOARD_ES7210_ADDR, 500) != ESP_OK) {
        std::snprintf(record->error_code, sizeof(record->error_code), "I2C_NO_ACK");
        std::snprintf(record->detail, sizeof(record->detail), "ES7210 did not acknowledge at 0x40");
        return ESP_ERR_NOT_FOUND;
    }
    if (init_i2s() != ESP_OK) {
        std::snprintf(record->error_code, sizeof(record->error_code), "I2S_INIT_FAILED");
        std::snprintf(record->detail, sizeof(record->detail), "Unable to initialize I2S RX");
        return ESP_FAIL;
    }
    if (!init_microphone_codec()) {
        std::snprintf(record->error_code, sizeof(record->error_code), "CODEC_INIT_FAILED");
        std::snprintf(record->detail, sizeof(record->detail), "ES7210 initialization failed");
        return ESP_FAIL;
    }

    auto &samples = s_pcm_buffer;
    std::array<long double, 2> square_sum = {};
    std::array<uint64_t, 2> nonzero = {};
    std::array<uint64_t, 2> clipped = {};
    float left_dbfs = -96.0f;
    float right_dbfs = -96.0f;
    constexpr int kBuffers = 250;
    for (int buffer = 0; buffer < kBuffers + 5; ++buffer) {
        if (esp_codec_dev_read(
                s_audio.device, samples.data(), samples.size() * sizeof(int16_t)) !=
            ESP_CODEC_DEV_OK) {
            std::snprintf(record->error_code, sizeof(record->error_code), "PCM_READ_FAILED");
            std::snprintf(record->detail, sizeof(record->detail), "ES7210 PCM read failed");
            return ESP_FAIL;
        }
        if (buffer < 5) {
            continue;
        }
        std::array<long double, 2> frame_squares = {};
        for (size_t i = 0; i < samples.size(); ++i) {
            const int channel = static_cast<int>(i & 1U);
            const int32_t value = samples[i];
            const int32_t magnitude = value < 0 ? -value : value;
            frame_squares[channel] += static_cast<long double>(value) * value;
            square_sum[channel] += static_cast<long double>(value) * value;
            nonzero[channel] += magnitude != 0;
            clipped[channel] += magnitude >= 32760;
        }
        left_dbfs = std::max(
            left_dbfs,
            amplitude_to_dbfs(std::sqrt(frame_squares[0] / kFramesPerBuffer) / 32768.0L));
        right_dbfs = std::max(
            right_dbfs,
            amplitude_to_dbfs(std::sqrt(frame_squares[1] / kFramesPerBuffer) / 32768.0L));
        if ((buffer % 10) == 0) {
            board_ui_show_meter("MICROPHONE", "48 kHz / 16-bit / stereo", left_dbfs, right_dbfs);
        }
    }

    const uint64_t samples_per_channel = static_cast<uint64_t>(kBuffers) * kFramesPerBuffer;
    const bool sustained_clipping =
        clipped[0] > samples_per_channel / 100 ||
        clipped[1] > samples_per_channel / 100;
    outcome->automatic_pass = factory_mic_threshold_pass(
        left_dbfs, right_dbfs, FACTORY_MIC_MIN_DBFS,
        nonzero[0] != 0, nonzero[1] != 0, sustained_clipping);
    std::snprintf(
        record->measurements, sizeof(record->measurements),
        "{\"sample_rate_hz\":48000,\"left_dbfs\":%.1f,\"right_dbfs\":%.1f,"
        "\"left_nonzero\":%llu,\"right_nonzero\":%llu,\"clipped\":%s}",
        left_dbfs, right_dbfs,
        static_cast<unsigned long long>(nonzero[0]),
        static_cast<unsigned long long>(nonzero[1]),
        sustained_clipping ? "true" : "false");
    if (!outcome->automatic_pass) {
        std::snprintf(
            record->error_code, sizeof(record->error_code),
            sustained_clipping ? "CLIPPING" : "LEVEL_LOW");
        std::snprintf(record->detail, sizeof(record->detail),
                      "L %.1f dBFS, R %.1f dBFS", left_dbfs, right_dbfs);
        return ESP_FAIL;
    }
    std::snprintf(record->detail, sizeof(record->detail),
                  "Both microphone channels passed");
    return ESP_OK;
}

void microphone_test_cleanup()
{
    audio_cleanup();
}

esp_err_t speaker_test_run(
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
    audio_cleanup();
    const FactoryBoardState &board = board_service_state();
    if (!board.i2c_ready || !board.expander_ready ||
        i2c_master_probe(board.i2c_bus, BOARD_ES8389_ADDR, 500) != ESP_OK) {
        std::snprintf(record->error_code, sizeof(record->error_code), "I2C_NO_ACK");
        std::snprintf(record->detail, sizeof(record->detail), "ES8389 or XL9555 unavailable");
        return ESP_ERR_NOT_FOUND;
    }
    if (init_i2s() != ESP_OK || !init_speaker_codec()) {
        std::snprintf(record->error_code, sizeof(record->error_code), "CODEC_INIT_FAILED");
        std::snprintf(record->detail, sizeof(record->detail), "ES8389/I2S initialization failed");
        return ESP_FAIL;
    }

    board_service_set_output(BOARD_XL_P07_SPK_EN, false);
    ESP_RETURN_ON_ERROR(
        board_service_set_output(BOARD_XL_P05_5V_EN, true), kTag, "enable 5V");
    vTaskDelay(pdMS_TO_TICKS(30));
    ESP_RETURN_ON_ERROR(
        board_service_set_output(BOARD_XL_P07_SPK_EN, true), kTag, "enable amplifier");
    vTaskDelay(pdMS_TO_TICKS(80));
    uint32_t levels = 0;
    ESP_RETURN_ON_ERROR(
        board_service_get_level(
            BOARD_XL_P05_5V_EN | BOARD_XL_P07_SPK_EN, &levels),
        kTag, "read speaker enables");
    if ((levels & (BOARD_XL_P05_5V_EN | BOARD_XL_P07_SPK_EN)) !=
        (BOARD_XL_P05_5V_EN | BOARD_XL_P07_SPK_EN)) {
        std::snprintf(record->error_code, sizeof(record->error_code), "ENABLE_READBACK");
        std::snprintf(record->detail, sizeof(record->detail), "P05/P07 readback mismatch");
        return ESP_FAIL;
    }

    auto &tone = s_pcm_buffer;
    for (int frame = 0; frame < kToneFrames; ++frame) {
        const int16_t value = static_cast<int16_t>(
            std::sin(2.0f * kPi * 1000.0f * frame / BOARD_AUDIO_SAMPLE_RATE) *
            0.60f * 32767.0f);
        tone[frame * 2] = value;
        tone[frame * 2 + 1] = value;
    }
    board_ui_show_status("SPEAKER", "Playing 1 kHz tone", "Listen for a clean tone");
    int writes = 0;
    for (; writes < 300; ++writes) {
        if (esp_codec_dev_write(
                s_audio.device, tone.data(),
                kToneFrames * 2 * sizeof(int16_t)) !=
            ESP_CODEC_DEV_OK) {
            std::snprintf(record->error_code, sizeof(record->error_code), "PCM_WRITE_FAILED");
            std::snprintf(record->detail, sizeof(record->detail), "ES8389 PCM write failed");
            return ESP_FAIL;
        }
    }
    board_service_set_output(BOARD_XL_P07_SPK_EN, false);
    vTaskDelay(pdMS_TO_TICKS(10));
    board_service_set_output(BOARD_XL_P05_5V_EN, false);
    outcome->automatic_pass = true;
    std::snprintf(record->measurements, sizeof(record->measurements),
                  "{\"frequency_hz\":1000,\"duration_ms\":3000,\"writes\":%d,"
                  "\"enable_readback\":true}", writes);
    std::snprintf(record->detail, sizeof(record->detail), "Confirm the 1 kHz tone was clean");
    return ESP_OK;
}

void speaker_test_cleanup()
{
    audio_cleanup();
}
