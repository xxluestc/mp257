# STM32MP2 DVR 行车记录系统

基于 STM32MP257 异构多核架构的行车记录仪，支持摄像头实时采集、LCD 显示、M核触发录制、SD 卡环形缓冲和紧急事件视频保存。

## 硬件平台

| 组件 | 型号/参数 |
|------|-----------|
| 主控 | STM32MP257F-DK 开发板 |
| A核 | Cortex-A35 (Linux) |
| M核 | Cortex-M33 (FreeRTOS) |
| 摄像头 | DCMIPP 接口 (OV5640), 640×480 @ ~24fps |
| 显示 | 800×480 LCD, RGB565 Framebuffer |
| 存储 | SD 卡 FAT32 (/run/media/mmcblk0p1) |
| 内存 | 762MB 总计 |

## 功能特性

- **实时显示**: 直接操作 `/dev/fb0` framebuffer，不依赖 Qt/Weston，~24fps 流畅
- **RGB565 原生管线**: 摄像头→LCD→缓冲区→ffmpeg 全链路 RGB565，零 CPU 格式转换
- **SD 卡环形缓冲**: 单文件磁盘缓冲（668MB），内存仅存索引（<10MB 进程内存）
- **异步编码**: `fork()` + ffmpeg 子进程编码，主线程不阻塞，LCD 仅冻结 ~8 秒
- **M 核触发**: 通过 RPMSG/OpenAMP 接收 M 核命令（TARGET_ON/WARNING/TARGET_OFF）
- **管道触发**: 命名管道 `/tmp/dvr_trigger_pipe` 支持外部进程发送命令
- **智能状态机**: IDLE → BUFFERING → SAVING 完整状态管理
- **片段管理**: 普通 WARNING 片段 FIFO 覆盖（最多3个），FALL/COLLISION 永久保护
- **SD 卡热插拔**: 检测到拔出时暂停写盘，进程不崩溃
- **恒定帧率输出**: 动态计算实际 fps + `-vsync cfr`，确保 30 秒视频流畅无抖动

## 快速开始

### 编译 A 核程序

```bash
source /opt/st/stm32mp2/5.0.3-snapshot/environment-setup-cortexa35-ostl-linux
cd dvr_project/A_Core && make
```

### 部署到开发板

```bash
# 传输可执行文件
scp A_Core/dvr root@192.168.88.10:/usr/local/bin/dvr

# 在开发板上启动
ssh root@192.168.88.10 "
  killall systemui weston 2>/dev/null   # 关闭Qt释放framebuffer
  sleep 2
  nohup /usr/local/bin/dvr > /tmp/dvr.log 2>&1 &
"
```

### 一键测试

```bash
# 在开发板上执行
test_dvr.sh
# 或手动:
echo 'TARGET_ON' > /tmp/dvr_trigger_pipe   # 开始缓冲
sleep 10
echo 'WARNING' > /tmp/dvr_trigger_pipe      # 触发紧急保存
sleep 50                                     # 等待编码完成
ls -lh /run/media/mmcblk0p1/emergency_*.mp4 # 查看保存的视频
```

### 将视频传回电脑

```bash
scp root@192.168.88.10:/run/media/mmcblk0p1/emergency_*.mp4 ./
```

## 触发命令

| 命令 | 来源 | 功能 |
|------|------|------|
| `TARGET_ON` | M核 / 管道 | 目标检测到，开始循环缓冲录制 |
| `TARGET_OFF` | M核 / 管道 | 目标消失，停止录制 |
| `WARNING` | M核 / 管道 | 预警信号，保存前后各15秒（共30秒） |
| `FALL` | 管道 | 摔倒信号，保存永久保护片段 |
| `COLLISION` | 管道 | 碰撞信号，保存永久保护片段 |

## 状态机

```
         TARGET_ON              WARNING/FALL/COLLISION
  IDLE ─────────────▶ BUFFERING ───────────────────▶ SAVING
    ▲                    │                              │
    │                    │ TARGET_OFF                  │ 编码完成
    │                    ▼                              │
    │                  IDLE ◄──────────────────────────┘
    │                  (目标消失)
    └──────────────────(目标仍在→继续BUFFERING)
```

## 视频输出参数

| 参数 | 值 |
|------|-----|
| 分辨率 | 640×480 |
| 时长 | **~30 秒** ✅ |
| 帧率 | 动态 (~24fps 实际, vsync cfr) |
| 编码 | MPEG-4 Simple Profile |
| 容器 | MP4 |
| 文件大小 | ~1MB |
| 像素格式 | rgb565 → yuv420p |

## 项目结构

```
dvr_project/
├── A_Core/                      # A核(Linux) 主程序
│   ├── Makefile                 # 交叉编译 (aarch64-ostl-linux-gcc)
│   ├── dvr_main.c               # 入口: 参数解析、模块初始化
│   └── dvr                      # 编译产物
├── camera/                      # 摄像头模块
│   ├── camera_v4l2.c/.h         # V4L2采集 + ISP控制 + RGB565输出
├── display/                     # LCD显示模块
│   ├── display_lcd.c/.h         # Framebuffer直接绘制(RGB565)
├── common/                      # 公共定义
│   ├── dvr_types.h              # 状态枚举、配置结构体、默认参数
│   ├── ring_buffer.c/.h         # SD卡单文件环形缓冲 + 异步写线程
├── recorder/                    # 录制引擎(核心)
│   ├── dvr_engine.c/.h          # 状态机 + select主循环 + 异步ffmpeg编码
├── ipc/                         # 通信模块
│   ├── trigger_receiver.c/.h    # 命名管道(FIFO)触发接收
│   └── rpmsg_channel.c/.h       # RPMSG通道(M↔A核通信)
├── M_Core/                      # M核固件备份
│   └── FREERTOS/App/
│       └── app_freertos.c       # DVR触发逻辑(4次命令循环)
├── test_dvr.sh                  # 一键测试脚本
├── test_videos/                 # 测试视频存档
├── README.md                    # 本文件
├── PROJECT_STRUCTURE.md         # 📐 架构图与数据流详解
└── DEBUG_LOG.md                 # 📝 完整调试记录与修复历史
```

> 详细架构图、数据流、状态机图、集成接口说明请查看 [PROJECT_STRUCTURE.md](PROJECT_STRUCTURE.md)

## 已解决的关键问题

| # | 问题 | 解决方案 |
|---|------|----------|
| 1 | LCD 颜色灰白/淡 | ISP 控制必须在 VIDIOC_STREAMON 之后调用 |
| 2 | LCD 右边缺一块 | 使用 framebuffer stride (line_length) 计算偏移 |
| 3 | 录制时 LCD 卡死 | fork() 子进程异步编码，父进程立即返回 |
| 4 | 屏幕黑条纹闪烁 | 移除每帧全屏 memset 清空操作 |
| 5 | 内存 OOM 崩溃 | 缓冲区从内存改为 SD 卡单文件存储 |
| 6 | 视频灰色波动 | RGB565→RGB24 格式匹配修复 |
| 7 | 视频只有 20 秒 | 动态 fps 计算 + DVR_BUFFER_SECONDS 调整为 38 |
| 8 | 13 秒处剧烈卡顿 | pause/resume 替代 545MB 内存预加载 |
| 9 | pause 死锁 | 先清空 pending 队列再设 paused 标志 |
| 10 | 播放抖动 | `-vsync cfr` 强制恒定帧率 + qsort 顺序读取优化 |
| 11 | M 核命令粘包 | 命令末尾加 `\n` + A 核按换行分割处理 |
| 12 | WARNING 误判为 PROTECTED | clip_type_t 类型映射修正 |

## M 核通信

M 核通过 OpenAMP/RPMSG 向 A 核发送 DVR 触发命令：

```
M核: app_freertos.c → VIRT_UART → RPMSG TTY → /dev/ttyRPMSG0
A核: rpmsg_channel.c → select()监听 → on_trigger()回调 → 状态机
```

M 核固件编译使用 STM32CubeIDE 内置 GCC 12.3.1，详见 [DEBUG_LOG.md](DEBUG_LOG.md)。

## 相关文档

- [PROJECT_STRUCTURE.md](PROJECT_STRUCTURE.md) — 完整架构图、数据流、状态机、集成接口
- [DEBUG_LOG.md](DEBUG_LOG.md) — 所有调试记录、修复历史、踩坑经验
- GitHub 仓库: https://github.com/xxluestc/mp257
