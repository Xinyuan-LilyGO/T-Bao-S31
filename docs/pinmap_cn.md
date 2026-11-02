# T-Bao-S31 引脚映射（最新原理图）

> 本文依据当前工作区中的最新原理图整理，根目录 `pinmap_cn.md` 仅作为表格格式参考，不复用其中的旧产品内容。
>
> - 主板：`hardware/T-Bao-S31_MAIN V0.1 26-09-04.PDF`（下文简称 `MAIN`）
> - 副板：`hardware/T-Bao-S31_PWR V0.1 26-09-04.PDF`（下文简称 `PWR`）
> - `MAIN` 与 `PWR` 通过一条 37 针排线连接；两侧原理图连接器按针号一一对应。
> - 文件名中的 `26-09-04` 按原理图版本日期记为 2026-09-04。
> - 文中的 `BOARD_*` 是建议的代码宏名，原理图网络名以反引号保留。
> - ESP32-S31 的“物理管脚”是芯片封装管脚号，不是 GPIO 编号。

## 1. 主控与公共总线

| 功能 | 当前宏/建议名 | GPIO/映射 | 来源 | 备注 |
| --- | --- | --- | --- | --- |
| 主 I2C SDA | `BOARD_I2C_SDA` | GPIO0，物理管脚 5，网络 `SDA` | MAIN Page 1/4；PWR Page 1/3 | MAIN 与 PWR 共用；触摸经过电平转换后接入 |
| 主 I2C SCL | `BOARD_I2C_SCL` | GPIO1，物理管脚 6，网络 `SCL` | MAIN Page 1/4；PWR Page 1/3 | MAIN 与 PWR 共用 |
| XL9555 中断 | `BOARD_XL9555_INT` | GPIO2，物理管脚 7，网络 `XL9555_INT` | MAIN Page 1/4；PWR Page 3/3 | 副板 U26 的 `INT`，有 10K 上拉 |
| 摄像头 I2C SDA | `BOARD_CAMERA_SDA` | GPIO3，物理管脚 8，网络 `CAM_SDA` | MAIN Page 1/3 | 摄像头独立 I2C 网络 |
| 摄像头 I2C SCL | `BOARD_CAMERA_SCL` | GPIO4，物理管脚 9，网络 `CAM_SCL` | MAIN Page 1/3 | 摄像头独立 I2C 网络 |
| 音频 MCLK | `BOARD_I2S_MCLK` | GPIO6，物理管脚 12，网络 `I2S_MCLK` | MAIN Page 1/4；PWR Page 2/3 | ES8389、ES7210 共用 |
| 音频数据输出到编解码器 | `BOARD_I2S_DSDIN` | GPIO7，物理管脚 13，网络 `I2S_DSDIN` | MAIN Page 1/4；PWR Page 2/3 | ESP32 -> ES8389 DSDIN |
| 音频位时钟 | `BOARD_I2S_SCLK` | GPIO8，物理管脚 14，网络 `I2S_SCLK` | MAIN Page 1/4；PWR Page 2/3 | 音频共用 |
| 音频左右声道时钟 | `BOARD_I2S_LRCK` | GPIO9，物理管脚 15，网络 `I2S_LRCK` | MAIN Page 1/4；PWR Page 2/3 | 音频共用 |
| 音频采集数据输入 | `BOARD_I2S_ASDOUT` | GPIO10，物理管脚 16，网络 `I2S_ASDOUT` | MAIN Page 1/4；PWR Page 2/3 | ES7210 `SDOUT1/TDMOUT` -> ESP32 |
| TF SDIO 数据 0 | `BOARD_SD_D0` | GPIO20，物理管脚 27，网络 `SD_D0` | MAIN Page 1/4；PWR Page 3/3 | 原生 SDIO 4-bit |
| TF SDIO 数据 1 | `BOARD_SD_D1` | GPIO21，物理管脚 28，网络 `SD_D1` | MAIN Page 1/4；PWR Page 3/3 | 原生 SDIO 4-bit |
| TF SDIO 数据 2 | `BOARD_SD_D2` | GPIO22，物理管脚 29，网络 `SD_D2` | MAIN Page 1/4；PWR Page 3/3 | 原生 SDIO 4-bit |
| TF SDIO 数据 3 | `BOARD_SD_D3` | GPIO23，物理管脚 31，网络 `SD_D3` | MAIN Page 1/4；PWR Page 3/3 | 原生 SDIO 4-bit |
| TF SDIO 时钟 | `BOARD_SD_CLK` | GPIO24，物理管脚 32，网络 `SD_CLK` | MAIN Page 1/4；PWR Page 3/3 | 原理图串联 R58 10R |
| TF SDIO 命令 | `BOARD_SD_CMD` | GPIO25，物理管脚 33，网络 `SD_CMD` | MAIN Page 1/4；PWR Page 3/3 | 原生 SDIO 4-bit |
| 主控串口 TX | `BOARD_UART_TX` | GPIO58，物理管脚 73，网络 `ESP32_TX` | MAIN Page 1/4；PWR Page 3/3 | 调试座与排线均引出 |
| 主控串口 RX | `BOARD_UART_RX` | GPIO59，物理管脚 74，网络 `ESP32_RX` | MAIN Page 1/4；PWR Page 3/3 | 调试座与排线均引出 |
| 启动选择 | `BOARD_BOOT_PIN` | GPIO61，物理管脚 76，网络 `ESP32_BOOT` | MAIN Page 1/4；PWR Page 3/3 | 低电平启动模式，具体时序由外围按键/电源电路决定 |
| 芯片复位/使能 | `BOARD_EN_PIN` | CHIP_PU，物理管脚 4，网络 `ESP32_EN` | MAIN Page 1/4；PWR Page 1/3 | 不是普通 GPIO |

## 2. ESP32-S31 已使用 GPIO 完整映射

下表按芯片物理管脚号排列，便于核对封装和 PCB。没有列出的 GPIO/电源管脚不应因为编号连续而擅自分配。

| 物理管脚 | 芯片引脚 | 当前宏/建议名 | GPIO/网络 | 来源 | 备注 |
| ---: | --- | --- | --- | --- | --- |
| 4 | `CHIP_PU` | `BOARD_EN_PIN` | `ESP32_EN` | MAIN Page 1/4 | 复位/使能 |
| 5 | `XTAL_32K_N/GPIO0` | `BOARD_I2C_SDA` | GPIO0 / `SDA` | MAIN Page 1/4 | 主 I2C |
| 6 | `XTAL_32K_P/GPIO1` | `BOARD_I2C_SCL` | GPIO1 / `SCL` | MAIN Page 1/4 | 主 I2C |
| 7 | `GPIO2` | `BOARD_XL9555_INT` | GPIO2 / `XL9555_INT` | MAIN Page 1/4 | 副板 IO 扩展中断 |
| 8 | `GPIO3` | `BOARD_CAMERA_SDA` | GPIO3 / `CAM_SDA` | MAIN Page 1/3 | 摄像头 I2C |
| 9 | `GPIO4` | `BOARD_CAMERA_SCL` | GPIO4 / `CAM_SCL` | MAIN Page 1/3 | 摄像头 I2C |
| 10 | `GPIO5` | `BOARD_MOTOR_PWM` | GPIO5 / `M_PWM` | MAIN Page 1/4 | 通过排线引到副板 |
| 12 | `GPIO6` | `BOARD_I2S_MCLK` | GPIO6 / `I2S_MCLK` | MAIN Page 1/4 | 音频主时钟 |
| 13 | `GPIO7` | `BOARD_I2S_DSDIN` | GPIO7 / `I2S_DSDIN` | MAIN Page 1/4 | 音频播放数据 |
| 14 | `GPIO8` | `BOARD_I2S_SCLK` | GPIO8 / `I2S_SCLK` | MAIN Page 1/4 | 音频位时钟 |
| 15 | `GPIO9` | `BOARD_I2S_LRCK` | GPIO9 / `I2S_LRCK` | MAIN Page 1/4 | 音频左右声道时钟 |
| 16 | `GPIO10` | `BOARD_I2S_ASDOUT` | GPIO10 / `I2S_ASDOUT` | MAIN Page 1/4 | 音频采集数据 |
| 17 | `GPIO11` | `BOARD_TOUCH_PAD` | GPIO11 / `TOUCH_PAD` | MAIN Page 1/4 | 触摸测试焊盘/检测输入 |
| 19 | `GPIO12` | `BOARD_DRV_BIN2` | GPIO12 / `DRV_BIN2` | MAIN Page 1/4 | 马达 B 通道 |
| 20 | `GPIO13` | `BOARD_DRV_BIN1` | GPIO13 / `DRV_BIN1` | MAIN Page 1/4 | 马达 B 通道 |
| 21 | `GPIO14` | `BOARD_DRV_AIN2` | GPIO14 / `DRV_AIN2` | MAIN Page 1/4 | 马达 A 通道 |
| 22 | `GPIO15` | `BOARD_DRV_AIN1` | GPIO15 / `DRV_AIN1` | MAIN Page 1/4 | 马达 A 通道 |
| 23 | `GPIO16` | `BOARD_LCD_BL_PWM` | GPIO16 / `BL_PWM` | MAIN Page 1/2 | 背光 PWM |
| 24 | `GPIO17` | `BOARD_LCD_WR` | GPIO17 / `LCD_WR` | MAIN Page 1/2 | LCD 写使能 |
| 25 | `GPIO18` | `BOARD_LCD_RS` | GPIO18 / `LCD_RS` | MAIN Page 1/2 | LCD 数据/命令 |
| 26 | `GPIO19` | `BOARD_LCD_CS` | GPIO19 / `LCD_CS` | MAIN Page 1/2 | LCD 片选 |
| 27 | `GPIO20` | `BOARD_SD_D0` | GPIO20 / `SD_D0` | MAIN Page 1/4 | SDIO DATA0 |
| 28 | `GPIO21` | `BOARD_SD_D1` | GPIO21 / `SD_D1` | MAIN Page 1/4 | SDIO DATA1 |
| 29 | `GPIO22` | `BOARD_SD_D2` | GPIO22 / `SD_D2` | MAIN Page 1/4 | SDIO DATA2 |
| 31 | `GPIO23` | `BOARD_SD_D3` | GPIO23 / `SD_D3` | MAIN Page 1/4 | SDIO DATA3 |
| 32 | `GPIO24` | `BOARD_SD_CLK` | GPIO24 / `SD_CLK` | MAIN Page 1/4 | SDIO CLK |
| 33 | `GPIO25` | `BOARD_SD_CMD` | GPIO25 / `SD_CMD` | MAIN Page 1/4 | SDIO CMD |
| 36 | `SPICS` | `BOARD_FLASH_CS` | `SPICS` | MAIN Page 1/1 | 内部 SPI Flash 专用信号 |
| 37 | `SPIQ` | `BOARD_FLASH_Q` | `SPIQ` | MAIN Page 1/1 | 内部 SPI Flash 专用信号 |
| 38 | `SPIWP` | `BOARD_FLASH_WP` | `SPIWP` | MAIN Page 1/1 | 内部 SPI Flash 专用信号 |
| 40 | `SPIHD` | `BOARD_FLASH_HD` | `SPIHD` | MAIN Page 1/1 | 内部 SPI Flash 专用信号 |
| 41 | `SPICLK` | `BOARD_FLASH_CLK` | `SPICLK` | MAIN Page 1/1 | 内部 SPI Flash 专用信号 |
| 42 | `SPID` | `BOARD_FLASH_D` | `SPID` | MAIN Page 1/1 | 内部 SPI Flash 专用信号 |
| 46 | `GPIO33` | `BOARD_USB_DM` | GPIO33 / `ESP_DM` | MAIN Page 1/4 | USB-C D-；同时为 USB Serial/JTAG 复用脚 |
| 47 | `GPIO34` | `BOARD_USB_DP` | GPIO34 / `ESP_DP` | MAIN Page 1/4 | USB-C D+；同时为 USB Serial/JTAG 复用脚 |
| 48 | `GPIO35` | `BOARD_LCD_RST` | GPIO35 / `LCD_RST` | MAIN Page 1/2 | LCD 复位 |
| 49 | `GPIO36` | `BOARD_LCD_D7` | GPIO36 / `LCD_D7` | MAIN Page 1/2 | LCD 数据 |
| 50 | `GPIO37` | `BOARD_LCD_D6` | GPIO37 / `LCD_D6` | MAIN Page 1/2 | LCD 数据 |
| 51 | `GPIO38` | `BOARD_LCD_D5` | GPIO38 / `LCD_D5` | MAIN Page 1/2 | LCD 数据 |
| 52 | `GPIO39` | `BOARD_LCD_D4` | GPIO39 / `LCD_D4` | MAIN Page 1/2 | LCD 数据 |
| 53 | `GPIO40` | `BOARD_LCD_D3` | GPIO40 / `LCD_D3` | MAIN Page 1/2 | LCD 数据 |
| 55 | `GPIO42` | `BOARD_LCD_D2` | GPIO42 / `LCD_D2` | MAIN Page 1/2 | LCD 数据 |
| 56 | `GPIO43` | `BOARD_LCD_D1` | GPIO43 / `LCD_D1` | MAIN Page 1/2 | LCD 数据 |
| 57 | `GPIO44` | `BOARD_LCD_D0` | GPIO44 / `LCD_D0` | MAIN Page 1/2 | LCD 数据 |
| 58 | `GPIO45` | `BOARD_CAMERA_RESET` | GPIO45 / `CAM_RESET` | MAIN Page 1/3 | 摄像头复位 |
| 59 | `GPIO46` | `BOARD_CAMERA_D2` | GPIO46 / `CAM_D2` | MAIN Page 1/3 | 摄像头并口数据 |
| 60 | `GPIO47` | `BOARD_CAMERA_D3` | GPIO47 / `CAM_D3` | MAIN Page 1/3 | 摄像头并口数据 |
| 61 | `GPIO48` | `BOARD_CAMERA_D4` | GPIO48 / `CAM_D4` | MAIN Page 1/3 | 摄像头并口数据 |
| 62 | `GPIO49` | `BOARD_CAMERA_D5` | GPIO49 / `CAM_D5` | MAIN Page 1/3 | 摄像头并口数据 |
| 65 | `GPIO50` | `BOARD_CAMERA_D6` | GPIO50 / `CAM_D6` | MAIN Page 1/3 | 摄像头并口数据 |
| 66 | `GPIO51` | `BOARD_CAMERA_D7` | GPIO51 / `CAM_D7` | MAIN Page 1/3 | 摄像头并口数据 |
| 67 | `GPIO52` | `BOARD_CAMERA_D8` | GPIO52 / `CAM_D8` | MAIN Page 1/3 | 摄像头并口数据 |
| 68 | `GPIO53` | `BOARD_CAMERA_D9` | GPIO53 / `CAM_D9` | MAIN Page 1/3 | 摄像头并口数据 |
| 69 | `MTDO/GPIO54` | `BOARD_CAMERA_PCLK` | GPIO54 / `CAM_PCLK` | MAIN Page 1/3 | 摄像头像素时钟 |
| 70 | `MTCK/GPIO55` | `BOARD_CAMERA_MCLK` | GPIO55 / `CAM_MCLK` | MAIN Page 1/3 | 摄像头主时钟 |
| 71 | `MTDI/GPIO56` | `BOARD_CAMERA_VSYNC` | GPIO56 / `CAM_VSYNC` | MAIN Page 1/3 | 摄像头帧同步 |
| 72 | `MTMS/GPIO57` | `BOARD_CAMERA_HREF` | GPIO57 / `CAM_HREF` | MAIN Page 1/3 | 摄像头行有效 |
| 73 | `GPIO58` | `BOARD_UART_TX` | GPIO58 / `ESP32_TX` | MAIN Page 1/4 | 调试/串口 |
| 74 | `GPIO59` | `BOARD_UART_RX` | GPIO59 / `ESP32_RX` | MAIN Page 1/4 | 调试/串口 |
| 75 | `GPIO60` | `BOARD_GPIO60` | GPIO60 / `GPIO60` | MAIN Page 1/1 | 原理图保留网络名，未给出更高层功能名 |
| 76 | `GPIO61` | `BOARD_BOOT_PIN` | GPIO61 / `ESP32_BOOT` | MAIN Page 1/4 | 启动选择 |

## 3. LCD、背光与触摸

### 3.1 LCD 与背光

LCD 接口为 MAIN Page 2/4 的 `J1 / FFC0.5MM-30P`。有效显示信号如下：

本项目显示模组配置为 `ST7796S`，1.54 英寸，`320(H)×320(V)` RGB；示例采用
8-bit Intel 8080（I80）接口。显示驱动依赖建议为
`espressif/esp_lcd_st7796^1.4.0`。

| 功能 | 当前宏/建议名 | GPIO/映射 | 来源 | 备注 |
| --- | --- | --- | --- | --- |
| LCD 复位 | `BOARD_LCD_RST` | GPIO35，J1 pin 28，网络 `LCD_RST` | MAIN Page 1/2 | 低有效行为由 LCD 驱动决定 |
| LCD 片选 | `BOARD_LCD_CS` | GPIO19，J1 pin 27，网络 `LCD_CS` | MAIN Page 1/2 |  |
| LCD 数据/命令 | `BOARD_LCD_RS` | GPIO18，J1 pin 26，网络 `LCD_RS` | MAIN Page 1/2 |  |
| LCD 写使能 | `BOARD_LCD_WR` | GPIO17，J1 pin 25，网络 `LCD_WR` | MAIN Page 1/2 |  |
| LCD D0 | `BOARD_LCD_D0` | GPIO44，J1 pin 22，网络 `LCD_D0` | MAIN Page 1/2 |  |
| LCD D1 | `BOARD_LCD_D1` | GPIO43，J1 pin 21，网络 `LCD_D1` | MAIN Page 1/2 |  |
| LCD D2 | `BOARD_LCD_D2` | GPIO42，J1 pin 20，网络 `LCD_D2` | MAIN Page 1/2 |  |
| LCD D3 | `BOARD_LCD_D3` | GPIO40，J1 pin 19，网络 `LCD_D3` | MAIN Page 1/2 |  |
| LCD D4 | `BOARD_LCD_D4` | GPIO39，J1 pin 18，网络 `LCD_D4` | MAIN Page 1/2 |  |
| LCD D5 | `BOARD_LCD_D5` | GPIO38，J1 pin 17，网络 `LCD_D5` | MAIN Page 1/2 |  |
| LCD D6 | `BOARD_LCD_D6` | GPIO37，J1 pin 16，网络 `LCD_D6` | MAIN Page 1/2 |  |
| LCD D7 | `BOARD_LCD_D7` | GPIO36，J1 pin 15，网络 `LCD_D7` | MAIN Page 1/2 |  |
| LCD 电源 | `BOARD_LCD_VDD` | J1 pin 3、5、6、29，网络 `LCD_VDD` | MAIN Page 2/4 | 与背光/模组电源相关，不能当作 GPIO |
| 背光阴极 | `BOARD_LCD_LED_K` | J1 pin 2，网络 `LEDK` | MAIN Page 2/4 | 由 AW9364 驱动 |
| 背光 PWM | `BOARD_LCD_BL_PWM` | GPIO16，网络 `BL_PWM` | MAIN Page 1/2 | AW9364 U24，原理图标注 I2C 地址 `0x15` |

J1 其余脚的原理图连接关系：

| J1 管脚 | 连接 | 备注 |
| ---: | --- | --- |
| 1、4 | GND | 模组地 |
| 30、31、32 | GND/屏蔽 | 连接器符号中的外壳/屏蔽接地结构 |
| 24 | 经 R50 10K 接 `LCD_VDD` | 没有独立的板级信号名 |
| 23、7~14 | NC | 当前原理图未形成有效功能网络 |

### 3.2 触摸

触摸控制器位于显示模组侧，MAIN 只放置 1.8 V/3.3 V 电平转换和连接器。U25 为 `RS0104YTQE12`。

| 功能 | 当前宏/建议名 | GPIO/映射 | 来源 | 备注 |
| --- | --- | --- | --- | --- |
| 触摸 SDA | `BOARD_TOUCH_SDA` | GPIO0，网络 `SDA` | MAIN Page 1/2 | 经 U25 电平转换到 `SDA_1V8` |
| 触摸 SCL | `BOARD_TOUCH_SCL` | GPIO1，网络 `SCL` | MAIN Page 1/2 | 经 U25 电平转换到 `SCL_1V8` |
| 触摸中断 | `BOARD_TOUCH_INT` | `XL9555 P00`，网络 `TP_INT` | MAIN Page 2/4；PWR Page 3/3 | 不是 ESP32 直连 GPIO |
| 触摸复位 | `BOARD_TOUCH_RST` | `XL9555 P01`，网络 `TP_RST` | MAIN Page 2/4；PWR Page 3/3 | 不是 ESP32 直连 GPIO |
| 触摸 1.8 V 中断 | - | `INT_1V8` | MAIN Page 2/4 | 连接触摸 FPC |
| 触摸 1.8 V SDA | - | `SDA_1V8` | MAIN Page 2/4 | 连接触摸 FPC |
| 触摸 1.8 V SCL | - | `SCL_1V8` | MAIN Page 2/4 | 连接触摸 FPC |
| 触摸 1.8 V 复位 | - | `RST_1V8` | MAIN Page 2/4 | 连接触摸 FPC |

当前触摸配置为 `FT6336U/FT6336`，I2C 地址为 `0x38`；触摸驱动依赖建议为
`lambage/esp_lcd_touch_ft6336u^1.0.8`。触摸 IC 位于显示模组侧，MAIN 通过
U25 电平转换器连接公共 I2C。触摸复位和中断分别由副板 XL9555（I2C 地址
`0x22`）的 P01、P00 提供；XL9555 驱动依赖建议为
`sheldonix/esp_io_expander_xl9555^0.8.0`。

## 4. 摄像头

摄像头连接器为 MAIN Page 3/4 的 `J4 / CAM`，原理图标注摄像头 I2C 地址 `0x78`。项目说明允许 OV2640/OV3660 共用该接口。

| 摄像头功能 | 当前宏/建议名 | J4 管脚 | GPIO/映射 | 来源 | 备注 |
| --- | --- | ---: | --- | --- | --- |
| I2C SDA | `BOARD_CAMERA_SDA` | 3 | GPIO3 / `CAM_SDA` | MAIN Page 1/3 |  |
| I2C SCL | `BOARD_CAMERA_SCL` | 5 | GPIO4 / `CAM_SCL` | MAIN Page 1/3 |  |
| 复位 | `BOARD_CAMERA_RESET` | 6 | GPIO45 / `CAM_RESET` | MAIN Page 1/3 |  |
| VSYNC | `BOARD_CAMERA_VSYNC` | 7 | GPIO56 / `CAM_VSYNC` | MAIN Page 1/3 | 传感器脚名 `VS` |
| HREF/RS | `BOARD_CAMERA_HREF` | 9 | GPIO57 / `CAM_HREF` | MAIN Page 1/3 | 传感器脚名 `RS` |
| D2 | `BOARD_CAMERA_D2` | 19 | GPIO46 / `CAM_D2` | MAIN Page 1/3 | 串联 100R |
| D3 | `BOARD_CAMERA_D3` | 21 | GPIO47 / `CAM_D3` | MAIN Page 1/3 | 串联 100R |
| D4 | `BOARD_CAMERA_D4` | 22 | GPIO48 / `CAM_D4` | MAIN Page 1/3 | 串联 100R |
| D5 | `BOARD_CAMERA_D5` | 20 | GPIO49 / `CAM_D5` | MAIN Page 1/3 | 串联 100R |
| D6 | `BOARD_CAMERA_D6` | 18 | GPIO50 / `CAM_D6` | MAIN Page 1/3 | 串联 100R |
| D7 | `BOARD_CAMERA_D7` | 16 | GPIO51 / `CAM_D7` | MAIN Page 1/3 | 串联 100R |
| D8 | `BOARD_CAMERA_D8` | 14 | GPIO52 / `CAM_D8` | MAIN Page 1/3 | 串联 100R |
| D9 | `BOARD_CAMERA_D9` | 12 | GPIO53 / `CAM_D9` | MAIN Page 1/3 | 串联 100R |
| PCLK | `BOARD_CAMERA_PCLK` | 17 | GPIO54 / `CAM_PCLK` | MAIN Page 1/3 | 串联 20R |
| MCLK | `BOARD_CAMERA_MCLK` | 13 | GPIO55 / `CAM_MCLK` | MAIN Page 1/3 | 串联 20R |

摄像头电源和未用脚：

| J4 管脚 | 连接 | 备注 |
| ---: | --- | --- |
| 4 | `AVDD` | 模拟电源 |
| 10 | `DVDD` | 数字内核电源 |
| 11 | `DOVDD` | IO 电源 |
| 2、15、25、26 | GND/AGND/DGND | 摄像头地 |
| 1 | NC | 原理图明确为 NC |
| 8 | `PWDN` 经 R61 100K 下拉到 GND | 未接 ESP32/XL9555 GPIO |
| 23、24 | D1、D0 接到 `DOVDD` | 未接 ESP32 数据 GPIO |

摄像头电源管理器 `SGM38121` 的 I2C 地址为 `0x28`，其 SDA/SCL 接主 I2C0（GPIO0/GPIO1，网络 `SDA`/`SCL`），与摄像头传感器的 I2C1（GPIO3/GPIO4，网络 `CAM_SDA`/`CAM_SCL`）不同。上电时先通过主 I2C 配置电源并使能 `DVDD`、`DOVDD`、`AVDD`，待电源稳定后再初始化摄像头。

## 5. 副板 XL9555 扩展 IO

U26 为 `XL9555QF24`，原理图标注 I2C 地址 `0x22`。P00~P07 为低 8 位，P10~P17 为高 8 位。

| XL9555 | 芯片管脚 | 当前宏/建议名 | 板级网络 | 方向/用途 | 来源 |
| --- | ---: | --- | --- | --- | --- |
| P00 | 1 | `BOARD_XL9555_P00_TOUCH_INT` | `TP_INT` | 输入，触摸中断 | PWR Page 3/3 |
| P01 | 2 | `BOARD_XL9555_P01_TOUCH_RST` | `TP_RST` | 输出，触摸复位 | PWR Page 3/3 |
| P02 | 3 | `BOARD_XL9555_P02_CHARGE_EN` | `CRG_EN` | 输出，充电控制 | PWR Page 1/3、3/3 |
| P03 | 4 | `BOARD_XL9555_P03_CHARGE_INT` | `CRG_INT` | 输入，充电中断 | PWR Page 1/3、3/3 |
| P04 | 5 | `BOARD_XL9555_P04_CHARGE_GOOD` | `CRG_PWR_GOOD` | 输入，充电状态 | PWR Page 1/3、3/3 |
| P05 | 6 | `BOARD_XL9555_P05_POWER_EN` | `PWR_EN` | 输出，5 V 电源使能 | PWR Page 1/3、3/3 |
| P06 | 7 | `BOARD_XL9555_P06_SENSOR_IRQ` | `SEN_IRQ` | 输入，传感器中断 | PWR Page 3/3 |
| P07 | 8 | `BOARD_XL9555_P07_SPK_CTRL` | `SPK_CTRL` | 输出，功放控制 | PWR Page 2/3、3/3 |
| P10 | 10 | `BOARD_XL9555_P10_SD_VDD_EN` | `SD_VDD_EN` | 输出，TF 电源使能 | PWR Page 3/3 |
| P11 | 11 | `BOARD_XL9555_P11_DRV_EN` | `DRV_EN` | 输出，马达驱动使能 | PWR Page 3/3 |
| P12 | 12 | `BOARD_XL9555_P12` | NC | 当前原理图未接 | PWR Page 3/3 |
| P13 | 13 | `BOARD_XL9555_P13` | NC | 当前原理图未接 | PWR Page 3/3 |
| P14 | 14 | `BOARD_XL9555_P14` | NC | 当前原理图未接 | PWR Page 3/3 |
| P15 | 15 | `BOARD_XL9555_P15` | NC | 当前原理图未接 | PWR Page 3/3 |
| P16 | 16 | `BOARD_XL9555_P16` | NC | 当前原理图未接 | PWR Page 3/3 |
| P17 | 17 | `BOARD_XL9555_P17` | NC | 当前原理图未接 | PWR Page 3/3 |

补充：U26 的 `INT` 接网络 `XL9555_INT`，通过 37 针排线到 MAIN GPIO2；`INT` 不应与 P06 的 `SEN_IRQ` 混淆。`DRV_EN` 是 **P11**，不是 P12。

## 6. 副板传感器

U9 为副板传感器接口 `HDR1X5`，原理图标注地址 `0x68/0x28`。传感器中断不直接占用 ESP32 GPIO，而是接入 XL9555。

| U9 管脚 | 信号 | 板级网络 | GPIO/映射 | 来源 | 备注 |
| ---: | --- | --- | --- | --- | --- |
| 1 | SDA | `SDA` | GPIO0 | PWR Page 3/3 | 公共 I2C |
| 2 | SCL | `SCL` | GPIO1 | PWR Page 3/3 | 公共 I2C |
| 3 | VDD | `VDD3V3` | - | PWR Page 3/3 | 3.3 V |
| 4 | GND | GND | - | PWR Page 3/3 |  |
| 5 | IRQ | `SEN_IRQ` | `XL9555 P06` | PWR Page 3/3 | 传感器中断 |

## 7. 副板音频

当前最新原理图使用 `ES8389`、`ES7210` 和 `NS4150B`，不要沿用旧资料中的 ES8311 名称。

### 7.1 ES8389 编解码器

U13 的 I2C 地址为 `0x10`。

| 功能 | 当前宏/建议名 | GPIO/映射 | 来源 | 备注 |
| --- | --- | --- | --- | --- |
| I2C SDA | `BOARD_ES8389_SDA` | GPIO0 / `SDA` | PWR Page 2/3 | 公共 I2C |
| I2C SCL | `BOARD_ES8389_SCL` | GPIO1 / `SCL` | PWR Page 2/3 | 公共 I2C |
| MCLK | `BOARD_ES8389_MCLK` | GPIO6 / `I2S_MCLK` | PWR Page 2/3 |  |
| SCLK/BCLK | `BOARD_ES8389_SCLK` | GPIO8 / `I2S_SCLK` | PWR Page 2/3 |  |
| LRCK | `BOARD_ES8389_LRCK` | GPIO9 / `I2S_LRCK` | PWR Page 2/3 |  |
| ESP32 -> codec 数据 | `BOARD_ES8389_DSDIN` | GPIO7 / `I2S_DSDIN` | PWR Page 2/3 | 连接 U13 `DSDIN` |
| 模拟输出 | `BOARD_AUDIO_OUT_L/R` | `OUT_L`、`OUT_R` | PWR Page 2/3 | 经 `OUT_P` 等网络到功放 |
| 麦克风输入 | `BOARD_MIC1/2` | `MIC1P/N`、`MIC2P/N` | PWR Page 2/3 | 连接 MEMS 麦克风 |

### 7.2 ES7210 麦克风 ADC

U23 的 I2C 地址为 `0x40`。

| 功能 | 当前宏/建议名 | GPIO/映射 | 来源 | 备注 |
| --- | --- | --- | --- | --- |
| I2C SDA/SCL | `BOARD_ES7210_SDA/SCL` | GPIO0/GPIO1，`SDA`/`SCL` | PWR Page 2/3 | 公共 I2C |
| MCLK | `BOARD_ES7210_MCLK` | GPIO6 / `I2S_MCLK` | PWR Page 2/3 |  |
| SCLK | `BOARD_ES7210_SCLK` | GPIO8 / `I2S_SCLK` | PWR Page 2/3 |  |
| LRCK | `BOARD_ES7210_LRCK` | GPIO9 / `I2S_LRCK` | PWR Page 2/3 |  |
| 采集数据 | `BOARD_ES7210_ASDOUT` | GPIO10 / `I2S_ASDOUT` | PWR Page 2/3 | U23 `SDOUT1/TDMOUT` -> ESP32 |
| 第二路数据 | - | U23 `SDOUT2/TDMIN` | PWR Page 2/3 | 当前未形成外部主控网络 |
| 麦克风 | `BOARD_MIC1/2` | `MIC1P/N`、`MIC2P/N` | PWR Page 2/3 | 两颗 MEMS 麦克风 |

### 7.3 功放

U20 为 `NS4150B`，功放控制为 `SPK_CTRL = XL9555 P07`，功放供电为 `SPK_VDD`，来自副板 `VDD5V` 电源域。音频输入网络为 `OUT_P`。

## 8. TF 卡

P11 为 TF 卡座，使用原生 SDIO 4-bit，总线通过 37 针排线连接 MAIN 与 PWR。

| TF 卡管脚 | 卡座信号 | 板级网络 | GPIO/映射 | 备注 |
| ---: | --- | --- | --- | --- |
| 1 | DATA2 | `SD_D2` | GPIO22 |  |
| 2 | DATA3 | `SD_D3` | GPIO23 |  |
| 3 | CMD | `SD_CMD` | GPIO25 |  |
| 4 | VDD | `SD_VDD` | 副板 U18 输出 | 不经过 37 针排线供电 |
| 5 | CLK | `SD_CLK` | GPIO24 | 前级串联 R58 10R |
| 6 | VSS | GND | - |  |
| 7 | DATA0 | `SD_D0` | GPIO20 |  |
| 8 | DATA1 | `SD_D1` | GPIO21 |  |
| 9 | CD# | GND | - | 当前直接接地，没有卡检测 GPIO |
| 10~13 | COM | GND | - | 卡座公共/屏蔽地 |

U18 为 `SY6280AAC`，输出 `SD_VDD`，使能脚为 `SD_VDD_EN = XL9555 P10`。

## 9. 充电、电源与磁吸 USB

### 9.1 充电和电源网络

| 功能 | 当前宏/建议名 | GPIO/映射 | 来源 | 备注 |
| --- | --- | --- | --- | --- |
| 充电器 I2C SDA | `BOARD_CHARGER_SDA` | GPIO0 / `SDA` | PWR Page 1/3 | U17 `SGM41529` |
| 充电器 I2C SCL | `BOARD_CHARGER_SCL` | GPIO1 / `SCL` | PWR Page 1/3 | U17 地址 `0x6B` |
| 充电使能 | `BOARD_CHARGE_EN` | `XL9555 P02` / `CRG_EN` | PWR Page 1/3、3/3 |  |
| 充电中断 | `BOARD_CHARGE_INT` | `XL9555 P03` / `CRG_INT` | PWR Page 1/3、3/3 |  |
| 充电状态 | `BOARD_CHARGE_POWER_GOOD` | `XL9555 P04` / `CRG_PWR_GOOD` | PWR Page 1/3、3/3 |  |
| USB 输入总线 | `BOARD_VBUS` | `VBUS` | MAIN/PWR Page 1/4、1/3 | 37 针排线 32~35 |
| 电池 | `BOARD_VBAT` | `VBAT` | PWR Page 1/3 | 连接充电器 BAT |
| 系统电源 | `BOARD_VSYS` | `VSYS` | PWR Page 1/3 | U6 输入电源域 |
| 3.3 V | `BOARD_3V3` | `VDD3V3` | MAIN/PWR | 37 针排线 28~31 |
| 5 V | `BOARD_5V` | `VDD5V` | PWR Page 1/3 | U7 输出，功放等使用 |
| 5 V 使能 | `BOARD_5V_EN` | `XL9555 P05` / `PWR_EN` | PWR Page 1/3、3/3 | U7 `SY8113` EN |
| 主控使能控制 | `BOARD_EN_PIN` | `ESP32_EN` / CHIP_PU | MAIN Page 1/4；PWR Page 1/3 | 由副板按键/电源控制电路参与控制 |

U6、U7 均为 `SY8113`；U6 输出 `VDD3V3`，U7 输出 `VDD5V`。

### 9.2 主板 USB-C

MAIN Page 4/4 的 U2 为 USB-C 接口。数据线没有使用 ESP32-S31 的专用物理管脚 44/45，而是经过网络 `ESP_DP/ESP_DM` 接到 GPIO34/33。

| U2 信号 | 板级网络 | GPIO/映射 | 备注 |
| --- | --- | --- | --- |
| D+（A6/B6） | `ESP_DP` | GPIO34，物理管脚 47 | USB Serial/JTAG 复用脚 |
| D-（A7/B7） | `ESP_DM` | GPIO33，物理管脚 46 | USB Serial/JTAG 复用脚 |
| VBUS | `VBUS` | - | 经电源路径进入系统 |
| CC1、CC2 | `CC1`、`CC2` | - | 各经 5.1K 下拉到 GND |
| SBU1、SBU2 | NC | - | 当前未使用 |
| GND | GND | - | 连接器地 |

### 9.3 磁吸 USB

J9 为 `USB_MAG`，其 USB 数据线当前没有连接到 ESP32：

| J9 管脚 | 信号 | 板级网络/状态 | 备注 |
| ---: | --- | --- | --- |
| 1 | VBUS | `MAG_USB_IN`，经 Q4 电源路径接入 `VBUS` | 仅供电 |
| 2 | DM | `DM` | 未接主控 USB 数据脚 |
| 3 | DP | `DP` | 未接主控 USB 数据脚 |
| 4 | GND | GND |  |

## 10. 马达接口

副板 J1 为 `GT-B0401-16B1101`，控制信号如下：

| J1 管脚 | 信号 | GPIO/映射 | 备注 |
| ---: | --- | --- | --- |
| 6 | `DRV_AIN1` | GPIO15 |  |
| 8 | `DRV_AIN2` | GPIO14 |  |
| 10 | `DRV_BIN1` | GPIO13 |  |
| 12 | `DRV_BIN2` | GPIO12 |  |
| 14 | `DRV_EN` | `XL9555 P11` | 马达驱动使能 |
| 1、3、5、7、9 | `VDD3V3` | - | 马达接口电源脚 |
| 2、4、16 | GND | - |  |
| 13、15、17、18、19、20 | GND | - |  |
| 11 | NC | - | 当前原理图未接 |

## 11. MAIN/PWR 37 针排线

两张原理图中的 37 针连接器按同一针号定义，以下表格表示 `MAIN pin N <-> PWR pin N`。表格按原理图针号排列，不代表从板背面观察时连接器左右方向。

| 针号 | MAIN 网络 | PWR 网络 | 类型/说明 |
| ---: | --- | --- | --- |
| 1 | `SD_CMD` | `SD_CMD` | SDIO CMD |
| 2 | `I2S_ASDOUT` | `I2S_ASDOUT` | 音频采集数据 |
| 3 | GND | GND | 地 |
| 4 | `I2S_LRCK` | `I2S_LRCK` | 音频 LRCK |
| 5 | `SD_CLK` | `SD_CLK` | SDIO CLK |
| 6 | `I2S_SCLK` | `I2S_SCLK` | 音频 SCLK |
| 7 | GND | GND | 地 |
| 8 | `I2S_DSDIN` | `I2S_DSDIN` | 音频播放数据 |
| 9 | `SD_D3` | `SD_D3` | SDIO DATA3 |
| 10 | `I2S_MCLK` | `I2S_MCLK` | 音频 MCLK |
| 11 | `SD_D2` | `SD_D2` | SDIO DATA2 |
| 12 | `M_PWM` | `M_PWM` | 马达 PWM/控制网络 |
| 13 | `SD_D1` | `SD_D1` | SDIO DATA1 |
| 14 | `XL9555_INT` | `XL9555_INT` | PWR U26 中断到 MAIN GPIO2 |
| 15 | `SD_D0` | `SD_D0` | SDIO DATA0 |
| 16 | `SCL` | `SCL` | 主 I2C SCL |
| 17 | `DRV_AIN1` | `DRV_AIN1` | 马达 A 通道 |
| 18 | `SDA` | `SDA` | 主 I2C SDA |
| 19 | `DRV_AIN2` | `DRV_AIN2` | 马达 A 通道 |
| 20 | `ESP32_EN` | `ESP32_EN` | 主控使能/复位 |
| 21 | `DRV_BIN1` | `DRV_BIN1` | 马达 B 通道 |
| 22 | `TP_RST` | `TP_RST` | 触摸复位 |
| 23 | `DRV_BIN2` | `DRV_BIN2` | 马达 B 通道 |
| 24 | `TP_INT` | `TP_INT` | 触摸中断 |
| 25 | GND | GND | 地 |
| 26 | GND | GND | 地 |
| 27 | `TOUCH_PAD` | `TOUCH_PAD` | 主控 GPIO11 测试焊盘 |
| 28 | `VDD3V3` | `VDD3V3` | 3.3 V |
| 29 | `VDD3V3` | `VDD3V3` | 3.3 V |
| 30 | `VDD3V3` | `VDD3V3` | 3.3 V |
| 31 | `VDD3V3` | `VDD3V3` | 3.3 V |
| 32 | `VBUS` | `VBUS` | USB/输入电源 |
| 33 | `VBUS` | `VBUS` | USB/输入电源 |
| 34 | `VBUS` | `VBUS` | USB/输入电源 |
| 35 | `VBUS` | `VBUS` | USB/输入电源 |
| 36 | GND | GND | 地/屏蔽 |
| 37 | GND | GND | 地/屏蔽 |

注意：`SD_VDD`、`VDD5V`、`VBAT`、`VSYS` 不在这条 37 针排线中；TF 卡电源由 PWR 本地 U18 产生。

## 12. 调试与测试接口

### 12.1 MAIN Page 4 调试接口

MAIN Page 4 还放置了独立测试/调试排针，原理图标出的网络包括：

| 接口 | 引出的网络 | 备注 |
| --- | --- | --- |
| 6P 调试座 | `ESP32_TX`、`ESP32_RX`、`ESP32_BOOT`、`ESP32_EN`、`VDD3V3`、GND | 用于串口、下载和复位 |
| 4P I2C 座 | `VDD3V3`、`SDA`、`SCL`、GND | 主 I2C 测试 |
| 13P 测试排针 | `VDD3V3`、`VBUS`、`ESP32_TX`、`ESP32_BOOT`、GND 等 | 不属于 37 针排线 |

### 12.2 PWR Page 3 的 P2

副板 `P2 / H_Header_13P_6x7` 是独立的调试/测试排针，不是 MAIN/PWR 37 针连接器。原理图引出的网络包括 `ESP32_TX`、`ESP32_RX`、`ESP32_BOOT`、`VDD3V3`、`VBUS` 和 GND。

## 13. I2C 地址表

以下地址均按 7-bit 地址记录；不同电压域或不同 I2C 网络上的同地址器件不一定冲突。

| 设备 | 器件/网络 | 地址 | 来源 | 备注 |
| --- | --- | ---: | --- | --- |
| LCD 背光驱动 | AW9364 U24 | `0x15` | MAIN Page 2/4 | 原理图标注 |
| 触摸控制器 | FT6336/FT6336U | `0x38` | 用户指定配置；MAIN Page 2/4 触摸接口 | 触摸 IC 位于模组侧，经 U25 接入公共 I2C |
| 音频编解码器 | ES8389 U13 | `0x10` | PWR Page 2/3 |  |
| 音频 ADC | ES7210 U23 | `0x40` | PWR Page 2/3 |  |
| 充电管理 | SGM41529 U17 | `0x6B` | PWR Page 1/3 |  |
| IO 扩展 | XL9555 U26 | `0x22` | PWR Page 3/3 |  |
| 摄像头 | OV2640/OV3660 接口 | `0x78` | MAIN Page 3/4；`AGENTS.md` | 摄像头侧 `CAM_SDA/CAM_SCL` |
| 摄像头电源管理 | SGM38121 U21 | `0x28` | MAIN Page 3/4；`AGENTS.md` | 主 I2C0，SDA=GPIO0、SCL=GPIO1；摄像头传感器另用 I2C1 |
| 外部传感器 | U9 `HDR1X5` | `0x68/0x28` | PWR Page 3/3 | 地址取决于传感器配置 |

## 14. 需要特别注意的管脚

- USB-C 的 D+、D- 实际使用 `ESP_DP`、`ESP_DM`，分别连接 GPIO34（物理管脚 47）和 GPIO33（物理管脚 46）。芯片专用物理管脚 44 `USB_DP`、45 `USB_DM` 在当前原理图中没有外部网络，不能按 44/45 编写 USB 映射。
- `GPIO33/GPIO34` 还具有 USB Serial/JTAG 复用功能；使用 USB 下载/调试时不要把它们当作普通 GPIO 使用。
- `GPIO41` 对应的物理管脚 54 是 `VDDPST_3` 电源脚，当前没有 GPIO41 外部引出。
- TF 卡使用原生 SDIO 4-bit，不存在单独的 SD SPI CS 映射；`CD#` 直接接地。
- 摄像头 D0/D1 没有接入 ESP32 数据总线，PWDN 由 100K 下拉，均不能当作可用 GPIO。
- `DRV_EN` 是 XL9555 的 P11；不要误写成 P12。P12 当前为 NC。
- 最新音频器件是 ES8389 + ES7210 + NS4150B；不要继续使用旧版 ES8311 的地址或引脚定义。
- `AW9364` 的地址为 `0x15`，FT6336U 的地址为 `0x38`；两者地址不同。软件仍应按实际总线连接确认背光控制器是否挂在同一 I2C 分段。
- 37 针排线的 28~31 为 `VDD3V3`，32~35 为 `VBUS`；排线中没有 `VDD5V`、`VBAT`、`VSYS` 或 `SD_VDD`。
