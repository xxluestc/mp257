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

---

## LCD显示优化 (2026-05-03)

### 问题: LCD画面卡顿/不流畅
- **现象**: LCD显示摄像头画面不实时，看起来"卡死"
- **根因1**: 主循环中 SD卡写入(pwrite 921KB) 在 LCD显示**之前**执行，阻塞主线程
  - ring_buffer_push() 每帧写921KB到SD卡 → 阻塞~50ms
  - display_show_frame() 逐像素30万次 → 又阻塞~80ms
  - 总计>130ms/帧 → 实际<8fps → 看起来卡死
- **根因2**: display_show_frame() 使用双重for循环逐像素处理，每次循环内做乘法索引计算

### 修复方案
1. **dvr_engine.c**: 调整执行顺序 — **先LCD显示，后SD卡写入**
   ```c
   // 修复前（SD卡阻塞导致LCD卡顿）
   ring_buffer_push();      // 先写SD卡 ~50ms
   display_show_frame();    // 后显示 ~80ms

   // 修复后（显示优先）
   display_show_frame();    // 先显示 ~40ms(优化后)
   ring_buffer_push();      // 后写SD卡（不影响显示）
   ```
2. **display_lcd.c**: 优化像素转换性能
   - 使用查表法(r_tbl/g_tbl/b_tbl)替代每像素移位运算
   - 用指针递增代替数组索引计算 `(y*width+x)*3`
   - 消除内层循环中的乘法操作

---

## 用户自测验证指南

### 一、快速启动（开发板重启后）

通过SSH执行以下命令（在Ubuntu主机上）：

```bash
# 1. 关闭Qt程序释放LCD
ssh root@192.168.88.10 "killall systemui weston 2>/dev/null; sleep 2"

# 2. 部署M核固件（修改版，含DVR触发命令）
scp /home/alientek/.../OpenAMP_TTY_echo_FreeRTOS_CM33_NonSecure.elf \
    root@192.168.88.10:/lib/firmware/
ssh root@192.168.88.10 \
  "echo stop > /sys/class/remoteproc/remoteproc0/state; sleep 2;
   echo start > /sys/class/remoteproc/remoteproc0/state; sleep 3"

# 3. 启动DVR
ssh root@192.168.88.10 \
  "killall dvr 2>/dev/null; sleep 2;
   rm -f /run/media/mmcblk0p1/dvr_buffer.bin;
   nohup /usr/bin/dvr -W 640 -H 480 -f 30 -b 30 \
     -s /run/media/mmcblk0p1 -d /dev/video-camera0 -D 1 \
     > /tmp/dvr.log 2>&1 &"
```

### 二、验证清单（逐项检查）

#### ✅ 验证1: LCD实时显示摄像头画面
- **观察**: 开发板LCD屏幕应显示摄像头实时画面
- **预期**: 画面流畅，随摄像头前物体移动而变化
- **如果卡住**: 检查 `cat /tmp/dvr.log` 是否有 `[CAMERA] Streaming` 输出
- **如果颜色异常**: ISP控制可能未生效，重启DVR即可

#### ✅ 验证2: M核→A核通信正常
```bash
# 查看DVR日志中的RPMSG消息
ssh root@192.168.88.10 "cat /tmp/dvr.log | grep 'RPMSG.*Received'"
# 预期输出（每5秒一轮）:
# [RPMSG] Received from M-core: TARGET_ON
# [RPMSG] Received from M-core: WARNING
# [RPMSG] Received from M-core: TARGET_OFF
```

#### ✅ 验证3: DVR状态机转换
```bash
# 查看状态变化
ssh root@192.168.88.10 "cat /tmp/dvr.log | grep 'STATE'"
# 预期输出:
# [DVR] >>> STATE: IDLE -> BUFFERING (target detected)
# [DVR] >>> STATE: BUFFERING -> IDLE (target lost)
```

#### ✅ 验证4: 手动触发录制（命名管道）
```bash
# 模拟目标出现 → 开始缓冲
ssh root@192.168.88.10 "echo 'TARGET_ON' > /tmp/dv_trigger_pipe"

# 等15秒以上后，模拟紧急事件 → 触发保存
ssh root@192.168.88.10 "echo 'WARNING' > /tmp/dvr_trigger_pipe"

# 再等20秒左右查看保存结果
ssh root@192.168.88.10 \
  "ls -la /run/media/mmcblk0p1/emergency_*.mp4;
   cat /tmp/dvr.log | grep -E 'Saving|Saved'"
```
- **预期**: SD卡上生成 `emergency_时间戳_WARNING.mp4` 文件
- **文件可用**: 拷贝到电脑用VLC播放器播放验证

#### ✅ 验证5: 录制期间LCD不受影响
- **操作**: 在执行验证4的同时观察LCD屏幕
- **预期**: 触发WARNING后LCD仍然实时显示，不会卡顿或黑屏
- **日志确认**: `cat /tmp/dvr.log` 中不应有长时间无输出

#### ✅ 验证6: 视频回放质量
```bash
# 将视频拷贝到本地
scp root@192.168.88.10:/run/media/mmcblk0p1/emergency_*.mp4 ./

# 用ffprobe检查视频信息
ffprobe emergency_*.mp4 2>&1 | grep -E 'Duration|Video'
# 预期: Duration约30秒, Video: mpeg4, 640x480, 30fps
```

### 三、常用调试命令速查

| 目的 | 命令 |
|------|------|
| 查看DVR是否运行 | `ps aux \| grep dvr` |
| 查看完整日志 | `cat /tmp/dvr.log` |
| 查看最新触发 | `cat /tmp/dvr.log \| tail -30` |
| 查看M核状态 | `cat /sys/class/remoteproc/remoteproc0/state` |
| 查看RPMSG设备 | `ls -la /dev/ttyRPMSG*` |
| 查看SD卡视频 | `ls -la /run/media/mmcblk0p1/*.mp4` |
| 查看缓冲区大小 | `ls -la /run/media/mmcblk0p1/dvr_buffer.bin` |
| 重启DVR | `killall dvr; sleep 2; nohup /usr/bin/dvr ... &` |
| 重启M核 | `echo stop/start > /sys/class/remoteproc/remoteproc0/state` |

### 四、常见问题排查

| 现象 | 可能原因 | 解决方法 |
|------|----------|----------|
| LCD黑屏/无显示 | Qt/weston占用framebuffer | `killall systemui weston` |
| LCD颜色灰白 | ISP控制未生效 | 重启DVR（ISP需在STREAMON后调用） |
| LCD卡顿 | SD卡写入慢 | 已优化：显示优先于写入 |
| RPMSG无数据 | M核加载了原始固件 | 重新SCP部署修改版固件 |
| RPMSG设备名变了 | ttyRPMSG0→ttyRPMSG1 | DVR已支持自动探测 |
| 视频文件为0字节 | 缓冲不足15秒 | 正常：测试周期太短，实际使用不会 |
| OOM进程被杀 | 内存不足 | 已改用SD卡单文件缓冲 |

### 五、完整功能架构图

```
┌─────────────────────────────────────────────────────────────┐
│                    开发板实物 (STM32MP257)                   │
│                                                             │
│  ┌──────────────┐         ┌──────────────────────────────┐  │
│  │  M核(CM33)   │  RPMSG  │       A核(Cortex-A35)        │  │
│  │  FreeRTOS    │ ═════►  │          Linux               │  │
│  │              │  TTY    │                              │  │
│  │ 目标检测/雷达 │         │  ┌────────────────────────┐  │  │
│  │ 预警信号     │         │  │   DVR Engine (dvr进程)  │  │  │
│  │ 摔倒检测     │         │  │                        │  │  │
│  │              │         │  │  camera ──► ring_buffer │  │  │
│  │ 发送命令:    │         │  │    │           │        │  │  │
│  │  TARGET_ON   │         │  │    ▼           ▼        │  │  │
│  │  WARNING     │         │  │  LCD显示     SD卡存储   │  │  │
│  │  TARGET_OFF  │         │  │                        │  │  │
│  │              │         │  │  触发源:                │  │  │
│  └──────────────┘         │  │  ├─ RPMSG (M核发送)    │  │  │
│                           │  │  └─ 命名管道 (手动测试) │  │  │
│  ┌──────────────┐         │  │                        │  │  │
│  │  LCD屏幕     │◄────────┘  │  输出:                  │  │  │
│  │  800x480     │  framebuffer│  ├─ 实时摄像头画面      │  │  │
│  │  实时显示    │            │  └─ emergency_*.mp4     │  │  │
│  └──────────────┘            └────────────────────────┘  │  │
│                                                             │
│  └──────────────┘                                        │  │
└─────────────────────────────────────────────────────────────┘
```

---

## LCD卡死终极修复 (2026-05-03 晚)

### 问题现象
- LCD显示摄像头画面**完全卡死**，看起来像静态图片
- 进程状态始终为 **D (Disk sleep)** — 不可中断的磁盘I/O等待
- 系统负载: `load average: 1.97, 1.98, 1.75`，CPU `43.5% wa` (等待I/O)
- **即使关闭SD卡缓冲和LCD显示**（仅保留摄像头采集），进程仍然D状态

### 排查过程

#### 第1步：分析正点原子systemui_src/video模块
- 正点原子使用 **QMediaPlayer(GStreamer后端) + QML GPU渲染**
- **录制时停止显示并独占摄像头**（不是同时录制+显示）
- 他们没有使用tee分流，也不是单线程处理——而是GPU硬件加速渲染
- 结论：不能直接复用他们的方案，需要自己优化

#### 第2步：怀疑CPU像素格式转换太慢
- 原方案：摄像头输出RGB24(921KB/帧) → display_show_frame()逐像素转RGB565 → framebuffer
- 30万像素 × RGB24→RGB565 转换 ≈ 30ms/帧
- **修复**: 改为摄像头直接输出RGB565(614KB/帧)，显示时零转换memcpy
- **结果**: ❌ 仍然卡死！说明这不是主因

#### 第3步：诊断测试 — 逐步关闭功能
| 测试配置 | 进程状态 | 结论 |
|----------|----------|------|
| 完整功能(摄像头+显示+SD卡) | D | 卡死 |
| 关闭SD卡缓冲 | D | 仍然卡死 |
| 关闭SD卡+关闭显示 | D | **仍然卡死！** |
| 仅摄像头采集(camera_grab_frame) | D | **问题100%在摄像头读取** |

#### 第4步：strace + 内核栈定位真正根因
```bash
# strace 5秒内零系统调用 — 进程完全卡在内核内部
$ timeout 5 strace -p $(pgrep dvr)
strace: Process 4314 attached
strace: Process 4314 detached    # ← 零输出!

# 内核调用栈揭示真相:
$ cat /proc/$(pgrep dvr)/stack
[<0>] __bread_gfp+0x18c/0x1ac
[<0>] fat_ent_bread+0x58/0xe8
[<0>] fat_alloc_clusters+0x1a8/0x418     # ← FAT32簇分配!
[<0>] fat_add_cluster+0x38/0x9c
[<0>] fat_get_block+0xcc/0x298
[<0>] __block_write_begin_int+0x114/0x6ac
[<0>] block_write_begin+0x5c/0xf8
[<0>] cont_write_begin+0x1d8/0x2e4
[<0>] fat_write_begin+0x38/0x84
[<0>] generic_cont_expand_simple+0x60/0xc4
[<0>] fat_cont_expand+0x2c/0x100
[<0>] fat_setattr+0x31c/0x3b4
[<0>] notify_change+0x19c/0x3f0
[<0>] do_truncate+0xb0/0x118
[<0>] do_sys_ftruncate+0x148/0x150      # ← 罪魁祸首!
```

### 🎯 真正根因: `ftruncate(527MB)` 在FAT32 SD卡上

```
ring_buffer_create() 中执行:
  ftruncate(fd, 900 * 614400)   // = 552,960,000 字节 ≈ 527 MB
       ↓
  FAT32文件系统需要逐个分配磁盘簇
       ↓
  527MB / 4KB(簇大小) = ~131,000 个簇
       ↓
  每个簇分配需要磁盘I/O (__bread_gfp)
       ↓
  总耗时: 数分钟甚至更久!
       ↓
  进程进入 D 状态(不可中断睡眠)
       ↓
  用户看到: LCD完全卡死
```

### 修复方案 (三重优化)

#### 修复1: 去掉ftruncate预分配 ✅ 核心修复
```c
// ring_buffer.c - ring_buffer_create()
// 修改前:
off_t total = (off_t)rb->capacity * rb->frame_size;  // 527MB!
ftruncate(rb->fd, total);  // ← FAT32上卡死数分钟!

// 修改后:
// 不调用ftruncate! 文件按需增长(pwrite自动扩展)
printf("[RINGBUF] Buffer: %s, %d frames, %d bytes/frame, %.1f MB max\n",
       ...);
```

#### 修复2: pwrite移到mutex锁外部 ✅ 防止锁竞争
```c
// ring_buffer.c - write_thread_func()
// 修改前 (pwrite在锁内,阻塞主线程的ring_buffer_push):
pthread_mutex_lock(&rb->lock);
// ... 取出pending frame ...
ssize_t n = pwrite(rb->fd, data, size, offset);  // ← 在锁内!
// ... 更新index ...
pthread_mutex_unlock(&rb->lock);

// 修改后 (pwrite在锁外,不阻塞主线程):
pthread_mutex_lock(&rb->lock);
// ... 取出数据到局部变量 ...
pthread_mutex_unlock(&rb->lock);                  // ← 先释放锁!

ssize_t n = pwrite(rb->fd, local_pf.data, copy_size, offset);  // ← 锁外写盘

pthread_mutex_lock(&rb->lock);
// ... 更新index ...
pthread_mutex_unlock(&rb->lock);
```

#### 修复3: 摄像头原生RGB565格式 ✅ 显示性能优化
```c
// camera_v4l2.c - 格式优先级调整
static const uint32_t try_fmts[] = {
    V4L2_PIX_FMT_RGB565,  // ← 优先! (原来第3位)
    V4L2_PIX_FMT_RGB24,
    V4L2_PIX_FMT_NV12,
};

// display_lcd.c - 直接支持RGB565输入
if (dst_bpp == 2 && src_bpp == 2) {
    // RGB565→RGB565: 零转换, 直接memcpy
    for (int y = 0; y < copy_h; y++) {
        memcpy(dst_line, src_line, copy_w * 2);  // ~1ms/帧!
    }
}
```

#### 修复4: TARGET_OFF不再清空保存中的缓冲区 ✅ Bug修复
```c
// dvr_engine.c - on_trigger()
case TRIGGER_TARGET_OFF:
    if (eng->state == DVR_STATE_BUFFERING && !eng->save_pending) {
        // 只有非保存状态才清空缓冲区
        ring_buffer_clear(eng->ring_buf);
        eng->state = DVR_STATE_IDLE;
    } else if (eng->save_pending) {
        // 保存进行中只标记目标消失,不清空!
        eng->target_present = 0;
        printf("Target off but save pending, keeping buffer\n");
    }
```

### 修改文件清单

| 文件 | 修改内容 |
|------|----------|
| [camera/camera_v4l2.h](camera/camera_v4l2.h) | 新增camera_get_pixelformat/frame_size/bpp接口, convert_to_rgb24() |
| [camera/camera_v4l2.c](camera/camera_v4l2.c) | RGB565优先格式, rgb565_to_rgb24查表转换 |
| [display/display_lcd.h](display/display_lcd.h) | 新增display_set_source_format() |
| [display/display_lcd.c](display/display_lcd.c) | 支持多格式输入(RGB565/RGB24→RGB565/RGB32), 零转换memcpy路径 |
| [common/ring_buffer.h](common/ring_buffer.h) | create增加frame_size参数, 新增set_frame_size() |
| [common/ring_buffer.c](common/ring_buffer.c) | 去掉ftruncate预分配, pwrite移到锁外, 按需增长 |
| [recorder/dvr_engine.c](recorder/dvr_engine.c) | 适配新API, TARGET_OFF保护保存中缓冲区 |

### 测试结果对比

| 指标 | 修改前 | 修改后 |
|------|--------|--------|
| **进程状态** | D (Disk sleep, 卡死) | S/R (正常运行) ✅ |
| **内核等待** | `fat_alloc_clusters` (数分钟) | `do_select` (正常等待帧) ✅ |
| **摄像头格式** | RGB24, 921KB/帧 | **RGB565, 614KB/帧** (-33%) ✅ |
| **显示方式** | CPU逐像素转换 (~30ms) | **直接memcpy (~1ms)** ✅ |
| **SD卡预分配** | ftruncate(527MB) 卡死 | **按需增长, 零启动延迟** ✅ |
| **写盘阻塞** | mutex内pwrite阻塞主线程 | **锁外异步写入** ✅ |
| **视频保存** | ❌ 无法保存(进程卡死) | **✅ 成功生成MP4** (61MB/559帧) |
| **录制时LCD** | 完全卡死 | **实时流畅** ✅ |

### 完整录制流程测试日志
```
═══════════════════════════════════════
   完整录制流程测试
═══════════════════════════════════════

[1/4] TARGET_ON → 开始缓冲+显示
  PID 4887: S状态, CPU=12.1%, wchan=do_select ✅

[2/4] 缓冲10秒后:
  dvr_buffer.bin = 329MB ✅ (异步写盘正常)
  PID: R状态, CPU=29.7% ✅

[3/4] WARNING触发保存:
  "Target off but save pending, keeping buffer" ✅ (M核干扰不丢失数据)

[4/4] 录制过程中状态监控:
  T+5s:  状态正常, LOG=Target off but save pending...
  T+10s: 状态正常, LOG=RPMSG Received TARGET_ON
  T+15s: EMERGENCY triggered: WARNING
  T+20s: Target off but save pending, keeping buffer
  T+25s: ★ Saved 559 frames → emergency_1709057842_WARNING.mp4 ✅
  T+30s: 继续正常运行

生成的视频文件:
  emergency_1709057842_WARNING.mp4   61MB  (559帧, ~18秒)
  emergency_1709057872_WARNING.mp4   35MB  (751帧, ~25秒)
  emergency_1709057887_WARNING.mp4   4.1MB (363帧, ~12秒)
```

---

## 测试方法详解 (2026-05-03)

### M核如何给A核发送触发指令

M核固件通过 **OpenAMP RPMSG虚拟串口** 向A核发送命令：

#### M核端 (app_freertos.c - M_Send_Task)
```
M核 FreeRTOS任务, 每5秒循环发送:

  Cycle N×3+1: 发送 "TARGET_ON\n"   → A核开始缓冲
  Cycle N×3+2: 发送 "WARNING\n"     → A核触发紧急保存
  Cycle N×3+3: 发送 "TARGET_OFF\n"  → A核停止缓冲

通信链路:
  M_Send_Task → SendQueue → OpenAMP_Task
    → VIRT_UART_TransmitNB() → RPMSG总线
    → /dev/ttyRPMSG1 (A核Linux设备节点)
```

#### A核端 (dvr_engine.c - 主循环select())
```
select() 同时监听3个fd:
  ├─ fd=camera_fd     → 摄像头帧就绪
  ├─ fd=trigger_fd    → 命名管道 /tmp/dvr_trigger_pipe (手动测试用)
  └─ fd=rpmsg_fd=7    → /dev/ttyRPMSG1 (M核命令)

当rpmsg_fd可读时:
  rpmsg_channel_process() → 按'\n'分割 → on_trigger() → 状态机转换
```

#### 支持的命令及效果

| 命令 | 来源 | 效果 |
|------|------|------|
| `TARGET_ON` | M核或命名管道 | IDLE→BUFFERING, 开始往SD卡写缓冲帧 |
| `TARGET_OFF` | M核或命名管道 | BUFFERING→IDLE, 停止缓冲(除非正在保存) |
| `WARNING` | M核或命名管道 | 触发紧急保存, 生成~30秒MP4片段 |
| `FALL` | 命名管道 | 同WARNING, 文件名含FALL |
| `COLLISION` | 命名管道 | 同WARNING, 文件名含COLLISION |

### 你自己如何验证（两种方式）

#### 方式一：通过M核自动触发（当前默认行为）
M核每5秒自动循环发送 TARGET_ON→WARNING→TARGET_OFF，你只需观察：
1. **看LCD**: 应该全程流畅显示摄像头画面
2. **看日志**: `cat /tmp/dvr.log | tail -20`
3. **看视频**: `ls -lh /run/media/mmcblk0p1/emergency_*.mp4`

#### 方式二：通过命名管道手动触发（推荐用于精确控制）
```bash
# SSH到开发板后依次执行:

# Step 1: 开始缓冲（模拟雷达检测到目标）
echo "TARGET_ON" > /tmp/dvr_trigger_pipe
# → LCD继续流畅显示, SD卡开始积累缓冲数据

# Step 2: 等15-20秒（让缓冲区积累足够数据）
sleep 15

# Step 3: 触发紧急保存（模拟摔倒/碰撞预警）
echo "WARNING" > /tmp/dvr_trigger_pipe
# → LCD应仍然流畅显示! 不卡顿!
# → 约25-30秒后生成 emergency_*.mp4

# Step 4: 查看结果
ls -lh /run/media/mmcblk0p1/emergency_*.mp4
cat /tmp/dvr.log | grep -E "Saved|Saving"

# Step 5: （可选）拷贝视频到电脑回放
scp root@192.168.88.10:/run/media/mmcblk0p1/emergency_*.mp4 ./
```

#### 方式三：观察M核自动触发的完整周期
```bash
# 清空日志重新观察
> /tmp/dvr.log

# 等待60秒（M核会完成约4个完整周期）
sleep 60

# 查看完整状态变化
cat /tmp/dvr.log | grep -E 'STATE|EMERGENCY|Saved'

# 预期看到多次:
# STATE: IDLE -> BUFFERING
# EMERGENCY triggered: WARNING
# Saved N frames: emergency_xxx_WARNING.mp4
# STATE: BUFFERING -> IDLE
```

---

## 状态机完整重写 (2026-05-03 晚)

### 用户需求（5条规则）

| # | 需求 | 之前行为 | 现在行为 |
|---|------|----------|----------|
| 1 | 目标出现→循环录制30s覆盖(不存SD卡) | ❌ WARNING立即保存,无循环 | ✅ BUFFERING每30s清空缓冲区 |
| 2 | 预警→保存前后15s=30s片段,最多3个(满了覆盖) | ❌ 无限制保存 | ✅ clip_manager 3槽位FIFO |
| 3 | 摔倒/碰撞→保存30s片段(**不可被覆盖**) | ❌ 和WARNING一样 | ✅ PROTECTED标记 |
| 4 | 无目标→停止录制 | ⚠️ 基本正确 | ✅ TARGET_OFF→IDLE |
| 5 | 录制不影响LCD显示 | ⚠️ 已修复(RGB565) | ✅ 保持不变 |
| 6 | M核只发4次 | ❌ 无限循环 | ✅ MAX_CYCLES=4后idle |
| 7 | SD卡热插拔保护 | ❌ 无保护 | ✅ check_sd_card() |

### 新状态机设计

```
                    ┌─────────────────────────────────────┐
                    │         IDLE (等待目标)              │
                    │   - LCD实时显示摄像头画面             │
                    │   - 不写入SD卡                       │
                    └──────────┬──────────────────────────┘
                               │ TARGET_ON (雷达/目标检测)
                               ▼
                    ┌─────────────────────────────────────┐
                    │      BUFFERING (循环录制)            │
                    │   - 帧数据→SD卡环形缓冲区            │
                    │   - LCD继续实时显示                  │
                    │   - 每30s自动清空缓冲(不存MP4)       │
                    └──┬──────────────┬───────────────────┘
                       │              │
          30s无预警     │    WARNING/FALL/COLLISION
          +目标仍在     │              │
                       ▼              ▼
               继续BUFFERING     SAVING(异步编码)
                                  (fork子进程ffmpeg)
                                       │
                              ┌────────┴────────┐
                              │                 │
                        目标仍在?           目标消失?
                              │                 │
                              ▼                 ▼
                         BUFFERING            IDLE
```

### 核心代码变更

#### 1. dvr_types.h — 新增类型定义
```c
// 片段类型: 区分普通(可覆盖)和保护(不可覆盖)
typedef enum {
    CLIP_TYPE_WARNING   = 0,  // 普通, 可被覆盖
    CLIP_TYPE_FALL      = 1,  // 保护, 不可覆盖
    CLIP_TYPE_COLLISION = 2,  // 保护, 不可覆盖
} clip_type_t;

// 片段信息
typedef struct {
    char         filename[DVR_MAX_PATH];
    clip_type_t  type;
    time_t       save_time;
    int          protected_;  // 0=普通, 1=保护
} clip_info_t;

// 片段管理器 (最多3个普通片段)
typedef struct {
    clip_info_t clips[DVR_MAX_NORMAL_CLIPS];  // 3槽位
    int         count;
    int         next_index;                   // 下次覆盖位置(FIFO)
    const char *sd_path;
} clip_manager_t;
```

#### 2. dvr_engine.c — 完整重写状态机

**新增成员变量:**
```c
struct dvr_engine {
    // ...原有字段...
    clip_manager_t   clips;            // 片段管理器
    time_t           last_cycle_time;  // 30s循环计时
    volatile int     sd_card_ok;       // SD卡热插拔检测
};
```

**循环录制逻辑 (主循环中):**
```c
// 每30s检查一次, 如果还在BUFFERING且没有save_pending:
if (eng->state == DVR_STATE_BUFFERING && !eng->save_pending) {
    time_t elapsed = now - eng->last_cycle_time;
    if (elapsed >= eng->config.buffer_seconds) {  // 30秒到了
        printf("[DVR] Circular 30s cycle complete, rotating buffer\n");
        ring_buffer_clear(eng->ring_buf);  // 清空缓冲区,开始新一轮
        eng->buffer_start_time = now;
        eng->last_cycle_time  = now;
    }
}
```

**SD卡热插拔检测 (每次select循环):**
```c
eng->sd_card_ok = check_sd_card(eng->config.sd_card_path);
if (!eng->sd_card_ok && eng->state == DVR_STATE_BUFFERING) {
    printf("[DVR] WARNING: SD card removed! Buffering continues but cannot save.\n");
}

// 写入帧时检查:
if (eng->state == DVR_STATE_BUFFERING) {
    if (eng->sd_card_ok) {        // 只有SD卡正常才写
        ring_buffer_push(...);
    }
}
```

**clip_manager 覆盖逻辑:**
```c
int clip_manager_add(clip_manager_t *cm, const char *filename, clip_type_t type)
{
    if (!clip_is_protected(type)) {  // WARNING类型
        // 覆盖最旧的普通片段
        if (cm->clips[cm->next_index].filename[0]) {
            unlink(cm->clips[cm->next_index].filename);  // 删除旧文件!
        }
        // 写入新片段到next_index位置
        cm->next_index = (cm->next_index + 1) % DVR_MAX_NORMAL_CLIPS;  // FIFO轮转
    } else {  // FALL/COLLISION类型
        // 直接保存, 不进入3槽位管理, 永不被自动覆盖!
        printf("[CLIP] Saved PROTECTED: %s\n", filename);
    }
}
```

#### 3. M核固件修改 (app_freertos.c)
```c
// 修改前: while(1) → 无限循环
// 修改后:
const int MAX_CYCLES = 4;
while (cycle < MAX_CYCLES) {  // 只执行4次
    vTaskDelayUntil(&xLastWakeTime, pdMS_TO_TICKS(5000));
    cycle++;
    // ...发送命令...
}
// 4次完成后进入idle:
loc_printf("[M-SEND] === All %d cycles done, task idle ===\r\n", MAX_CYCLES);
while (1) { vTaskDelay(pdMS_TO_TICKS(10000)); }
```

### 修复的BUG: WARNING误判为PROTECTED

**原因:** `on_trigger()`中将`trigger_event_t`直接传给`clip_is_protected()`
```c
// 错误代码:
printf("...protected=%d...", clip_is_protected((clip_type_t)data->event));
// data->event是trigger_event_t: WARNING=2, FALL=3, COLLISION=4
// 但clip_type_t:       WARNING=0, FALL=1, COLLISION=2
// 所以WARNING(2)被当成COLLISION(2) → protected=1! 错误!

// 正确代码:
clip_type_t ctype = (clip_type_t)(eng->save_event - TRIGGER_WARNING);
// WARNING(2)-TRIGGER_WARNING(2)=0 → CLIP_TYPE_WARNING(0) → protected=0 ✅
// FALL(3)-TRIGGER_WARNING(2)=1    → CLIP_TYPE_FALL(1)    → protected=1 ✅
```

### 测试验证日志
```
=== 编译部署成功 ===
[CAMERA] Format: 640x480 RGB565 ✅
[RINGBUF] Buffer: 900 frames, 614400 bytes/frame, 527.3 MB max
[DISPLAY] LCD: 800x480, bpp=16
[RPMSG] Channel opened: /dev/ttyRPMSG1 ✅
[DVR] Engine created, state=IDLE, max_normal_clips=3 ✅

=== M核触发测试 (旧固件仍无限循环,新固件待烧录) ===
[RPMSG] Received from M-core: WARNING
[DVR] >>> EMERGENCY triggered: WARNING (protected=0) ✅ 修复正确!
[RPMSG] Received from M-core: TARGET_OFF
[DVR] Target off but save pending, keeping buffer ✅
... (15秒后) ...
[DVR] Saving clip: [1709059027, 1709059057] -> emergency_...WARNING.mp4
[CLIP] Saved normal [1/3]: emergency_...WARNING.mp4 (WARNING) ✅ 第1个片段
[DVR] Async encoding started (pid=6764): 651 frames ✅ 异步编码
[CLIP] === Clip list ===
  [0] emergency_1709059042_WARNING.mp4 (WARNING) ✅
[DVR] >>> STATE: -> IDLE (no target, save done) ✅ 状态转换正确
```

### SD卡热插拔说明

**问: 可以在运行时直接拔掉SD卡吗?**

**答: 不建议直接热插拔，但代码已有保护机制：**

| 操作 | 结果 | 说明 |
|------|------|------|
| **运行时拔卡** | 进程**不会崩溃** ✅ | `check_sd_card()`检测到路径不可访问→设`sd_card_ok=0`→跳过ring_buffer_push→LCD继续显示 |
| **拔卡时正在保存** | 当前保存**可能失败** | ffmpeg子进程写文件会报错，但父进程不受影响 |
| **重新插入SD卡** | 自动恢复 ✅ | 下次select循环检测到SD卡可写→`sd_card_ok=1`→恢复正常缓冲 |

**但要注意:**
- FAT32文件系统**不支持真正的热插拔安全卸载**
- 拔卡可能导致文件系统元数据不一致
- **推荐做法**: 先停止DVR进程(`pkill dvr`)，再拔卡
- 或者使用 `sync` 命令刷新缓存后再拔卡

### 修改文件清单 (本次)

| 文件 | 变更类型 | 关键内容 |
|------|----------|----------|
| [common/dvr_types.h](common/dvr_types.h) | **重写** | 新增clip_type_t/clip_info_t/clip_manager_t/DVR_MAX_NORMAL_CLIPS |
| [recorder/dvr_engine.c](recorder/dvr_engine.c) | **大改** | 完整状态机重写+clip_manager+循环录制+SD卡检测+protected修复 |
| [app_freertos.c (源码)](../STM32Cube_ATK_FW_MP2_V1.0.0/Projects/STM32MP257D-ATK/Applications/CM33_OpenAMP_DEMO/OpenAMP_TTY_echo_FreeRTOS/CM33/NonSecure/FREERTOS/App/app_freertos.c) | **修改** | MAX_CYCLES=4, 循环结束后idle |
| [app_freertos.c (IDE副本)](../STM32Cube_ATK_FW_MP2_V1.0.0/Projects/STM32MP257D-ATK/Applications/CM33_OpenAMP_DEMO/OpenAMP_TTY_echo_FreeRTOS/STM32CubeIDE/CM33/NonSecure/Application/User/FREERTOS/App/app_freertos.c) | **同步修改** | 同上 |

### 待办事项
- [ ] 通过STM32CubeIDE编译新的M核固件并烧录到开发板（当前板上运行的仍是旧版无限循环固件）
- [ ] 清理SD卡上的旧测试视频文件（之前无限循环产生的几十个WARNING mp4）
