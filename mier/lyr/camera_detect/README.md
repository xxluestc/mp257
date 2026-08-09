# STM32MP257 骑行辅助核心工程

这是 `mier` 中唯一维护的 A35 Linux 侧工程。主程序 `radar_fusion` 集成：

- 摄像头采集、NPU 道路目标检测与 DVR 前后 15 秒保存
- 毫米波雷达多目标解析、危险目标选择、方向滤波和碰撞告警
- IMU 摔倒与 V2X RPMsg 事件
- LED、骨传导音频、OLED/HUD 和手机导航 UDP
- CH9140 蓝牙方向灯（WBA 左/右碰撞风险提示）
- 雷达/摄像头/IMU 同步记录、本地 Web Dashboard 与人工实验标注

雷达增强是在原有融合链路上增量实现的，没有改动 NPU、DVR、LED、Audio
和 IMU 摔倒的既有决策逻辑。

## 目录结构

```text
camera_detect/
├── Makefile
├── radar_fusion.cpp          # 唯一主程序入口
├── camera.c/.h               # V4L2 摄像头
├── npu_detect.cpp/.h         # STM32MP2 NPU 推理
├── nav_tts.c/.h              # 导航、OLED 与本地语音
├── start_dvr.sh              # 开发板统一启动脚本
├── dvr.service.example       # systemd 服务示例
├── radar_config.example      # 现场配置模板
├── dashboard/                # Python 标准库 Web 服务与静态页面
├── ota/                      # 独立 OTA 服务、事务安装器、打包与主机测试
├── hud_project/              # HUD 可执行程序源码
├── models/                   # NPU 模型与标签
├── sounds/                   # 碰撞、摔倒、V2X、录制完成提示音
├── nav_tts_cache/            # 预生成导航语音
├── scripts/                  # 编码worker、存储、安全退出、M33和日志维护脚本
├── stai_mpu/                 # NPU 运行库和头文件
├── board/                    # 当前板级 DTS 源码与说明
└── docs/                     # 当前架构及雷达实验文档
```

历史的 `camera_detect`、`fusion_detect` 示例入口和 `.bak` 文件已经移除。
需要回看旧实现时使用 Git 历史，不再复制旧版本目录。

## 编译

主机需要 `aarch64-linux-gnu-gcc/g++` 和 Python 3：

```bash
cd /home/alientek/dvr_project/mier/lyr/camera_detect
make clean
make
```

`make` 会完成：

1. 交叉编译 `radar_fusion`
2. 交叉编译 `hud_project/hud`
3. 检查 Dashboard Python 语法

也可单独执行：

```bash
make radar-fusion
make hud
make dashboard-check
```

在另一台Linux电脑从零拉取时：

```bash
git clone https://github.com/xxluestc/mp257.git
cd mp257/mier/lyr/camera_detect
make clean
make
make deploy-radar BOARD_IP=192.168.88.10
```

仓库已跟踪A35编译所需源码、NPU头文件/运行库、模型、声音、Dashboard和部署脚本；
主机仍需安装AArch64交叉编译器、Python 3和`make`。部署要求能够SSH/SCP登录
开发板root账号，且已有现场`radar_config`不会被覆盖。

生成 A35 运行时 OTA 包：

```bash
make ota-package VERSION=1.0.8
```

输出位于 `dist/`。完整的目录迁移、板端服务部署和回滚方法见
[A35 OTA 操作指南](docs/OTA.md)。OTA版本选择、最终SHA-256、云端元数据和主题切换
验收见[1.0.8发布说明](docs/RELEASE_1.0.8.md)。当前重新构建的1.0.8为唯一可交付包。

MP257、CH9140 与 WBA 左右碰撞方向灯的接线、协议、烧录、测试和实机联调
记录见 [蓝牙方向灯联调文档](docs/BLUETOOTH_DIRECTION_LED.md)。

比赛答辩和现场演示前，建议先阅读
[比赛事件链与关键节点](docs/COMPETITION_PREPARATION.md)。该文档按导航、雷达视觉
融合、摔倒、V2X、方向灯和事件录像分别说明“事件入口—判断节点—输出动作—验证
证据”，并区分作品报告设计与当前源码实装边界。

## 部署

开发板恢复在线后，按需覆盖地址：

```bash
make deploy-radar BOARD_IP=192.168.88.10 BOARD_DIR=/xxl/camera_detect
```

该目标会部署主程序、HUD、模型、脚本、所有本地 WAV、Dashboard、
启动脚本和配置模板。已有 `/xxl/camera_detect/radar_config` 不会被覆盖。

板端运行：

```bash
cd /xxl/camera_detect
export LD_LIBRARY_PATH=/usr/lib:/vendor/lib
./start_dvr.sh
```

配置为开机启动：

```bash
cp /xxl/camera_detect/dvr.service.example /etc/systemd/system/dvr.service
systemctl daemon-reload
systemctl enable --now dvr.service
```

Dashboard使用独立服务：

```bash
cp /xxl/camera_detect/radar-dashboard.service.example \
  /etc/systemd/system/radar-dashboard.service
systemctl daemon-reload
systemctl enable --now radar-dashboard.service
```

## 雷达配置

首次部署会从 `radar_config.example` 生成 `radar_config`：

```ini
TTC=2.5
DIST=1.2
LEFT_ANGLE=-10
RIGHT_ANGLE=10
ANGLE_ALPHA=0.35
DIRECTION_SAMPLES=3
DASHBOARD_PORT=8080
RADAR_LOG_DIR=/usr/local/helmet/radar_experiments
BLE_LED_ENABLED=1
BLE_LED_UART=/dev/ttySTM0
```

- `LEFT_ANGLE`、`RIGHT_ANGLE`：LEFT/CENTER/RIGHT 可配置边界
- `ANGLE_ALPHA`：每个 `objId` 独立的角度 EMA 权重
- `DIRECTION_SAMPLES`：连续多少帧同方向后确认切换
- `BLE_LED_UART`：J15 USART2 所接 CH9140 的 Linux 串口

## WBA 左右方向灯

`radar_fusion` 只把现有融合逻辑最终确认的碰撞风险发送给 WBA，不改变雷达、
NPU 或录像判定：

| 最终状态 | 串口命令 | WBA 动作 |
|---|---|---|
| 左后方风险 | `RISK LEFT` | PA7 左灯亮 |
| 右后方风险 | `RISK RIGHT` | PA5 右灯亮 |
| 正后方风险 | `RISK CENTER` | 两灯同时亮 |
| 无碰撞风险 | `RISK CLEAR` | 两灯熄灭 |

没有雷达环境时，先停业务服务，避免两个进程同时使用 `/dev/ttySTM0`，再逐项
测试：

```bash
systemctl stop dvr.service
/xxl/camera_detect/scripts/ble_led_test.sh ping
/xxl/camera_detect/scripts/ble_led_test.sh left
/xxl/camera_detect/scripts/ble_led_test.sh right
/xxl/camera_detect/scripts/ble_led_test.sh center
/xxl/camera_detect/scripts/ble_led_test.sh clear
systemctl start dvr.service
```

## Dashboard 与实验标注

Dashboard 由独立的 `radar-dashboard.service` 常驻托管；旧系统没有该 unit 时，
`start_dvr.sh` 仍保留兼容启动路径。电脑连接开发板 WiFi 后访问：

```text
http://<开发板IP>:8080
```

页面按 `RADAR / FUSION / HEALTH / TIMING / CONTROL` 五个栏目切换。桌面宽度下每个栏目
优先完整放入一屏，并在内容超过实际窗口高度时提供栏目内滚动，避免固定高度截断：
既保留危险目标、四条实时曲线、摄像头/NPU、M33 IMU、
摔倒 UDP 投递链和跨传感器时间线，也增加关键服务/进程、CPU/内存/温度、设备
节点、启动里程碑、启动后的关键业务事件和任务控制。窄屏会退化为栏目内部滚动，
避免压缩到无法阅读。

新增观测接口均为只读：`/api/system` 读取 `/proc`、`/sys` 和 systemd 状态，
`/api/boot` 汇总 systemd、单调时钟启动日志和本次开机后的关键业务事件。只有打开
对应栏目时才轮询：系统/控制状态间隔 5 秒，关键事件 10 秒，启动信息缓存 60 秒；
浏览器切到后台后暂停请求。成功的高频 API
访问日志也按接口最多每 60 秒记录一次，避免 Dashboard 自身制造无意义磁盘 I/O。

`CONTROL` 只允许运行/暂停 `dvr.service` 和 `helmet-ota.service`，并要求二次确认、
同源请求和服务端控制令牌。Dashboard、M33、hostapd 和 dnsmasq 是受保护服务，
不提供控制入口。每次控制写入 `control_events.csv`；暂停融合业务不会停止独立
Dashboard、M33 或 WiFi。

同页“设备运维”区域提供TF挂载/安全弹出、安全停止项目和安全关机，所有动作均有
二次确认。命令行等价入口是`scripts/tf_card_control.sh`和
`scripts/project_safe_stop.sh`。安全停止项目只停止`dvr.service`并同步存储，Dashboard、
M33、网络和OTA继续运行，因此仍可在网页或SSH中重新启动业务或继续执行安全关机。
安全关机会进一步请求systemd有序停止整机。录像主存储为板载ext4，所以TF弹出不影响
录像服务。设备运维区的项目按钮会按`dvr.service`状态在“安全停止项目”和
“安全启动项目”之间自动切换。

五类测试事件均可点击开始/结束，结果保存到：

```text
/usr/local/helmet/radar_experiments/
├── radar_data.csv
├── radar_state.json
├── sensor_events.csv
├── imu_delivery.csv
├── control_events.csv
└── labels.csv
```

单独启动 Dashboard：

```bash
python3 dashboard/radar_dashboard.py \
  --host 0.0.0.0 --port 8080 \
  --data-dir /tmp/radar_experiment
```

接口快速检查：

```bash
curl -fsS http://127.0.0.1:8080/api/state
curl -fsS http://127.0.0.1:8080/api/system
curl -fsS http://127.0.0.1:8080/api/boot
curl -fsS http://127.0.0.1:8080/api/control
```

详细的字段、危险目标算法和现场测试步骤见
[雷达实验文档](docs/RADAR_EXPERIMENT.md)。

## 基础测试方法

主机侧可执行：

```bash
make
bash -n start_dvr.sh
python3 -m py_compile dashboard/radar_dashboard.py
```

板端启动后确认 `dvr.service`、`radar_fusion`、HUD 和 Dashboard 都在运行，
再检查 `/api/state` 的 `stale=false` 以及 `/api/events` 中 CAM/NPU、IMU 和
HUD 事件时间线。随后依照 [雷达实验文档](docs/RADAR_EXPERIMENT.md) 完成
正后方、左后方、右后方快速接近，以及“安全接近（预期不告警）”和
“危险接近（预期告警）”的现场标定。

`start_dvr.sh -t N` 只模拟 A35 本地 LED/音频/DVR 摔倒动作，不经过 M33、
HUD 或手机短信，不能作为端到端短信测试。完整边界见
[摔倒与短信链路](docs/FALL_SMS_PIPELINE.md)。

## 运行依赖与输出

开发板需具备 `aplay`、`ffmpeg` 和 `python3`。`edge-tts` 只用于联网环境
生成新语音资产，不是运行时必需。

- 系统日志：`/xxl/camera_detect/dvr_system.log`
- 雷达 Dashboard 日志：`/xxl/camera_detect/radar_dashboard.log`
- 紧急视频：`/usr/local/helmet/dvr/emergency_*.mp4`
- 雷达实验数据：`/usr/local/helmet/radar_experiments/`

比赛录像以板载 `userfs` ext4 为主存储，不再直接写 TF 卡。编码 worker 会在
`/tmp` 生成并完整解码，复制到 ext4 后再次解码，通过后才原子提交正式 MP4。
最多保留最近 12 段或约 2 GiB。故障 TF 卡不能作为比赛录像介质，详见
[DVR可靠性与TF故障复盘](docs/DVR_RELIABILITY.md)。

同一进程两次间隔触发以及三段正式MP4整段解码已通过板端回归。比赛前仍应执行：

```bash
/xxl/camera_detect/scripts/verify_dvr_videos.sh --full
```

`start_dvr.sh` 会启动日志容量维护：系统日志最多约 40 MiB，Dashboard 日志
最多约 20 MiB；雷达、同步传感器和 IMU 投递 CSV 分别保留固定数量的轮转文件。
具体容量、异常恢复验证和 RAM DVR 方案见
[运行可靠性与存储](docs/RUNTIME_STORAGE.md)。

## 维护文档

- [项目技术知识库：Linux、内核、驱动与业务分层](docs/PROJECT_TECHNICAL_KNOWLEDGE_BASE.md)
- [嵌入式 Linux 岗位面试准备与项目讲解](docs/INTERVIEW_PREPARATION.md)
- [日常操作指南](docs/操作指南.md)
- [完整数据流](docs/DATA_FLOW.md)
- [雷达实验、CSV 与人工标注](docs/RADAR_EXPERIMENT.md)
- [M33 摔倒判断与手机短信链路](docs/FALL_SMS_PIPELINE.md)
- [STM32WBA54 BLE Central、CH9140和V2V方向灯固件](../../../E04-2G4M10S1AX/README.md)
- [启动优化、M33 U-Boot 启动与回退](docs/BOOT_OPTIMIZATION.md)
- [运行可靠性、日志容量与 TF/RAM 缓存](docs/RUNTIME_STORAGE.md)
- [DVR可靠性、板载ext4与故障TF复盘](docs/DVR_RELIABILITY.md)
- [A35 应用层 OTA 打包、部署、测试与回滚](docs/OTA.md)
- [Android 使用的 OTA HTTP API](docs/OTA_API.md)
- [Android/云端 OTA 分工与联调交接](docs/OTA_HANDOFF.md)
- [OTA 1.0.8 发布包、云端元数据与演示验收](docs/RELEASE_1.0.8.md)
- [设备树与 MAX98357A 配置](board/DEVICE_TREE.md)
