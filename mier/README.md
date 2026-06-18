# 电动车BSD雷达 + 行车记录 + NPU融合系统

## 系统概述

基于米尔 myd-ld25x (STM32MP257F) 开发板的电动车BSD雷达 + 行车记录一体化系统。雷达检测到目标后，通过摄像头NPU验证是否为道路用户，确认后录制视频并保存到TF卡。

## 系统架构

```
┌──────────────────────────────────────────────────────────────────┐
│                        开发板 myd-ld25x                           │
│                                                                   │
│  ┌──────────────┐           ┌──────────────────────────────────┐ │
│  │  radar_link   │           │  DVR (dvr) + NPU Fusion         │ │
│  │  (雷达进程)    │  管道     │  (行车记录进程)                    │ │
│  │               │  事件      │                                  │ │
│  │ /dev/ttySTM1  │──TARGET_ON─→│ /dev/video6 (USB摄像头, 独占)      │ │
│  │ (UART, 921600)│──COLLISION─→│                                  │ │
│  │               │──TARGET_OFF→│ 状态机: IDLE → BUFFERING → SAVING │ │
│  │ TTC < 10s     │           │                                  │ │
│  │ → LED 闪烁     │           │ 环形缓冲 → TP卡 (前后15s)          │ │
│  └──────────────┘           │                                  │ │
│                              │ NPU: SSD MobileNet V2             │ │
│  ┌──────────────┐           │ 道路用户检测: person/bicycle/      │ │
│  │  audio        │           │ car/motorcycle/bus/truck          │ │
│  │  (音频模块)    │           │ 连续2帧确认 → 信任雷达             │ │
│  │ (暂未联调)     │           │ 连续3帧否认 → 雷达误触发           │ │
│  └──────────────┘           └──────────────────────────────────┘ │
│                                                                   │
│  存储: /run/media/mmcblk0p1/dvr/ (TF卡)                           │
└──────────────────────────────────────────────────────────────────┘
```

## 目录结构

```
mier/
├── pro/                   # 联调程序 (部署到 /xxl/pro/)
│   ├── radar_link.c       # 雷达程序 (UART + TTC + 管道事件)
│   ├── npu_detector.hpp/cpp   # NPU检测器 (参考文件)
│   ├── Makefile           # 编译脚本
│   └── start_pro.sh       # 联调启动脚本
├── dvr/                   # 行车记录 DVR
│   ├── src/               # 源码
│   │   ├── dvr_main.c     # 主入口
│   │   ├── dvr_engine.c   # 核心引擎 (状态机 + NPU融合)
│   │   ├── dvr_types.h    # 数据类型
│   │   ├── usb_camera.c   # USB摄像头 V4L2
│   │   ├── ring_buffer.c  # 环形缓冲
│   │   ├── trigger_receiver.c # 命名管道触发
│   │   ├── frame_decoder.c    # 帧解码 (MJPEG/YUYV → RGB)
│   │   ├── npu_fusion.cpp     # NPU融合 C API
│   │   └── npu_detector.cpp   # NPU检测器
│   ├── scripts/           # 测试脚本
│   ├── docs/              # 开发文档
│   └── Makefile           # 编译脚本
├── radar/                 # 雷达模块
│   ├── radar_init.c       # 原始雷达测试程序
│   ├── RADAR_COMMANDS.md  # 雷达命令参考
│   └── README.md          # 雷达模块文档
├── audio/                 # 音频模块 (暂未联调)
│   └── docs/
│       └── README.md
└── dts/                   # 设备树
    └── myb-stm32mp257x-2GB.dts
```

## 硬件接口

| 组件 | 开发板接口 | 设备节点 | 说明 |
|------|-----------|---------|------|
| 雷达 | PG14/PG15 (USART1) | `/dev/ttySTM1` | 921600bps |
| 摄像头 | USB 2.0 Host | `/dev/video6` | MJPEG 1280x720 |
| LED | GPIO | `gpio_led` | 雷达告警闪烁 |
| TF卡 | SDMMC | `/run/media/mmcblk0p1/dvr/` | 视频存储 |

## 快速开始

### 1. 编译 (在虚拟机上)

```bash
# 雷达程序
cd /home/alientek/dvr_project/mier/pro
export PATH=/home/alientek/Phytium_syscode/GCC编译器/gcc-arm-10.2-2020.11-x86_64-aarch64-none-linux-gnu/bin:$PATH
make

# DVR 程序
cd /home/alientek/dvr_project/mier/dvr
make CC=aarch64-none-linux-gnu-gcc CXX=aarch64-none-linux-gnu-g++
```

### 2. 部署到开发板

```bash
cd /home/alientek/dvr_project/mier/pro && make deploy
cd /home/alientek/dvr_project/mier/dvr && make deploy
```

### 3. 在开发板上运行

```bash
ssh root@192.168.88.10
cd /xxl/pro
./start_pro.sh              # 启动雷达 + DVR联调
./start_pro.sh stop         # 停止
```

### 4. 运行模式

| 模式 | 命令 | 说明 |
|------|------|------|
| 联调 (雷达+DVR) | `./start_pro.sh` | 雷达触发 → DVR录制 + NPU验证 |
| 纯雷达 | `./radar_link` | 仅雷达数据解析 |
| DVR 无NPU | `./dvr -d /dev/video6` | 信任雷达直接触发 |
| DVR + NPU | `./dvr --npu-model ...` | 摄像头AI验证雷达目标 |
| 自动测试 | `./dvr --auto` | 自动模拟目标→碰撞流程 |

## 触发逻辑

| 雷达事件 | 管道命令 | DVR 响应 |
|----------|---------|---------|
| 目标出现 | `TARGET_ON` | 开始缓冲录制 + NPU验证 |
| TTC < 10s | `COLLISION` | NPU确认后 → 保存前后15s视频 |
| 目标消失 | `TARGET_OFF` | 停止录制 |

## 编译说明

| 编译目标 | 命令 | 平台 |
|---------|------|------|
| DVR (VM) | `cd dvr && make` | x86_64 VM |
| DVR (交叉编译) | `cd dvr && make CC=aarch64-linux-gnu-gcc CXX=aarch64-linux-gnu-g++` | x86_64 → aarch64 |
| DVR (开发板含NPU) | `cd dvr && make board CC=gcc CXX=g++ HAS_LIBJPEG=1` | aarch64 开发板 |
| 雷达 (交叉编译) | `cd pro && make` | x86_64 → aarch64 |

## 各模块文档

- [雷达模块](radar/README.md) — 雷达硬件接口、协议、联调
- [DVR开发日志](dvr/docs/DEVELOPMENT_LOG.md) — 行车记录开发历程和NPU融合
- [音频模块](audio/docs/README.md) — 音频输出 (暂未联调)
- [设备树](dts/README.md) — 设备树配置说明