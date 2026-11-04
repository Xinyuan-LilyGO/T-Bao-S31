# T-Bao S31

基于 **T-Bao S31 / ESP32-S31** 的 AI 萌宠固件。项目将小智语音对话、圆形触摸屏表情、摄像头视觉、音乐播放和运动控制整合到一套 ESP-IDF 应用中。

> 当前状态：T-Bao S31 板级配置、摄像头/音频/显示驱动和应用功能已接入工程，代码可用于构建固件。

[English README](README.md)

## 功能

- **小智语音对话**：离线唤醒词（“你好小智”）+ WebSocket 云端 ASR、LLM 和 TTS，支持表情状态和实时字幕。
- **触摸 UI**：LVGL 9 + `esp_emote_expression`，提供表情主页、启动器、对话、摄像头、音乐、视频、游戏、番茄钟、动作、设置和关于页面。
- **摄像头与视觉**：OV2640/OV3660 兼容的 PARLIO JPEG 摄像头；支持屏幕预览、HTTP MJPEG `/stream`、JPEG 抓拍 `/snapshot`、录像、轻量人脸检测/人脸跟随和番茄钟守护模式。
- **音乐与视频**：microSD 本地音频/视频播放；在线音乐搜索会从多个公共 API 解析可播放地址并交给播放器。
- **运动控制**：双电机和舵机控制，支持触摸、Web、语音/MCP 等控制源，并带有速度限制、序列动作和故障保护。
- **联网与控制**：Wi-Fi 配网、音量/亮度控制、设备设置、MCP 工具调用和 HTTP 预览服务。

## 硬件与板级配置

板型名称为 `tbao_s31_board`，配置位于 [`boards/tbao_s31_board`](boards/tbao_s31_board)。

| 模块 | 当前配置 |
| --- | --- |
| 主控 | ESP32-S31，目标名 `esp32s31`，16 MB Flash + PSRAM |
| LCD | ST7796S，约 1.54 英寸，320×320，8-bit PARLIO/I80，总线 GPIO36/37/38/39/40/42/43/44 |
| 触摸 | FT6336/FT6336U，I2C 地址 `0x38`，触摸复位/中断由 XL9555 提供 |
| 音频 | ES8389 播放 + ES7210 双麦采集，I2S GPIO6/7/8/9/10 |
| 摄像头 | OV2640/OV3660，PARLIO JPEG；SCCB GPIO3/4，RESET GPIO45，XCLK GPIO55，数据 GPIO46..53，PCLK GPIO54，HREF GPIO57 |
| 存储 | microSD，SDMMC 4-bit：D0..D3 = GPIO20..23，CLK = GPIO24，CMD = GPIO25，挂载点 `/sdcard` |
| 扩展与执行器 | XL9555 GPIO 扩展、双电机驱动、舵机输出、LCD 背光 PWM |

更完整的引脚、电源和连接器说明见 [`pinmap_cn.md`](pinmap_cn.md)。

## 环境要求

- ESP-IDF **6.1 或更高版本**，目标芯片为 `esp32s31`。
- Python、CMake、Ninja 和 `riscv32-esp-elf` 工具链由 ESP-IDF 安装器提供。
- Windows 用户可在 ESP-IDF PowerShell 环境中执行下面的命令；Linux/macOS 只需将 PowerShell 脚本步骤替换为对应 shell 命令。

检查环境：

```bash
idf.py --version
idf.py --list-targets | findstr esp32s31
```

## 构建与烧录

在仓库根目录执行：

```bash
# 首次配置目标芯片
idf.py set-target esp32s31

# 可选：重新解析或拉取依赖
idf.py reconfigure

# 编译
idf.py build

# 烧录（如需指定串口，可追加 `-p COM12`）
idf.py flash
```

### 首次配网与大模型配置

固件烧录成功后，根据屏幕提示完成 Wi-Fi 配网。配网完成后，打开 [xiaozhi.me](https://xiaozhi.me/)，进入控制台并在右上角选择“添加设备”，输入屏幕或串口日志中显示的 6 位验证码，即可完成大模型配置。

### 语音控制说明

固件启动后，第一次呼叫“你好小智”时设备会应答，但此时只是进入唤醒状态，暂时无法进行对话。需要第二次呼叫“你好小智”，设备可能不会再次应答，但这时就可以直接说“讲个笑话”等内容开始对话。后续如果出现对话无反应，可以再次呼叫“你好小智”重新唤醒。

### 人脸跟随调参

人脸跟随效果与静摩擦力有关，也会受到小车重量和桌面材质影响。如果跟随过程出现异常，可微调 [`main/face_follow_controller.c`](main/face_follow_controller.c) 第 26 行的默认参数：

```c
#define FACE_FOLLOW_DEFAULT_YAW_DRIVE 0.14f
```

建议围绕 `0.14f` 小幅调整；不确定参数含义时，可以先保留默认值或咨询 AI。

板级 YAML（`boards/tbao_s31_board`）发生变化后，请重新执行：

```bash
idf.py gen-bmgr-config -b tbao_s31_board -c ./boards
idf.py reconfigure
idf.py build
```


## SD 卡目录

应用将 SD 卡挂载到 `/sdcard`：

| 内容 | 位置 |
| --- | --- |
| 音乐 | `/sdcard` 根目录下的可播放音频文件 |
| 视频 | `/sdcard` 根目录下的 MP4/AVI/MJPEG 文件 |

> 注意：下载的 MP4 文件需要先通过 FFmpeg 转码后才能播放。如果本地没有 FFmpeg，可以使用 [FFmpeg 在线工具](https://ffmpeg.gcc.ac.cn/advanced_zh.html)。
>
> 参数 1 填写：`-y`
>
> 参数 2 填写：
>
> ```text
> -map 0:v:0 -vf scale=320:320 -c:v mjpeg -q:v 5 -pix_fmt yuvj420p -an -r 10 -movflags +faststart -f mp4
> ```

## 项目结构

```text
main/                         应用、页面、摄像头、音频、运动和 HTTP 服务
boards/tbao_s31_board/       T-Bao S31 Board Manager 配置
components/                   本地组件（esp-dl、人脸检测、板级适配等）
managed_components/           ESP-IDF Component Manager 依赖
patches/                      对 managed_components 的可重复应用补丁
assets/                       启动画面和 UI 资源
docs/                         设计记录与移植文档
tests/                        Python 合约测试
```

## 许可证

除非文件另有说明，应用代码采用 Apache-2.0。组件、模型和资源包沿用上游项目许可证，详情请查看对应组件目录。
