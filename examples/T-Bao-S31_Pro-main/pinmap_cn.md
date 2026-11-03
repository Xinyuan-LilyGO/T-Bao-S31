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

---

# HoloCubic 应用移植实施方案

> 本节建立在第 1~14 节硬件边界之上。前面的引脚表是约束条件，不因移植任何 HoloCubic app 而重新分配 GPIO、I2C、I2S、LCD 或摄像头管脚。

**目标：** 从 `D:\Code2\holocubic-apps-main` 中选择适合 T-Bao-S31 的功能和视觉资源，重写为目标工程的 ESP-IDF/LVGL 原生页面，第一阶段交付不依赖 HoloCubic Lua 运行时的本地视觉 app。

**架构：** HoloCubic 的 `package/` 只作为交互、算法和资源参考；目标工程不引入 `/sd/apps` app 运行时，也不直接加载 HoloCubic 的 `.so`。移植后的功能挂在现有 `ui_page_manager` 页面生命周期下，由 `page_*` 负责 LVGL 界面，由 `app_*` 负责音频、SD、JPEG 等硬件资源。

**技术栈：** ESP-IDF 6.1、ESP32-S31、LVGL 9、`esp_xiaozhi_chat_display_page_root()`、`ui_touch` 手势、PSRAM RGB565 canvas、现有 Board Manager、`esp_new_jpeg`/`dl_fft`（仅在对应阶段显式加入依赖）。

**依据：** 本文件第 1~14 节硬件边界；目标工程的 `main/tbao_s31_board.h`（`TBAO_S31_*` 宏）、`main/ui_page_manager.h`、`main/esp_xiaozhi_chat_app.c`、`main/page_game.c`、`main/app_sdcard.*`、`main/app_audio_dsp.*`；源仓库的 `README.md`、`README_LUA.md`、`README_LVGL.md` 及各 app 的 `package/main.lua`。

## 15.1 调研结论

### 不能直接拷贝运行

`holocubic-apps-main` 的普通 app 由 `/sd/apps/<app-id>/app.info` 和 `main.lua` 组成，依赖 HoloCubic 提供的 `app`、`key`、`tmr`、`file`、`http`、`wifi`、`lv_*` 等 Lua API。T-Bao-S31 当前是静态编译的 ESP-IDF C/C++ 工程，入口由 `main/esp_xiaozhi_chat_app.c` 注册固定的 `ui_page_t` 页面，因此没有直接把 `package/` 复制进固件即可运行的路径。

源仓库中的原生模块也不能直接链接到本工程：`mp3_player/src/main/audio_module.c`、`mjpeg_player/src/main/jpg_module.c` 和 `radio/src/main/radio_module.c` 都通过 `module_abi.h` 取得 Lua、显示、音频或 I2S host API，最终导出给 Lua 的 `module_luaopen_v1`/`module_create_v2`。它们是 HoloCubic 动态模块，不是普通 ESP-IDF 静态库。

### 可以移植的是三类内容

1. 纯算法：生命游戏、代码雨、流体点阵、2048 的状态转移和动画节奏。
2. UI/交互：布局、颜色、画布绘制、相册滑动、播放器控制和表盘视觉语言。
3. 硬件功能参考：`Spectrum`/`Afterglow` 的麦克风可视化、`photos` 的 SD 图片轮播、`mjpeg_player` 的双缓冲思路。

源 app 普遍按 `320x240` 编排，例如 `2048/package/main.lua`、`ConwayLife/package/main.lua`、`photos/package/photos.lua` 和 `NixieClock/package/main.lua` 都直接定义了 `SCREEN_W=320`、`SCREEN_H=240`。S31 屏幕是 `320x320`，所有布局必须使用 `LV_HOR_RES/LV_VER_RES` 或显式重排，不能照搬纵向坐标。

## 15.2 候选 app 评估

| 优先级 | 源 app | 结论 | S31 依赖 | 主要改造 |
| --- | --- | --- | --- | --- |
| P0 | `ConwayLife` | 推荐首个移植 | ST7796S、LVGL、定时器、PSRAM | Lua 网格逻辑改为 C；用单个 RGB565 canvas，不创建数万 LVGL 子对象；从 320x240 改为 320x320 |
| P0 | `MatrixRain` | 推荐与上项共用页面 | ST7796S、LVGL、定时器 | 用同一画布和帧缓冲；把 30 ms 轮询限制为 20~30 FPS，退出页面立即停表 |
| P1 | `FluidPendant` | 可移植，作为视觉模式 | ST7796S、LVGL、定时器、PSRAM | 保留点阵流体算法，限制计算网格和帧率；触摸只用于切换/退出，不引入额外硬件 |
| P0 | `2048` | 不重复移植 | 仅 LCD、触摸 | 目标 `main/page_game.c` 已有原生 2048；只参考源 app 的配色、动画和字体，不再引入第二套状态机 |
| P1 | `Spectrum`、`afterglow` | 推荐第二阶段 | ES7210、I2S RX、现有音频回调、FFT | 不得独立重建 I2S；从现有采集回调取样，增加环形缓冲和资源仲裁；无 IMU 时删除倾斜/手柄分支 |
| P1 | `photos` | 推荐第二阶段 | SDIO、`/sdcard`、JPEG 解码、PSRAM | 把 `/sd/images` 映射为 `/sdcard/images`；双图槽位解码；用 LVGL 页面显示，不能使用源 app 的直接显示 host ABI |
| P1 | `PipBoy` | 低风险但价值较低 | LCD、系统时间 | 只移植视觉表盘，不移植 Holo WebUI 和文件结构 |
| P2 | `NixieClock`、`weather` | 只选一个小子集 | WiFi、NTP、HTTP、字体/资源 | 先移植一个本地表盘；天气作为后台缓存数据；不复制约 6 MB 的完整表盘资源和 Web 配置页 |
| P2 | `mp3_player`、`radio` | 复用设计，不做平行 app | ES8389、SD、WiFi | 目标已有 `page_music.c`、`app_music_player.c` 和 `app_sdcard.c`；只吸收歌词/播放列表/频谱 UI，避免两个播放器争用 ES8389 |
| P2 | `mjpeg_player`、`pc_mirror` | 暂不做首批 | JPEG、LVGL/显示、WiFi、SD | 源模块直接取得显示 surface 并 DMA 推屏，会和 face/LVGL 所有权冲突；先稳定现有 camera preview 后再重写为 LVGL 图像交换 |
| 不推荐 | `launcher`、`settings`、`devtools`、`wifi_guide`、`xiaozhi-service` | 不移植为 app | HoloCubic runtime | 目标已有 launcher、设置、配网和原生 Xiaozhi；移植这些会重复定义系统级生命周期 |
| 不推荐 | `bilibili`、`BTC`、`qqmusic`、`netease` | 不纳入固件首版 | 外部 API、账号、网络服务 | 与硬件适配无关，服务可用性、认证和版权风险高 |

**建议的首个可交付物：** 新增一个 `PAGE_VISUAL` 页面，先实现 `ConwayLife`，再在同一页面中加入 `MatrixRain` 和 `FluidPendant` 模式。这样能验证画布、手势、页面进出、PSRAM 和 320x320 布局，而不会先触碰最容易互相干扰的音频、摄像头和网络资源。

## 15.3 T-Bao-S31 适配边界

| 资源 | 允许的使用方式 | 禁止事项 |
| --- | --- | --- |
| LCD | 通过 `esp_xiaozhi_chat_display_page_root()` 构建 LVGL 页面；页面切换交给 `ui_page_manager` | app 自己调用 LCD flush、I80/PARLIO 寄存器或直接抢显示 surface；face 模式下继续画屏 |
| 触摸 | 使用 `ui_touch` 的 `on_gesture`/`on_gesture_at`；坐标按现有 S31 旋转处理 | 使用 HoloCubic `key.*` 假定物理按键；把 XL9555 P00/P01 当作 ESP32 GPIO；在 app 内重新初始化 FT6336U |
| 主 I2C | 复用 Board Manager 已创建的总线；需要设备时使用既有 board/driver 接口 | 把 GPIO0/GPIO1 重新创建成另一条总线；改写 `0x22` XL9555、`0x38` FT6336、`0x28` 摄像头 PMIC、`0x10/0x40` 音频器件的初始化顺序 |
| 音频 | ES8389 播放和 ES7210 采集都通过现有 Xiaozhi/codec 运行时；可视化只读取受控的 PCM 快照 | 直接调用 `i2s_driver_install` 或独立占用 GPIO6~10；在 TTS、唤醒词、录音之间无锁切换；未确认功放状态时输出声音 |
| TF 卡 | 使用 `app_sdcard_init()`、`app_sdcard_mounted()` 和 `/sdcard`；图片/音乐功能先接受无卡状态 | 使用源路径 `/sd`；假定存在 card-detect GPIO；为了一个 app 改动 GPIO20~25；忘记 XL9555 P10 的 SD 电源使能 |
| 摄像头 | 只有 camera/视频功能按 `app_activity_camera_acquire()` 申请运行；复用 `app_camera` 的 PMIC、PARLIO、JPEG 生命周期 | 把 GPIO46~53 当作普通 GPIO；假定 D0/D1/PWDN 可用；页面退出后仍保持摄像头 DMA 和 PSRAM 缓冲 |
| 马达与外设 | 视觉 app 不访问 GPIO5、12~15 或 XL9555 P11；运动功能仍走 `actuator_control`/`motion_manager` | 不能因为源 app 有 tilt/gamepad 就擅自把马达线当输入；当前没有已确认的 IMU，不能承诺倾斜控制 |
| 保留管脚 | GPIO33/34 是 USB-C 数据和 USB Serial/JTAG 复用；GPIO41 对应电源脚；内部 Flash 管脚不可用 | 不按编号连续性分配 GPIO33/34、41、Flash 专用管脚，或把 GPIO44/45 当作本板 USB 数据脚 |
| 尺寸与内存 | 统一 320x320；全屏 RGB565 buffer 约 204800 bytes，优先 PSRAM；页面退出释放 buffer、timer、文件和解码器 | 为每个细胞创建 LVGL object；保留 HoloCubic 的 320x240 坐标；多个 heavy app 同时跑；在 LVGL 锁外修改 UI |

补充：硬件支持原生 SDIO 4-bit，但现有 `main/app_sdcard.c` 的软件路径按 1-bit 使用。首批移植只依赖既有 `/sdcard` API，不在 app 级别切换总线宽度；性能优化另开任务验证。

## 15.4 目标架构和数据流

```text
FT6336U -> ui_touch -> ui_page_manager -> page_visual/page_spectrum/page_photos
                                           |
                                           +-> LVGL page_root
                                           +-> app_audio_dsp / app_sdcard / app_camera
                                           +-> Board Manager 已有 I2C/I2S/SD/JPEG 资源
```

每个移植页面必须实现现有 `ui_page_t` 的 `on_enter`、`on_exit`、`on_gesture` 或 `on_gesture_at`、`on_tick`，并遵循以下生命周期：

1. `on_enter` 创建页面对象、申请资源、启动自己的 timer/task。
2. 后台 task 只产生数据或写入受保护状态；UI 更新在 LVGL 线程/锁内完成。
3. `on_tick` 只处理轻量状态刷新，不做 HTTP、JPEG 解码、SD 扫描和 I2S 阻塞读。
4. `on_exit` 停止 task/timer，释放 canvas、字体、文件句柄、音频/摄像头申请，并恢复到可切换的 page 状态。

HoloCubic 的 Lua 名称到目标工程的映射如下：

| HoloCubic | T-Bao-S31 |
| --- | --- |
| `lv_scr_act()` | `esp_xiaozhi_chat_display_page_root()` 返回的 page root |
| `tmr.create()` | `ui_page_manager_tick()` 驱动的 `on_tick`，或页面私有 FreeRTOS task |
| `key.*` | `ui_touch` 的手势/坐标回调 |
| `file.listdir()` | `app_sdcard` 挂载后使用 POSIX `opendir/readdir` |
| `/sd/...` | `/sdcard/...` |
| `audio.i2s`/`i2s` | 现有 `esp_codec_dev`、Xiaozhi recorder 和 `app_audio_dsp` 边界 |
| Holo `.so` | 不保留动态模块；把需要的解码/算法改为 ESP-IDF component 或本地 C 模块 |

## 15.5 分阶段实施计划

### Task 0：锁定基线、许可和资源清单

**Files:** 只读检查 `pinmap_cn.md`、`main/ui_page_manager.h`、`main/esp_xiaozhi_chat_app.c`、`main/page_game.c`、`main/app_audio_dsp.*`、`main/app_sdcard.*`；源仓库对应 app 的 `app.info`、`LICENSE`、`README` 和 `package`。

- [ ] 运行 `git status --short`，确认不覆盖现有工作区修改。
- [ ] 运行 `idf.py build` 记录 S31 当前基线；新功能开始前必须能区分基线错误与移植错误。
- [ ] 为每个要复制的字体、位图和算法保留来源、作者、许可证和版本记录；没有明确授权的资源只参考外观，不复制文件。
- [ ] 先只纳入 `ConwayLife`、`MatrixRain`、`FluidPendant`；音频和图片 app 不进入第一轮验收。

### Task 1：实现纯逻辑视觉引擎

**Files:**

- Create: `main/visual_engine.h`
- Create: `main/visual_engine.c`
- Create: `tools/test_visual_engine.c`
- Create: `tools/test_visual_engine.py`

**Interface:**

```c
typedef enum {
    VISUAL_MODE_CONWAY = 0,
    VISUAL_MODE_MATRIX,
    VISUAL_MODE_FLUID,
} visual_mode_t;

typedef struct visual_engine visual_engine_t;

esp_err_t visual_engine_create(visual_mode_t mode, int width, int height,
                               uint32_t seed, visual_engine_t **out);
void visual_engine_step(visual_engine_t *engine, uint32_t elapsed_ms);
void visual_engine_render_rgb565(const visual_engine_t *engine,
                                 uint16_t *pixels, int stride_pixels);
void visual_engine_destroy(visual_engine_t *engine);
```

- [ ] 把 `ConwayLife` 的邻居计数、周期注入和停滞重置改为可测试的 C 状态机；用固定种子测试生成结果。
- [ ] 把 `MatrixRain` 的列状态和 `FluidPendant` 的点阵状态放入同一个 opaque engine，渲染只写调用方提供的 RGB565 缓冲区。
- [ ] 禁止 engine 访问 LVGL、I2C、I2S、文件系统和 GPIO，使算法可以用现有 clang 测试方式在主机运行。
- [ ] 在 `tools/test_visual_engine.c` 中验证：固定种子可复现、每次 step 不越界、Conway 的 block/blinker 规则正确、销毁后无悬挂资源。
- [ ] 运行 `python tools/test_visual_engine.py`；目标环境有 clang 时编译并执行，否则至少执行 `-fsyntax-only`。

### Task 2：新增视觉页面并接入导航

**Files:**

- Create: `main/page_visual.h`
- Create: `main/page_visual.c`
- Modify: `main/ui_page_manager.h`
- Modify: `main/esp_xiaozhi_chat_app.c`
- Modify: `main/page_launcher.c`
- Modify: `main/CMakeLists.txt`
- Modify: `tools/test_launcher_contract.py`

- [ ] 在 `ui_page_id_t` 中增加 `PAGE_VISUAL`，在 `s_pages` 中注册 `page_visual`，为 launcher 增加一个视觉 app 入口。
- [ ] `page_visual_on_enter()` 申请一个 320x320 RGB565 canvas buffer，优先使用 `MALLOC_CAP_SPIRAM`，失败时使用明确的降级错误页，不创建大量 LVGL 子对象。
- [ ] 使用目标工程已有的 `lv_canvas_set_buffer()`、`lv_canvas_init_layer()`、`lv_canvas_finish_layer()` 路径；页面不调用 LCD driver 或直接 flush。
- [ ] `on_tick` 按模式使用固定帧率：Conway 160 ms 一步，MatrixRain 33~50 ms 一步，FluidPendant 50~66 ms 一步；页面不可见或退出后停止更新。
- [ ] 上滑/下滑返回 launcher，左右切换三种视觉模式；手势处理只能通过 `ui_touch`，不注册 Holo `key.*`。
- [ ] `page_visual_on_exit()` 停止 timer/task、销毁 engine、释放 canvas buffer，并清除页面引用。
- [ ] 运行 `python tools/test_launcher_contract.py`，再运行 `idf.py build`。

### Task 3：加入麦克风可视化前的音频资源边界

**Files:**

- Modify: `main/app_audio_dsp.h`
- Modify: `main/app_audio_dsp.c`
- Create: `main/app_audio_viz.h`
- Create: `main/app_audio_viz.c`
- Modify: `main/esp_xiaozhi_chat_app.c`
- Modify: `main/idf_component.yml`
- Modify: `main/CMakeLists.txt`
- Create: `tools/test_audio_viz.c`
- Create: `tools/test_audio_viz.py`

**Interface:**

```c
esp_err_t app_audio_viz_acquire(void);
void app_audio_viz_release(void);
size_t app_audio_viz_copy_samples(int16_t *out, size_t capacity,
                                  uint32_t *sample_rate);
```

- [ ] 在已有 `app_audio_dsp_mic_input_cb()` 内写入固定大小的无阻塞环形缓冲；回调不得 malloc、加长互斥锁或修改送入 AFE 的原始数据。
- [ ] 明确采集所有权：Xiaozhi recorder、Spectrum 页面和录音功能不能同时重新配置 I2S；页面只消费快照，不能调用底层 I2S install/read。
- [ ] 给 `main/idf_component.yml` 增加显式 `espressif/dl_fft` 依赖，并在 `main/CMakeLists.txt` 的 `REQUIRES` 中加入 `espressif__dl_fft`，不依赖 `esp-sr` 的传递依赖。
- [ ] 用 256 或 512 点固定 FFT，先测试 16 kHz；在 `tools/test_audio_viz.c` 中用 1 kHz 正弦输入验证主频 bin，验证缓冲区满/空和释放后的读行为。
- [ ] 运行 `python tools/test_audio_viz.py` 和 `idf.py build`。只有此任务通过后才开始 `Spectrum`/`Afterglow` 页面。

### Task 4：实现 Spectrum/Afterglow 页面

**Files:**

- Create: `main/page_spectrum.h`
- Create: `main/page_spectrum.c`
- Modify: `main/ui_page_manager.h`
- Modify: `main/esp_xiaozhi_chat_app.c`
- Modify: `main/page_launcher.c`
- Modify: `main/CMakeLists.txt`

- [ ] 先显示当前 mic RMS 和波形；当 `app_audio_viz_copy_samples()` 没有新数据时显示静止状态，不伪造频谱。
- [ ] 再加入 `Spectrum` 的频带柱状图和 `Afterglow` 的波形风格；绘制复用视觉 canvas，不为每根柱子创建长期存在的 LVGL object。
- [ ] 删除源 app 的倾斜/手柄分支。当前硬件边界没有已确认的 IMU，外部传感器 U9 的具体型号和协议不足以作为移植前提。
- [ ] 进入页面时 acquire，退出页面时 release；与 TTS、唤醒词、录音同时运行时以音频主链路为准，不能让可视化抢占采集。
- [ ] 在设备上验证：静音、说话、TTS 播放、连续切出切入 100 次，均不触发 I2S 重建、WDT、音频卡顿或页面黑屏。

### Task 5：实现本地照片浏览器

**Files:**

- Create: `main/app_photo_store.h`
- Create: `main/app_photo_store.c`
- Create: `main/page_photos.h`
- Create: `main/page_photos.c`
- Modify: `main/app_sdcard.h`
- Modify: `main/app_sdcard.c`
- Modify: `main/ui_page_manager.h`
- Modify: `main/esp_xiaozhi_chat_app.c`
- Modify: `main/page_launcher.c`
- Modify: `main/CMakeLists.txt`

**Interface:**

```c
esp_err_t app_photo_store_scan(const char *directory);
int app_photo_store_count(void);
const char *app_photo_store_name_at(int index);
esp_err_t app_photo_store_decode(int index, lv_image_dsc_t *out,
                                 void **owned_buffer);
void app_photo_store_release(lv_image_dsc_t *image, void *owned_buffer);
```

- [ ] 固定使用 `/sdcard/images`，扫描只接受 JPG/JPEG/PNG/BMP 中实际有解码器支持的格式；没有 SD 卡时返回空列表状态。
- [ ] 用后台 task 读取和解码下一张图，使用两个 PSRAM RGB565 槽位；UI 线程只在完整帧准备好后交换 descriptor。
- [ ] 复用 `esp_new_jpeg` 或目标工程现有 JPEG 解码依赖；不移植 `mjpeg_player` 的 `display.acquire()` 直推屏逻辑。
- [ ] 用左右滑动切换、点击显示文件名、下滑返回；退出时停止解码 task、释放两个槽位和所有文件句柄。
- [ ] 用一张 320x320、超尺寸、损坏、空目录和拔卡后的测试集验证解码失败不会破坏 page root 或后续 camera page。

### Task 6：按需移植时钟、天气和媒体功能

**Files:** 仅在前面页面稳定后新增对应 `main/page_clock.c`、`main/page_weather.c` 或修改现有 `main/page_music.c`；不把源仓库的整个 `package/` 复制进目标工程。

- [ ] `PipBoy`/`NixieClock` 先选一个本地表盘，使用 `esp_timer`/系统时间和现有字体；天气请求放到后台任务并缓存结果，页面只消费缓存。
- [ ] `weather` 的 HTTP/JSON 只复用数据模型，不复制天气服务的 token、WebUI 或 320x240 布局。
- [ ] `mp3_player`/`radio` 只增强现有 `page_music.c`；播放器生命周期仍由 `app_music_player` 单一拥有，页面离开必须 stop。
- [ ] `mjpeg_player`/`pc_mirror` 只有在 camera activity、JPEG 内存和 LVGL 显示交换经过压力测试后再进入计划；首版不承诺视频播放。

### Task 7：整机验收

- [ ] 静态检查：`rg -n "GPIO|i2c_new_master_bus|i2s_driver_install|display.acquire|/sd/|key\." main`，移植页面不得新增硬编码错误引脚、重复总线初始化、Holo 路径或 Holo 按键 API。
- [ ] 构建检查：`idf.py build`；确认目标为 `esp32s31`，且没有因 `dl_fft`、JPEG 或资产依赖产生未解析符号。
- [ ] 页面检查：face、launcher、visual、game、music、camera、motion 来回切换各 100 次，观察堆、PSRAM、LVGL object 数和任务数不持续增长。
- [ ] 输入检查：单击、左右滑动、上下滑动、边缘滑动和触摸释放都能退出或切换；不能把返回手势误送给页面内部动画。
- [ ] 资源检查：无 SD、无 WiFi、无麦克风数据、JPEG 损坏、音频播放中进入页面时均显示可恢复状态，不重启、不访问未连接硬件。
- [ ] 硬件检查：逻辑分析仪/串口确认 I2C0 仍为 GPIO0/1、音频仍为 GPIO6~10、SD 仍为 GPIO20~25、USB GPIO33/34 未被 app 占用；任何新增硬件映射必须先更新本文件。

## 15.6 交付顺序和完成标准

建议按以下顺序交付，每个阶段单独构建和回归：

1. `ConwayLife` 单模式视觉页，证明 320x320 canvas 和页面生命周期。
2. `MatrixRain`、`FluidPendant` 模式，证明同一页的 renderer 切换和帧率限制。
3. `Spectrum`/`Afterglow`，证明 ES7210 采集快照与 Xiaozhi 音频链路共存。
4. `photos`，证明 SD、JPEG、PSRAM 双槽位和页面退出清理。
5. 选做一个本地时钟表盘；媒体和视频放在独立里程碑。

当且仅当第 15.5 节 Task 7 全部通过，才把对应 app 标记为“已移植”。在此之前只能称为“已复用算法/UI 参考”，不能称为 HoloCubic app 直接兼容。
