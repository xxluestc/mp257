# DVR 行车记录系统 - 调试记录

## 测试日期: 2026-05-01

## 系统配置
- 硬件: STM32MP2 开发板 + DCMIPP 摄像头
- LCD: 800x480, RGB565 (16bpp)
- 摄像头设备: /dev/video-camera0
- 存储: SD卡 /run/media/mmcblk0p1
- 交叉编译器: aarch64-ostl-linux-gcc (STM32MP2 SDK 5.0.3)

## 已修复问题

### 1. LCD显示灰白/颜色淡
- 原因: 缺少ISP(图像信号处理器)配置
- 修复: 在camera_v4l2.c中添加dcmipp-isp-ctrl调用
- 参考: systemui_src/mediaengine/camera.cpp 中的 ISPCtrolThread
- 关键: ISP控制必须在VIDIOC_STREAMON之后调用

### 2. LCD显示不全(右边缺一块)
- 原因: 直接用分辨率计算内存偏移，未考虑framebuffer stride
- 修复: 使用finfo.line_length作为stride计算偏移
- 文件: display/display_lcd.c

### 3. 录制时LCD卡顿
- 原因: ffmpeg编码在主线程执行，阻塞摄像头采集和LCD刷新
- 修复: fork()子进程异步编码
  - 父进程fork前收集帧偏移量(持锁)
  - 子进程独立打开buffer文件，pread()读取帧，管道送ffmpeg
  - 父进程立即返回，不阻塞主循环
- 文件: recorder/dvr_engine.c

### 4. 刷屏黑条纹
- 原因: 每帧全屏memset(0)清空导致闪烁
- 修复: 移除全屏清空，只绘制摄像头画面区域

### 5. 内存OOM
- 原因: 环形缓冲区全部在内存中(~800MB)
- 修复: 改为SD卡单文件缓冲，内存仅存索引
- 文件: common/ring_buffer.c (使用pwrite/pread避免lseek竞争)

## 测试结果

### 触发流程测试
```
TARGET_ON → 状态: IDLE → BUFFERING
WARNING   → 紧急保存触发，等待15秒缓冲
→ 异步编码启动 (pid=3282), 440帧
→ 保存完成: 440帧, 598KB, 14.67秒
```

### 视频参数
- 分辨率: 640x480 @ 30fps
- 编码: MPEG-4 Simple Profile
- 容器: MP4
- 码率: ~331 kb/s

### 内存占用
- DVR进程: ~7.5MB VSZ
- 系统可用: ~497MB

## 待解决问题

### 摄像头分辨率
- 摄像头支持 16x16 ~ 4096x2160 (Continuous)
- v4l2-ctl可设置800x480并成功流传输
- 但V4L2 API直接设置800x480时STREAMON失败(EINVAL)
- 可能原因: DCMIPP驱动对单平面API有分辨率约束
- 临时方案: 使用640x480，LCD居中显示

## 关键文件
- A_Core/dvr_main.c       - 主程序入口
- camera/camera_v4l2.c    - V4L2摄像头采集 + ISP控制
- display/display_lcd.c   - Framebuffer LCD显示
- common/ring_buffer.c    - SD卡环形缓冲区
- recorder/dvr_engine.c   - DVR状态机 + 异步编码
- ipc/trigger_receiver.c  - 命名管道触发接收
- ipc/rpmsg_channel.c     - RPMSG通道(M核↔A核通信)

---

## 测试日期: 2026-05-03

## 异构通信集成 (M核 ↔ A核)

### 架构概述
```
M核 (Cortex-M33, FreeRTOS)          A核 (Cortex-A35, Linux)
┌─────────────────────────┐         ┌──────────────────────────┐
│ M_Send_Task (5s周期)     │         │ DVR Engine               │
│   TARGET_ON / WARNING   │         │   rpmsg_channel (fd=7)   │
│   / TARGET_OFF          │         │   trigger_receiver (pipe)│
│         ↓               │         │         ↑                │
│   SendQueue             │  RPMsg  │   select() 监听          │
│         ↓               │ ════════│         ↑                │
│   OpenAMP_Task          │  TTY    │   命令解析 → 状态机      │
│   VIRT_UART_TransmitNB  │         │                          │
└─────────────────────────┘         └──────────────────────────┘
```

### M核固件修改
- 文件: app_freertos.c (M_Send_Task)
- 原功能: echo demo (发送11-20)
- 新功能: DVR触发命令循环发送
  - Cycle 1: TARGET_ON (开始缓冲)
  - Cycle 2: WARNING (紧急保存)
  - Cycle 3: TARGET_OFF (停止缓冲)
- 关键修复: 命令末尾添加换行符 `\n`，防止粘包

### M核编译
- 工具链: STM32CubeIDE内置 GCC 12.3.1
  - 路径: `/home/alientek/download/makeself_dir_RBGMxz/y/plugins/com.st.stm32cube.ide.mcu.externaltools.gnu-tools-for-stm32.12.3.rel1.linux64_1.1.0.202410170702/tools`
- 编译命令:
  ```bash
  export PATH="$GCC_TOOLS/bin:$PATH"
  cd STM32CubeIDE/CM33/NonSecure/CA35TDCID_m33_ns_sign
  make main-build
  ```
- 编译脚本: /tmp/build_m33_2.sh
- 输出: OpenAMP_TTY_echo_FreeRTOS_CM33_NonSecure.elf
- 注意: 需先移除所有.mk文件中的 `-fcyclomatic-complexity` 选项(GCC 11.3不支持)

### M核固件部署
```bash
# 停止M核
echo stop > /sys/class/remoteproc/remoteproc0/state

# 拷贝固件
scp OpenAMP_TTY_echo_FreeRTOS_CM33_NonSecure.elf root@192.168.88.10:/lib/firmware/

# 加载固件
echo OpenAMP_TTY_echo_FreeRTOS_CM33_NonSecure.elf > /sys/class/remoteproc/remoteproc0/firmware
echo start > /sys/class/remoteproc/remoteproc0/state
```

### A核DVR修改
- 新增文件: ipc/rpmsg_channel.c / ipc/rpmsg_channel.h
- 功能: 打开/dev/ttyRPMSG0(或1)，非阻塞读取M核命令
- 支持命令: TARGET_ON, TARGET_OFF, WARNING, FALL, COLLISION
- RPMSG设备自动探测: 先尝试ttyRPMSG0，失败则尝试ttyRPMSG1
- dvr_engine.c: 主循环select()同时监听camera fd、trigger pipe fd、rpmsg fd

### 当前状态 (2026-05-03)

#### 已完成
- ✅ M核固件编译成功 (MD5: ee4c99c65e448e5789f2ad4ae424d893)
- ✅ M核固件部署到开发板 /lib/firmware/
- ✅ M核启动成功，remoteproc状态: running
- ✅ RPMSG通道创建: /dev/ttyRPMSG0
- ✅ DVR启动成功，RPMSG通道打开 (fd=7)
- ✅ DVR主循环运行，摄像头采集正常，LCD显示正常
- ✅ 命名管道触发正常

#### 待解决: M核RPMSG无数据流通 ✅ 已解决 (2026-05-03)
- 现象: M核启动后，/dev/ttyRPMSG0 无任何数据输出
- 根因1: **开发板重启后加载的是原始固件** `OpenAMP_TTY_echo_CM33_NonSecure.elf`（非修改版）
  - 修复: 每次重启后需重新SCP部署修改后固件到 /lib/firmware/
- 根因2: **IDE副本缺少换行符** — Makefile编译用的是STM32CubeIDE目录下的副本
  - 源码副本(CM33/NonSecure/FREERTOS/App/): 有 `\n` ✅
  - IDE副本(STM32CubeIDE/.../Application/User/FREERTOS/App/): **无 `\n`** ❌
  - 修复: 在IDE副本中添加 `\n`: `"TARGET_ON\n"`, `"WARNING\n"`, `"TARGET_OFF\n"`
- 根因3: **DVR的RPMSG解析不处理粘包** — 多条命令粘在一起时只处理第一条
  - 修复: 重写 rpmsg_channel_process()，按 `\n` 分割逐条处理

### 端到端测试结果 (2026-05-03) ✅ 全部通过

#### 测试环境
- 开发板IP: 192.168.88.10, 刚重启
- M核固件: OpenAMP_TTY_echo_FreeRTOS_CM33_NonSecure.elf (含\n分隔符)
- RPMSG设备: /dev/ttyRPMSG1 (自动探测)
- DVR参数: 640x480@30fps, SD卡缓冲30秒, LCD显示开启

#### 测试日志 (关键输出)
```
[RPMSG] Cannot open /dev/ttyRPMSG0: No such file or directory
[RPMSG] Channel opened: /dev/ttyRPMSG1 (fd=7)        ← 自动探测成功
[DVR] Engine created, state=IDLE
[DVR] Main loop started

[RPMSG] Received from M-core: TARGET_ON               ← 命令正确分割!
[DVR] >>> STATE: IDLE -> BUFFERING (target detected)   ← 状态转换正确!
[RPMSG] Received from M-core: WARNING                 ← 紧急命令到达
[DVR] >>> EMERGENCY triggered: WARNING, will save after 15s buffer
[RPMSG] Received from M-core: TARGET_OFF
[DVR] >>> STATE: BUFFERING -> IDLE (target lost)      ← 循环正常

[RPMSG] Received from M-core: TARGET_ON               ← 第2轮循环
[DVR] >>> STATE: IDLE -> BUFFERING (target detected)
[RPMSG] Received from M-core: WARNING
[DVR] >>> EMERGENCY triggered: WARNING, will save after 15s buffer
```

#### 验证项
| 测试项 | 结果 | 说明 |
|--------|------|------|
| M核编译(GCC 12.3.1) | ✅ | EXIT=0, ELF 3875992字节 |
| M核固件部署 | ✅ | SCP到/lib/firmware/ |
| M核启动 | ✅ | remoteproc状态: running |
| RPMSG通道创建 | ✅ | /dev/ttyRPMSG1 |
| RPMSG设备自动探测 | ✅ | 先试ttyRPMSG0失败→自动切换ttyRPMSG1 |
| M核→A核数据传输 | ✅ | cat /dev/ttyRPMSG1 收到 TARGET_ON |
| 命令分割(去粘包) | ✅ | 每条命令独立接收，不再粘连 |
| TARGET_ON → BUFFERING | ✅ | IDLE → BUFFERING 状态转换 |
| WARNING → EMERGENCY | ✅ | 紧急保存触发 |
| TARGET_OFF → IDLE | ✅ | BUFFERING → IDLE 状态转换 |
| 多轮循环稳定 | ✅ | 连续2+轮循环均正常 |

#### 已知限制
- 测试周期5秒太短，WARNING触发时缓冲不足15秒导致"No frames in range"
- 实际使用中TARGET_ON会持续更长时间（雷达持续检测到目标），不会有此问题

### 调试命令速查
```bash
# 查看M核状态
cat /sys/class/remoteproc/remoteproc0/state

# 查看RPMSG设备
ls -la /dev/ttyRPMSG*

# 查看内核日志
dmesg | grep -i 'rpmsg\|remoteproc'

# 启动DVR(含RPMSG)
killall dvr 2>/dev/null
rm -f /run/media/mmcblk0p1/dvr_buffer.bin
nohup /usr/bin/dvr -W 640 -H 480 -f 30 -b 30 -s /run/media/mmcblk0p1 -d /dev/video-camera0 -D 1 > /tmp/dvr.log 2>&1 &

# 查看DVR日志
cat /tmp/dvr.log

# 手动触发测试(命名管道)
echo "TARGET_ON" > /tmp/dvr_trigger_pipe
echo "WARNING" > /tmp/dvr_trigger_pipe
echo "TARGET_OFF" > /tmp/dvr_trigger_pipe
```

### 项目文件结构
```
dvr_project/
├── A_Core/
│   ├── Makefile
│   ├── dvr_main.c
│   └── dvr              # 编译产物
├── camera/
│   ├── camera_v4l2.c
│   └── camera_v4l2.h
├── display/
│   ├── display_lcd.c
│   └── display_lcd.h
├── common/
│   ├── ring_buffer.c
│   └── ring_buffer.h
├── recorder/
│   ├── dvr_engine.c
│   └── dvr_engine.h
├── ipc/
│   ├── trigger_receiver.c
│   ├── trigger_receiver.h
│   ├── rpmsg_channel.c
│   └── rpmsg_channel.h
├── M_Core/                    # M核固件备份
│   └── FREERTOS/App/
│       └── app_freertos.c     # 原始echo demo备份
├── README.md
└── DEBUG_LOG.md
```
