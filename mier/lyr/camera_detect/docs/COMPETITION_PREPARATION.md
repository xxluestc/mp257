# 比赛准备：事件链、关键节点与现场讲解

本文用于比赛答辩、作品演示和现场排障。它不按 Linux 知识点组织，而是从“现场
发生了什么事件”出发，说明事件从哪里产生、经过哪些接口、在哪一层做判断、最后
触发什么动作，以及每一跳如何验证。

学习 Linux 和准备面试仍以 [项目技术知识库](PROJECT_TECHNICAL_KNOWLEDGE_BASE.md)
和 [面试准备](INTERVIEW_PREPARATION.md) 为主；本文只解决比赛现场最常遇到的三个
问题：

1. 这个现象背后走的是哪条链？
2. 演示到哪一步，能证明什么？
3. 如果最后一个动作没有发生，应从哪一跳开始查？

## 1. 阅读规则：先区分三种事实

比赛材料、当前仓库和开发板运行版本并不完全等价。本文使用以下标记：

- **当前实装**：当前 A35/WBA 源码中存在，并有板端或专项文档验证。
- **作品设计**：作品报告中描述的完整系统方案，可能还依赖不在本仓库中的
  M33、手机 App 或云端实现。
- **待闭环**：本地链路已经发送，但缺少对端 ACK、现场标定或完整场景验证。

答辩时可以先讲完整作品目标，再落到“当前原型是如何实现的”。不要把设计图中的
每一条箭头都说成当前仓库已经完成了端到端闭环。

## 2. 一张图记住整个系统

```text
                           ┌──────── 手机 App / 云端 ────────┐
                           │ 导航、道路风险、紧急联系人       │
                           │ UDP 8888             UDP 8889   │
                           ▼                                ▲
摄像头 /dev/video7 ──> A35 Linux radar_fusion ──UDP 8890──> HUD
  USB UVC/MJPEG          │      │       │                    │
                         │      │       ├── OLED/HUD          │
毫米波雷达 /dev/ttySTM1 ─┘      │       ├── 骨传导/ALSA       │
  BSD目标、距离/速度/角度       │       ├── PD11告警灯        │
                                │       └── 外置TF事件录像    │
                                │
M33 IMU/V2X ─OpenAMP/RPMsg──────┘
  /dev/ttyRPMSG0

A35最终碰撞风险 ─USART2 /dev/ttySTM0─> CH9140 Peripheral
                                      ))) BLE (((
                              STM32WBA54 Central
                                ├── PA7 左灯
                                └── PA5 右灯
```

讲解时按四层展开：

```text
感知输入 → 事件判断 → 跨模块传递 → 人可感知的输出/数据留存
```

不要从“用了哪些模块”开始罗列。先说场景，再沿数据走向说明模块为什么存在。

## 3. 当前实机关键节点

### 3.1 设备和接口

| 节点 | 当前用途 | 驱动/协议 | 关键检查 |
|---|---|---|---|
| `/dev/video7` | 720P MJPEG采集 | USB 2.0 UVC、`uvcvideo` | `v4l2-ctl -d /dev/video7 --all` |
| `/dev/ttySTM1` | 60GHz雷达 | 921600、二进制BSD帧 | 主程序雷达首帧与ACK日志 |
| `/dev/ttyRPMSG0` | M33事件 | OpenAMP/RPMsg TTY | M33 `running`、ready消息 |
| `/dev/ttySTM0` | CH9140方向灯链路 | 115200 8N1、文本命令 | `PONG`、`ACK RISK ...` |
| `/dev/gpiochip3` | A35本机告警灯 | GPIO字符设备 | PD11闪烁 |
| `/usr/local/helmet` | CSV实验数据 | 板载userfs/ext4 | 已挂载且可写 |
| `/dev/mmcblk0[p1]` | DVR事件录像 | 外置TF，自动识别布局 | 已真实挂载、可写且存在`dvr`目录 |

`radar_fusion` 使用 Sonix/Microdia `0c45:636b` USB UVC 摄像头，对应当前
`/dev/video7`。比赛讲摄像头采集时，以这条实机链路为准。

### 3.2 进程和端口

| 进程/服务 | 职责 | 端口/文件 |
|---|---|---|
| `dvr.service` / `radar_fusion` | 雷达、摄像头、NPU、RPMsg、DVR和本地告警 | 设备节点、CSV/JSON |
| `hud` | OLED导航、IMU事件转发到手机 | UDP 8888、8890、8889 |
| `radar-dashboard.service` | 状态展示、事件时间线、人工标签 | TCP 8080 |
| `hostapd` + `dnsmasq` | 手机连接头盔热点并获取地址 | Wi-Fi AP/DHCP |
| `helmet-ota.service` | A35应用升级 | TCP 8090 |

当前 `hud` 和 `radar_fusion`都涉及 UDP 8888。现场必须确认手机发包方式和实际接收
日志，不要仅凭“端口已监听”认定 OLED 与语音两条消费路径都收到了同一个报文。

## 4. 事件总表

| 现场事件 | 第一判断点 | 当前主要输出 | 最重要的证据 |
|---|---|---|---|
| 侧后方目标出现 | 雷达BSD解析、NPU道路使用者状态 | Dashboard状态、必要时DVR预缓存 | `TARGET ON`、NPU target日志 |
| 侧后方碰撞风险 | 雷达阈值 + NPU连续确认 | PD11、碰撞音、DVR、WBA方向灯 | `fusion_alert=true`、`RISK ...` ACK |
| 目标离开 | 雷达3秒超时、NPU连续否定 | 清碰撞状态、`RISK CLEAR` | `TARGET GONE`、ACK CLEAR |
| 摔倒 | M33姿态判断 | 摔倒音、PD11、DVR、HUD→手机 | 同一`event_id`跨CSV |
| 急刹 | M33 IMU判断 | 当前主要转发HUD/手机 | `hard_brake`投递记录 |
| 路面颠簸 | M33 IMU判断 | 当前主要转发HUD/手机 | `road_bump`及同类冷却 |
| V2X协同危险输入 | M33发`V2X_ALERT` | 方向对应骨传导语音 | `V2X_ALERT direction=...` |
| 手机导航 | App生成`navi`/`navi_tts` | OLED/HUD、导航语音 | UDP接收和OLED/播放日志 |
| 云端/手机道路风险 | `danger_tts`阶段与关键词 | 本地固定异常提示音 | preload/trigger日志 |
| 录像触发完成 | DVR触发后继续采集15秒 | H.264/MP4文件 | trigger、encoder exit、文件 |

下面逐条展开。

## 5. 上电到业务可用

### 5.1 当前启动链

```text
BootROM → TF-A → OP-TEE → U-Boot → Linux kernel
  → systemd/udev
      ├── dvr-m33.service → remoteproc启动M33
      ├── USB/UART/GPIO/TF/Wi-Fi设备就绪
      ├── radar-dashboard.service
      └── dvr.service → start_dvr.sh
            ├──检查具体业务设备
            ├──确认TF完成fsck并挂载
            ├──复用已运行M33/RPMsg
            ├──启动HUD
            └──启动radar_fusion
                  ├──雷达初始化
                  ├──摄像头STREAMON
                  ├──NPU模型加载
                  ├──RPMsg线程
                  └──导航接收线程
```

### 5.2 2026-08-09只读实机观察

本次板端RTC仍不是可靠比赛时间源，启动分析使用内核单调时间：

| 里程碑 | 本次时间 | 含义 |
|---|---:|---|
| Linux进入userspace | 2.029 s | kernel阶段结束 |
| M33 early service完成 | 约3.9 s | remoteproc链可用 |
| USB摄像头总线发现 | 1.288 s | 硬件已经枚举到USB总线 |
| `uvcvideo`识别摄像头 | 8.276 s | 媒体模块完成加载和绑定 |
| `/dev/video7` udev完成 | 8.288 s | 应用设备节点可用 |
| `dvr.service`脚本开始 | 8.843 s | 摄像头已提前约0.56秒就绪 |
| `radar_fusion`入口 | 10.896 s | 主程序启动 |
| 雷达初始化完成 | 12.181 s | 四条命令及稳定间隔结束 |
| 摄像头开始流式采集 | 12.561 s | `VIDIOC_STREAMON`完成 |
| NPU模型加载完成 | 12.782 s | 融合核心初始化完成 |
| 运行态ready | 13.126 s | 导航线程等已启动 |
| 摄像头首帧 | 13.164 s | 实际采集链闭环 |
| RPMsg ready发送 | 13.783 s | 含固定1秒等待 |

这说明“摄像头8秒出现”不是摄像头硬件耗时8秒。USB主控在0.58秒就绪，摄像头
在1.29秒已被总线发现，后面的时间主要消耗在udev冷插拔队列和按需加载
`mc`、`videodev`、`uvcvideo`等模块。当前它早于`dvr.service`，没有落在这次业务
关键路径上。

`dvr.service`晚启动的实际链路是：`rng-tools.service`等待OP-TEE提供的
`/dev/hwrng`到8.258秒，随后`sysinit.target`在8.303秒、`basic.target`在8.377秒
到达，Dashboard启动后才轮到DVR。脚本起来后又等待本次带dirty bit的TF卡完成
fsck和挂载，直到10.518秒才继续。比赛解释启动时要把“systemd基础链”“存储等待”
和“应用初始化”分开，不要把时间相近的USB摄像头枚举误认为服务依赖。

后续曾将`optee_rng`提前加载并连续重启三次：`sysinit.target`中位数提前约3.5秒，
但Fusion ready中位数没有改善，摄像头节点反而更晚。原因是基础服务更早启动后与
尚未完成的udev/存储初始化发生资源争用。该方案已经回退。比赛讲这一点时，重点是
“优化指标必须是业务ready，而不是某个target；无业务收益的改动应主动撤销”。

### 5.3 现场怎样判断真正ready

不能只说`multi-user.target`到了。完整业务至少检查：

```text
M33 running
  + /dev/ttyRPMSG0存在
  + radar_fusion运行
  + camera_first_frame出现
  + NPU日志持续更新
  + Dashboard stale=false
  + TF可写
```

## 6. 侧后方目标与碰撞风险链

这是比赛最核心的一条本地感知链。

### 6.1 雷达侧

```text
AT6010雷达
  → /dev/ttySTM1，921600
  → radar_fusion读取字节流
  → 按帧头、长度和sum8校验拆帧
  → 解析BSD多目标
  → 每个objId得到距离、速度、角度
  → 接近目标计算TTC = distance / -velocity
  → 根据TTC/距离阈值产生radar.should_alert
```

当前现场配置：

- TTC阈值：`2.5 s`；
- 距离阈值：`1.2 m`；
- LEFT：映射后角度`<= -10°`；
- RIGHT：映射后角度`>= 10°`；
- 方向角EMA系数：`0.35`；
- 边界滞回：`2°`；
- 连续3次候选方向后切换稳定方向。

多个目标同时存在时，程序按
`min(distance / distance_threshold, TTC / TTC_threshold)`选择当前最危险目标，
并确保最终显示的距离、TTC、方向来自同一个`objId`。

### 6.2 摄像头/NPU侧

```text
USB UVC摄像头
  → /dev/video7输出1280×720 MJPEG，25 FPS
  → V4L2 MMAP取帧
      ├──需要录像时写DVR缓冲
      └──每10帧取1帧
            → JPEG解码为RGB
            → resize到模型输入
            → SSD MobileNet V2 NPU推理
            → NMS/类别过滤
            → 连续状态确认
```

当前只把模型中的道路使用者类别用于业务确认。连续2次推理看到目标后设置
`npu_confirmed`；连续3次推理未看到后设置`npu_denied`。这不是摄像头与雷达的
像素级联合标定，而是两条感知链各自形成状态后再做决策级融合。

### 6.3 最终触发条件

有摄像头时：

```text
radar.should_alert
  AND npu_confirmed
  AND NOT npu_denied
    → g_radar_npu_alert = 1
```

触发后当前实装动作：

1. `g_led_alert`置位，独立线程闪烁A35本机PD11；
2. 播放`collision_alert.wav`；
3. DVR进入保存状态；
4. 按危险目标方向向CH9140发送`RISK LEFT/RIGHT/CENTER`；
5. 更新`radar_state.json`、`radar_data.csv`、`sensor_events.csv`和Dashboard。

摄像头不可用时保留radar-only降级判断，但没有视觉确认和DVR。比赛现场应把它说成
“故障降级路径”，不能说与完整融合模式效果相同。

### 6.4 目标消失

雷达连续3秒没有新的目标报告时：

```text
TARGET GONE
  → target_active=0
  → g_radar_npu_alert=0
  → PD11碰撞状态清除
  → BLE发送RISK CLEAR
  → Dashboard发布空闲心跳
```

如果DVR已经触发，不会因为目标消失立即停止。录像仍按触发后15秒完成，避免丢掉
事故后过程。

### 6.5 演示关键证据

```text
雷达收到目标：      TARGET ON
危险目标变化：      target=... obj=... dist=... ttc=... dir=...
NPU发现目标：       NPU: ROAD USER DETECTED
NPU连续确认：       NPU CONFIRMED
最终风险：          ALERT: COLLISION RISK - NPU CONFIRMED
录像触发：          [DVR] Save triggered
外部方向灯：        RISK LEFT/RIGHT/CENTER + 对应ACK
风险清除：          TARGET GONE + ACK RISK CLEAR
```

Dashboard上`radar_alert`只是雷达阈值结果，`fusion_alert`才是最终融合结果。这两个
状态必须区分。

## 7. 外接WBA方向灯链

这条链消费的是上一节已经确认的最终风险，不重新做目标判断：

```text
g_radar_npu_alert + dangerous.direction
  → ble_risk_update()
  → /dev/ttySTM0，115200 8N1
  → CH9140 UART输入
  → CH9140 BLE Peripheral：FFF1 Notify
  → STM32WBA54 BLE Central
      → 解析RISK文本
      → PA7/PA5控制灯
      → FFF2回写ACK
  → CH9140 UART输出
  → MP257记录ACK
```

| 风险状态 | WBA动作 | 返回 |
|---|---|---|
| `RISK LEFT` | PA7亮、PA5灭 | `ACK RISK LEFT` |
| `RISK RIGHT` | PA5亮、PA7灭 | `ACK RISK RIGHT` |
| `RISK CENTER` | 两灯都亮 | `ACK RISK CENTER` |
| `RISK CLEAR` | 当前风险灯保持1秒后灭 | `ACK RISK CLEAR` |

断线或WBA复位时两灯默认熄灭，MP257串口暂不可用时每2秒重试，不阻塞雷达主循环。

必须准确说明当前E04工程边界：当前
[`E04-2G4M10S1AX`](../../../../E04-2G4M10S1AX/README.md)固件是“WBA作为BLE
Central连接CH9140并控制方向灯”的专用工程，旧GNSS、IMU、语音和完整V2X状态广播
业务已经移除。它可以作为BLE协同终端和风险输出链的实装证明，但不能单独证明作品
报告中完整的双车位置/速度广播算法已经在当前E04固件里运行。

### 7.1 不要混淆独立`V2V_keil`语音终端

本机还收到了一份独立工程：
`/home/alientek/dvr_project/V2V_keil/E04-2G4M10S1AX`。它不是当前CH9140方向灯
工程，而是“WBA终端直接BLE广播/扫描双车状态，结合GPS/IMU计算相对位置，再通过
UART2复用板驱动MP3语音”的另一条试验链。两者内部目录名相同，但不能混烧：

| 工程 | 无线链路 | 风险来源 | 输出 |
|---|---|---|---|
| 根目录`E04-2G4M10S1AX` | CH9140 Peripheral ↔ WBA Central | MP257雷达/NPU最终风险 | PA7/PA5方向灯 |
| `V2V_keil/E04-2G4M10S1AX` | 两个WBA终端广播并扫描协议v2状态 | 两端GPS/IMU、距离趋势和状态位 | UART2 MP3语音 |

2026-08-10代码和CX-SP5F手册审查确认原版有两类独立循环问题。应用层只有3秒冷却，
播放后同一风险候选没有锁存，会每隔约3秒重新入队；模块层只收到`0x07`指定曲目帧，
若CX-SP5F处于模式1“全部循环”，一次入队也会从指定曲目开始循环播放后续文件。设置
启动曲目6后接着听到7、8，属于第二类问题。

`v1.8.1`已把`bridgeVoiceReportValid`接入事件锁存：同一连续风险只播报一次，
风险连续解除3秒才重新武装，对端超时立即清除；GPS降级提示要求RSSI有效；开机不再
播放曲目1；只有成功入队才打印`ALERT_QUEUE`。

当前`v1.8.2`在每次播报前再发送“停止→模式6单曲播放完停止→指定曲目”三帧，消除
模块模式残留造成的连续播放。风险工程只请求曲目1～5，启动曲目默认0，不会用风险曲目
做开机问候。当前`build.log`显示最近记录的
一次构建因对象文件权限问题失败，必须在Keil执行无错误Rebuild，并在启动日志确认
`V2X Node v1.8.2`后再测试。

比赛前的正确验收标准是：同一风险持续超过10秒仍只提示一次；风险解除不足3秒又恢复
时不重复；连续解除至少3秒出现`ALERT_REARM`后，再次风险才允许第二次提示。

两端固件还必须是互补ID：当前源码默认A端`local=0x1001 / expected=0x1002`，B端要
单独改成`local=0x1002 / expected=0x1001`后构建。两块板烧同一份默认HEX不会互相处理，
不能误判为射频或GPS故障。

完整验收不要只听语音，应保存USART1日志并逐层核对：

| 测试 | 期望日志/结果 |
|---|---|
| 对端广播正常 | 序号变化，没有2秒超时 |
| GPS和航向有效 | `WARN`含方向、`y_front_cm/x_right_cm`和heading来源 |
| 左/右/左前/右前 | `ALERT_SRC prompt=`与真实摆放方向一致 |
| 同一风险保持10秒 | 仅一次`ALERT_QUEUE`和一次`[VOICE] play`，语音只播一首后停止 |
| 安全不足3秒又恢复 | 不产生第二次语音 |
| 安全连续3秒 | 出现`ALERT_REARM` |
| GPS降级 | 仅在对端运动且有效RSSI≥-75 dBm时提示一次附近来车 |
| 对端断电超过2秒 | 旧滤波、趋势和语音锁存被清除 |

方向演示还要考虑算法边界：目标在本机后方超过2 m时设计为不提示；当前没有CENTER和
纯正前方曲目，只在左、右、左前、右前之间选择；压缩GPS的米级量化会影响近距离方向。
因此比赛现场应预留数米横向间距，避免把终端摆在左右滞回边界附近。

引脚、协议字段、状态位、阈值和完整调试顺序以独立工程`README.md`为准；MP257侧
[DATA_FLOW.md](DATA_FLOW.md)仅保留跨工程数据流索引。

## 8. 摔倒事件链

### 8.1 M33判断

当前记录的M33判断条件：

- IMU姿态数据有效且年龄不超过500 ms；
- `abs(roll) >= 6000`或`abs(pitch) >= 6000`；
- 倾倒姿态持续2000 ms；
- 形成`IMU_ALERT type=fall reason=tilt_hold ...`。

当前最终摔倒条件主要是持续倾倒，不要求先检测冲击。因此安装角度、姿态零点和静态
大角度倾斜都会影响结果，比赛前必须按实际头盔安装姿态标定。

### 8.2 完整投递链

```text
M33 IMU判断
  → IMU_ALERT type=fall ... seq=...
  → OpenAMP/RPMsg
  → /dev/ttyRPMSG0
  → A35 rpmsg_thread
      ├── handle_fall_trigger()
      │    ├── g_imu_fall_alert=1
      │    ├── fall_alert.wav
      │    ├── PD11闪烁
      │    └── DVR触发/补启动
      ├── 生成event_id=m33-seq-timestamp
      ├── sensor_events.csv
      └── UDP 127.0.0.1:8890
            → HUD
              ├── imu_delivery.csv
              ├──同类60秒冷却
              └── UDP广播192.168.152.255:8889
                    → 手机App
                      → 应急联系人/短信接口
```

### 8.3 每一跳能证明什么

| 观察结果 | 能证明 | 不能证明 |
|---|---|---|
| M33输出`IMU_ALERT` | 姿态判断成立 | A35已处理 |
| PD11亮、摔倒音播放 | A35进入本地摔倒状态 | 手机收到 |
| `sensor_events.csv`有`imu_m33` | RPMsg到A35 | HUD已广播 |
| `a35_hud status=sent` | 数据交给本机UDP栈 | HUD已消费 |
| `hud_received` | HUD收到并解析 | 手机收到 |
| `app_broadcast status=sent` | HUD广播调用成功 | 手机或短信成功 |

当前手机App源码和短信成功ACK不在本仓库，所以最后一跳仍是待闭环。答辩时应说
“开发板侧已经记录到UDP广播，手机最终短信结果还需要App回执才能端到端确认”。

## 9. 急刹和路面颠簸

### 9.1 急刹

M33当前记录以Y轴相对IIR基线变化为主，普通事件需在1.5秒内连续确认，强事件可直接
成立。消息为：

```text
IMU_ALERT type=hard_brake reason=... seq=...
```

A35映射为`type=emergency_brake`，记录后经8890→HUD→8889转发手机。

**当前实装边界**：`radar_fusion`只对`type=fall`调用本地摔倒处理。急刹当前不会在
A35自动复用摔倒的PD11/DVR触发逻辑。作品报告中的“急刹灯语和事件录像”属于完整
作品设计或M33/其他端动作，不能用当前A35源码中的这条分支直接证明。

### 9.2 路面颠簸

M33对Z轴变化、合加速度变化、横向加速度和角速度做窗口确认，形成：

```text
IMU_ALERT type=road_bump reason=sustained_road_shock ...
```

A35映射为`type=road_hazard`并转发。HUD对摔倒、急刹、颠簸分别使用独立60秒冷却，
避免高频颠簸占用全局冷却后吞掉真正摔倒。

现有静态板曾约每2秒产生一次颠簸事件，说明阈值或安装基线仍需现场标定。比赛前应
清楚区分“链路工作正常”和“物理阈值已经适用于真实骑行”。

## 10. V2X协同危险输入

当前A35已实现的入口协议是：

```text
V2X_ALERT direction=nearby|left_front|right_front|left|right
```

链路为：

```text
M33或协同侧形成V2X危险
  → RPMsg /dev/ttyRPMSG0
  → rpmsg_thread解析direction
  → handle_v2x_alert()
  → 对应v2x_*.wav
  → 2秒语音冷却
```

当前A35代码明确让V2X只走定向语音，不驱动PD11，也不自动触发DVR，避免告警状态
无法可靠清除。作品报告中的“双车状态广播、相对位置风险评估、V2X触发录像”是完整
方案描述；比赛若要按完整链路演示，必须确认相应M33/协同端固件和现场版本，而不能
只以A35存在`V2X_ALERT`解析分支作为证明。

不要把两条BLE相关链混在一起：

```text
协同危险输入： M33/V2X → RPMsg → A35 → 定向语音
方向灯输出：   A35最终雷达/NPU风险 → CH9140 → WBA → PA7/PA5
```

## 11. 手机导航和道路风险

### 11.1 极简导航`type=navi`

```json
{"type":"navi","turn":2,"distance":100}
```

```text
手机App/地图SDK
  → 提取下一动作和距离
  → Wi-Fi UDP 8888
  → OLED/HUD更新箭头和距离
  → 距离<=50m时使用本地模板播报
```

同一转向15秒内不重复播报。导航信号数秒未更新时OLED进入`NO DATA`，避免显示过期
指令。

### 11.2 完整导航文本`type=navi_tts`

```json
{"type":"navi_tts","tts_type":1,"seq":12,"text":"前方100米右转进入人民路"}
```

当前优先匹配预生成的本地WAV。在线TTS默认关闭，比赛场地没有公网时不会把网络失败
误当成骨传导硬件故障。

### 11.3 道路异常`type=danger_tts`

```text
App/云端风险点匹配
  → preload阶段：提前发送并记录，不播放
  → trigger阶段：进入正式播报
      → 匹配“障碍物”或“施工”等本地关键词WAV
      → 每类8秒冷却
```

已准备的本地异常提示主要是障碍物和施工。未匹配关键词且没有完整缓存时会跳过，不应
在答辩中说成任意云端文本都可在离线状态实时合成。

## 12. DVR事件录像链

### 12.1 当前状态机

```text
IDLE
  → NPU首次发现道路使用者
  → BUFFERING：MJPEG帧写入TF临时文件
  → 雷达+NPU最终风险 或 摔倒
  → TRIGGERED：记录触发时间
  → 继续采集15秒
  → 停止写入
  → fork子进程
  → GStreamer：JPEG解码 → V4L2 H.264编码 → MP4封装
  → 主循环waitpid非阻塞回收
  → 清理状态，准备下一事件
```

正常碰撞风险在NPU刚发现目标时已经开始缓存，因此能保留触发前画面。摔倒发生前若
没有任何目标，程序只能在摔倒时补启动录像，本次视频可能没有完整的前15秒。讲解时
不要把“设计目标前后各15秒”和“任何事件都一定已经有15秒预缓存”混为一谈。

### 12.2 为什么先存MJPEG再转H.264

- 摄像头原生输出MJPEG，预缓存阶段无需主循环重新编码每一帧；
- H.264用于降低最终视频体积；
- MP4只是容器；
- 编码放在子进程中，主循环仍可处理雷达、摄像头和告警。

### 12.3 当前边界

内存中的帧索引有上限，但TF上的原始缓存文件仍持续append，不是真正的定长文件环形
队列。比赛可以讲“事件预缓存和异步编码”，不要声称当前已完成掉电安全、无限循环
录像或严格零拷贝全链路。

## 13. Dashboard如何帮助比赛演示

Dashboard不是核心产品功能，但它是把“黑盒演示”变成“过程可解释演示”的工具。

### 13.1 关键状态

| 页面/数据 | 要看什么 | 说明 |
|---|---|---|
| 雷达状态 | 目标、距离、TTC、方向 | 原始风险输入 |
| `radar_alert` | 雷达阈值是否成立 | 尚不是最终融合告警 |
| `fusion_alert` | 雷达+NPU最终状态 | 真正驱动碰撞动作 |
| NPU事件 | 类别、置信度、推理耗时、confirmed | 视觉确认链 |
| IMU时间线 | `event_id`和各跳stage | 跨核/跨进程投递 |
| 系统状态 | 服务、进程、CPU和日志新鲜度 | 判断是否假在线 |
| 人工标签 | 场景开始/结束 | 后续按时间区间复盘 |

### 13.2 三类日志不要混淆

- `radar_data.csv`：每个雷达目标和阈值结果；
- `sensor_events.csv`：NPU、IMU、DVR和跨模块关键事件；
- `imu_delivery.csv`：HUD收到、冷却、向手机广播的结果。

进程存活只能证明程序在运行；状态持续刷新才能证明数据链活着；现场物理目标和人工
标签才能评价算法效果。

## 14. 比赛演示建议顺序

### 14.1 上场前只读检查

```bash
systemctl is-active \
  dvr.service radar-dashboard.service dvr-m33.service \
  hostapd.service dnsmasq.service

test -e /dev/video7
test -e /dev/ttySTM1
test -e /dev/ttySTM0
test -e /dev/ttyRPMSG0
test -e /dev/gpiochip3

cat /sys/class/remoteproc/remoteproc0/state
curl -fsS http://127.0.0.1:8080/api/state
/xxl/camera_detect/scripts/tf_card_control.sh status
```

通过标准不是全显示`active`，而是Dashboard `stale=false`、NPU事件持续刷新、雷达
状态有心跳、TF可写。

### 14.2 推荐演示流程

1. **上电启动**：Dashboard显示设备、服务和关键ready时间；
2. **HUD导航**：App下发方向/距离，观察OLED/HUD和本地导航音；
3. **道路异常**：发送施工/障碍物事件，观察trigger阶段骨传导提示；
4. **雷达视觉融合**：目标进入雷达和摄像头视场，先展示两个输入，再展示
   `fusion_alert`；
5. **方向灯**：让目标从左/中/右接近，展示WBA灯和ACK；
6. **风险清除**：目标离开，观察`RISK CLEAR`和状态归零；
7. **摔倒链**：使用真实M33事件展示本地告警、DVR和HUD→手机投递；
8. **事件录像**：等待触发后15秒和异步编码完成，再打开MP4；
9. **总结闭环**：事前预警、事中声光干预、事后取证。

不要一开始同时触发多个事件。音频、灯、录像和手机提示同时变化时，评委看不到每条
链的因果关系，也不利于现场定位。

### 14.3 演示异常时的最短排查路径

```text
没有画面/NPU
  → /dev/video7 → v4l2-ctl → camera_first_frame → NPU日志

雷达无目标
  → /dev/ttySTM1 → 初始化ACK → radar_first_report → radar_data.csv

有雷达风险但不告警
  → radar_alert → npu_confirmed/npu_denied → fusion_alert

WBA灯不亮
  → fusion_alert → RISK命令 → ttySTM0 → CH9140供电
  → WBA GAP/GATT ready → FFF1 Notify → GPIO → ACK

摔倒本地有反应但手机没有
  → IMU_ALERT → sensor_events.csv → UDP 8890
  → imu_delivery.csv → UDP 8889 → App权限/存活/短信接口

有触发但没有视频
  → TF挂载/空间 → dvr buffer started → recording triggered
  → post 15s → encoder exit → MP4文件
```

## 15. 评委追问时的讲解主线

### 15.1 为什么使用A35+M33

答题主线：A35适合Linux、摄像头、NPU、网络和文件系统等复杂软件；M33适合周期性
IMU和低延迟事件。`remoteproc`负责M33生命周期，OpenAMP/RPMsg负责跨核消息，两者
不是一个概念。

### 15.2 为什么雷达还要摄像头

雷达擅长接近趋势和距离速度，视觉侧用于确认画面里是否存在道路使用者。当前采用
状态/决策级融合，不是高成本的像素级联合标定。连续确认和超时用于降低单帧抖动。

### 15.3 为什么录像不一直编码

业务目标是保存风险前后过程。持续在线编码会增加计算和存储写入压力；当前先
保存摄像头原生MJPEG，在事件成立后异步生成H.264/MP4，把编码故障和主检测循环隔离。

### 15.4 BLE-V2X是不是标准C-V2X

不是。当前是面向原型验证的BLE轻量化协同和CH9140透明串口控制链，适合短距离遮挡
场景演示，但不具备标准C-V2X的协议、认证、调度和大规模组网能力。

### 15.5 如何证明不是模块堆叠

从一个事件贯穿回答：例如碰撞风险必须经过雷达帧校验、危险目标选择、方向滤波、
NPU连续确认、统一状态机，再联动本机灯、外接灯、语音、DVR和日志；摔倒事件使用同一
`event_id`贯穿M33、A35、HUD和手机投递。模块之间有明确的事件协议和状态边界。

## 16. 比赛前必须再次确认的事项

1. 摄像头画面方向、曝光和USB线缆在实际头盔安装状态下稳定；
2. 雷达左右角度符号与安装方向一致，LEFT/RIGHT灯没有反接；
3. IMU完成头盔安装姿态标定，静止时不持续产生`road_bump`或`fall`；
4. WBA显示`GAP=connected GATT=ready`，四条`RISK`命令均有ACK；
5. 手机连接正确Wi-Fi AP，UDP 8888/8889链路和后台权限正常；
6. 外置TF已真实挂载、可写且`<挂载点>/dvr`空间足够，提前完成一次真实触发并确认日志出现
   `DVR-WORKER VALIDATED`，再用`verify_dvr_videos.sh --full`整段解码；
7. 所有本地WAV在无公网环境可播放，避免依赖在线TTS；
8. Dashboard时间线、人工标签和系统状态在普通笔记本分辨率下一屏可读；
9. 明确哪些指标来自作品报告，哪些已有本次比赛版本的原始日志；
10. 准备“主功能失败时如何降级和如何恢复”的口头说明，不在比赛现场临时改内核、
    DTB、U-Boot或传感器时序。

## 17. 相关资料入口

- 完整A35数据流：[DATA_FLOW.md](DATA_FLOW.md)
- 启动与业务ready：[BOOT_OPTIMIZATION.md](BOOT_OPTIMIZATION.md)
- 雷达阈值和现场标注：[RADAR_EXPERIMENT.md](RADAR_EXPERIMENT.md)
- 摔倒到手机投递：[FALL_SMS_PIPELINE.md](FALL_SMS_PIPELINE.md)
- MP257/CH9140/WBA方向灯：[BLUETOOTH_DIRECTION_LED.md](BLUETOOTH_DIRECTION_LED.md)
- 项目底层知识：[PROJECT_TECHNICAL_KNOWLEDGE_BASE.md](PROJECT_TECHNICAL_KNOWLEDGE_BASE.md)
- WBA固件说明：[E04-2G4M10S1AX README](../../../../E04-2G4M10S1AX/README.md)
- 作品报告：`/home/alientek/dvr_project/基于STM32MP257的复杂城市环境下的主动安全骑行辅助系统-作品报告.pdf`

比赛准备的核心不是背完整代码，而是能够对任意现场现象回答：

```text
事件在哪里产生？
经过什么接口？
哪个状态决定最终动作？
每一跳留下什么证据？
如果这一跳失败，系统怎样降级？
```
