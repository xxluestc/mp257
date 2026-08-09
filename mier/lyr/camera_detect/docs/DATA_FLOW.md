# radar_fusion 数据流说明

## 1. 总体架构

```text
┌─────────────────────────────────────────────────────────────────────────────┐
│                           radar_fusion (A35 Linux)                          │
├─────────────────────────────────────────────────────────────────────────────┤
│  主循环                                                                      │
│   ├── 摄像头 /dev/video7 ──MJPEG帧──┬──> DVR 缓冲 ──触发保存──> MP4 (TF卡)   │
│   │                                 │                                        │
│   │                                 └──> NPU 推理 (每10帧1次)                │
│   │                                              │                           │
│   │                                              v                           │
│   │                                    是否道路使用者?                        │
│   │                                              │                           │
│   ├── 雷达 /dev/ttySTM1 ──BSD目标───────────────┘                            │
│   │                    │                                                    │
│   │                    v                                                    │
│   │         雷达告警 + NPU确认                                                │
│   │                    │                                                    │
│   │                    +---> LED 闪烁 (PD11)                                 │
│   │                    +---> 播放碰撞告警音                                    │
│   │                    +---> DVR 保存前后15秒为 MP4                            │
│   │                                                                         │
│   ├── RPMsg /dev/ttyRPMSG0 (M33核) ──IMU_ALERT──> LED/DVR + HUD/App UDP      │
│   │                                                                         │
│   └── UDP 8888 (手机APP) ──导航/异常/预警──> nav_tts 线程                     │
│                              ├── navi:       OLED显示 + <=50m 转向播报         │
│                              ├── danger_tts: 异常路况语音播报                  │
│                              └── alert:      固定预警提示音                    │
└─────────────────────────────────────────────────────────────────────────────┘
```

## 2. 主循环数据流

主循环是单线程，核心工作：

1. **定时取帧**（25 FPS）
   - 从摄像头读取 MJPEG 帧
   - 帧时间戳基于程序启动时间 `g_t_start`

2. **DVR 缓冲**
   - 若正在录制 (`dvr_recording`)，把帧写入 TF 卡循环缓冲目录 `/run/media/mmcblk0p1/dvr/.buffer`
   - 缓冲长度约 `DVR_SAVE_BEFORE_SEC * FPS * 2` 帧，保证触发前 15 秒数据不丢

3. **NPU 推理**（每 10 帧 1 次）
   - JPEG 解码为 RGB
   - resize 到模型输入尺寸
   - `NpuDetector::detect()` 推理
   - 只关心 `ROAD_USER_CLASSES`：person、bicycle、car、bus、truck、motorcycle
   - 每次推理将目标类型、置信度、数量、耗时和确认状态写入 `sensor_events.csv`

4. **NPU 状态机**
   - 连续 `NPU_CONFIRM_FRAMES` 帧看到道路用户 → `npu_confirmed = 1`
   - 连续 `NPU_DENY_FRAMES` 帧看不到 → 判定为雷达虚警，`npu_denied = 1`
   - NPU 首次看到目标 → 启动 DVR 缓冲
   - NPU 丢失目标且未触发保存 → 停止 DVR 缓冲

5. **雷达处理**
   - 串口按 BSD 协议解析
   - 保留本帧最多 8 个 `objId` 的距离、速度、角度、TTC 与滤波方向
   - 按归一化 TTC/距离选择同一个当前危险目标
   - 输出 `radar_data.csv`，并原子更新 Dashboard 使用的 `radar_state.json`
   - 雷达发现目标时更新 `target_active` 和 `t_last_bsd`

6. **融合决策**
   - **有摄像头**：`radar.should_alert && npu_confirmed` → 真实碰撞风险
   - **纯雷达模式**：`radar.should_alert` → 直接告警
   - 触发后调用 `dvr_trigger_save()` 保存前后各 15 秒视频，并播放 `collision_alert.wav`

7. **LED 控制**
   - `g_led_alert = g_radar_npu_alert || g_imu_fall_alert`
   - LED 线程独立运行，根据 `g_led_alert` 闪烁 PD11

## 3. DVR 保存流程

```text
NPU 看到目标 ──────────────────────────────┐
                                           │
                                           v
                                   dvr_start() 开始缓冲
                                           │
                                           v
           雷达告警 + NPU 确认 ───> dvr_trigger_save() 标记触发
                                           │
                                           v
                                   继续录制 DVR_SAVE_AFTER_SEC 秒
                                           │
                                           v
                                   dvr_stop() 停止写入
                                           │
                                           v
                                   dvr_encode_mp4() 异步编码
                                           │
                                           v
                                   /run/media/mmcblk0p1/dvr/<name>.mp4
```

- 编码在 `fork` 子进程中执行，不阻塞主循环；当前优先使用 GStreamer 的
  `v4l2slh264enc` 生成 H.264/MP4，编码器不可用或失败时再走兼容路径
- 编码完成后主循环回收子进程并复位告警状态

## 4. RPMsg 与短信投递数据流

M33 核通过 `/dev/ttyRPMSG0` 发送两类消息：

| 消息前缀 | 含义 | 处理 |
|---|---|---|
| `IMU_ALERT` | `fall / hard_brake / road_bump` | 摔倒时置 `g_imu_fall_alert` 并保存 DVR；全部事件写同步日志并转发 HUD/App |
| `V2X:` | V2X 预警（如盲区来车） | 解析方向字段，播放对应方向 `v2x_*.wav`，2 秒冷却避免连播 |

HUD 对三类 IMU 事件分别执行 60 秒同类冷却，再广播到手机 UDP 8889。
该协议目前没有手机或短信回执，所以 LED 亮、开发板 UDP 成功都不能证明短信已发出。
详细阈值、已修复的跨类型冷却问题和排查方法见
[M33 摔倒判断与手机短信投递链路](FALL_SMS_PIPELINE.md)。

## 5. UDP 8888 导航/异常/预警数据流

由 `nav_tts.c` 独立线程处理，不影响主循环性能。

### 5.1 导航数据 `type=navi`

```json
{"type":"navi","turn":2,"distance":100}
```

- 更新 OLED 显示（方向箭头 + 距离）
- 距离 `<= 50m` 且为左转/右转/掉头/到达时，播放固定模板语音
- 同一转向 15 秒内不重复播报

### 5.2 完整路名 `type=navi_tts`

```json
{"type":"navi_tts","tts_type":1,"seq":12,"text":"前方100米右转进入人民路"}
```

- 优先查找本地缓存
- 缓存未命中时尝试 `edge-tts` 在线生成
- **无网络时不再自动兜底播报**，避免和 `type=navi` 结构化播报冲突

### 5.3 异常路况 `type=danger_tts`

```json
{"type":"danger_tts","phase":"preload","alert_type":"其他异常","text":"前方停车场出口，请减速观察","distance":67}
{"type":"danger_tts","phase":"trigger","alert_type":"其他异常","text":"前方26米停车场出口，请减速观察","distance":26}
```

- `phase=preload`：只记录，不播放
- `phase=trigger`：调用 `nav_tts_speak_danger(text)`
  - 优先按关键词匹配本地 `danger_*.wav` 固定提示音，**不尝试网络**
  - 每个关键词独立 8 秒冷却，不同类型互不阻塞
  - 未匹配到任何关键词/默认文件 → 尝试本地完整缓存
  - 在线 TTS 默认已禁用（`NAV_ONLINE_TTS_ENABLE 0`），不会再出现 edge-tts 失败日志
- 所有收到的 `danger_tts`（含 preload/trigger）都会去重记录到
  `/xxl/camera_detect/danger_tts_samples.log`，用于确认手机实际会发送哪些异常文案

当前手机端实际上报的本地异常提示音只有两种：

| 关键词 | 本地文件 | 播报内容 |
|---|---|---|
| 障碍物 | `danger_障碍物.wav` | 前方路面有障碍物，请注意避让 |
| 施工 | `danger_施工.wav` | 前方施工路段，请减速慢行 |

未匹配到这两种已知关键词时直接跳过，不再播放默认提示。后续手机端若新增异常类型，只需在 `nav_tts.c` 的 `g_danger_hints` 和 `nav_tts_cache/` 中同步添加即可。

### 5.4 固定预警 `type=alert`

```json
{"type":"alert","message":"前方车辆靠近"}
```

- 播放 `alert_前方车辆靠近.wav` 或 `alert_请注意.wav`
- 10 秒冷却

## 6. 线程列表

| 线程 | 源文件 | 职责 |
|---|---|---|
| 主循环 | radar_fusion.cpp | 摄像头、雷达、NPU、DVR、LED 状态融合 |
| led_thread | radar_fusion.cpp | 根据 `g_led_alert` 闪烁 PD11 |
| rpmsg_thread | radar_fusion.cpp | 接收 M33 核 IMU/V2X 告警 |
| nav_recv_thread | nav_tts.c | UDP 8888 接收手机数据 |
| nav_watchdog_thread | nav_tts.c | 5 秒无导航信号则 OLED 显示 NO DATA |
| aplay_thread | nav_tts.c | 串行播放 WAV，避免多路音频抢设备 |
| nav_tts_generate_thread | nav_tts.c | 后台调用 edge-tts 生成完整语音 |
| test_fall_thread / test_v2x_thread | radar_fusion.cpp | 命令行测试线程（-t / -V） |

## 7. 关键状态变量

| 变量 | 含义 | 设置者 | 使用者 |
|---|---|---|---|
| `g_radar_npu_alert` | 雷达+NPU 确认碰撞风险 | 主循环 | LED、DVR 触发 |
| `g_imu_fall_alert` | IMU 摔倒告警 | RPMsg 线程 | LED、DVR 触发 |
| `g_v2x_alert` | V2X 告警 | RPMsg 线程 | 语音播报 |
| `g_led_alert` | LED 是否闪烁 | 主循环 | led_thread |
| `npu_has_target` | NPU 当前是否看到道路用户 | 主循环 | DVR 缓冲控制 |
| `npu_confirmed` | NPU 已确认真实目标 | 主循环 | 雷达告警融合 |
| `dvr_recording` | 是否正在 DVR 缓冲 | dvr_start/stop | 主循环写帧 |
| `dvr_save_triggered` | 是否已触发保存 | dvr_trigger_save | post-trigger 计时 |

## 8. 源码文件职责

| 源文件 | 类型 | 核心职责 |
|---|---|---|
| [radar_fusion.cpp](../radar_fusion.cpp) | C++ | 主程序：雷达 BSD 解析、摄像头采集、NPU 道路用户验证、DVR 缓冲/编码、LED/音频告警、RPMsg 告警、导航 UDP 集成 |
| [dashboard/radar_dashboard.py](../dashboard/radar_dashboard.py) | Python | 本地 Web/API 服务，合并雷达/NPU/IMU/投递时间线并写入 `labels.csv` |
| [RADAR_EXPERIMENT.md](RADAR_EXPERIMENT.md) | 文档 | 雷达阈值、Dashboard、CSV 字段与现场标注方法 |
| [nav_tts.c](../nav_tts.c) | C | HUD 导航 UDP 接收、OLED 显示、骨传导语音播报（含 danger_tts 本地兜底） |
| [nav_tts.h](../nav_tts.h) | C 头 | 导航语音接口与 danger_tts handler 类型定义 |
| [camera.c](../camera.c) | C | V4L2 摄像头打开、启动、采集、关闭封装 |
| [camera.h](../camera.h) | C 头 | `camera_t` 结构与摄像头 API 声明 |
| [npu_detect.cpp](../npu_detect.cpp) | C++ | STM32MP2 NPU 模型加载、推理、SSD MobileNet V2 后处理（NMS） |
| [npu_detect.h](../npu_detect.h) | C++ 头 | `NpuDetector` 类与检测结果结构定义 |

## 9. 本地音频资产

由于开发板无网络，所有语音播报优先使用本地预存 WAV，避免在线 TTS 失败导致无声音：

| 目录 | 用途 | 代表文件 |
|---|---|---|
| `nav_tts_cache/base_*.wav` | 方向兜底提示 | `base_左转.wav`、`base_右转.wav` |
| `nav_tts_cache/nav_前方*.wav` | 结构化导航模板（<=50m 触发） | `nav_前方50米右转.wav` |
| `nav_tts_cache/alert_*.wav` | 固定预警提示 | `alert_前方车辆靠近.wav` |
| `nav_tts_cache/danger_*.wav` | 异常路况固定提示 | `danger_障碍物.wav`、`danger_施工.wav` |
| `sounds/*.wav` | 系统级告警 | `fall_alert.wav`、`collision_alert.wav`、`recording_complete.wav`、`v2x_*.wav` |

生成脚本：
- `scripts/gen_danger_sounds.sh`：批量生成 `danger_*.wav`（需要 edge-tts + ffmpeg，在联网 PC 上执行）
- `scripts/gen_nav_tts.sh`：在线生成任意导航文案缓存（开发板无网络时不使用）

## 10. 部署目录

```text
/xxl/camera_detect/
├── radar_fusion                 # 可执行文件
├── models/
│   ├── ssd_mobilenet_v2_fpnlite_10_256_int8_per_tensor.nb
│   └── labels_coco_dataset_80.txt
├── scripts/
│   ├── gen_nav_tts.sh           # 在线生成导航 TTS
│   └── gen_danger_sounds.sh     # 批量生成本地异常提示音
├── sounds/
│   ├── fall_alert.wav
│   ├── collision_alert.wav
│   └── v2x_*.wav
└── nav_tts_cache/
    ├── base_*.wav               # 方向兜底提示
    ├── nav_前方*.wav            # 导航结构化模板
    ├── alert_*.wav              # 固定预警
    └── danger_*.wav             # 异常路况固定提示
```
