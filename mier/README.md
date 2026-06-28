# 电动车 BSD + NPU 目标识别 + DVR 行车记录融合系统

## 系统概述

基于米尔 myd-ld25x (STM32MP257F) 开发板的一体化系统：

- **摄像头 NPU**：负责目标识别（人、自行车、汽车等道路用户）
- **毫米波雷达**：负责目标测距、测速、TTC 碰撞时间估计
- **M33 核 IMU/V2X**：检测摔倒、急刹、路面颠簸，接收周边车辆 V2X 告警
- **融合决策**：
  - 雷达告警 + NPU 确认 → 触发保存
  - M33 IMU 摔倒告警 → 触发保存
- **DVR 行车记录**：检测到目标后先缓冲，触发后保存前后 15 秒视频到 TF 卡
- **LED 告警**：PD11 引脚闪烁告警
- **骨传导音频**：摔倒/碰撞/V2X 方向告警语音提示

## 当前融合逻辑

```
摄像头始终采集 (25fps)
    │
    ├─ NPU 推理 (每 5 帧)
    │   ├─ 检测到道路用户 → 开始 DVR 缓冲
    │   ├─ 连续 N 帧确认 → npu_confirmed = 1
    │   └─ 丢失目标 → 无触发则清理缓冲
    │
    ├─ 雷达数据 (目标距离/速度/TTC)
    │   ├─ 检测到目标 → target_active
    │   └─ 目标消失 3s → target_active = 0
    │
    ├─ M33 RPMsg (IMU/V2X 告警)
    │   ├─ IMU_ALERT type=fall → g_imu_fall_alert = 1
    │   └─ V2X_ALERT direction=xxx → g_v2x_alert = 1, 播放方向语音
    │
    └─ 触发条件:
        │
        ├─ 雷达 TTC 危险 AND npu_confirmed
        │       │
        │       ├─ LED 闪烁告警
        │       ├─ 碰撞音频提示
        │       ├─ DVR 保存触发 → 继续录制 15s
        │       └─ fork 子进程 ffmpeg 编码 MP4 (异步)
        │
        ├─ IMU 摔倒告警
        │       │
        │       ├─ 若未缓冲 → 立即启动缓冲，从摔倒瞬间保存 15s
        │       ├─ 若已缓冲 → 保存前后各 15s (约 30s)
        │       ├─ LED 闪烁告警
        │       ├─ 摔倒音频提示
        │       └─ fork 子进程 ffmpeg 编码 MP4 (异步)
        │
        └─ V2X 告警
                ├─ LED 闪烁告警
                ├─ 方向语音播报
                └─ 若正在缓冲 → 继续保留缓冲 (DVR 可选扩展)
```

**关键设计变化**: 与早期版本不同，现在由 **摄像头 NPU 掌管目标是否出现**，雷达只负责提供距离/TTC 信息用于最终触发判断；M33 IMU 摔倒作为第二独立触发源。

## 目录结构

```
mier/
├── camera_detect/          # 当前主程序: 雷达+NPU+DVR+IMU 融合
│   ├── radar_fusion.cpp    # 主程序入口
│   ├── start_dvr.sh        # 一键启动脚本 (推荐)
│   ├── IMU_FALL_TEST.md    # IMU 摔倒触发 DVR 测试指南
│   ├── camera.c/h          # USB 摄像头 V4L2 采集
│   ├── npu_detect.cpp/h    # NPU 推理 (SSD MobileNet V2)
│   ├── jpeg_decoder.c      # JPEG 解码
│   ├── stai_mpu/           # 正点原子 NPU 库 (libstai_mpu.so)
│   └── Makefile
│
├── v2x/                    # M33 固件与 A35 接收脚本
│   ├── 使用方式.md
│   ├── a35_read_v2x_alerts.sh
│   └── STM32Cube_ATK_FW_MP2_V1.0.0/...
│
├── dvr/                    # 早期独立 DVR 引擎 (已不用于主流程)
│   ├── src/                # DVR 状态机、环形缓冲、触发接收
│   ├── scripts/
│   └── docs/
│
├── pro/                    # 独立雷达程序 (参考/备用)
│   ├── radar_link.c        # 雷达串口解析 + TTC 计算
│   └── Makefile
│
├── radar/                  # 雷达模块文档
│   ├── radar_init.c
│   ├── RADAR_COMMANDS.md
│   └── README.md
│
├── audio/                  # 音频模块
├── dts/                    # 设备树
└── docs/
    └── debug_notes.md      # 完整调试经验记录
```

## 硬件接口

| 组件 | 开发板接口 | 设备节点 | 说明 |
|------|-----------|---------|------|
| 雷达 | USART1 | `/dev/ttySTM1` | 921600bps |
| 摄像头 | USB 2.0 Host | `/dev/video7` | MJPEG 1280x720 |
| LED | GPIO PD11 | `/dev/gpiochip3` line 11 | 告警闪烁 |
| TF 卡 | SDMMC | `/run/media/mmcblk0p1/dvr/` | 视频存储 |
| M33 核 | RPMsg | `/dev/ttyRPMSG0` | IMU/V2X 告警 |
| 音频 | I2S + MAX98357A | ALSA default | 骨传导提示音 |

## 快速开始

### 1. 交叉编译 (虚拟机)

```bash
cd /home/alientek/dvr_project/mier/camera_detect
make clean
make radar-fusion CC=aarch64-linux-gnu-gcc CXX=aarch64-linux-gnu-g++
```

### 2. 部署到开发板

```bash
make deploy-radar
# 或手动:
# scp radar_fusion start_dvr.sh root@192.168.88.10:/xxl/camera_detect/
```

### 3. 开发板运行（推荐一键启动）

```bash
ssh root@192.168.88.10
cd /xxl/camera_detect
./start_dvr.sh
```

终端只显示关键事件，完整日志写入 `/xxl/camera_detect/dvr_system.log`。

按 `Ctrl+C` 停止，脚本会自动清理并在 4-5 秒内退出。

默认雷达阈值（针对后方电动车快速靠近、即将追尾场景）：TTC 2.5s，距离 3m。现场可通过 `./start_dvr.sh -T 1.5 -D 2` 等方式调整。

### 4. 模拟摔倒测试

```bash
cd /xxl/camera_detect
./start_dvr.sh -t 10
```

启动 10 秒后自动模拟 IMU 摔倒，验证 LED、音频、视频保存是否正常。

### 5. 拷贝视频到虚拟机

```bash
scp root@192.168.88.10:/run/media/mmcblk0p1/dvr/emergency_*.mp4 ~/
```

## 运行模式

| 模式 | 命令 | 说明 |
|------|------|------|
| 一键完整系统 | `./start_dvr.sh` | 推荐，自动启动 M33 + radar_fusion |
| 摄像头 + 雷达融合 | `./radar_fusion` | 主程序, NPU 缓冲 + 雷达确认触发 |
| 模拟摔倒测试 | `./radar_fusion -t 10` | 10 秒后自动触发摔倒 |
| 纯雷达 | `./radar_link` | 仅雷达数据解析 |
| 旧版联调 | `pro/start_pro.sh` | 早期雷达 + DVR 管道方案 (不推荐) |

## 触发逻辑

| 来源 | 事件 | 当前响应 |
|------|------|---------|
| 摄像头 NPU | 检测到道路用户 | 开始 DVR 缓冲 |
| 摄像头 NPU | 连续确认道路用户 | `npu_confirmed = 1` |
| 雷达 | TTC < 阈值 / 距离过近 | `radar.should_alert = 1` |
| 融合 | `radar.should_alert AND npu_confirmed` | 触发保存、LED 闪烁、碰撞音频 |
| M33 IMU | `IMU_ALERT type=fall` | 触发保存、LED 闪烁、摔倒音频 |
| M33 V2X | `V2X_ALERT direction=xxx` | LED 闪烁、方向语音播报 |
| 雷达 | 目标消失 3s | `target_active = 0` |
| NPU | 目标丢失 | 无触发则清理缓冲 |

> 默认雷达阈值（针对后方电动车快速靠近场景）：TTC < **2.5s** 或距离 ≤ **3m** 触发告警。可通过 `./start_dvr.sh -T X -D Y` 现场调整。
>
> 注：早期通过 `TARGET_ON` / `COLLISION` / `TARGET_OFF` 管道事件触发 DVR，当前主程序已改为内部直接调用，不再依赖管道。

## DVR 录像流程

| 阶段 | 操作 | 是否编码 |
|------|------|---------|
| 缓冲 | 摄像头 MJPEG 帧写入 `dvr_raw.bin` | 否, 原始 JPEG |
| 索引 | 帧大小/偏移写入 `dvr_index.bin` | 否 |
| 触发 | 继续缓冲 15s post-trigger | 否 |
| 保存 | 从 bin 提取 JPEG 帧，生成 filelist | 否 |
| 编码 | ffmpeg 将 JPEG 序列编码为 MP4 (mpeg4) | **是** |

**ffmpeg 作用**: 仅在触发保存后运行，把缓冲的 JPEG 帧序列压缩成 MP4 视频文件，与主进程并行（fork 子进程），不阻塞检测。

**关于保存延迟**: 触发后到 MP4 可用有固定延迟：
1. 必须等待 15 秒 post-trigger 录制
2. ffmpeg 编码耗时（通常几秒到十几秒）

编码期间主进程继续跑检测，但新的 DVR 缓冲不会启动，避免多个 ffmpeg 并发压垮系统。

## 关键配置

```cpp
#define DVR_BASE_DIR           "/run/media/mmcblk0p1/dvr"
#define DVR_BUFFER_DIR         "/run/media/mmcblk0p1/dvr/.buffer"
#define DVR_SAVE_BEFORE_SEC    15      // 触发前保存 15s
#define DVR_SAVE_AFTER_SEC     15      // 触发后保存 15s
#define DVR_CAPTURE_FPS        25      // 25fps 采集
#define TTC_THRESHOLD_DEFAULT  2.5f    // 雷达 TTC 阈值，启动脚本可覆盖
#define DIST_THRESHOLD_DEFAULT 3       // 雷达距离阈值，启动脚本可覆盖
#define NPU_CONFIRM_FRAMES     2       // NPU 连续 2 帧确认
#define NPU_DENY_FRAMES        3       // NPU 连续 3 帧否认
```

## 视频存储位置

```
/run/media/mmcblk0p1/dvr/
├── .buffer/                          # 临时缓冲 (触发后清理)
│   ├── dvr_raw.bin
│   ├── dvr_index.bin
│   └── filelist.txt
└── emergency_YYYYMMDD_HHMMSS.mp4    # 保存的紧急视频
```

## 各模块文档

- [IMU 摔倒触发 DVR 测试指南](camera_detect/IMU_FALL_TEST.md) — 一键启动、测试方法、常见问题
- [V2X/IMU 使用方式](v2x/使用方式.md) — M33 固件启动、RPMSG 监听、硬件连接
- [调试经验记录](docs/debug_notes.md) — 完整 Bug 记录、调试过程、排错方法
- [雷达模块](radar/README.md) — 雷达硬件接口、协议
- [DVR 开发日志](dvr/docs/DEVELOPMENT_LOG.md) — 早期 DVR 开发历程
- [音频模块](audio/docs/README.md) — 音频输出
- [设备树](dts/README.md) — 设备树配置说明

## 开发板 NPU 库位置

运行时依赖的动态库:

| 库 | 开发板路径 | 来源 |
|---|-----------|------|
| `libstai_mpu.so.6` | `/xxl/camera_detect/stai_mpu/` 或 `/usr/lib/` | 正点原子 rootfs |
| `libstai_mpu_ovx.so.6` | `/xxl/camera_detect/stai_mpu/` 或 `/usr/lib/` | 正点原子 rootfs |
| `libjpeg.so.62` | `/xxl/camera_detect/stai_mpu/` | 正点原子 rootfs |

如果下电重启后运行失败，优先检查:
```bash
ls -la /usr/lib/libstai_mpu.so* /vendor/lib/libstai_mpu.so*
# 如不存在, 使用 LD_LIBRARY_PATH=/usr/lib:/vendor/lib:/xxl/camera_detect/stai_mpu
```
