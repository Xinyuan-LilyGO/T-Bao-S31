# AV Processor 组件（音视频处理）

本组件提供在 ESP 平台上进行音频录制/播放/喂数以及视频捕获/解码/渲染的统一封装，基于 GMF（General Media Framework）、esp-audio-render、esp-video-codec、esp-capture 等中间组件实现。

## 功能概览 🎛️

- **音频 🎧**：
  - 录音（AEC/VAD/AFE 可选）
  - 播放（支持 URL、本地文件，解码由配置决定）
  - 数据喂入（feeder，适合边接收边播放的场景，如 RTC/流媒体）
  - 可选混音器，多路流间音量渐变（fade）与焦点控制
- **视频 🎥**：
  - 多路视频采集
  - 视频解码与渲染回调

---

## 快速开始

### 1. 音频使用

1. 初始化音频管理器：

```c
// 方式一：使用默认配置宏（推荐）
audio_manager_config_t cfg = DEFAULT_AUDIO_MANAGER_CONFIG();
cfg.play_dev = your_play_dev;
cfg.rec_dev = your_rec_dev;
strcpy(cfg.mic_layout, "RMNM"); // 麦克风布局（可选，用于 AFE 配置）
cfg.board_sample_rate = 16000;
cfg.board_bits = 32;
cfg.board_channels = 2;
cfg.play_volume = 80;
cfg.rec_volume = 60;
cfg.rec_ref_volume = 60;
cfg.enable_mixer = true; // 需要 feeder 与 playback 混音时开启

audio_manager_init(&cfg);
```

2. 录音（可轮询读取）：

```c
// 录音事件回调（可选）
void recorder_event_cb(void *event, void *ctx) {
    // 处理录音事件（如 AFE 事件）
}

// 配置录音器（使用默认配置宏）
audio_recorder_config_t recorder_cfg = DEFAULT_AUDIO_RECORDER_CONFIG();
// 可选：配置 AFE（运行时配置，优先级高于 Kconfig）
recorder_cfg.afe_config.vad_enable = true;
recorder_cfg.afe_config.vad_mode = 4;
recorder_cfg.afe_config.agc_enable = true;
recorder_cfg.recorder_event_cb = recorder_event_cb;

// 打开录音器
audio_recorder_open(&recorder_cfg);
uint8_t buf[2048];
audio_recorder_read_data(buf, sizeof(buf));

audio_recorder_close();
```

3. Feed播放（feeder，适合外部码流→本地播放）：

```c
// 配置 feeder（使用默认配置宏）
audio_feeder_config_t feeder_cfg = DEFAULT_AUDIO_FEEDER_CONFIG();
// 可选：使用 OPUS 解码器时，task_stack 建议 >= 4096 * 10
// feeder_cfg.feeder_task_config.task_stack = 4096 * 10;

audio_feeder_open(&feeder_cfg);
audio_feeder_run();
// 按需多次喂入外部数据块
audio_feeder_feed_data(pkt, pkt_len);

audio_feeder_stop();
audio_feeder_close();
```

4. 普通播放（URL 或本地）：

```c
// 配置播放器（使用默认配置宏）
audio_playback_config_t playback_cfg = DEFAULT_AUDIO_PLAYBACK_CONFIG();

audio_playback_open(&playback_cfg);
audio_playback_play("http://<ip>:<port>/audio.mp3");

audio_playback_pause();
audio_playback_resume();
audio_playback_stop();
audio_playback_close();
```

5. 提示音播放 

```c
// 配置播放器（使用默认配置宏）
audio_prompt_config_t prompt_cfg = DEFAULT_AUDIO_PROMPT_CONFIG();

audio_prompt_open(&prompt_cfg);
audio_prompt_play("spiffs://audio.mp3");
audio_prompt_close();
```
> 提示音播放  和 配置播放器是相似的，只是 `提示音播放` 是阻塞式的播放，适合使用短音频的播放。目的是在使用 普通播放 的播放的时候又需要播放
> 提示音，可以有很好响应，并且提示音播放的优先级是最高的

6. 焦点/渐变控制（启用混音器时）：

```c
// 注意：使用混音器前需要先打开播放器和 feeder
audio_processor_mixer_open();
audio_processor_ramp_control(AUDIO_MIXER_FOCUS_FEEDER);     // 侧重喂数音频
audio_processor_ramp_control(AUDIO_MIXER_FOCUS_PLAYBACK);   // 侧重音频播放
audio_processor_mixer_close();
```

### 2. 视频使用

1. 渲染（解码）回调：

```c
void decoded_cb(void *ctx, const uint8_t *data, size_t size) {
    // 处理解码后的帧数据
}

video_render_config_t rcfg = {
    .decode_cfg = your_vdec_cfg,
    .resolution = {.width = 640, .height = 480},
    .decode_cb = decoded_cb,
};
video_render_handle_t r = video_render_open(&rcfg);
video_render_start(r);

video_frame_t f = {.data = enc_frame, .size = enc_size};
video_render_frame_feed(r, &f);

video_render_stop(r);
video_render_close(r);
```

2. 采集（多路）：

```c
void capture_cb(void *ctx, int index, video_frame_t *frame) {
    // 处理采集到的视频帧
}

video_capture_config_t ccfg = {0};
ccfg.camera_config = &your_cam_cfg;
ccfg.sink_num = 2;
ccfg.sink_cfg[0] = your_sink0_cfg;
ccfg.sink_cfg[1] = your_sink1_cfg;
ccfg.capture_frame_cb = capture_cb;

video_capture_handle_t c = video_capture_open(&ccfg);
video_capture_start(c);
video_capture_stop(c);
video_capture_close(c);
```

---

## 媒体数据保存（Media Dump）💾

- 使能条件 ✅：在 `menuconfig` 勾选 `MEDIA_DUMP_ENABLE` 后生效。
- 保存方式 📤：支持 SD 卡与 UDP 两种输出（可二选一）。
  - SD 卡 💾：开启 `CONFIG_MEDIA_DUMP_SINK_SDCARD`，输出到文件 `CONFIG_MEDIA_DUMP_SDCARD_FILENAME`，时长由 `CONFIG_MEDIA_DUMP_DURATION_SEC` 控制。
  - UDP 📡：开启 `CONFIG_MEDIA_DUMP_SINK_UDP`，通过 `CONFIG_MEDIA_DUMP_UDP_IP` 与 `CONFIG_MEDIA_DUMP_UDP_PORT` 发送原始媒体数据（可使用 `script/udp_reciver.py` 脚本）。
- 保存点位 🎚️（AEC 处理前/后）：在 `menuconfig` 的“Audio Save Mode (AEC point)”中选择
  - `MEDIA_DUMP_AUDIO_BEFORE_AEC`（Save Before AEC）：保存 AEC 前的麦克风原始音频（便于分析底噪/回授）
  - `MEDIA_DUMP_AUDIO_AFTER_AEC`（Save After AEC）：保存 AEC 处理后的音频（便于评估 AEC 效果）
- 典型用途 🔍：问题复现时导出原始音/视频数据，离线用 Audacity 等工具检查饱和、回授、格式是否正确。

## 配置说明 ⚙️

### 运行时配置（推荐）

AFE 配置可以通过 `audio_recorder_config_t` 结构体中的 `afe_config` 字段在运行时配置，优先级高于 Kconfig 配置：

```c
audio_recorder_config_t recorder_cfg = DEFAULT_AUDIO_RECORDER_CONFIG();
// 配置 AFE
recorder_cfg.afe_config.ai_mode_wakeup = false;        // AI 模式：true=唤醒模式，false=直连模式
recorder_cfg.afe_config.vad_enable = true;              // 使能 VAD
recorder_cfg.afe_config.vad_mode = 4;                   // VAD 模式（1-4），值越大越敏感
recorder_cfg.afe_config.vad_min_speech_ms = 64;        // 最小语音时长（ms）
recorder_cfg.afe_config.vad_min_noise_ms = 1000;       // 最小噪声时长（ms）
recorder_cfg.afe_config.agc_enable = true;              // 使能 AGC
recorder_cfg.afe_config.enable_vcmd_detect = false;     // 使能 VCMD
recorder_cfg.afe_config.vcmd_timeout_ms = 5000;         // VCMD 超时（ms）
recorder_cfg.afe_config.mn_language = "cn";             // 模型语言："cn" 或 "en"
recorder_cfg.afe_config.wakeup_time_ms = 10000;         // 唤醒时间（ms）
recorder_cfg.afe_config.wakeup_end_time_ms = 2000;      // 唤醒结束时间（ms）

// 打开录音器时传入配置
audio_recorder_open(&recorder_cfg);
```

### Kconfig 配置（备选）

如果 `audio_recorder_config_t.afe_config` 中的字段使用默认值或未设置，则使用 Kconfig 配置。通过 `idf.py menuconfig` 进入配置菜单进行配置。


#### 💾 Media Dump 配置

在 `menuconfig` 的 **"Component config" -> "Audio/Video Processor Configuration" -> "Media Dump"** 下配置：

- **`MEDIA_DUMP_ENABLE`**：使能媒体数据保存功能，默认关闭
  - 启用后可以保存原始音频/视频数据用于调试分析

- **`MEDIA_DUMP_AUDIO_POINT`**：音频保存点位选择（choice 类型）
  - **`MEDIA_DUMP_AUDIO_BEFORE_AEC`**：保存 AEC 处理前的原始音频
    - 便于分析底噪、回授等问题
  - **`MEDIA_DUMP_AUDIO_AFTER_AEC`**：保存 AEC 处理后的音频
    - 便于评估 AEC 处理效果
  - **`MEDIA_DUMP_AUDIO_NONE`**：不保存音频（默认）

- **`MEDIA_DUMP_DURATION_SEC`**：保存时长（秒），默认 20
  - 类型：整数（int）
  - 范围：1-3600 秒

- **`MEDIA_DUMP_SINK`**：保存方式选择（choice 类型）
  - **`MEDIA_DUMP_SINK_SDCARD`**：保存到 SD 卡文件（默认）
    - 文件路径配置项：`CONFIG_MEDIA_DUMP_SDCARD_FILENAME`（默认：`/sdcard/media_dump.bin`）
    - 需要确保 SD 卡已正确挂载
  - **`MEDIA_DUMP_SINK_UDP`**：通过 UDP 发送
    - 目标 IP 配置项：`CONFIG_MEDIA_DUMP_UDP_IP`（默认：`192.168.1.100`）
    - 目标端口配置项：`CONFIG_MEDIA_DUMP_UDP_PORT`（默认：5000）
    - 可使用 `script/udp_reciver.py` 脚本接收数据

### 任务配置说明

音频处理组件内部使用多个 FreeRTOS 任务，可以通过各功能模块的配置结构体中的任务配置字段进行自定义：

#### 各功能模块任务配置

- **录音器**（`audio_recorder_config_t`）：
  - `afe_feed_task_config`：AFE feed 任务配置（默认栈大小 3KB）
  - `afe_fetch_task_config`：AFE fetch 任务配置（默认栈大小 3KB）
  - `recorder_task_config`：录音任务配置（默认栈大小 5KB，使用 OPUS 编码器时建议 >= 40KB）

- **播放器**（`audio_playback_config_t`）：
  - `playback_task_config`：播放任务配置（默认栈大小 4KB）

- **Feeder**（`audio_feeder_config_t`）：
  - `feeder_task_config`：Feeder 任务配置（默认栈大小 5KB，使用 OPUS 解码器时建议 >= 40KB）

**重要提示**：
- 当使用 OPUS 编码器或解码器时，`recorder_task_config` 和 `feeder_task_config` 的 `task_stack` 需要设置为至少 `4096 * 10` 字节（40KB）。
- 任务配置中的 `task_stack` 设置为 0 时，将使用默认值。
- `task_stack_in_ext` 设置为 `true` 时，任务栈将分配在外部内存中，有助于节省内部 RAM。
- 任务配置应在各自模块的配置结构体中设置（如 `audio_recorder_config_t`、`audio_playback_config_t`、`audio_feeder_config_t`），而不是在 `audio_manager_config_t` 中。

---

## API 参考 📚

头文件：

- `include/audio_processor.h`
- `include/video_processor.h`
- `include/av_processor_type.h`

常用函数（节选）：

- 音频管理：
  - `audio_manager_init`/`audio_manager_deinit`：初始化/反初始化音频管理器
- 录音：
  - `audio_recorder_open`：打开录音器（支持编码器配置和事件回调）
  - `audio_recorder_read_data`：读取录音数据
  - `audio_recorder_get_afe_manager_handle`：获取 AFE manager 句柄
  - `audio_recorder_close`：关闭录音器
- 喂数：
  - `audio_feeder_open`：打开 feeder（支持解码器配置）
  - `audio_feeder_run`：启动 feeder
  - `audio_feeder_feed_data`：喂入音频数据
  - `audio_feeder_stop`：停止 feeder
  - `audio_feeder_close`：关闭 feeder
- 播放：
  - `audio_playback_open`/`audio_playback_close`：打开/关闭播放器
  - `audio_playback_play`：播放音频（支持 URL 或本地文件，如 `http://...` 或 `file:///sdcard/...`）
  - `audio_playback_stop`：停止播放
  - `audio_playback_pause`/`audio_playback_resume`：暂停/恢复播放
  - `audio_playback_get_state`：获取播放状态
- 混音器：
  - `audio_processor_mixer_open`：打开混音器（必须在 playback 和 feeder 都打开后调用）
  - `audio_processor_mixer_close`：关闭混音器
  - `audio_processor_ramp_control`：控制音频焦点和音量渐变
- 视频渲染：
  - `video_render_open`/`video_render_close`：打开/关闭视频渲染器
  - `video_render_start`/`video_render_stop`：启动/停止渲染
  - `video_render_frame_feed`：喂入视频帧
- 视频采集：
  - `video_capture_open`/`video_capture_close`：打开/关闭视频采集器
  - `video_capture_start`/`video_capture_stop`：启动/停止采集

---

## 常见问题 ❓

### 音频问题

- **出现"自问自答"现象（设备播放的声音被麦克风重复收录）**：
  - 在 `menuconfig` 使能 `MEDIA_DUMP_ENABLE`，复现问题并导出保存的音频数据以便进一步分析
  - 使用 [Audacity](https://www.audacityteam.org/download/) 打开导出的音频文件，观察波形/频谱判断是否存在饱和或回授
  - 如出现饱和剪切，调低麦克风增益(`esp_codec_dev_set_in_gain`)；或适当减小扬声器音量(`esp_codec_dev_set_out_vol`)，避免过度回授

- **OPUS 编解码器使用时出现栈溢出**：
  - 确保 `recorder_task_config` 和 `feeder_task_config` 的 `task_stack` 设置为至少 `4096 * 10` 字节（40KB）
  - 考虑将任务栈分配在外部内存（设置 `task_stack_in_ext = true`）

- **混音器无法正常工作**：
  - 确保在调用 `audio_processor_mixer_open()` 之前，已经打开了播放器（`audio_playback_open`）和 feeder（`audio_feeder_open`）
  - 确保在 `audio_manager_init` 时设置了 `enable_mixer = true`

### 开发调试

- **修改组件源码（espressif__av_processor）**：

- **媒体数据保存（Media Dump）**：
  - UDP 方式：可使用 `script/media_dump_server.py` 脚本接收数据
  - SD 卡方式：确保 SD 卡已正确挂载，文件路径可访问

---

## 版本信息

当前版本：v0.1.0

## 许可证

本组件遵循 MIT 许可证，详见仓库随附的 `LICENSE`。

## 相关资源

- 问题反馈：https://github.com/espressif/esp-gmf/issues
