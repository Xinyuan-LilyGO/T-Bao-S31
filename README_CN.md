# T-Bao-S31

[English README](README.md)

T-Bao-S31 是一个面向乐鑫 **ESP32-S31** 的 ESP-IDF 板级工程，包含显示、
摄像头、音频、充电、TF 卡、Wi-Fi、I2C、电机和舵机等硬件验证示例，以及
面向生产的整机工厂测试程序。目标芯片是 **ESP32-S31，不是 ESP32-S3**。

所有示例按 **ESP-IDF 6.1** 维护。低于 IDF 6.1 的版本不受支持，可能无法
正确配置或编译这些示例。

## 板卡概览

| 项目 | 设计信息 |
| --- | --- |
| 主控 | 乐鑫 ESP32-S31；参考 [ESP32-S31 产品信息](https://www.espressif.com/en/products/socs/esp32-s31)，并使用 IDF 6.1 对应目标 |
| Flash | 板级配置为 16 MB；不同示例可以使用不同的分区布局 |
| 显示 | 1.54 英寸 ST7796S，320(H) × 320(V)，BGR，8 位 Intel 8080（I80）接口 |
| 触摸 | FT6336/FT6336U，原生 240 × 240 坐标映射到 320 × 320 屏幕 |
| 摄像头 | 当前默认 OV2640；摄像头示例支持 OV3660，但安装前必须选择对应电源配置 |
| 音频 | ES8389 播放编解码器、ES7210 麦克风 ADC、NS4150B 功放 |
| 电源和充电 | SGM38121 摄像头电源管理、SY8113 3.3 V/5 V 稳压、SGM41529 双节电池充电器 |
| 扩展 IO | I2C 地址 `0x22` 的 XL9555，负责电源、触摸复位、充电、功放、SD 和电机控制 |
| 运动部件 | DRV8833 双路电机驱动器，以及连接到电机/舵机接口的 ES9051 舵机 |
| 存储 | ESP32-S31 原生 SDMMC 4-bit 接口连接 TF 卡 |
| 无线 | Wi-Fi 扫描、连接和局域网 HTTP 控制示例 |

ESP32-S31 芯片规格包括最高 320 MHz 的双核 32 位 RISC-V、512 KB SRAM、
2.4 GHz Wi-Fi 6、Bluetooth 和 IEEE 802.15.4。这里列出的是芯片能力；本仓库
当前演示的是 Wi-Fi，并未覆盖所有无线协议。摄像头示例关闭 PSRAM，使用芯片
JPEG 硬件解码器，不能根据芯片规格推断当前板卡实际安装的 PSRAM 容量。

板卡包含 3.3 V、1.8 V 和摄像头专用电压域。连接外部信号前应先检查原理图。
可控 5 V 电源为舵机和功放供电，GPIO5 只承载舵机控制信号。

## 硬件连接

### I2C 总线和地址

下表地址均为 **7 位 I2C 地址**，与 ESP-IDF I2C 主机驱动和 `i2c_tools`
示例的表示方式一致。

| 总线 | 引脚 | 示例使用的速率 | 设备 | 地址 | 功能 |
| --- | --- | ---: | --- | ---: | --- |
| 主 I2C0 | SDA `GPIO0`、SCL `GPIO1` | 电源和诊断使用 100 kHz；显示/音频示例可能使用 400 kHz | ES8389 | `0x10` | 音频播放编解码器 |
| 主 I2C0 | SDA `GPIO0`、SCL `GPIO1` | 同一总线 | XL9555 | `0x22` | 电源、复位、充电、功放、SD 和电机控制 |
| 主 I2C0 | SDA `GPIO0`、SCL `GPIO1` | 100 kHz | SGM38121 | `0x28` | 摄像头 DVDD、DOVDD、AVDD 电源 |
| 主 I2C0 | SDA `GPIO0`、SCL `GPIO1` | 同一总线 | FT6336/FT6336U | `0x38` | 触摸控制器 |
| 主 I2C0 | SDA `GPIO0`、SCL `GPIO1` | 100 kHz | ES7210 | `0x40` | 麦克风 ADC |
| 主 I2C0 | SDA `GPIO0`、SCL `GPIO1` | 根据传感器配置 | MPU-6050 模块（如安装） | `0x68` | 三轴加速度计和三轴陀螺仪；当前没有专用示例 |
| 主 I2C0 | SDA `GPIO0`、SCL `GPIO1` | 100 kHz | SGM41529 | `0x6B` | 双节电池充电和监测 |
| 摄像头 I2C1 | SDA `GPIO3`、SCL `GPIO4` | 100 kHz | OV2640 | `0x30` | 摄像头识别和配置 |
| 摄像头 I2C1 | SDA `GPIO3`、SCL `GPIO4` | 100 kHz | OV3660 | `0x3C` | 摄像头识别和配置 |

原理图还标注了地址为 `0x15` 的 AW9364 背光控制器。当前显示示例按板级
配置使用 `GPIO16` 控制背光，因此直接访问该地址前应先确认实际硬件。通用
传感器接口在原理图上标注为 `0x68/0x28`；这不表示 MPU-6050 支持 `0x28`，
使用 `0x28` 的替代传感器会与主总线上的 SGM38121 冲突。

摄像头连接器标注的 `0x78` 是 OV3660 的 8 位写地址；软件使用 7 位地址，
OV2640 为 `0x30`，OV3660 为 `0x3C`。

### ESP32-S31 信号分配

| 功能 | GPIO 或控制信号 | 说明 |
| --- | --- | --- |
| 主 I2C | `GPIO0=SDA`、`GPIO1=SCL` | 电源板和大多数外设共用 |
| 摄像头 I2C | `GPIO3=CAM_SDA`、`GPIO4=CAM_SCL` | 独立 I2C1 总线 |
| 舵机 PWM | `GPIO5` | 50 Hz，建议 1100–1900 µs，中位 1500 µs |
| 音频 I2S | `GPIO6=MCLK`、`GPIO7=DSDIN`、`GPIO8=BCLK`、`GPIO9=LRCK`、`GPIO10=SDOUT` | ES8389/ES7210 共用时钟和数据连接 |
| DRV8833 电机 A | `GPIO15=AIN1`、`GPIO14=AIN2` | 正值使用 AIN1，负值使用 AIN2 |
| DRV8833 电机 B | `GPIO13=BIN1`、`GPIO12=BIN2` | 正值使用 BIN1，负值使用 BIN2 |
| LCD I80 | `D0..D7=GPIO44,43,42,40,39,38,37,36`；`RS=18`、`WR=17`、`CS=19`、`RST=35` | 320 × 320 ST7796S 屏幕 |
| LCD 背光 | `GPIO16` | PWM/背光信号；电源部分取决于屏幕板硬件 |
| 摄像头并行数据 | `D0..D7=GPIO46..53`、`PCLK=54`、`XCLK=55`、`VSYNC=56`、`HREF=57`、`RESET=45` | `esp_cam_io_parl` 并行摄像头接口 |
| TF SDMMC | `D0..D3=GPIO20..23`、`CLK=GPIO24`、`CMD=GPIO25` | 原生 4-bit SDMMC；XL9555 P10 控制卡电源 |
| USB | 通过板上的 USB 网络连接到 `GPIO33/34` | 不要把芯片封装物理管脚号直接当作板级 GPIO 号 |

### XL9555 引脚

XL9555 端口位是连续编号：P00–P07 对应 bit 0–7，P10–P17 对应 bit 8–15。
因此 **P11 是 bit 9（`0x0200`）**，不是 bit 11。

| XL9555 | 板级功能 | 方向 / 有效行为 |
| --- | --- | --- |
| P00 | FT6336 触摸中断 | 输入 |
| P01 | FT6336 触摸复位 | 输出 |
| P02 | SGM41529 充电 `nCE` | 输出；电池检查通过前必须保持禁用 |
| P03 | 充电中断 | 输入 |
| P04 | 充电 power-good | 输入 |
| P05 | 5 V 电源 / `PWR_EN` | 输出；打开舵机、功放及其他 5 V 负载 |
| P06 | 可选传感器中断 | 安装对应板卡器件时为输入 |
| P07 | NS4150B 功放使能 | 输出 |
| P10 | TF/SD 电源使能 | 输出 |
| P11 | DRV8833 `DRV_EN` | 输出；置高后电机桥才允许运行 |

完整的物理管脚和连接器映射见
[`docs/pinmap_cn.md`](docs/pinmap_cn.md)，其中包括 MAIN/PWR 排线、摄像头连接器、
音频、SD 卡座、USB 网络以及原理图版本说明。

## 硬件资料和器件参考

仓库的 [`hardware/`](hardware/) 目录包含板卡原理图和部分芯片数据手册。
对于没有随仓库提供精确数据手册的器件，下表提供厂商页面、兼容参考或驱动
组件链接。

| 器件或模块 | 功能 | 资料 |
| --- | --- | --- |
| ESP32-S31 | 主控芯片 | [Espressif ESP32-S31](https://www.espressif.com/en/products/socs/esp32-s31) |
| ST7796S | LCD 控制器 | [ST7796S 数据手册](https://www.displayfuture.com/Display/datasheet/controller/ST7796s.pdf) · [ESP-IDF 组件](https://components.espressif.com/components/espressif/esp_lcd_st7796) |
| FT6336/FT6336U | 电容触摸控制器 | [FocalTech 产品网站](https://www.focaltech-electronics.com/) · [驱动组件](https://components.espressif.com/components/lambage/esp_lcd_touch_ft6336u) |
| XL9555 | 16 位 I2C IO 扩展器 | [驱动组件](https://components.espressif.com/components/sheldonix/esp_io_expander_xl9555) · [电源板原理图](<hardware/T-Bao-S31_PWR V0.1 26-09-04.PDF>)；仓库未附精确芯片数据手册 |
| SGM38121 | 摄像头电源管理 | [SGM38121 本地数据手册](<hardware/SGM38121.pdf>) |
| OV2640 / OV3660 | 摄像头传感器 | [OmniVision 产品页面](https://www.ovt.com/) · [摄像头驱动组件](https://components.espressif.com/components/haqqscripter/esp_cam_io_parl) |
| AW9364 | LCD 背光控制器，原理图地址 `0x15` | [主板原理图](<hardware/T-Bao-S31_MAIN V0.1 26-09-04.PDF>) |
| ES8389 | 音频编解码器 / DAC | [ES8389 本地数据手册](<hardware/ES8389.pdf>) · [ESP codec 组件](https://components.espressif.com/components/espressif/esp_codec_dev) |
| ES7210 | 麦克风 ADC | [ES7210 本地数据手册](<hardware/ES7210.PDF>) · [ESP codec 组件](https://components.espressif.com/components/espressif/esp_codec_dev) |
| NS4150B | 扬声器功放 | [电源板音频原理图](<hardware/T-Bao-S31_PWR V0.1 26-09-04.PDF>) |
| SY8113 / SY6280 | 3.3 V、5 V 和 SD 电源轨 | [电源板原理图](<hardware/T-Bao-S31_PWR V0.1 26-09-04.PDF>) |
| SGM41529 | 双节电池充电器；必须使用独立保护的电池组 | [SGM41529 本地数据手册](<hardware/SGMICRO-SGM41529.pdf>) |
| DRV8833 | 双路 H 桥电机驱动器 | [DRV8833 本地数据手册](<hardware/DRV8833.pdf>) |
| ES9051 舵机 | 外接舵机模块 | [电机/舵机板原理图](<hardware/T-Bao-S31_Motor .PDF>) |
| MPU-6050 或板载传感器 | 可选运动传感器 | [MPU-6050 本地数据手册](<hardware/MPU-6050.pdf>) |
| ST7796S/FT6336 板和电源板 | 板级连接 | [主板原理图](<hardware/T-Bao-S31_MAIN V0.1 26-09-04.PDF>) · [电源板原理图](<hardware/T-Bao-S31_PWR V0.1 26-09-04.PDF>) |

更换摄像头、触摸屏、传感器或电源器件前，应根据实际板卡版本确认器件型号、
地址绑带、连接器方向、电源域和电压要求。

## 项目文件结构

```text
.
├── .github/workflows/
│   └── build-examples.yml       # 使用 ESP-IDF 6.1 编译所有示例
├── examples/
│   ├── audio_test/              # ES8389 播放和 ES7210 麦克风测试
│   ├── camera_display/          # OV2640/OV3660 采集并显示到 LCD
│   ├── charger_test/            # SGM41529 双节充电监测
│   ├── display_touch_test/      # ST7796S 彩条和 FT6336 触摸测试
│   ├── factory_test/             # 生产测试流程
│   ├── i2c_tools/                # 交互式 I2C 串口工具
│   ├── motor_servo_web_control/ # 浏览器控制电机和舵机
│   ├── sdcard_test/              # SDMMC/FAT 读写验证
│   └── wifi_scan_display/        # Wi-Fi 扫描和显示界面
├── hardware/                    # 原理图和芯片数据手册
├── docs/                        # 工程文档资源
├── other/                       # 设计记录和测试资料
├── README.md
└── README_CN.md
```

每个示例都是独立的 ESP-IDF 工程，应用代码和板级配置位于各自的 `main/`
目录中。`sdkconfig` 和 `sdkconfig.defaults` 选择 ESP32-S31 目标以及内存、
分区等配置。`main/idf_component.yml` 声明组件版本约束；存在的
`dependencies.lock` 文件记录解析后的版本。很多示例会忽略生成的 `sdkconfig`、
`managed_components/` 和构建目录；仓库也忽略本地工程规则 `AGENTS.md`。
为保持可复现性，应保留各示例已经提交的 defaults 和 override，不要在未确认
IDF 6.1 兼容性的情况下直接复制其他示例的组件。

当前示例主要请求的组件版本如下：

| 组件 | 版本约束 | 用途 |
| --- | --- | --- |
| `espressif/esp_lcd_st7796` | `^1.4.0` | `examples/display_touch_test/components/esp_lcd_st7796` 中的共享兼容版本 |
| `lambage/esp_lcd_touch_ft6336u` | `^1.0.8` | 触摸输入 |
| `sheldonix/esp_io_expander_xl9555` | `^0.8.0` | IO 扩展控制 |
| `espressif/esp_codec_dev` | `^1.6.2` | ES8389 和 ES7210 音频驱动 |
| `haqqscripter/esp_cam_io_parl` | `^0.1.0` | 并行摄像头；factory_test 使用本地 override |
| `lvgl/lvgl` | `^9.6.0~1` | 使用 LVGL 的示例界面 |

独立的 `espressif/es7210` 1.0.1~1 声明支持的 IDF `<6.0`；音频示例改用
`esp_codec_dev` 中提供的 ES7210 驱动。编译时应保留完整仓库，因为多个示例
通过相对路径引用共享的 LCD 组件。

## 示例功能

| 示例 | 主要功能 | 所需硬件 |
| --- | --- | --- |
| `audio_test` | 生成 1 kHz ES8389 音调，采集 ES7210 双声道麦克风，并显示 RMS/峰值 | LCD、XL9555、ES8389、ES7210、功放和麦克风 |
| `camera_display` | 配置和识别 OV2640/OV3660，采集 JPEG，使用 ESP32-S31 JPEG 硬件解码并显示 | LCD、摄像头模块、SGM38121 电源 |
| `charger_test` | 校验 SGM41529 器件和配置，监测受保护双节电池充电过程 | 受保护 2S 电池、USB/VBUS、电源和充电板 |
| `display_touch_test` | 显示 LCD 彩条，并将 FT6336 坐标映射到 320 × 320 屏幕 | LCD、触摸屏、XL9555 |
| `factory_test` | 按顺序执行生产测试，提供安全清理、人工确认、稳定错误码和机器 JSON 结果 | 完整测试治具；接电前必须阅读该示例 README |
| `i2c_tools` | 通过串口提供 `i2cconfig`、`i2cdetect`、`i2cget`、`i2cset`、`i2cdump` | 任意可访问的 I2C 器件 |
| `motor_servo_web_control` | 提供局域网页面控制 DRV8833 电机 A/B 和 ES9051 舵机 | 电机、DRV8833、舵机、XL9555、Wi-Fi |
| `sdcard_test` | 打开 TF 电源，不格式化文件系统，写读确定性文件，重新挂载并再次校验 | 已格式化的 FAT TF 卡 |
| `wifi_scan_display` | 扫描网络，显示 RSSI/信道/安全状态，可连接指定 SSID 并显示 DHCP 地址 | LCD 和局域网 Wi-Fi |

每个示例目录中的 README 还包含专用引脚、串口输出、凭据和硬件限制。

## 编译、烧录和监视

### 1. 安装工具链

按照[官方安装指南](https://docs.espressif.com/projects/esp-idf/en/latest/esp32s31/get-started/index.html)安装 ESP-IDF **6.1**。
Windows 使用 ESP-IDF 6.1 终端；Linux/macOS 安装 IDF 工具后 source IDF
目录中的 `export.sh`。先克隆本仓库：

```sh
git clone https://github.com/Xinyuan-LilyGO/T-Bao-S31.git
cd T-Bao-S31
```

下面命令使用 PowerShell，然后确认版本和目标名称：

```powershell
idf.py --version
idf.py --preview --list-targets | Select-String esp32s31
```

工程目标是 `esp32s31`；选择 `esp32s3` 会编译到不同芯片，不受支持。示例
使用 ESP Component Registry，首次配置/编译可能需要联网下载托管组件。

### 2. 编译示例

进入需要编译的示例目录，并使用独立构建目录，避免不同示例的输出互相污染：

```powershell
cd examples\display_touch_test
idf.py --preview --build-dir build_s31 set-target esp32s31
idf.py --preview --build-dir build_s31 build
```

将 `display_touch_test` 换成 `examples/` 下的其他目录即可。全新工程只需
执行一次 `set-target`；它会重新生成配置，因此应先保存有意修改过的
`menuconfig`。CI 工作流使用
`espressif/idf:release-v6.1` 容器对所有工程执行同样的编译。

### 3. 烧录和串口监视

通过 USB/串口连接板卡，把 `COMx` 换成实际端口：

```powershell
idf.py --preview --build-dir build_s31 -p COMx flash monitor
```

Linux/macOS 使用正斜杠、`cp` 代替 `Copy-Item`，并将端口替换为
`/dev/ttyACM0` 或 `/dev/cu.usbmodem...` 等实际设备；其余 `idf.py` 命令相同。
请使用板卡上可传输数据的主 USB-C 接口；随附原理图中的磁吸 USB 接口只供电。

串口监视器使用 `Ctrl-]` 退出。打开电机、舵机、充电器或摄像头电源前，
必须使用合适的电源，并阅读对应示例的安全说明。

### 4. 本地配置文件

在仓库根目录只创建当前示例需要的文件，填写配置后再重新编译。凭据和测试站
配置已通过 Git 忽略：

```powershell
Copy-Item examples\wifi_scan_display\main\wifi_credentials.h.example `
  examples\wifi_scan_display\main\wifi_credentials.h
Copy-Item examples\factory_test\main\factory_config.h.example `
  examples\factory_test\main\factory_config.h
Copy-Item examples\motor_servo_web_control\main\wifi_credentials.h.example `
  examples\motor_servo_web_control\main\wifi_credentials.h
```

只在本地填写测试凭据，不要提交到仓库，也不要在串口日志中输出密码。

### 5. 烧录后的检查

| 示例 | 烧录后的操作 |
| --- | --- |
| [显示/触摸](examples/display_touch_test/README.md) | 检查彩条，沿屏幕四边滑动，确认触摸标记和串口坐标。 |
| [音频](examples/audio_test/README.md) | 使用 `MIC`、`DAC` 或 `AUTO`；对两个麦克风说话，并确认扬声器确实有声音。 |
| [摄像头](examples/camera_display/README.md) | 安装摄像头前在 `main/board_config.h` 选择匹配的电源配置。观察图像和串口诊断；不支持的型号或配置不匹配会报告身份信息。 |
| [充电器](examples/charger_test/README.md) | 上电前确认电池；默认配置在检查通过后可能自动开始充电。观察电压、电流、温度和故障状态。 |
| [工厂测试](examples/factory_test/README.md) | 配置测试站参数，选择 `CAMERA OV2640` 或 `CAMERA OV3660`，再执行 `RUN`（也可使用触摸界面），按提示完成人工确认并保存 `FACTORY_RESULT` JSON。 |
| [I2C 工具](examples/i2c_tools/README.md) | 先配置一条总线，再使用下面的命令扫描。摄像头需要先完成电源、时钟和复位时序才会响应。 |
| [电机/舵机](examples/motor_servo_web_control/README.md) | 在同一网络打开 LCD 显示的 HTTP 地址。电机先以 30–50% 空载测试，舵机先置于 1500 µs。下载前点击 `STOP ALL`。 |
| [SD 卡](examples/sdcard_test/README.md) | 使用专用 FAT 卡，观察 `SD CARD TEST: PASS`；`/sdcard/tbao_s31_sd_test.bin` 会被覆盖，程序不会自动格式化。 |
| [Wi-Fi](examples/wifi_scan_display/README.md) | 检查扫描列表和 DHCP 地址。没有本地凭据时只扫描，不会连接网络。 |

```text
i2cconfig --scl=1 --sda=0 --freq=100000
i2cdetect
i2cconfig --scl=4 --sda=3 --freq=100000
i2cdetect
```

建议先运行显示/触摸和 I2C 诊断，再执行完整工厂测试。遇到编译错误时，
记录 IDF 版本、示例名称和第一条编译错误；遇到硬件故障时，还应记录板卡版本、
摄像头配置、电源和串口日志。I2C 寄存器读回正常不等于电源电压已经测量合格。

## CI 编译检查

[`build-examples.yml`](.github/workflows/build-examples.yml) 会自动发现
`examples/` 下所有直接包含 `CMakeLists.txt` 的示例，然后在 ESP-IDF 6.1
的 `esp32s31` 容器矩阵中逐个编译。修改 `examples/`、修改该工作流或手动
触发时都会运行。任一示例失败都会使工作流失败；`fail-fast: false` 会让
其他示例继续完成，从一次运行中收集全部编译错误。

## 上电、调试和安全注意事项

- **使用正确芯片和 IDF。** ESP32-S31 与 ESP32-S3 是不同目标。保持 IDF 6.1，
  不要通过降低工程版本来绕过 IDF 6.1 的组件或编译问题。
- **先打开摄像头电源。** 通过 I2C0 的 `0x28` 配置 SGM38121 并打开需要的
  电源，再通过 I2C1 探测摄像头。使用 OV2640 或 OV3660 对应配置，不要在已
  连接摄像头时通过反复改变电压来猜测核心电压。
- **保护充电器和电池。** `charger_test` 面向受保护的 2S 电池和 8.40 V
  充电配置，不是单节电池充电示例。使用限流电源，电池或充电器发热时立即
  断电。
- **保持执行器安全。** DRV8833 输入为 GPIO12–15，`DRV_EN` 是 XL9555 P11
  （`0x0200`）。初始化时四个输入必须为低电平；交给 LEDC 前释放继承的
  `gpio_hold`，空闲、停止、复位和下载前重新将其锁低。只有准备好非零电机
  命令后才打开 P11。舵机 PWM 为 GPIO5、50 Hz；先打开 XL9555 P05 并等待
  5 V 稳定，再输出脉冲。烧录电机程序前必须点击 `STOP ALL` 并确认运动已经
  停止。Bootloader 模式不会运行应用程序；空白板、断电后或无法安全停止时，
  应在烧录前断开电机电源。若要让保护独立于上一次固件，电机输入和使能线上
  还需要硬件下拉。
- **谨慎使用 I2C 工具。** 默认诊断命令为主总线
  `i2cconfig --scl=1 --sda=0 --freq=100000`，摄像头总线
  `i2cconfig --scl=4 --sda=3 --freq=100000`，然后执行 `i2cdetect`。未知器件
  不要直接写寄存器。
- **协调共享资源。** 触摸、音频、充电、电源和传感器共用 I2C0。组合示例时
  应共享总线/扩展器句柄，并协调 GPIO、LEDC 定时器和 I2S 资源。P05 同时为
  舵机和功放供电，关闭它会影响两个负载。
- **网页控制只用于本地。** 电机网页服务器没有身份验证和 TLS。两秒心跳超时或
  Wi-Fi 断开会停止输出，但不能替代物理断开电机电源的手段。
- **替换模块必须重新验证。** 摄像头地址、传感器 ID、触摸有效电平、上下拉、
  电压域和连接器方向都应与实际模块和板卡版本核对。
- **首次测试应空载。** 第一次上电和执行器测试应使用空载电机、活动范围充足
  的舵机、限流电源，并将控制板与外部驱动器共地。

软件编译通过或自动测试通过不等于完成电气、热、充电、电机或量产安全认证；
量产前仍需进行完整的硬件验证和安全评审。
