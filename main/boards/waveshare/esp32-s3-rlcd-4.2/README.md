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

## 低功耗体系（DVFS + Tickless + 自动 light sleep）

由 `managers/power_manager.{h,cc}` 统一管理，构建时通过板级 `config.json` 的 `sdkconfig_append` 打开 `CONFIG_PM_ENABLE` / `CONFIG_FREERTOS_USE_TICKLESS_IDLE` / `CONFIG_PM_POWER_DOWN_CPU_IN_LIGHT_SLEEP`。

- **DVFS**：`esp_pm_configure(max_freq, min_freq = 40 MHz, light_sleep_enable = true)`。空闲时 CPU 自动降到 40 MHz，任务就绪或中断到来立即回到最高频率，对应用层透明。
- **三档最高频率**由应用层已有的 `Board::SetPowerSaveLevel()` 驱动（对话/播放/待机等状态都会调用）：PERFORMANCE = 240 MHz、BALANCED = 160 MHz、LOW_POWER = 80 MHz；最低频率始终 40 MHz。
- **Tickless + 自动 light sleep**：`CONFIG_FREERTOS_IDLE_TIME_BEFORE_SLEEP=3`（tick），空闲超过阈值即进 light sleep，由最近的任务超时或 esp_timer 唤醒；`CONFIG_PM_SLP_SPIRAM_HALFSLEEP_ENABLED` 保证 PSRAM 内容不丢。
- **PM Lock（RAII，成对持有/释放）**：
  - 音频：`AudioService` 在 codec 输入或输出打开期间持有 `ESP_PM_NO_LIGHT_SLEEP`（锁名 `audio_active`），两者都关闭（音频省电超时）后释放。唤醒词常开时 I2S 采集必须连续，休眠会丢帧、漏检。
  - 屏幕：`CustomLcdDisplay::FlushCommit()` 全程持 `ESP_PM_APB_FREQ_MAX` + `NO_LIGHT_SLEEP`，保证 SPI 40 MHz 刷屏不被降频或休眠打断。
  - SD 卡：`SdcardManager::init()` / `listFiles()` 同样持双锁。
  - 未开启 `CONFIG_PM_ENABLE` 时 `esp_pm_lock_create` 返回 `ESP_ERR_NOT_SUPPORTED`，所有钩子静默降级，行为与之前一致。

**实际生效情况**：唤醒词常开（IDLE 监听）时 I2S 连续采集，light sleep 不会触发，此时的省电来自 DVFS（CPU 40~80 MHz）；只有 codec 全部关闭（唤醒词关闭、配网模式、音频省电超时后）系统才会真正进入 light sleep。

**实机验证**：

1. 串口启动日志出现 `PowerManager: DVFS 已启用: CPU 40~240 MHz, 自动 light sleep 开启`。
2. USER 长按进系统信息页，CPU 频率随负载在 40 / 80 / 160 / 240 MHz 之间变化（取 `rtc_clk_cpu_freq_get_config`，是实时值）。
3. 电流对比：万用表串在电池回路，记录 (a) 对话中、(b) 待机听唤醒词、(c) 关闭唤醒词后静置 三档电流，与改动前对比（待补充：实测数值）。

**风险与回滚**：40 MHz 与 octal PSRAM 的组合本机无硬件验证；若出现异常复位或花屏，把 `power_manager.h` 里的 `static constexpr int kMinFreqMhz = 40;` 改为 80，或在 `Apply()` 里把 `light_sleep_enable` 置 false（只保留 DVFS），也可整项 `git revert`。

## 农历 / 节气 / 节日

时钟卡片底部新增一行农历标签（大字时钟下方），内容按优先级取：农历节日 > 公历节日 > 当日节气，都没有时只显示农历日期。

- 例：`十月初七 小雪`、`八月十五 中秋`、`正月初一 春节`、`十二月廿九 除夕`、`十一月初二 冬至`。
- 字体用 `font_puhui_16_4`（7415 常用汉字）；时间/日期用的 `alibaba_puhui_16` 字符集有限，节气与节日里的生僻字会缺字。
- 布局：时钟卡内 `time_label_` 改 `LV_ALIGN_TOP_MID`（横屏 y=10 / 竖屏 y=2），农历标签 `LV_ALIGN_BOTTOM_MID`（横屏 -8 / 竖屏 -2），不再用 `lv_obj_center`，避免两行叠加。

**实现方式（表驱动，运行时零浮点零天文计算）**

- `managers/lunar_calendar.{h,cc}`：1900–2100 全量数据编译进固件。
  - `kLunarYearInfo[201]`：每年一个 24-bit 整数——低 4 位闰月号（0=无闰月）、bit4..15 为 1–12 月是否 30 天、bit16 为闰月是否 30 天。
  - `kSolarTermDay[201][24]`：每年 24 个节气的日号（1 字节/个，月份由索引隐含，0=小寒…23=冬至）。
  - 换算全部是整数加减：先 `DaysFromCivil` 求距 1900-01-31 的天数，再按年长/月长逐级扣除，最后查节气、节日表。
  - 表共约 5 KB（201×4 B + 201×24 B），比运行 Meeus 算法省 PSRAM 且不受 DVFS 降频影响。
- 表由离线脚本 `tools/gen_lunar_table.py`（Meeus 新月 + 太阳视黄经二分求节气 + ΔT 改正 + 北京时间 +8h）生成，**不要手改 lunar_calendar.cc**。
  生成时脚本自带校验：春节日期锚点（2020–2030）、年长与实际天数一致、正月初一落在 1/21–2/21、每月 29 或 30 天。
- 主机验证：脚本旁的 `test_lunar.cc`（不入库）覆盖 1900-01-31–2100-12-31 共 73384 天往返换算、春节/端午/中秋/除夕/闰月锚点、5 个节气日、若干 UI 标签样例，全部通过。

**接入点**：`data_update_task.cc` 每分钟刷新块里，日期数字更新后调用 `lunar::FormatLabel()` 写入 `lunar_label_`；跨天自然由分钟刷新带出。

**改年份范围/回滚**：数据范围受表限制（超出返回 false，标签保持上一值）；回滚直接 `git revert` 本提交，标签与成员一并消失，不影响其他功能。



