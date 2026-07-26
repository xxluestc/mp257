# STM32MP257 骑行辅助核心工程

这是 `mier` 中唯一维护的 A35 Linux 侧工程。主程序 `radar_fusion` 集成：

- 摄像头采集、NPU 道路目标检测与 DVR 前后 15 秒保存
- 毫米波雷达多目标解析、危险目标选择、方向滤波和碰撞告警
- IMU 摔倒与 V2X RPMsg 事件
- LED、骨传导音频、OLED/HUD 和手机导航 UDP
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
├── hud_project/              # HUD 可执行程序源码
├── models/                   # NPU 模型与标签
├── sounds/                   # 碰撞、摔倒、V2X、录制完成提示音
├── nav_tts_cache/            # 预生成导航语音
├── scripts/                  # M33、日志、启动优化和语音维护脚本
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
RADAR_LOG_DIR=/run/media/mmcblk0p1/dvr/radar_experiments
```

- `LEFT_ANGLE`、`RIGHT_ANGLE`：LEFT/CENTER/RIGHT 可配置边界
- `ANGLE_ALPHA`：每个 `objId` 独立的角度 EMA 权重
- `DIRECTION_SAMPLES`：连续多少帧同方向后确认切换

## Dashboard 与实验标注

`start_dvr.sh` 会同时启动 Dashboard。电脑连接开发板 WiFi 后访问：

```text
http://<开发板IP>:8080
```

页面显示当前危险目标、四条实时曲线、摄像头/NPU 状态、M33 IMU 事件、
摔倒 UDP 投递链和跨传感器同步时间线。五类测试事件均可点击开始/结束，结果保存到：

```text
/run/media/mmcblk0p1/dvr/radar_experiments/
├── radar_data.csv
├── radar_state.json
├── sensor_events.csv
├── imu_delivery.csv
└── labels.csv
```

单独启动 Dashboard：

```bash
python3 dashboard/radar_dashboard.py \
  --host 0.0.0.0 --port 8080 \
  --data-dir /tmp/radar_experiment
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
正后方、左后方、右后方快速接近，以及触发/不触发接近的现场标定。

`start_dvr.sh -t N` 只模拟 A35 本地 LED/音频/DVR 摔倒动作，不经过 M33、
HUD 或手机短信，不能作为端到端短信测试。完整边界见
[摔倒与短信链路](docs/FALL_SMS_PIPELINE.md)。

## 运行依赖与输出

开发板需具备 `aplay`、`ffmpeg` 和 `python3`。`edge-tts` 只用于联网环境
生成新语音资产，不是运行时必需。

- 系统日志：`/xxl/camera_detect/dvr_system.log`
- 雷达 Dashboard 日志：`/xxl/camera_detect/radar_dashboard.log`
- 紧急视频：`/run/media/mmcblk0p1/dvr/emergency_*.mp4`
- 雷达实验数据：`/run/media/mmcblk0p1/dvr/radar_experiments/`

`start_dvr.sh` 会启动日志容量维护：系统日志最多约 40 MiB，Dashboard 日志
最多约 20 MiB；雷达、同步传感器和 IMU 投递 CSV 分别保留固定数量的轮转文件。
具体容量、异常恢复验证和 RAM DVR 方案见
[运行可靠性与存储](docs/RUNTIME_STORAGE.md)。

## 维护文档

- [日常操作指南](docs/操作指南.md)
- [完整数据流](docs/DATA_FLOW.md)
- [雷达实验、CSV 与人工标注](docs/RADAR_EXPERIMENT.md)
- [M33 摔倒判断与手机短信链路](docs/FALL_SMS_PIPELINE.md)
- [启动优化、M33 U-Boot 启动与回退](docs/BOOT_OPTIMIZATION.md)
- [运行可靠性、日志容量与 TF/RAM 缓存](docs/RUNTIME_STORAGE.md)
- [设备树与 MAX98357A 配置](board/DEVICE_TREE.md)
