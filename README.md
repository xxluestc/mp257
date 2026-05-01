# STM32MP2 DVR 行车记录系统

基于 STM32MP2 异构多核架构的行车记录仪系统，支持摄像头实时采集、LCD 显示、环形缓冲和紧急事件触发录制。

## 硬件平台

- **主控**: STM32MP257F-DK 开发板
- **摄像头**: DCMIPP 接口摄像头 (OV5640)
- **显示**: 800×480 LCD (RGB565)
- **存储**: SD 卡 (/run/media/mmcblk0p1)

## 功能特性

- **V4L2 摄像头采集**: 640×480 @ 30fps RGB24 格式，mmap 零拷贝
- **ISP 颜色校正**: 集成 dcmipp-isp-ctrl 图像信号处理器配置
- **Framebuffer LCD 显示**: 直接操作 /dev/fb0，不依赖 Qt/Weston
- **SD 卡环形缓冲区**: 单文件磁盘缓冲，内存仅存索引，解决 OOM 问题
- **异步视频编码**: fork() 子进程执行 ffmpeg 编码，不阻塞主线程
- **命名管道触发**: 支持目标检测、预警、摔倒、碰撞等多种触发信号
- **DVR 状态机**: IDLE → BUFFERING → SAVING 完整状态转换逻辑

## 目录结构

```
dvr_project/
├── A_Core/                  # A核(Linux)主程序
│   ├── Makefile             # 交叉编译 Makefile
│   ├── dvr_main.c           # 主程序入口，命令行参数解析
│   └── test_trigger.sh      # 触发测试脚本
├── camera/                  # 摄像头采集模块
│   ├── camera_v4l2.c        # V4L2 采集 + ISP 控制
│   └── camera_v4l2.h
├── display/                 # LCD 显示模块
│   ├── display_lcd.c        # Framebuffer 直接显示
│   └── display_lcd.h
├── common/                  # 公共模块
│   ├── dvr_types.h          # 类型定义和默认参数
│   ├── ring_buffer.c        # SD 卡环形缓冲区
│   └── ring_buffer.h
├── recorder/                # 录制引擎
│   ├── dvr_engine.c         # DVR 状态机 + 异步编码
│   └── dvr_engine.h
├── ipc/                     # 进程间通信
│   ├── trigger_receiver.c   # 命名管道触发接收
│   └── trigger_receiver.h
├── DEBUG_LOG.md             # 调试记录
└── README.md
```

## 编译

```bash
# 加载 STM32MP2 SDK 环境
source /opt/st/stm32mp2/5.0.3-snapshot/environment-setup-cortexa35-ostl-linux

# 编译
cd A_Core
make
```

## 部署

```bash
# 关闭 Qt/Weston（释放 framebuffer）
ssh root@192.168.88.10 "killall -9 weston systemui 2>/dev/null"

# 传输可执行文件
scp dvr root@192.168.88.10:/usr/bin/

# 启动 DVR
ssh root@192.168.88.10 "nohup /usr/bin/dvr > /tmp/dvr.log 2>&1 &"
```

## 触发命令

通过命名管道 `/tmp/dvr_trigger_pipe` 发送指令：

| 命令 | 功能 |
|------|------|
| `TARGET_ON` | 目标检测开始，进入循环缓冲 |
| `TARGET_OFF` | 目标消失，停止缓冲 |
| `WARNING` | 预警触发，保存前后各 15 秒视频 |
| `FALL` | 摔倒触发，保存紧急片段 |
| `COLLISION` | 碰撞触发，保存紧急片段 |

### 测试示例

```bash
# 模拟目标检测
echo 'TARGET_ON' > /tmp/dvr_trigger_pipe

# 等待缓冲积累
sleep 5

# 触发紧急保存
echo 'WARNING' > /tmp/dvr_trigger_pipe

# 查看生成的视频
ls -lh /run/media/mmcblk0p1/emergency_*.mp4
ffprobe /run/media/mmcblk0p1/emergency_*.mp4
```

## 状态机

```
IDLE  ──TARGET_ON──▶  BUFFERING  ──TARGET_OFF──▶  IDLE
  │                      │
  │    WARNING/FALL/     │
  │    COLLISION         │
  └────── 紧急保存 ──────┘
```

- **IDLE**: 空闲状态，摄像头持续采集并显示，不写缓冲区
- **BUFFERING**: 缓冲状态，持续写入环形缓冲区
- **紧急保存**: 收到紧急信号后等待 15 秒积累触发后画面，保存前后各 15 秒共 30 秒视频

## 视频参数

- 分辨率: 640×480
- 帧率: 30 fps
- 编码: MPEG-4 Simple Profile
- 容器: MP4
- 码率: ~330 kbps

## 已解决的问题

1. **LCD 颜色异常** — ISP 控制必须在 VIDIOC_STREAMON 之后调用
2. **LCD 显示不全** — 使用 framebuffer stride (line_length) 计算内存偏移
3. **录制卡顿** — fork() 子进程异步编码，父进程立即返回
4. **刷屏黑条纹** — 移除每帧全屏 memset 清空操作
5. **内存 OOM** — 环形缓冲区改为 SD 卡单文件存储，内存仅存索引
