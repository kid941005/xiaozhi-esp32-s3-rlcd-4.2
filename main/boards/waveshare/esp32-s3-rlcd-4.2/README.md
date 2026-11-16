# 产品链接

[微雪电子 ESP32-S3-RLCD-4.2](https://www.waveshare.net/shop/ESP32-S3-RLCD-4.2.htm)

# 编译配置命令

**克隆工程**

```bash
git clone https://github.com/78/xiaozhi-esp32.git
```

**进入工程**

```bash
cd xiaozhi-esp32
```

**配置编译目标为 ESP32S3**

```bash
idf.py set-target esp32s3
```

**打开 menuconfig**

```bash
idf.py menuconfig
```

**选择板子**

```bash
Xiaozhi Assistant -> Board Type -> Waveshare ESP32-S3-RLCD-4.2
```

**编译**

```ba
idf.py build
```

**下载并打开串口终端**

```bash
idf.py build flash monitor
```

## 硬件核对（对照微雪官方文档与官方示例）

数据来源：

- 文档：https://docs.waveshare.net/ESP32-S3-RLCD-4.2/
- 官方示例仓库：https://github.com/waveshareteam/ESP32-S3-RLCD-4.2 （gitee 镜像 https://gitee.com/waveshare/ESP32-S3-RLCD-4.2）

| 项目 | 官方示例 | 本固件 | 结果 |
| --- | --- | --- | --- |
| RLCD SPI | DC 5 / CS 40 / SCK 11 / MOSI 12 / RST 41 / TE 6 | `config.h` 同名宏 | 一致 |
| ST7305 初始化 | `11_U8G2_Test/components/u8g2_st7305/u8g2_st7305.c`（D6/D1/C0/C1/C2/C4/C5/D8/B2/B3/B4/62/B7/B0/11/C9/36/3A/B9/B8/21/2A/2B/35/D0） | `rlcd_driver.cc` 的 `RLCD_Init()` 同序列 | 一致 |
| I2C | SDA 13 / SCL 14 | `config.h` | 一致 |
| 音频 I2S | MCLK 16 / BCLK 9 / WS 45 / DIN 10 / DOUT 8 | `config.h` | 一致 |
| 功放 PA | GPIO46（ES8311 输出 + ES7210 输入，见 `codec_board/board_cfg.txt` 的 `Board: S3_RLCD_4_2`） | `AUDIO_CODEC_PA_PIN` | 一致 |
| SHTC3 | I2C 0x70 | `managers/sensor_manager.cc` | 一致 |
| PCF85063 | I2C 0x51 | `managers/sensor_manager.cc` | 一致 |
| 电池 ADC | ADC1_CH3（GPIO4），电压 = 校准值 × 3（`03_ADC_Test/components/port_bsp/adc_bsp.cpp`） | `BatterygetVoltage()` 同通道、同系数 | 一致 |
| SD 卡 | SDMMC 1-bit，CLK 38 / CMD 21 / D0 39（`06_SD_Card/components/port_bsp/sdcard_bsp.h`） | `SdcardManager::init()` 默认参数 | 一致 |
| 按键 | BOOT GPIO0 / KEY GPIO18 | `BOOT_BUTTON_GPIO` / `USER_BUTTON_GPIO` | 一致 |
| 分辨率 | 面板原生 300×400；官方 LVGL 示例按 400×300 横屏使用 | 默认 `RLCD_WIDTH 400` / `RLCD_HEIGHT 300`，`RLCD_PORTRAIT=1` 时切 300×400 | 一致（支持双方向） |

### 硬件决定、固件不参与的部分

- **PWR 按键**（长按关机 / 单击开机）：电源管理芯片直接控制，没有 GPIO 连到 SoC。
- **CHG / WRN 指示灯**：由充电芯片驱动，SoC 读不到充电状态。固件改用**电池电压趋势**推断充电状态（见下节），不依赖 STAT 引脚。
- **RTC 备用电池、18650 电池座、2×8 排母**：纯硬件，无固件动作。

## 充电状态检测（电压趋势推断）

硬件没有把充电芯片的 STAT 引脚接到 SoC，所以固件用电池电压的变化趋势来推断是否在充电，实现在 `waveshare-s3-rlcd-4.2.cc` 的 `GetBatteryLevel()`：

- 12 槽环形缓冲记录电压采样，采样间隔 5 s（窗口约 60 s）。
- 窗口内电压上升 ≥ 30 mV → 判为充电中；下降 ≥ 30 mV → 判为放电中。
- 变化小于阈值时**保持上一次状态**，所以充电进入 CV 阶段（电压几乎不动）、或静置时，图标不会来回跳。

状态栏电池图标因此会在 `battery_charging` 与 `battery_full/medium/low` 之间正确切换；低电量弹窗也只在既非充电、又在放电时才提示。

限制：纯软件推断，插拔瞬间的负载跳变会影响判断精度；它不是电量计，不能给出真实充电电流。

## 竖屏 300×400 模式切换

面板原生分辨率是 300×400。固件默认按 400×300 横屏（2×2 卡片网格）使用；改成竖屏只需一个宏：

```c
// main/boards/waveshare/esp32-s3-rlcd-4.2/config.h
#define RLCD_PORTRAIT 0   // 0 = 横屏 400x300（默认）  1 = 竖屏 300x400
```

改成 `1` 后重新编译烧录即可。竖屏不是把横屏画面转 90°（那样文字会侧躺），而是**重新排版**：

- 天气主页：时钟 → 日历 → AI 对话 → 备忘录 四张卡片改为整宽单列自上而下堆叠（104 / 100 / 80 / 58 px 高）。
- 音乐页：唱片 150→110 px，右侧信息卡加高到 150 px 以容纳三行歌词，进度条随之下移。
- 番茄钟页：进度条下移到 y=250，避免和居中倒计时重叠。

驱动层对应地按 `width > height` 选 LUT（`InitLandscapeLUT` / `InitPortraitLUT`），LUT 行跨度固定为 `RLCD_LUT_STRIDE=400`，两种方向都不会越界。

两个方向都已编译通过（`build/merged-binary.bin` 为当前 `RLCD_PORTRAIT` 取值对应的产物）。


