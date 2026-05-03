# DVR 行车记录系统 — 项目架构与数据流

## 一、系统总览

```
┌─────────────────────────────────────────────────────────────────────────┐
│                        STM32MP257 异构双核平台                          │
│                                                                         │
│  ┌──────────────────────┐          ┌──────────────────────────────┐    │
│  │   M核 (Cortex-M33)    │  RPMSG   │     A核 (Cortex-A35)         │    │
│  │   FreeRTOS 实时系统    │ ═════►  │      Linux 用户空间           │    │
│  │                      │  TTY     │                              │    │
│  │  ┌────────────────┐  │          │  ┌────────────────────────┐  │    │
│  │  │ app_freertos.c │  │          │  │    dvr_engine.c        │  │    │
│  │  │ (DVR触发逻辑)   │  │          │  │    (状态机 + 主循环)    │  │    │
│  │  └───────┬────────┘  │          │  └────┬───────┬───────────┘  │    │
│  │          │ 发送命令   │          │       │       │              │    │
│  │  ┌───────▼────────┐  │          │  ┌────▼──┐ ┌─▼─────────┐    │    │
│  │  │ OpenAMP/VIRT_   │  │          │  │camera │ │ display   │    │    │
│  │  │ UART (RPMSG)    │  │          │  │v4l2   │ │ lcd       │    │    │
│  │  └────────────────┘  │          │  └───┬───┘ └─────┬─────┘    │    │
│  └──────────────────────┘          │      │           │          │    │
│                                     │  ┌───▼───────────▼──┐      │    │
│                                     │  │  ring_buffer.c   │      │    │
│                                     │  │  (SD卡环形缓冲)   │      │    │
│                                     │  └────────┬─────────┘      │    │
│                                     │           │                │    │
│                                     │  ┌────────▼────────┐      │    │
│                                     │  │   ffmpeg(子进程)  │      │    │
│                                     │  │   → MP4视频输出   │      │    │
│                                     │  └────────┬─────────┘      │    │
│                                     │           │                │    │
│                                     │  ┌────────▼────────┐      │    │
│                                     │  │   SD卡 /run/media│      │    │
│                                     │  │   /mmcblk0p1/    │      │    │
│                                     │  └─────────────────┘      │    │
│                                     └──────────────────────────────┘    │
└─────────────────────────────────────────────────────────────────────────┘
```

---

## 二、目录结构与职责

```
dvr_project/
│
├── A_Core/                          # A核(Linux) 主程序
│   ├── Makefile                     # 交叉编译配置 (aarch64-ostl-linux-gcc)
│   ├── dvr_main.c                   # 程序入口: 参数解析、模块初始化、主循环启动
│   └── dvr                          # 编译产物 (ARM64可执行文件)
│
├── camera/                          # 摄像头采集模块
│   ├── camera_v4l2.h                # 接口定义: open/grab/close/convert
│   └── camera_v4l2.c                # 实现:
│       ├─ V4L2设备打开 + 格式协商(RGB565优先)
│       ├─ ISP图像信号处理器配置(dcmipp-isp-ctrl)
│       ├─ mmap零拷贝帧采集(camera_grab_frame)
│       └─ RGB565→RGB24格式转换(camera_convert_to_rgb24)
│
├── display/                         # LCD显示模块
│   ├── display_lcd.h                # 接口定义: init/show/close
│   └── display_lcd.c                # 实现:
│       ├─ Framebuffer设备打开(/dev/fb0)
│       ├─ RGB565→Framebuffer格式转换(含亮度调整)
│       └─ 直接memcpy写入显存(无需Qt/Weston)
│
├── common/                          # 公共数据类型与工具
│   ├── dvr_types.h                  # 全局定义:
│       ├─ DVR状态枚举(IDLE/BUFFERING/SAVING)
│       ├─ 触发事件枚举(TARGET_ON/OFF/WARNING/FALL/COLLISION)
│       ├─ 片段类型枚举(WARNING/FALL/COLLISION)
│       ├─ 默认参数(分辨率/帧率/缓冲时长)
│       └─ 配置结构体(dvr_config_t)
│   ├── ring_buffer.h                # 接口: create/push/pause/resume/destroy
│   └── ring_buffer.c                # SD卡单文件环形缓冲区实现:
│       ├─ 预分配磁盘空间(ftruncate)
│       ├─ 内存索引数组(timestamp + file_offset)
│       ├─ 异步写线程(pwrite, PENDING_QUEUE_SIZE=16)
│       ├─ pause/resume机制(编码时暂停写盘)
│       └─ 帧范围查询(stream_range)
│
├── recorder/                        # 录制引擎(核心)
│   ├── dvr_engine.h                 # 接口: create/run/destroy
│   └── dvr_engine.c                 # DVR核心实现:
│       ├─ 模块初始化(camera+display+ring_buffer+trigger+rpmsg)
│       ├─ select()主事件循环
│       │   ├─ camera_fd可读 → 抓帧→LCD显示→写入缓冲区
│       │   ├─ trigger_fd可读 → 解析命名管道命令
│       │   └─ rpmsg_fd可读 → 解析M核命令
│       ├─ on_trigger() 回调 — 状态机转换
│       ├─ save_clip_to_mp4() — 异步视频编码
│       │   ├─ 暂停写线程(pause)
│       │   ├─ 查询时间范围内的帧偏移量
│       │   ├─ fork()子进程执行ffmpeg
│       │   │   ├─ qsort排序offsets(顺序读取优化)
│       │   │   ├─ pread/read从SD卡读取原始帧
│       │   │   └─ 通过pipe送入ffmpeg编码
│       │   └─ 恢复写线程(resume)
│       ├─ clip_manager — 片段管理(FIFO覆盖/永久保护)
│       └─ SD卡热插拔检测(check_sd_card)
│
├── ipc/                             # 进程间/核间通信
│   ├── trigger_receiver.h/.c        # 命名管道(FIFO):
│       ├─ 创建 /tmp/dvr_trigger_pipe
│       └─ 按行读取命令(TARGET_ON/WARNING等)
│   └── rpmsg_channel.h/.c           # RPMSG通道(M↔A通信):
│       ├─ 打开 /dev/ttyRPMSG0 或 RPMSG1
│       ├─ 非阻塞读取M核数据
│       └─ 按换行符分割处理粘包
│
├── M_Core/                          # M核固件备份
│   ├── FREERTOS/App/
│   │   └── app_freertos.c           # M核主逻辑:
│       │   ├─ OpenAMP初始化
│       │   ├─ M_Send_Task (5s周期发送DVR触发命令)
│       │   │   └─ 4次循环: TARGET_ON→WARNING→TARGET_OFF→TARGET_ON
│       │   └─ M_Receive_Task (接收A核回传)
│   ├── OPENAMP/mbox_ipcc.c          # OpenAMP邮箱驱动
│   └── Core/Src/stm32mp2xx_it.c    # 中断处理
│
├── test_dvr.sh                      # 一键测试脚本
│
├── test_videos/                     # 测试视频存档(SCP拉取的mp4)
│
├── README.md                        # 项目说明
├── DEBUG_LOG.md                     # 调试日志(完整修复记录)
└── PROJECT_STRUCTURE.md             # 本文档 — 架构与数据流
```

---

## 三、核心数据流

### 3.1 正常运行流（无触发）

```
                    时间轴 ──────────────────────────────────────▶

摄像头(DCMIPP)                                               A核主循环
    │                                                           │
    │  DMA搬运一帧RGB565(614400字节)                             │
    │  ↓                                                         │
    │  camera_grab_frame() ←──────────────────────── select()返回
    │  (mmap零拷贝)                                              │
    │  ↓                                                         │
    ├──────────── display_show_frame() ──────────────────────▶  │
    │                  (RGB565→FB memcpy, ~2ms)                  │
    │                                                            │
    │  [state == IDLE] → 不写缓冲区, 返回等待下一帧               │
    │  [state == BUFFERING] → 继续 ▼                             │
    │                                                            │
    ├──────────── ring_buffer_push() ─────────────────────────▶  │
    │                  (frame_buf → pending队列)                   │
    │                                                            │
    │                                              异步写线程      │
    │                                              (独立pthread)  │
    │                                                    │        │
    │                                            pwrite(SD卡)     │
    │                                            (614KB/帧)        │
    │                                                    │        │
    │                                              更新索引数组     │
    │                                              (timestamp+offset)
```

**性能指标**: 每帧总耗时 ~8-12ms (摄像头实际~24fps)

---

### 3.2 触发保存流（WARNING为例）

```
T=0s        T=10s         T=15s          T=30s         T=38s
  │           │             │              │             │
  │ TARGET_ON │             │  WARNING     │  编码完成    │
  │    ↓      │             │    ↓         │     ↓        │
  │ IDLE→     │ 缓冲积累中   │  save_pending│  resume写线程│
  │ BUFFERING │ 240帧(~10s) │  =1          │  LCD恢复     │
  │    ↓      │    ↓         │    ↓         │             │
  │ 开始写SD卡 │  写入继续   │  计算时间范围 │  ffmpeg子进程│
  │ 环形缓冲   │             │  [T-15, T+15]│  退出        │
  │             │             │    ↓         │     ↓        │
  │             │             │  pause写线程 │  mp4文件生成  │
  │             │             │  (清空pending│  ~1MB       │
  │             │             │   fsync)     │             │
  │             │             │    ↓         │             │
  │             │             │  fork()      │             │
  │             │             │  ┌──────────┤             │
  │             │             │  │ 子进程:   │             │
  │             │             │  │ 1.qsort   │             │
  │             │             │  │  offsets  │             │
  │             │             │  │ 2.顺序read │             │
  │             │             │  │  730帧    │             │
  │             │             │  │ 3.pipe→   │             │
  │             │             │  │  ffmpeg   │             │
  │             │             │  │  (-r 24   │             │
  │             │             │  │  -vsync   │             │
  │             │             │  │  cfr)     │             │
  │             │             │  └──────────┤             │
  │             │             │    ↓         │             │
  │             │             │  resume写线程│             │
  │             │             │  LCD恢复实时  │             │
```

---

### 3.3 M核→A核 触发信号流

```
┌──────────────────────────────────────────────────────────────────┐
│ M核 (FreeRTOS)                                                  │
│                                                                  │
│  M_Send_Task (osDelay 5000ms 周期)                              │
│  ┌──────────────────────────────────────────────────────────┐   │
│  │ static int cycle = 0;                                    │   │
│  │ const char *cmd;                                         │   │
│  │ switch(cycle % 4) {                                      │   │
│  │   case 0: cmd = "TARGET_ON\n";  break;  // 开始录制      │   │
│  │   case 1: cmd = "WARNING\n";    break;  // 紧急保存      │   │
│  │   case 2: cmd = "TARGET_OFF\n"; break;  // 停止录制      │   │
│  │   case 3: cmd = "TARGET_ON\n";  break;  // 再开始(演示)   │   │
│  │ }                                                         │   │
│  │ OPENAMP_Send(cmd);                                        │   │
│  │ if(++cycle >= 4) vTaskSuspend(NULL);  // 只发4次!        │   │
│  └──────────────────────┬───────────────────────────────────┘   │
│                           │                                      │
│                           ▼ OpenAMP/RPMSG                       │
└───────────────────────────┼──────────────────────────────────────┘
                            │
                            ▼
┌──────────────────────────────────────────────────────────────────┐
│ A核 (Linux)                                                     │
│                                                                  │
│  /dev/ttyRPMSG0 (或 RPMSG1)                                      │
│       │                                                          │
│       ▼                                                          │
│  rpmsg_channel_process()                                         │
│  ┌──────────────────────────────────────────────────────────┐   │
│  │ char *line = strtok(buf, "\n");  // 按换行分割(防粘包)  │   │
│  │ while(line) {                                             │   │
│  │   trigger_data_t data = parse_command(line);              │   │
│  │   callback(&data, user_data);  // → on_trigger()         │   │
│  │   line = strtok(NULL, "\n");                             │   │
│  │ }                                                         │   │
│  └──────────────────────┬───────────────────────────────────┘   │
│                           │                                      │
│                           ▼                                      │
│  on_trigger() — 统一回调                                         │
│  ┌──────────────────────────────────────────────────────────┐   │
│  │ switch(data->event) {                                     │   │
│  │   case TARGET_ON:                                         │   │
│  │     state = BUFFERING;  target_present = 1;              │   │
│  │     break;                                                │   │
│  │   case WARNING:                                          │   │
│  │     save_pending = 1;  save_event = TRIGGER_WARNING;      │   │
│  │     break;                                                │   │
│  │   case TARGET_OFF:                                        │   │
│  │     if(!save_pending) state = IDLE;                      │   │
│  │     break;                                                │   │
│  │ }                                                         │   │
│  └──────────────────────────────────────────────────────────┘   │
└──────────────────────────────────────────────────────────────────┘
```

---

## 四、状态机详解

```
                        ┌─────────────┐
                        │             │
                  ┌─────▶    IDLE     ◀─────┐
                  │     │             │     │
                  │     │  目标消失    │     │
                  │     │ (TARGET_OFF) │     │
                  │     └──────┬──────┘     │
                  │            │             │
                  │  TARGET_ON │             │
                  │            ▼             │
                  │     ┌─────────────┐     │
                  │     │  BUFFERING  │     │
                  │     │  (循环录制)  │     │
                  │     └──────┬──────┘     │
                  │            │             │
                  │     ┌──────┴──────┐     │
                  │     │             │     │
                  │  WARNING/FALL/  TARGET_OFF
                  │    COLLISION      │
                  │     │             │
                  │     ▼             │
                  │  ┌─────────────┐  │
                  │  │   SAVING     │  │
                  │  │ (异步编码中)  │  │
                  │  └──────┬──────┘  │
                  │         │          │
                  │    编码完成        │
                  │         │          │
                  └─────────┴──────────┘
                    (目标仍在→BUFFERING)
                    (目标消失→IDLE)

状态说明:
┌──────────┬──────────────────────────────────────────────────┐
│ IDLE     │ 摄像头采集+LCD显示正常，不写缓冲区，最低资源占用   │
├──────────┼──────────────────────────────────────────────────┤
│BUFFERING │ 持续写入SD卡环形缓冲区，每30秒循环覆盖(不清空重建) │
│          │ 收到紧急信号后进入SAVING，等待补录触发后画面       │
├──────────┼──────────────────────────────────────────────────┤
│ SAVING   │ 暂停写线程→fork子进程ffmpeg编码→恢复写线程        │
│          │ 编码在后台进行，主循环立即返回继续采集+显示        │
└──────────┴──────────────────────────────────────────────────┘
```

---

## 五、关键设计决策

| 决策点 | 选择 | 原因 |
|--------|------|------|
| **像素格式** | RGB565(原生) | 摄像头DCMIPP直接输出，避免CPU格式转换 |
| **缓冲介质** | SD卡单文件 | 内存仅762MB，30秒视频需~550MB内存→OOM |
| **编码方式** | fork()+ffmpeg | 异步非阻塞，不影响LCD实时显示 |
| **SD卡读取优化** | qsort+顺序read | SD卡顺序读~10MB/s vs 随机pread~1MB/s |
| **写线程pause** | 编码期间暂停 | 避免编码时I/O竞争导致丢帧 |
| **ffmpeg帧率** | 动态计算 | 摄像头实际~24fps(非标称30)，确保视频时长正确 |
| **恒定帧率** | -vsync cfr | 消除非标准帧率导致的播放抖动 |
| **片段管理** | FIFO 3槽位 | 普通片段循环覆盖，FALL/COLLISION永久保护 |
| **M核通信** | RPMSG TTY | 标准OpenAMP框架，支持双向通信 |

---

## 六、外部集成接口

### 6.1 输入接口（谁可以触发DVR）

| 接口 | 方式 | 来源 | 示例 |
|------|------|------|------|
| **命名管道** | `echo CMD > /tmp/dvr_trigger_pipe` | 任意进程 | 目标检测程序、雷达进程 |
| **RPMSG** | M核OpenAMP发送 | M核固件 | app_freertos.c |

### 6.2 输出接口（DVR产生什么）

| 输出 | 路径 | 格式 | 说明 |
|------|------|------|------|
| **紧急视频** | `/run/media/mmcblk0p1/emergency_*.mp4` | MP4(MPEG4) | 30秒片段，~1MB |
| **运行日志** | `/tmp/dvr.log` | 文本 | 状态转换、错误信息 |
| **LCD显示** | `/dev/fb0` | Framebuffer | 实时摄像头画面 |

### 6.3 预留扩展点

```
┌─────────────────────────────────────────────────────┐
│                   未来可集成的模块                    │
│                                                     │
│  [目标检测进程] ──echo "TARGET_ON"──▶ 命名管道       │
│  [雷达进程]     ──echo "WARNING" ──▶ 命名管道       │
│  [摔倒判断进程] ──echo "FALL"     ──▶ 命名管道       │
│                                                     │
│  [M核传感器]   ──RPMSG─────────────▶ rpmsg_channel   │
│  (加速度计/陀螺仪)                                     │
│                                                     │
│  [网络模块]     ◀──读取emergency_*.mp4──▶ SD卡       │
│  (4G/WiFi上传)    (scp或http服务器)                  │
└─────────────────────────────────────────────────────┘
```

---

## 七、编译与部署

### 7.1 A核(DVR主程序)

```bash
# 交叉编译
source /opt/st/stm32mp2/5.0.3-snapshot/environment-setup-cortexa35-ostl-linux
cd dvr_project/A_Core && make

# 部署到开发板
scp dvr root@192.168.88.10:/usr/local/bin/dvr
```

### 7.2 M核(触发固件)

```bash
# 使用STM32CubeIDE内置GCC 12.3.1编译
export PATH="/home/alientek/download/makeself_dir_RBGMxz/y/plugins/.../tools/bin:$PATH"
cd STM32CubeIDE/CM33/NonSecure/CA35TDCID_m33_ns_sign
make main-build

# 部署
scp *.elf root@192.168.88.10:/lib/firmware/
ssh root@192.168.88.10 "
  echo stop > /sys/class/remoteproc/remoteproc0/state
  sleep 2
  echo start > /sys/class/remoteproc/remoteproc0/state
"
```

### 7.3 一键测试

```bash
# 在开发板上执行
test_dvr.sh
# 或手动:
/usr/local/bin/dvr > /tmp/dvr.log 2>&1 &
sleep 10
echo "TARGET_ON" > /tmp/dvr_trigger_pipe
sleep 10
echo "WARNING" > /tmp/dvr_trigger_pipe
sleep 50
ls -lh /run/media/mmcblk0p1/emergency_*.mp4
```

---

## 八、性能参数

| 参数 | 数值 | 说明 |
|------|------|------|
| 摄像头分辨率 | 640×480 | DCMIPP硬件限制 |
| 摄像头帧率 | ~24fps 实际 / 30fps 标称 | STM32MP2 DCMIPP限制 |
| 像素格式 | RGB565 (2Bpp) | 原生输出，零转换 |
| LCD分辨率 | 800×480 | 居中显示带黑边 |
| 缓冲区容量 | 1140帧 × 614400B = **668MB** | DVR_BUFFER_SECONDS=38 |
| 缓冲区路径 | `/run/media/mmcblk0p1/dvr_buffer.bin` | SD卡单文件 |
| 异步写队列 | 16帧 (PENDING_QUEUE_SIZE) |容忍SD卡写入延迟 |
| 视频时长 | **~30秒** | 动态fps计算 + vsync cfr |
| 视频大小 | **~1MB** | MPEG4 q:v 5 |
| 编码耗时 | ~5-8秒 | qsort顺序读取优化 |
| LCD冻结时间 | **~8秒** | 仅pause写线程期间 |
| DVR进程内存 | **<10MB** | 单文件缓冲方案 |
| 系统可用内存 | **>500MB** / 762MB总量 | 无OOM风险 |
