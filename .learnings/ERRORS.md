# ERRORS

## [ERR-20261122-001] 本地 VAD 打断在 TTS 回放期间自打断

- **现象**: 将 `kLocalVadBargeInEnabled` 从 false 改为 true 后，设备刚开始播报问候语约 2 秒，就打印 `本地 VAD 打断：检测到用户持续说话`、`Abort speaking`，状态从 `speaking` 回到 `listening`；此时用户还没有说“停止”。
- **环境**: waveshare/esp32-s3-rlcd-4.2，firmware 2.7.3，COM7；`kLocalVadBargeInWarmupUs = 2000ms`，`kLocalVadBargeInHoldUs = 800ms`。
- **关键日志**: `D:/dsh/esp32/.tmp_dev/cap_localvad_orch.log` 第 253-259 行：
  - `10.271 Application: << 我认真听着呢，请讲。`
  - `12.271 Application: 本地 VAD 打断：检测到用户持续说话`
  - `12.271 Application: Abort speaking`
  - `12.271 StateMachine: State: speaking -> listening`
- **根因**: 触发点正好是 TTS 起播后 2 秒，即 warmup 结束时刻；AEC 未能完全消除扬声器回放的残余回声，VAD 将其误判为人声。启动日志同时有 `Server sample rate 16000 does not match device output sample rate 24000, resampling may cause distortion`，重采样会进一步削弱 AEC 参考信号。
- **结论**: 当前音频/AEC 参数下不能只把 `kLocalVadBargeInEnabled` 改成 true，否则设备会自打断；需先修正 AEC/采样率链路，或提高 warmup/hold 并实机验证，不能仅凭编译通过启用。
- **处理**: 已回退 `kLocalVadBargeInEnabled = false`，重建并烧录稳定版；服务端全双工打断仍正常。

### 2026-11-28 复发记录
- **现象**: `kLocalVadBargeInEnabled` 又被置为 true，实测串口 `cap_light_20261128c.log`：TTS 播报 175.1s，177.9s 首次本地 VAD 打断，179.2s 把回声识别成“您需要什么帮助？”，随后每约 2 秒自打断并重复调用 control_device；ASR 乱码“激果”被 LLM 推断成“关掉它”并实际关灯。
- **启动确认**: `boot_aec_20261128.log` 的 AFE Pipeline 初次为 `AEC(FD_LOW_COST, NLP_VERY_AGGRESSIVE) -> VAD -> WakeNet`，说明 AEC 配置存在；但残余回声仍足以让本地 VAD 误判。
- **处理**: 只把 `main/application.cc:40` 的 `kLocalVadBargeInEnabled` 改回 false，其余音乐续播/AEC 实验保留；构建 4,320,656 B sha256 1341eef9ca74c380ddb9877e261657b327be00e5b5a9a8354a1c1908d8ad9c64，烧录 COM7 Hash verified。

### 2026-11-28 后续发现：本地 VAD 关闭后服务端 ASR 仍拾取回声
- **现象**: `cap_live_20261128.log` 第 69-135 行，一次约 108 秒的长故事对话里，设备 TTS 被服务端 ASR 识别成用户输入，触发自问自答：
  - `192.705 << 时间过得真快，刚才还想着给你讲个故事呢，`
  - `195.942 >> 时间过得真快。刚才`
  - `197.971 << 时间过得真快，刚才还在这儿说着话呢，一眨眼就到了收尾的当口。`
  - `206.031 << 从前有一只小蜗牛……背上的壳有一道金黄色的花纹……`
  - `213.850 >> 背上的可有一道金黄色的`
  - `228.824 >> 截图`（把“看仔细”误识别成“截图”）
- **根因**: 关闭 `kLocalVadBargeInEnabled` 只阻止设备侧 `Abort speaking`；当 `aec_mode_=kAecOnDeviceSide` 时 `listening_mode_=kListeningModeRealtime`，TTS 播放期间仍持续上行麦克风音频，AEC 残余回声被服务端 ASR 当成用户语音，服务端继续生成新一轮回复。
- **待办**: 需修正 AEC 参考/延迟对齐，或在 AEC 未达标时改为半双工（TTS 播放期间静音上行麦克风）；不能仅靠关闭本地 VAD 解决。

### 2026-11-28 实施半双工（方案1）
- **改动**: main/application.cc:1373-1385 Listening 分支所有模式都先判断 `!audio_service_.IsPlaybackIdle()`，未排空则 `pending_listening_start_=true`，不再边播边开麦；main/application.cc:1393-1405 Speaking 分支无条件 `EnableVoiceProcessing(false)`，仅保留 `EnableWakeWordDetection(IsAfeWakeWord())` 和 `ResetDecoder()`。
- **构建/烧录**: `build/xiaozhi.bin` 4,320,640 B sha256 `932323ba8b3afdd228fe78810a6e02f14efb31e16922535c50ba080b7907a905`；COM7 write-flash 0x20000，Hash of data verified。
- **实测**: `voice_halfduplex.log` 用 PC 扬声器（Microsoft Huihui）在 `/wake` 后说“请给我讲一个三百字的长故事”；12.199s `>> 请给我讲1个300`，18.050s 起连续 TTS 到 178s+（`voice_halfduplex_end.log`），全程无 `>>` 上行、无 `本地 VAD 打断`、无 `Abort speaking`，服务端不再自问自答。
- **代价**: TTS 播放期间不能用普通语音打断，只能靠唤醒词打断；若后续要求全双工打断，需先修正 AEC 参考/延迟对齐。

## [ERR-20261123-001] ops_edit_file 保留 mtime 导致 Ninja 不重建

- **现象**: 修改源码后执行 `scripts/build.py` 返回 exit 0、日志有 `Project build complete`，但 `build/xiaozhi.bin` 的 sha256 与修改前完全一致，改动未生效。
- **根因**: `ops_edit_file` 保留原文件 mtime，Ninja 以 mtime 判断是否需要重编译，因此没有触发编译。
- **处理**: 编辑后必须 `touch` 所有改动文件，再重新构建；已用此方法让二进制 hash 从 a32b7... 变为 b7bde0...。

## [ERR-20261123-002] 组件网络请求 IncompleteRead 导致 CMake 配置失败

- **现象**: 构建在 `CMake Error at .../build.cmake:707` 报 `HTTP request error ('Connection broken: IncompleteRead(97700 bytes read, 12885 more expected)')`，URL 为 `https://components-file.espressif.com/components/espressif/esp-sr.json`，构建失败。
- **根因**: 组件管理器拉取 esp-sr 组件清单时网络连接中断，属外部网络抖动，不是代码错误。
- **处理**: 直接重试同一构建命令，第二次成功；无需修改工程文件。

## [ERR-20261124-001] git add dist 被 .gitignore 忽略

- **现象**: 发布脚本执行 `git add dist/merged-binary-landscape.bin dist/merged-binary-portrait.bin` 报 `The following paths are ignored by one of your .gitignore files: dist`，提交未生成，随后 `git tag` 也因前一步失败未执行。
- **根因**: 仓库 .gitignore 忽略 `dist`，但历史 release 又需要把两个整片镜像纳入版本库；对已跟踪路径显式 add 仍受 ignore 规则拦截。
- **处理**: 使用 `git add -f dist/merged-binary-landscape.bin dist/merged-binary-portrait.bin`，再提交并打 tag；发布前确认 `git status` 仅剩预期文件。

## [ERR-20261125-001] lv_obj_get_style_pad_row 第二个参数是 lv_part_t

- **现象**: 在 `custom_lcd_display.cc` 用 `lv_obj_get_style_pad_row(memo_rows_, 0)` 读取行间距，编译报 `error: invalid conversion from 'int' to 'lv_part_t'`。
- **根因**: LVGL 9 的 `lv_obj_get_style_pad_row(obj, part)` 第二个参数是部件选择器枚举，不是整数 0。
- **处理**: 改为 `lv_obj_get_style_pad_row(memo_rows_, LV_PART_MAIN)`，重新构建通过。

## [ERR-20261126-001] 15px 全量中文点阵字库撑爆 app 分区

- **现象**: 待办区新增 15px 粗体字库后，构建报 `Error: All app partitions are too small for binary xiaozhi.bin size 0x52d7b0: ... Part 'ota_0' ... size 0x4f0000 (overflow 0x3d7b0)`，二进制 5,431,216 B，超出 ota_0 约 246 KB。
- **根因**: 用 `lv_font_conv -r 0x0-0xfffff` 生成全量 SimHei 15px/1bpp 字库，编译对象 1,123,416 B；仅新增字库就把二进制从 4.3 MB 推到 5.4 MB。
- **处理**: 改为 GB2312 一级常用字（3755 字）+ ASCII/标点子集，生成对象 167,184 B；并在生成的 C 文件里把 `.fallback = NULL` 改为 `.fallback = &font_puhui_16_1_full`，生僻字回退 16px 全字库。最终 build/xiaozhi.bin 4,474,320 B，正常烧录。
- **经验**: 新增中文点阵字库前先按 `ota_0` 剩余空间估算；动态文本字体必须保留 fallback，不能只做固定文案子集。
