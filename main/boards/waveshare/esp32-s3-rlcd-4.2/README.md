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

## 界面布局（复刻微雪成品界面）

尺寸来自对微雪成品实机照片的实测（把屏幕区域透视矫正到 400×300 后按像素量取），横屏采用「左栏时钟 + AI，右栏整高待办」的两栏布局：

~~~text
┌──────────────────────────────────────────────┐
│ 环境 32.1°C 39%      十月初八 ╭─WiFi─▭─99%─╮ │
│ ┌─────────────────┐ ┌─────────────┐          │
│ │ 10月2日 星期五   │ │ 待办    ☑ ★ │          │
│ │ ███ 17:18 ███    │ │ ─────────── │          │
│ │ 成都 晴间多云 23°C│ │ ★ 抢火车票  │          │
│ ├─────────────────┤ │ ★ 记得吃饭  │          │
│ │ [猫] 待命 │AI 待命│ │ ★ 联动方案  │          │
│ └─────────────────┘ └─────────────┘          │
└──────────────────────────────────────────────┘
~~~

- 卡片尺寸：时钟卡 199×120 @(10,41)、待办卡 173×249 @(217,41)、AI 卡 199×100 @(10,179)；`pad=10`、`gap=8`。
- 顶栏（黑底白字）：左侧 `sensor_label_` 显示「环境 温度 湿度」，右侧 `lunar_label_` 显示农历/节气/节日（14px），最右是 WiFi + 电池胶囊（110×24）。
- 时钟卡内三行：日期行（`day_label_`，16px，居中）→ **翻页时钟**（4 张 39×50 黑底圆角卡片，每张一位数字，`alibaba_black_64` 白字，卡片中央叠 1px 白色中缝；两位之间是两个 5×5 黑色圆角冒号方块；数字由 `time_digit_label_[4]` 分别写入时十/时个/分十/分个）→ 天气行（`weather_label_`，16px，居中，「城市 天气 温度°C」）。外框 2px + 内框 2px 双层圆角描边。
- 待办卡：标题「待办」+ 右上 ☑ ★ 两个 material symbols 图标 + 分隔线 + 列表容器 `memo_rows_`（`LV_FLEX_FLOW_COLUMN`）。每行是「★ + 文本」的横向 flex，文本 `LV_LABEL_LONG_WRAP` 自动换行，最多 6 条，超出显示「…还有N条」。
- 待办数据来自 NVS `memo/items`（JSON 数组，字段 `t`=时间 `c`=内容），由 MCP 工具 `self.memo.add / list / done / clear` 增删，或由 `self.todo.sync` 整批覆盖；`RefreshMemoDisplayInternal()` 每次清空容器重建行。
- AI 卡：左侧 56px 表情区（48px 表情图 + 情绪标签），竖线分隔（距左边 70px），右侧状态文字左对齐垂直居中；卡片高度 < 90px 时自动切紧凑模式（表情 30px、14px 字体）。
- 字体：中文文案统一用 `font_puhui_16_4`（7415 常用汉字）和 `font_puhui_14_1`；图标用 `font_material_symbols_16_4`（`material_symbols.h` 的 `MATERIAL_SYMBOLS_STAR` / `MATERIAL_SYMBOLS_CHECK_CIRCLE`）；大字时间用 `alibaba_black_64`。**注意**：`alibaba_puhui_16` 字符集不含「环境」，顶栏必须用 `font_puhui_16_4`，否则整词不显示。
- 竖屏 300×400 为整宽纵向堆叠：时钟卡 280×140 → 待办卡 280×138 → AI 卡 280×55。

**微调位置**（都在 `weather_ui.cc` 的卡片几何块）：改 `clock_h` / `todo_h` / `ai_h` 可调整三张卡比例；`day_label_` / 翻页时钟 / `weather_label_` 的 `lv_obj_align` / `flip_y` 控制卡内三行位置；翻页卡片尺寸是 `flip_card_w`(39) / `flip_card_h`(50) / `flip_gap`(3) / `colon_w`(10)；`lunar_label_` 的 `-128` 是胶囊左侧的让位量；待办行间距是 `memo_rows_` 的 `lv_obj_set_style_pad_row`。

## 天气数据源（免密钥）

固件没有和风天气 Key 时，`WeatherManager::update()` 自动走免密钥链路，**不依赖任何 API Key**：

1. 主源：中国天气网数据接口 `http://t.weather.itboy.net/api/weather/city/<城市代码>`（纯 HTTP，国内直连快），取 `data.wendu` 温度与 `data.forecast[0].type` 天气现象，城市名取 `cityInfo.city` 并去掉结尾「市」。
2. 备用源：Open-Meteo `https://api.open-meteo.com/v1/forecast?...&current=temperature_2m,weather_code`，WMO 天气代码转中文。

配置项在 `config.h`：`WEATHER_CITY_CODE`（中国天气网城市代码，默认成都 `101270101`）、`WEATHER_LAT` / `WEATHER_LON`（备用源坐标）、`WEATHER_CITY`（兜底显示名）。城市代码可在 weather.com.cn 城市页 URL 里查到（如 `101270101` = 成都）。

拉取由板级独立任务 `weather_fetch`（栈 8192）负责：启动后延迟 15 s 首次拉取，成功每 30 分钟一次、失败 60 秒重试；HTTP 不占用 LVGL 锁，不会卡界面。日志关键字 `天气更新成功(中国天气网)` / `Open-Meteo`。

## 待办（MCP 接入点，设备自取）

设备本身就是 MCP 客户端：板级任务 `todo_mcp`（栈 8192）用独立 websocket 连外部 MCP 接入点，
按标准 JSON-RPC 握手后调用 `list_todos`（`view=today`, `limit=10`），把每条待办格式化成
`11.16(日) 20:00 标题` 的一行，写入 NVS `memo/items` 并重绘右侧待办卡。

- **长连接**：握手（initialize）只做一次，之后在**同一条 websocket 上**每分钟发一次 `tools/call`。
  早期版本是每次拉取都新建连接（每次都要重做 TLS 握手），实测跑满 5 分钟后第二次握手会被对端
  在握手阶段关闭 TCP（`mbedtls_ssl_handshake returned -0x7280` = `MBEDTLS_ERR_SSL_CONN_EOF`），
  所以改成常驻会话 + 断线重连。
- 断线处理：建连最多重试 3 次（间隔 3 s）；连续 2 次拉取失败或 websocket 报错则关闭会话，
  等 20 s（或网络恢复事件）后重连。服务端每 20 s 发 ping，客户端自动回 pong 保活。
- 地址存 NVS `todo/mcp_url`；可用 MCP 工具 `self.todo.set_url`（参数 `url`，必须以 `ws://` 或 `wss://` 开头）修改，改完立即拉一次。
- 构建期默认值：`config.h` 的 `MCP_TODO_URL`（提交版为空）。本机可把真实地址放进
  `managers/todo_mcp_secret.h`（已在 `.gitignore`，避免 token 入库），首次启动会把编译期地址落到 NVS，
  之后一律以 NVS 为准。
- 时序：上电延迟 20 s 首次拉取；之后每 60 s 一次；网络刚连上会立刻补一次。
- 日志关键字：`连接待办 MCP 接入点(1/3)` / `MCP 会话已建立` / `已拉取当日待办 N 条` /
  `等待 list_todos 响应超时` / `连接已断开` / `空闲内存 ... 最大连续块 ...`。
- 协议要点：JSON-RPC 的真实路径是 `/mcp_endpoint/call/`（`/mcp/` 是注册侧，握手不回包）；
  握手 `id=1` initialize、`id=2` tools/call，结果在 `result.content[0].text`（一个 JSON 字符串：
  `{"ok":true,"count":N,"items":[{"title","due_text","all_day",...}]}`）。

手动兜底：后端 AI 也可调用设备工具 `self.todo.sync`（`items` 为 JSON 字符串数组，最多 10 行，`[]` 清空）直接覆盖列表。

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
| SD 卡 | SDMMC 1-bit，CLK 38 / CMD 21 / D0 39（`06_SD_Card/components/port_bsp/sdcard_bsp.h`） | 未启用（白噪音功能已移除） | 引脚一致、固件不使用 |
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

## OTA 服务器与 DNS 兜底

- 默认 OTA 地址在板级 `config.json` 的 `sdkconfig_append` 里写死为 `CONFIG_OTA_URL="https://xiaozhiweb.hqgg.top:59826/xiaozhi/ota/"`（必须带结尾斜杠，服务端按该路径路由）。
- NVS `wifi/ota_url` 优先于编译期默认值（配网页「高级 → OTA 地址」可改）。实测踩坑：手动填入时若漏掉结尾斜杠或拼错路径，OTA 会拿到 `{"code":404,"msg":"资源不存在"}` 或 nginx `405`，设备表现为「检查新版本失败」循环。
- **DNS 兜底**：部分热点（实测 Windows 移动热点/ICS）DHCP 不下发 DNS，`esp_netif_get_dns_info` 读到 `0.0.0.0`，域名解析直接失败（`getaddrinfo() returns 202`）。`SetNetworkEventCallback()` 在 `NetworkEvent::Connected` 时检查主 DNS，若为 0 则写入公共 DNS（223.5.5.5 主 / 119.29.29.29 备），并打印 `DHCP 未下发 DNS，已兜底使用 223.5.5.5`。
- 排查日志：连接后打印 `STA main DNS / STA backup DNS`；OTA 失败时打印 `status code`、请求方法与 URL、响应体前 300 字节。
- 回滚：删除 `SetNetworkEventCallback()` 覆写即可（DNS 恢复由 DHCP 决定）；OTA 诊断日志在 `main/ota.cc`。
- 如果设备里已经存了错误的 `ota_url`（NVS 优先于编译默认值），两种修法：① 进配网 AP 页面重新填写正确地址；② 不改 WiFi 凭据的前提下离线改 NVS：`esptool.py read_flash 0x9000 0x6000 nvs.bin`，定位 `ota_url` 的变长条目（type=0x21，dataSize 在条目偏移 24，dataCrc32 在偏移 28），改字节后按 NVS 规则重算 dataCrc32 与条目 CRC32（`crc = zlib.crc32(bytes, 0xFFFFFFFF)`，无末尾取反），再 `write_flash` 回对应 4 KB 页。



## 屏幕截图（调试用）

设备连上网络后启动一个只读 HTTP 服务（端口 8080），直接把 RLCD 显存按 1bpp 导出成 BMP，方便远程核对界面：

- 串口在拿到 IP 后打印：`设备 IP: 192.168.137.104，截图地址: http://192.168.137.104:8080/shot.bmp`。
- `GET /shot.bmp` → 400×300 单色 BMP（15662 字节，62 B 文件头 + 52 B/行 × 300）。
- `GET /dbg` → 纯文本，输出 `chat_status_label_` 文本/隐藏/位置/尺寸、父容器尺寸、`emotion_label_` 文本、`emotion_img_` 状态、主题 emoji 集合数量，用于排查「控件不显示」。
- 主机侧脚本 `shot_fetch.py`：`python shot_fetch.py <设备IP> [out.png]`，取 BMP 后用 PIL 转 PNG（放大 2 倍便于查看）。
- 实现：`screenshot_server.{h,cc}` 起 `esp_http_server`（`lru_purge_enable=true`、栈 8192，BMP 缓冲放 PSRAM）；像素读取用 `RlcdDriver::RLCD_GetPixel(x, y)` 直接查显存 LUT，1 = 白、0 = 黑，因此截图就是面板真实画面。
- 注意：`main/CMakeLists.txt` 的 `PRIV_REQUIRES` 需要 `esp_http_server`；8080 与配网页 80 不冲突。

## AI 卡：内置猫脸（无 emoji 素材时的兜底）

- 小智的表情图片来自 assets 包（`main/assets.cc` 读 `emoji_collection` 数组），本板当前使用的 `generated_assets.bin` 里**不含 emoji 素材**，`theme->emoji_collection()` 为 null。
- 因此 weather_ui.cc 用 `CreateCatFace()` 以 `lv_obj`（圆脸/眼睛/鼻子）+ `lv_line`（耳朵/嘴/胡须）在 48×48 内画了一只猫脸线稿，位置与 `emotion_img_` 相同（AI 卡左侧、`LEFT_MID(12, -14)`），RLCD 只有黑白两色，线稿正好。
- `ApplyDefaultEmotion()` / `SetEmotion()` 的规则：assets 有对应表情图 → 显示 `emotion_img_`、隐藏 `ai_face_`；没有 → 隐藏 `emotion_img_`、显示 `ai_face_`。
- `chat_message_label_`（基类使用）指向一个**独立隐藏标签**，避免基类直接 `lv_label_set_text(chat_message_label_, "")` 把 AI 卡的 `chat_status_label_` 清空（这是之前右侧文字空白的根因）。
