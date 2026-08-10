# STM32MP257 骑行辅助系统技术知识库

本文不是面试答案合集，而是当前项目的技术底图。目标是把硬件、启动固件、Linux
内核、驱动、用户态框架和业务代码放到同一套逻辑中。

阅读本文后，应该能够回答三类问题：

1. 一个业务动作从哪里来，经过哪些进程、内核接口和硬件模块？
2. 一个设备节点为什么会出现，用户程序怎样通过它访问硬件？
3. 出现启动慢、设备打不开、事件丢失或录像失败时，应该从哪一层开始检查？

本文可以直接提供给没有开发板访问权限的 GPT。它包含当前仓库的文件结构、运行路径、
实测结论和已知边界。需要查看面试表达时，再配合
[INTERVIEW_PREPARATION.md](INTERVIEW_PREPARATION.md)。

## 1. 事实等级和阅读规则

为了避免把设想当成已经完成，本文使用四种事实等级：

- **已实现**：当前仓库中存在对应代码或配置。
- **实测**：开发板日志或专项文档中有实际验证记录。
- **候选**：已经分析过，但尚未实施或没有完成回归。
- **待核实**：仓库不同材料存在差异，需要开发板在线后确认。

遇到“Linux通常如何工作”和“本项目当前如何实现”不一致时，以项目源码为准；源码
与板端运行状态不一致时，以板端版本、日志和设备状态为准。

一个例子是摄像头：2026-08-09在线实机已经通过sysfs、`v4l2-ctl`和`media-ctl`
确认，当前业务使用的`/dev/video7`是Sonix/Microdia `0c45:636b` USB 2.0 UVC
摄像头，由`uvcvideo`驱动。文档中的硬件链路以这次实机确认结果为准。

同日录像可靠性排查确认当前TF介质落盘不可靠。当前主存储已经切换到板载
`/usr/local/helmet`（userfs/ext4），TF仅作为可更换的人工导入导出介质。文中TF
启动耗时和FAT修复内容保留为历史案例，不再代表当前录像依赖。

## 2. 先建立完整分层

### 2.1 从硬件到业务

```text
业务功能
  风险检测 / 摔倒告警 / V2X / HUD / 骨传导 / 事件录像
        │
用户态程序和服务
  radar_fusion / hud / Dashboard / OTA / systemd units
        │
系统调用和用户态库
  open/read/write/ioctl/mmap/select/socket/fork/exec
  libc / pthread / stai_mpu / libjpeg / GStreamer / ALSA
        │
Linux内核子系统
  V4L2 / TTY / GPIO / ALSA-ASoC / remoteproc-RPMsg
  VFS / FAT / MMC / 网络协议栈 / 调度器 / 内存管理
        │
设备模型与硬件描述
  Device Tree / platform bus / clocks / resets / pinctrl / DMA / IRQ
        │
SoC与外设
  Cortex-A35 / Cortex-M33 / NPU / USB / SAI / UART / GPIO
  摄像头 / 60GHz雷达 / TF卡 / MAX98357A / CH9140 / STM32WBA
```

业务代码通常不直接访问物理寄存器。它打开 `/dev` 下的设备节点或调用库，内核驱动
再处理寄存器、中断、DMA、时钟和电源。这是理解嵌入式 Linux 与单片机裸机开发差异
的第一步。

### 2.2 各层负责什么

| 层 | 主要职责 | 本项目例子 |
|---|---|---|
| 硬件 | 实际采样、传输、运算和输出 | USB UVC摄像头、雷达、NPU、M33、SAI、TF卡 |
| Bootloader/固件 | 建立可信启动、DDR、装载内核和设备树 | TF-A、OP-TEE、U-Boot、extlinux |
| Linux内核 | 进程调度、内存、文件系统、网络和驱动 | V4L2、TTY、remoteproc、ASoC、MMC |
| systemd/udev | 用户空间启动、依赖和设备事件管理 | `dvr.service`、设备节点就绪、日志 |
| 用户态框架 | 提供较高层接口和处理流水线 | GStreamer、stai_mpu、ALSA、Python HTTP |
| 项目业务 | 状态机、风险决策、告警联动和数据记录 | `radar_fusion`、HUD、OTA事务 |

### 2.3 不要混淆“框架”和“功能”

例如事件录像：

- 业务功能：风险发生时保存前后视频；
- 数据来源：V4L2摄像头输出MJPEG；
- 媒体框架：GStreamer；
- 编码格式：H.264；
- 容器格式：MP4；
- 内核接口：V4L2采集和V4L2编码器；
- 存储路径：VFS→FAT→MMC→TF卡。

只说“用了GStreamer”不能解释功能，只说“实现了录像”也没有说明Linux技术落点。

## 3. SoC和硬件资源

### 3.1 Cortex-A35与Cortex-M33

STM32MP257把不同类型的处理器放在一个SoC中：

- Cortex-A35运行Linux，具备MMU，适合复杂应用、文件系统、网络、NPU和视频；
- Cortex-M33运行实时固件，适合IMU周期采样、实时状态判断和对调度抖动敏感的任务。

这里的“异构”不只是指指令集或性能不同，还包括运行环境和职责不同：Linux有虚拟
内存、进程隔离和动态资源管理，M33固件通常直接管理外设和中断。

### 3.2 A35与M33不是完全独立的两块板

两边仍共享或依赖SoC资源：

- 共享DDR或专用共享内存区域；
- 使用IPCC/mailbox通知对端；
- M33固件和vring占用设备树中的reserved-memory；
- Linux remoteproc驱动控制M33复位、固件加载和启动状态；
- 两边需要约定资源表、通道名和消息格式。

因此不能随意释放M33的reserved-memory，也不能让U-Boot和Linux同时重复接管M33。

### 3.3 设备树中的M33资源

当前DTS的M33节点包含：

- `compatible = "st,stm32mp2-m33"`：选择匹配的Linux驱动；
- `resets`和`reset-names`：控制M33复位和hold boot；
- `mboxes`：RPMsg vring和shutdown通知；
- `memory-region`：M33固件、数据、共享内存、vring和buffer；
- `status = "okay"`：允许驱动probe。

reserved-memory中可以看到：

- `cm33-cube-fw@80100000`；
- `cm33-cube-data@80a00000`；
- `ipc-shmem-1@81200000`；
- `vdev0vring0/1`；
- `vdev0buffer`。

这些区域标记为`no-map`或`shared-dma-pool`，不是普通Linux应用可随意使用的空闲
内存。删除它们可能让Linux可见内存变大，但会破坏M33、RPMsg或DMA。

### 3.4 外设控制常见资源

一个SoC外设正常工作，通常不只需要寄存器地址，还需要：

- clock：给模块提供时钟；
- reset：把模块从复位状态释放；
- pinctrl：把引脚切换到正确复用功能；
- interrupt：硬件事件通知CPU；
- DMA：在外设和内存间搬运数据；
- power domain/regulator：保证模块和外设供电；
- IOMMU/reserved-memory/CMA：满足连续或受保护的内存需求。

所以“设备节点没出现”不能只检查`status = "okay"`。驱动probe可能因为时钟、DMA、
引脚、电源、endpoint或依赖设备未就绪而延迟或失败。

## 4. 从上电到业务运行

### 4.1 启动链总览

```text
上电
  → BootROM
  → TF-A
  → OP-TEE / 安全世界初始化
  → U-Boot
  → 读取extlinux配置
  → 加载Image.gz、initramfs、DTB
  → Linux内核
  → initramfs挂载并切换rootfs
  → systemd成为PID 1
  → sysinit/basic/multi-user targets
  → M33、网络、Dashboard和主业务
  → Fusion core ready
```

不同镜像的具体TF-A/OP-TEE阶段可能有所不同，但本项目测量边界必须明确：
`systemd-analyze`只从Linux内核阶段开始统计，无法给出完整上电耗时。

### 4.2 每个阶段的作用

**BootROM**

芯片内部固化代码。选择启动介质、完成最早期安全检查，装载下一阶段。项目没有修改。

**TF-A**

ARM Trusted Firmware。负责早期平台初始化、安全状态和后续固件切换。DDR、电源和
安全配置相关修改风险很高。

**OP-TEE**

OP-TEE是运行在Arm TrustZone安全世界中的可信执行环境，Linux运行在普通世界。
它不是普通systemd服务，也不等同于一个`optee_rng`内核模块：TF-A负责世界切换，
Linux侧`optee`驱动建立通信，`tee-supplicant`为部分可信应用提供普通世界协助，
`optee_rng`只是通过OP-TEE取得硬件随机数的客户端驱动。

即使当前业务代码没有直接调用TEE，也不能据此关闭整个OP-TEE。平台安全服务、密钥、
随机数、固件接口或后续安全启动能力可能依赖它；错误修改还可能影响TF-A到Linux的
启动链。2026-08-09只测试了提前加载Linux侧`optee_rng`模块，OP-TEE和`rng-tools`
始终保留。该测试让`sysinit.target`提前约3.5秒，但没有缩短Fusion ready，反而使
摄像头模块在三次重启中更晚完成，因此板端已回退，未纳入默认优化。

**U-Boot**

读取启动配置，加载内核、initramfs和DTB，传递bootargs。当前`bootdelay=0`，
extlinux菜单仍约有2秒窗口。它也是内核无法启动时的重要恢复入口。

**Linux kernel**

解压、建立页表和调度器、初始化驱动、挂载临时根文件系统，最终启动PID 1。当前
内核约2秒，不是原103秒问题的主要来源。

**initramfs**

内核启动早期的临时根文件系统，负责发现和挂载真实rootfs再`switch_root`。当前
一次性resize已经完成，但initramfs仍参与根分区切换，不能不经A/B就删除。

**systemd**

作为PID 1管理服务、挂载、socket、target、依赖和进程回收。原来最大的90秒等待
发生在这一阶段。

### 4.3 Image、DTB、initramfs和rootfs的区别

- `Image`或`Image.gz`：Linux内核本体；
- DTB：板级硬件描述，告诉内核有哪些设备和资源；
- initramfs：启动早期临时文件系统；
- rootfs：系统最终使用的根文件系统，包含systemd、库、程序和配置；
- 应用release：`/xxl/releases/<version>`，只是rootfs中的一组业务文件。

应用OTA只切换最后一项，不等于更新内核、DTB或rootfs。

### 4.4 当前启动时间线

当前连续冷启动中位数：

| 里程碑 | 时间 | 含义 |
|---|---:|---|
| Linux kernel完成 | 约2.06 s | 进入userspace |
| M33 running | 3.908 s | remoteproc状态正常 |
| USB UVC摄像头节点就绪 | 约8.50～9.20 s | `/dev/video7`，最晚业务设备之一；多为并行项 |
| TF检查/挂载 | 约9 s量级 | 历史测量；当前不再是录像前置条件 |
| systemd总启动 | 11.841 s | 不含bootloader |
| Fusion core ready | 12.602 s | 融合主业务初始化完成 |
| RPMsg ready发送 | 13.605 s | 含线程固定1秒等待 |

这些动作部分并行，不能相加。

## 5. Linux内核基础：从项目反推概念

### 5.1 用户态和内核态

A35应用运行在用户态，不能直接执行任意特权操作。访问设备和系统资源时通过系统调用
进入内核，例如：

| 用户态调用 | 内核负责的事情 | 项目例子 |
|---|---|---|
| `open` | 查找VFS对象或打开设备文件 | 打开`/dev/video7`、串口、GPIO |
| `read/write` | 文件、TTY、RPMsg或socket数据传输 | 雷达帧、RPMsg消息、日志 |
| `ioctl` | 执行设备特定控制命令 | V4L2配置、GPIO line request |
| `mmap` | 把内核/设备缓冲映射进进程地址空间 | V4L2采集缓冲 |
| `select/poll` | 等待多个文件描述符就绪 | 雷达串口、HUD UDP |
| `socket` | 创建网络端点 | UDP导航和HTTP服务 |
| `fork/exec` | 创建进程并加载新程序 | GStreamer编码、`aplay` |
| `waitpid` | 回收子进程并取得退出状态 | 异步录像编码回收 |

理解系统调用的关键不是背编号，而是知道用户态只表达请求，真正的调度、缓冲、驱动和
权限检查在内核中完成。

### 5.2 进程、线程和调度

进程拥有独立虚拟地址空间和文件描述符表。线程共享进程地址空间、全局变量和多数文件
描述符，但有独立栈和调度实体。

本项目的典型关系：

```text
systemd
  └── start_dvr.sh
      ├── hud
      ├── radar_fusion
      │   ├── 主线程
      │   ├── LED线程
      │   ├── RPMsg线程
      │   ├── 导航接收线程
      │   ├── 导航watchdog线程
      │   └── 临时音频/录像子进程
      └── 日志维护进程（兼容路径）

radar-dashboard.service
  └── Python Dashboard

helmet-ota.service
  └── Python OTA HTTP服务
```

Linux调度器决定每个线程何时在A35上运行。普通用户态线程不具备硬实时保证；因此IMU
周期采样和部分实时判断放在M33更合适。

### 5.3 并发不等于并行

- 并发：多个任务在时间上交错推进；
- 并行：多个CPU核同一时刻执行不同任务。

systemd并发启动两个90秒超时服务，它们的时间区间重叠，因此关键路径约增加90秒，
不是180秒。`systemd-analyze blame`列出的时间也不能直接求和。

### 5.4 中断、DMA和用户态轮询

外设数据通常经历：

```text
硬件事件
  → 中断控制器
  → 内核驱动中断处理
  → DMA完成/缓冲就绪
  → 唤醒等待队列
  → select/poll/read返回用户态
```

视频和音频数据量大，通常依赖DMA而不是CPU逐字节搬运。UART数据量较小，驱动仍可
使用中断和DMA。用户程序看到的是文件描述符就绪，不直接处理硬件中断。

### 5.5 DMA、连续内存和缓存一致性

DMA让外设直接读写内存，但要解决：

- 物理地址是否连续；
- 设备能否访问该地址范围；
- CPU cache和设备看到的数据是否一致；
- 内存生命周期是否覆盖整个传输；
- 不同安全域和处理器是否允许访问。

Linux通过DMA API、CMA、IOMMU或reserved-memory处理这些问题。当前DTS包含128MiB
`linux,cma`以及M33、GPU等保留区。应用不能因为`free`显示内存少，就随意删除这些
区域。

### 5.6 虚拟内存

每个进程看到的是虚拟地址。`malloc`得到的地址不是直接的物理地址；内核按页映射，
并可能按需分配。V4L2 `mmap`把驱动管理的缓冲映射到进程，使应用可以访问采集数据，
并不表示应用拥有这块缓冲的永久所有权。

项目实测Linux可见RAM约1767MiB，业务运行时约1.5GiB可用，swap为0。扩大tmpfs
上限不会增加物理内存，只会允许它消耗更多现有RAM。

### 5.7 文件描述符

Linux把文件、设备、socket和管道统一成文件描述符。这个抽象使`select/poll`能够用
类似方式等待串口和网络事件。

需要理解的生命周期：

```text
open/socket → fd
  → read/write/ioctl/mmap
  → fork时子进程继承
  → close释放本进程引用
  → 最后一个引用关闭后内核对象才真正释放
```

进程残留会继续持有串口、摄像头或日志文件，导致新服务启动时出现“设备忙”或无法
重新绑定。

### 5.8 阻塞、非阻塞和I/O多路复用

- 阻塞fd：没有数据时`read`睡眠；
- `O_NONBLOCK`：没有数据立即返回`EAGAIN`；
- `select/poll`：先等待就绪，再执行读写。

雷达和RPMsg打开为非阻塞，并使用`select`加超时。这样主循环可以同时处理定时取帧、
串口输入和子进程回收，不会永久卡在一次`read`上。

### 5.9 时钟

- 墙钟：现实日期时间，可能被RTC错误、NTP或手机校时改变；
- `CLOCK_MONOTONIC`：系统运行期间单调递增，不含挂起时间的语义依平台；
- `CLOCK_BOOTTIME`：从启动开始单调递增，并考虑挂起时间；
- `gettimeofday`：墙钟语义，不适合跨校时的启动阶段耗时。

启动优化使用journal单调时间和`CLOCK_BOOTTIME`，避免RTC校准让日志看起来倒退或
突然跳跃。业务CSV仍可能需要墙钟用于人类阅读，因此常同时记录墙钟和单调时间。

### 5.10 VFS、页缓存和写回

应用调用`write`后，数据通常先进入内核页缓存，不一定立即落到TF卡。需要区分：

- `fflush`：把C库用户态缓冲交给内核；
- `fsync`：要求内核把文件数据和必要元数据提交到存储；
- `rename`：同一文件系统内可作为原子名称切换；
- `sync`：请求全局文件系统写回，粒度较大。

因此“文件已经close”并不等于异常断电时一定完整。候选录像方案提出先写`.partial`，
完成后`fsync`再`rename`，是为了避免半成品冒充正式MP4。

### 5.11 信号和进程退出

systemd停止服务时通常先发送SIGTERM，超时后再SIGKILL。SIGTERM允许程序清理文件、
关闭设备和等待子进程；SIGKILL不可捕获，内核直接结束进程。

信号处理函数中能安全调用的函数很少。当前`radar_fusion`处理函数只修改运行标志，
主循环再走正常清理，方向是合理的。多线程共享状态的严格同步仍需要原子变量或锁，
`volatile`本身不提供线程同步语义。

### 5.12 Linux实时性和M33实时性的区别

普通Linux强调总体吞吐、公平性和资源共享，不保证某个用户线程一定在固定微秒内运行。
延迟可能来自：

- 更高优先级任务和中断；
- 缺页、文件系统和存储I/O；
- 锁竞争；
- CPU调频和电源状态；
- 内核不可抢占区；
- 用户态进程被调度出去。

实时调度策略、CPU affinity和PREEMPT_RT可以改善最坏延迟，但必须测量并防止高优先级
线程饿死系统。当前仓库没有证据表明主业务使用了PREEMPT_RT或实时调度策略，因此
不能声称Linux侧是硬实时。

M33没有Linux用户进程和复杂文件系统，周期采样和中断响应更容易控制。把IMU任务放在
M33是职责分离，而不是说M33任何代码天然都满足实时要求；M33固件仍要分析中断优先级、
临界区和最坏执行时间。

## 6. Device Tree和Linux驱动模型

### 6.1 Device Tree解决什么问题

ARM SoC不像PC那样能自动枚举全部板级外设。DTB描述“这块板上实际连接了什么”，
内核驱动描述“这类硬件怎样操作”。两者通过`compatible`等属性匹配。

典型节点：

```dts
device@address {
    compatible = "vendor,device";
    reg = <...>;
    interrupts = <...>;
    clocks = <...>;
    resets = <...>;
    pinctrl-0 = <...>;
    dmas = <...>;
    status = "okay";
};
```

### 6.2 驱动probe的逻辑

```text
内核解析DTB
  → 为节点创建device
  → compatible匹配driver
  → driver probe
  → 申请时钟/复位/引脚/IRQ/DMA
  → 注册到某个内核子系统
  → sysfs出现设备关系
  → udev根据uevent创建/命名/dev节点
```

`probe`失败时应先看`dmesg`中的错误和`-EPROBE_DEFER`依赖，而不是直接在应用里循环
重试几十秒。

### 6.3 `/dev`、`/sys`和`/proc`

- `/dev`：应用访问设备或伪设备的入口；
- `/sys`：内核设备模型、驱动绑定、属性和拓扑；
- `/proc`：进程和系统运行状态，也包含部分历史接口；
- `/run`：本次启动的运行时数据，通常在tmpfs中；
- `/var`：长期状态和日志；
- `/run/media/mmcblk0`或`/run/media/mmcblk0p1`：可移除TF的动态挂载点。旧故障卡已
  停用；2026-08-09的新卡为整盘FAT布局并通过三轮卸载重挂校验，仅用于导入/导出。

本项目例子：

| 路径 | 含义 |
|---|---|
| `/dev/video7` | V4L2采集节点 |
| `/dev/ttySTM1` | 雷达串口 |
| `/dev/ttySTM0` | CH9140方向灯串口 |
| `/dev/ttyRPMSG0` | RPMsg TTY/字符通道 |
| `/dev/gpiochip3` | GPIO字符设备控制器 |
| `/sys/class/remoteproc/remoteproc0/state` | M33 remoteproc状态 |
| `/sys/class/pwm/pwmchip*` | PWM sysfs控制接口 |
| `/proc/asound/cards` | ALSA声卡列表 |
| `/proc/interrupts` | 中断计数，可观察DMA/外设活动 |

### 6.4 pinctrl和引脚复用

同一个引脚可能是GPIO、UART、I2C、SAI或PWM。pinctrl决定当前功能和电气属性。
本项目为了MAX98357A音频：

- 禁用占用PB4/PB5/PB6的I2C2和UART4；
- 把PB4/PB5/PB6配置为SAI4B的FS/SD/SCK；
- 使用SAI4B、HPDMA和`simple-audio-card`；
- 把PB11作为MAX98357A控制GPIO；
- PF10配置为TIM2_CH3硬件PWM。

错误AF编号、把pinctrl放错节点或选择错误DMA，都可能让设备节点存在但数据不流动。

### 6.5 为什么错误DTB可能导致整机启动失败

DTB不仅描述普通GPIO，还包含DDR可见范围、reserved-memory、安全内存、remoteproc、
DMA和电源关系。仓库记录曾使用内核源码中的通用DTS替换开发板实际DTB，因保留内存
布局不同导致启动失败或DMA异常。

所以当前规则是：从开发板正在运行的原始DTB反编译修改，备份后再A/B验证。不能因为
设备型号看起来相同，就假设通用DTS与厂商镜像完全匹配。

### 6.6 设备节点怎样连接到驱动函数

字符设备节点包含major/minor编号。应用`open("/dev/gpiochip3")`时，VFS根据设备号
找到已注册的字符设备和`file_operations`。之后不同系统调用进入不同回调：

```text
open   → driver open
read   → driver read
write  → driver write
ioctl  → driver unlocked_ioctl
mmap   → driver mmap
poll   → driver poll + wait queue
close  → driver release
```

并不是每个驱动都实现全部回调。V4L2、TTY、ALSA和GPIO还在通用字符设备之上叠加了
各自子系统的公共框架，所以应用看到统一API，具体硬件驱动只实现子系统要求的操作。

`select/poll`能够睡眠等待，是因为驱动的poll回调把当前任务挂到等待队列；中断或DMA
完成后，驱动更新状态并唤醒等待者。用户程序不需要不停循环读取寄存器。

检查设备号和驱动关系可以使用：

```bash
ls -l /dev/video7 /dev/ttySTM1 /dev/gpiochip3
udevadm info /dev/video7
readlink -f /sys/class/video4linux/video7/device/driver
readlink -f /sys/class/tty/ttySTM1/device/driver
```

### 6.7 内建驱动、内核模块和Device Tree的关系

驱动可能编进内核，也可能编译成`.ko`模块：

- 内建驱动在内核启动时按initcall阶段初始化；
- 模块由`modprobe`或设备事件加载；
- `lsmod`只显示模块，不显示内建驱动；
- DT节点`status = "okay"`不代表驱动一定存在，内核还要启用对应Kconfig；
- 驱动存在也不代表probe成功，还要满足资源和依赖。

排查时要组合查看：

```bash
zcat /proc/config.gz | grep CONFIG_关键项
lsmod
modinfo 模块名
dmesg | grep -i 设备关键词
readlink -f /sys/.../driver
```

当前生产内核文档记录为`6.6.48-gbebcf479fd77`。它启用了printk时间戳，但没有可直接
使用的FTRACE配置。若要分析内建驱动启动耗时，可以制作独立诊断启动项启用
`initcall_debug`或ftrace，不能把诊断参数无评估地留在生产配置中。

### 6.8 platform driver、总线驱动和子系统驱动

“驱动”不是一种固定形态：

- SoC内部控制器常作为platform device，由DT描述；
- I2C/SPI sensor先由控制器驱动建立总线，再由子设备驱动匹配；
- USB设备可以运行时枚举，不一定写入DT；
- V4L2、ALSA、TTY、GPIO等是内核子系统，统一用户接口；
- USB摄像头由USB核心完成枚举，`uvcvideo`接入V4L2并创建视频节点。

当前摄像头是USB UVC设备，因此排查重点是USB主控和Hub枚举、`uvcvideo`加载、
V4L2节点创建及格式协商。当前`/dev/video7`的归属来自sysfs驱动路径和实机媒体
信息，而不是根据设备节点编号猜测。

## 7. 摄像头和V4L2

### 7.1 当前实机链路

当前业务实机链路：

```text
Sonix/Microdia USB 2.0 Camera（0c45:636b）
  → EHCI host / USB hub
  → uvcvideo
  → /dev/video7（Video Capture）
  → /dev/video8（Metadata Capture）
  → /dev/media2
```

启动脚本中的“USB摄像头”标签与当前业务硬件相符。确认方法不是只看设备节点编号，
而是同时检查USB身份、sysfs驱动路径和V4L2信息：

```bash
readlink -f /sys/class/video4linux/video7/device
cat /sys/class/video4linux/video7/name
v4l2-ctl --list-devices
media-ctl -d /dev/media2 -p
```

`video7`编号可能随驱动和枚举顺序变化，长期产品配置最好使用稳定udev符号链接或按
media entity匹配，不应只依赖编号永远不变。

### 7.2 V4L2采集步骤

当前`camera.c`使用streaming + MMAP：

1. `open("/dev/video7", O_RDWR)`；
2. `VIDIOC_QUERYCAP`检查capture/streaming能力；
3. `VIDIOC_S_FMT`优先协商MJPEG，失败回退YUYV；
4. `VIDIOC_S_PARM`请求25 FPS；
5. `VIDIOC_REQBUFS`申请4个MMAP缓冲；
6. `VIDIOC_QUERYBUF`取得每个缓冲的offset和长度；
7. `mmap`映射到用户空间；
8. `VIDIOC_QBUF`把缓冲全部交给驱动；
9. `VIDIOC_STREAMON`启动采集；
10. 循环`VIDIOC_DQBUF`取帧，处理后`VIDIOC_QBUF`归还；
11. `STREAMOFF`、`munmap`、`close`释放。

### 7.3 缓冲所有权

V4L2队列的核心规则：

- QBUF后，缓冲所有权交给驱动，驱动可能随时写入；
- DQBUF后，应用暂时拥有这帧，可以读取；
- 应用处理完成后再QBUF归还。

当前`camera_capture()`在返回指针前已经重新QBUF，调用者随后才使用该地址。实际负载
下驱动可能覆盖该缓冲。这是**代码审查发现的潜在竞态，尚未用实机复现**。后续应在
DQBUF和QBUF之间完成拷贝/处理，或者把buffer index和显式release接口交给调用者。

这类问题说明：会调用V4L2 API还不够，必须理解缓冲所有权和生命周期。

### 7.4 MJPEG到NPU

摄像头帧是压缩JPEG，NPU模型需要固定尺寸RGB输入，因此流程是：

```text
MJPEG帧
  → libjpeg解码RGB
  → resize到模型输入尺寸
  → 量化/归一化
  → stai_mpu执行NPU推理
  → SSD框解码、置信度过滤、NMS
```

每10帧推理一次，是在采集连续性、计算量和响应时间之间的工程选择，不代表NPU只能
达到这个频率。

### 7.5 NPU在内核中的位置

应用通过`stai_mpu`用户态库加载`.nb`模型并提交推理。库再与厂商运行时和内核驱动
交互。当前仓库没有NPU内核驱动源码，因此可以讲模型输入、运行库调用和后处理，但
不要声称自己实现了NPU驱动、调度器或内存分配器。

## 8. UART、TTY和协议解析

### 8.1 UART硬件与TTY抽象

UART是硬件串行控制器；Linux TTY层把它抽象为`/dev/ttySTM*`。用户程序通过termios
设置波特率、数据位、停止位、校验和流控。

当前用途：

- `/dev/ttySTM1`：60GHz毫米波雷达；
- `/dev/ttySTM0`：MP257到CH9140的BLE透明串口。

### 8.2 串口不是“每次read就是一帧”

TTY提供字节流。一次`read`可能得到：

- 半帧；
- 一帧；
- 多帧拼在一起；
- 命令回复和异步上报混合。

所以协议解析器需要维护pending buffer，根据帧头和长度找完整帧，并处理错位和未知
字节。雷达初始化优化正是先解析完整`HEAD_REPLY`，再检查命令字是否与当前命令匹配；
异步`HEAD_REPORT`不能误认为ACK。

### 8.3 ACK优化为什么安全性比sleep更好

旧逻辑无论回复是否已经到达，每条命令都等待满1秒。新逻辑：

```text
收到完整且匹配的ACK → 提前结束
没有收到匹配ACK       → 保留原1秒超时
命令之间               → 保留200ms稳定间隔
```

这不是盲目缩短sleep，而是把“固定时间猜测”改为“协议事件驱动”，同时保留最坏路径。

### 8.4 UART问题的分层排查

1. 硬件：供电、共地、TX/RX交叉、电平；
2. pinctrl/驱动：串口节点和引脚复用；
3. 设备：`/dev/ttySTM*`是否存在、谁在占用；
4. termios：波特率、8N1、流控；
5. 字节流：是否收到数据、帧头和长度是否正确；
6. 协议：命令、ACK和异步上报是否区分；
7. 业务：解析后的状态是否进入告警逻辑。

## 9. remoteproc、OpenAMP和RPMsg

### 9.1 三者的职责

- remoteproc：装载固件、控制M33启动/停止、管理资源表和状态；
- OpenAMP：异构通信的软件框架；
- RPMsg：基于virtio/vring的消息通道。

项目使用Linux内核已有驱动和用户态设备接口，没有自行实现底层共享内存驱动。

### 9.2 一条消息可能经过什么

```text
M33业务代码写RPMsg endpoint
  → M33 OpenAMP
  → 共享内存vdev buffer / vring
  → IPCC mailbox通知A35
  → Linux remoteproc/virtio/rpmsg驱动
  → RPMsg TTY或字符设备
  → /dev/ttyRPMSG0
  → A35 rpmsg_thread read
  → IMU/V2X业务处理
```

vring保存描述符和队列状态，实际payload位于共享buffer。mailbox主要用于通知，不是
把整条消息逐字节搬过去。

### 9.3 `running`、设备出现和业务ready是三层状态

1. remoteproc `running`：M33已离开复位并运行固件；
2. `/dev/ttyRPMSG0`出现：RPMsg通道已被Linux枚举；
3. A35发送ready且能收业务消息：项目协议进入可工作状态。

本项目RPMsg线程打开设备后固定等待1秒，再发送ready。这个等待仍是优化候选，但在
没有M33明确握手协议和现场回归时没有删除。

### 9.4 为什么A35应用重启不默认重置M33

M33生命周期由独立服务管理。A35业务重启或OTA时如果顺便重置M33，可能丢失正在采样
的状态、破坏RPMsg通道或扩大升级影响面。因此默认保留M33运行，仅在M33根本未运行
时使用回退启动路径。

### 9.5 共享内存一致性需要知道什么

底层框架负责vring同步、cache维护和通知。自己扩展共享内存时必须考虑：

- 双方看到的物理地址和映射是否一致；
- cache是否需要clean/invalidate；
- 写数据和更新队列的内存屏障顺序；
- 对端是否已经准备好；
- 重启任一侧后队列如何重新初始化。

当前项目没有绕开RPMsg直接自建共享内存，所以面试时理解原理即可，不要声称做过
自定义共享内存零拷贝。

## 10. GPIO、PWM和灯光

### 10.1 GPIO字符设备

`radar_fusion`打开`/dev/gpiochip3`，通过`GPIO_GET_LINEHANDLE_IOCTL`申请PD11 line，
再用`GPIOHANDLE_SET_LINE_VALUES_IOCTL`设置电平。这是GPIO字符设备uAPI，不是旧的
`/sys/class/gpio`导出方式。

申请line后得到新的fd，关闭gpiochip fd不会释放line；关闭line fd才释放。其他进程
同时申请同一line通常会失败，因此进程异常退出和资源回收很重要。

### 10.2 硬件PWM

PF10使用TIM2_CH3硬件PWM。用户脚本通过`/sys/class/pwm/pwmchip*`配置period、
duty_cycle和enable。真正的周期翻转由定时器硬件完成，不需要用户态线程持续sleep和
写GPIO，因此CPU占用和抖动更小。

### 10.3 BLE方向灯

MP257不直接控制远端左右灯GPIO，而是：

```text
融合方向状态
  → UART文本命令
  → CH9140 BLE Peripheral
  → STM32WBA Central
  → GPIO/MOSFET/灯
```

WBA回复ACK，BLE断开时清灯。这里同时包含Linux TTY、BLE连接状态、应用协议和GPIO
输出四层，排障时要逐层确认。

### 10.4 E04工程在V2V功能中的位置

STM32WBA54侧源码位于仓库外层的
[`E04-2G4M10S1AX`](../../../../E04-2G4M10S1AX/README.md)。这个工程补全了
MP257 Linux工程看不到的BLE另一端实现。

当前链路可以理解成：

```text
MP257 radar_fusion形成左右风险状态
  → /dev/ttySTM0 / USART2
  → CH9140 UART RX
  → CH9140把串口数据放入FFF1 Notify
  → STM32WBA54 BLE Central收到通知
  → 解析RISK文本命令
  → PA7/PA5方向灯

WBA生成ACK
  → FFF2 Write Without Response
  → CH9140 UART TX
  → MP257 /dev/ttySTM0读取ACK
```

它属于V2V/V2X功能中的无线接入、风险状态传递和执行器联动部分。当前WBA仓库的README
明确说明旧GNSS、IMU、语音和V2X业务已经移除，因此不能把这个工程单独说成完整V2V
系统，更不能说成标准C-V2X协议栈。风险来源和业务决策仍在MP257及系统其他模块中。

### 10.5 为什么WBA是Central，CH9140是Peripheral

CH9140默认从机模式是标准BLE Peripheral，广播名为`CH9140BLE2U`。它的所谓主机
模式只面向部分WCH芯片，不能当作通用Central连接STM32WBA。因此当前角色固定为：

| 设备 | BLE角色 | 主要职责 |
|---|---|---|
| CH9140 | Peripheral | UART与BLE透明传输、广播FFF0服务 |
| STM32WBA54 | Central / GATT Client | 扫描、连接、发现服务、订阅通知、回写ACK |

两边都设成Peripheral时不会自动连接；把CH9140切到不兼容的主机模式也不能解决。

### 10.6 GAP和GATT分别做什么

WBA端先执行GAP流程：

1. 启动扫描；
2. 解析Advertising Report；
3. 按`CH9140BLE2U`名称或`FFF0`服务UUID匹配目标；
4. 停止扫描；
5. 使用目标地址建立连接；
6. 断线后清理状态并重新扫描。

建立连接后进入GATT Client状态机：

```text
DISCONNECTED
  → DISCOVERING_SERVICE       查找FFF0
  → DISCOVERING_CHARACTERISTICS 查找FFF1/FFF2
  → DISCOVERING_DESCRIPTORS   查找FFF1的CCCD 0x2902
  → ENABLING_NOTIFY           写CCCD使能Notify
  → READY                     开始透明数据和命令处理
```

三项UUID的用途：

| UUID | 数据方向 | 用途 |
|---|---|---|
| FFF0 | 服务 | CH9140透明串口服务 |
| FFF1 | CH9140→WBA | CH9140 UART RX数据通过Notify发给WBA |
| FFF2 | WBA→CH9140 | WBA Write后从CH9140 UART TX输出 |
| 0x2902 | Client配置描述符 | 使能FFF1 Notify |

“GAP connected”不等于业务ready。只有FFF0、FFF1、FFF2和CCCD发现成功并完成Notify
使能，状态才进入`GATT=ready`。

### 10.7 WBA固件运行框架

WBA工程不是Linux程序，也没有使用当前仓库可见的RTOS任务模型。它采用STM32 HAL、
CMSIS、STM32_WPAN BLE协议栈和`UTIL_SEQ`协作式调度：

```text
main
  → HAL/clock/cache/RNG/RTC/UART/GPIO初始化
  → MX_APPE_Init / APP_BLE_Init
  → while(1)
      ├── MX_APPE_Process → UTIL_SEQ_Run → BLE host/HCI事件任务
      ├── BLE_UART_Bridge_Process
      ├── CH9140_Client_Process
      └── 每3秒heartbeat和GAP/GATT状态日志
```

BLE事件通过STM32_WPAN的HCI/ACI回调进入`app_ble.c`和`ch9140_client.c`。主循环
不应该长时间阻塞，否则BLE事件、串口桥接和状态机推进都会受到影响。

### 10.8 UART桥接和BLE分包

WBA UART1配置为115200 8N1，通过`HAL_UARTEx_ReceiveToIdle_DMA`接收调试端或测试端
串口数据。接收回调只把数据送入环形队列，再由主循环处理，避免在中断回调中执行复杂
BLE操作。

`ble_uart_bridge.c`维护两个512字节环形队列：

- `uart_to_ble`：WBA UART输入准备写入FFF2；
- `ble_to_uart`：FFF1通知和诊断日志准备从WBA UART输出。

单次BLE发送按20字节分块；WBA诊断UART每次最多取64字节。环形队列满时增加dropped
计数，不覆盖未处理数据。这里需要理解三个不同长度：UART DMA回调长度、环形队列容量
和BLE单包payload，它们不是同一概念。

### 10.9 文本协议和执行器

命令以换行或回车结束，接收缓冲为64字节。当前主要命令：

| 命令 | WBA动作 | 回复 |
|---|---|---|
| `PING` | 链路检查 | `PONG` |
| `RISK LEFT` | PA7左灯亮、PA5右灯灭 | `ACK RISK LEFT` |
| `RISK RIGHT` | PA5右灯亮、PA7左灯灭 | `ACK RISK RIGHT` |
| `RISK CENTER` | 两灯同时亮 | `ACK RISK CENTER` |
| `RISK CLEAR` | 两灯同时灭 | `ACK RISK CLEAR` |

此外保留`LED ON/OFF`、`LEFT ON/OFF`和`RIGHT ON/OFF`用于分层测试。未知命令返回
`ERR UNKNOWN CMD`，超过缓冲长度返回`ERR CMD TOO LONG`。

2026-08-10更换后的PA7和PA5方向灯与MP257 PD11告警灯同款，为高电平点亮；底板PA2诊断LED仍为低电平点亮。外接大功率灯不能直接由GPIO
驱动，应使用限流、MOSFET和独立供电，并保证共地。

### 10.10 失败处理和安全状态

- 连接断开：`link_ready=0`，清空待发送UART数据，左右风险灯立即熄灭；
- GATT发现失败：终止连接，返回扫描流程；
- FFF1通知未使能：不能进入ready；
- 命令不完整：继续缓存到换行；
- 环形队列满：记录dropped，不覆盖旧数据；
- WBA复位：GPIO初始化时先清灯；
- HAL初始化失败：进入Error Handler，输出`[FATAL] halted`并翻转诊断灯。

清灯是fail-safe设计：通信失效时宁可取消过期提示，也不能让上一次风险方向永久保留。
但这并不代表整套系统已经达到功能安全标准。

### 10.11 实机验证和安全边界

2026-08-07实机记录已验证：

- WBA UART 115200 8N1日志；
- WBA扫描并连接CH9140；
- `GAP=connected GATT=ready`；
- MP257发送`PING`并收到`PONG`；
- `RISK LEFT/RIGHT/CENTER/CLEAR`收到ACK并正确控制灯；
- 指令测试后连接保持ready；
- MP257 USART2已移除console/getty占用。

最初一直处于scanning的原因是CH9140没有3.3V供电，不是BLE状态机错误。这个案例说明
应先区分“完全没有收到广播”和“收到了其他广播但没匹配目标”。

当前代码按名称或FFF0服务识别目标。是否启用配对、链路加密和应用层身份认证，需要
结合CH9140能力及实际连接事件另行验证；仅靠名称、UUID和文本ACK不能抵御伪造节点，
因此不能把当前链路描述成量产级安全V2V通信。

### 10.12 WBA源码导航

| 文件 | 作用 |
|---|---|
| `E04-2G4M10S1AX/README.md` | 硬件角色、接线、命令和实测记录 |
| `Core/Src/main.c` | HAL初始化、UART DMA回调、主循环和GPIO |
| `Core/Src/ble_uart_bridge.c` | 双向环形队列、UART/BLE分块和诊断输出 |
| `STM32_WPAN/App/app_ble.c` | GAP扫描、广播匹配、连接和断线重扫 |
| `STM32_WPAN/App/ch9140_client.c` | GATT发现、Notify、文本协议、ACK和方向灯 |
| `Core/Inc/app_conf.h` | BLE地址、认证和协议栈配置 |
| `MDK-ARM/02_test.uvprojx` | Keil编译目标`E04_BLE_UART` |

阅读顺序建议：README→`main.c`→`app_ble.c`扫描连接部分→`ch9140_client.c`状态机→
`ble_uart_bridge.c`。不需要先读完整STM32_WPAN和HAL库。

## 11. 音频：ALSA、ASoC、SAI和骨传导

### 11.1 音频链路

```text
WAV文件
  → aplay / ALSA userspace
  → /dev/snd/pcm*
  → ALSA PCM core
  → ASoC machine driver (simple-audio-card)
  → SAI4B CPU DAI
  → HPDMA
  → I2S BCLK/LRCLK/DATA
  → MAX98357A
  → 骨传导执行器
```

ALSA是Linux音频子系统。ASoC是面向SoC音频的框架，把CPU DAI、Codec DAI、platform
DMA和machine routing组合成声卡。

### 11.2 Device Tree做了什么

项目DTS：

- 启用SAI4/SAI4B；
- 设置I2S格式、2个slot、32bit slot width；
- 指定SAI的DMA请求；
- 注册MAX98357A codec；
- 使用`simple-audio-card`把CPU DAI和codec连接；
- 配置PB4/PB5/PB6为SAI4B引脚。

`/proc/asound/cards`出现MAX98357A声卡只说明声卡注册成功。进一步还要检查播放时DMA
中断增长、引脚复用和实际声音。

### 11.3 为什么音频要串行播放

导航、碰撞、摔倒和V2X都可能同时要求播放声音。如果多个`aplay`并发打开同一个PCM
设备，可能出现`device busy`、混音冲突或后来的提示覆盖前面的提示。项目使用播放
线程和冷却策略控制并发。

如果需要真正的优先级抢占和混音，后续可以设计单一音频队列、优先级、可中断策略或
长期运行的音频服务，而不是临时启动多个`aplay`。

## 12. 存储、文件系统和事件录像

### 12.1 从文件到TF卡

```text
fwrite/write
  → C库缓冲（如果使用stdio）
  → VFS
  → FAT文件系统
  → block layer / I/O scheduler
  → MMC驱动
  → SD总线
  → TF卡flash控制器
```

写文件慢或损坏不一定是应用本身，可能来自文件系统检查、卡性能、掉电、挂载状态、
页缓存写回或临时缓存写放大。

### 12.2 fsck和mount为什么不能并发

fsck直接检查和修复文件系统元数据；挂载后的业务也会修改同一文件系统。如果两边
并发操作FAT，可能互相覆盖判断，造成目录、簇链和文件丢失。

当前脚本检查fsck unit是否仍为`activating`，等待自动挂载完成，确认fsck不活动时
才允许手工挂载。不能为了启动时间简单关闭fsck。

### 12.3 DVR当前实现

```text
V4L2 MJPEG帧
  → dvr_raw.bin追加压缩帧和时间戳
  → 内存帧索引保留最近窗口
  → 风险触发后继续采集
  → fork编码子进程
  → 提取JPEG序列
  → GStreamer v4l2slh264enc
  → H.264 + MP4
  → emergency_*.mp4
```

编码子进程通过`setsid`与父进程会话分离，父进程主循环用`waitpid(..., WNOHANG)`回收，
避免编码阻塞风险处理。

### 12.4 当前缓存限制

内存帧索引最多750帧，但`dvr_raw.bin`持续append。逻辑窗口有界不等于TF写入有界。
目标持续存在时会产生写放大，也会增加异常断电后出现恢复文件的概率。

候选RAM环形队列应同时限制：

- 时间上限，例如15秒；
- 总字节上限，例如256MiB；
- 达到任一上限丢弃最旧帧；
- 内存压力时优先缩短窗口，不能OOM。

256MiB只是最大允许值，不应该一次性固定占满。

### 12.5 多线程进程中fork的注意点

当前主程序是多线程进程，录像编码时`fork`，子进程在`exec`前还会调用malloc、stdio、
目录和文件操作。在POSIX模型中，fork后的子进程只保留调用fork的那个线程，其他线程
可能持有的用户态锁也被复制；子进程在exec前调用非async-signal-safe函数存在死锁
风险。

这是**代码审查层面的潜在风险，当前实测没有证明已经发生**。更稳妥的方向包括：

- 把准备工作放在fork前，子进程立即`exec`；
- 使用`posix_spawn`；
- 独立长期编码服务；
- 使用GStreamer API但明确线程和pipeline生命周期。

## 13. 网络、UDP、Dashboard和手机

### 13.1 Linux网络路径

用户程序调用`socket/bind/sendto/recvfrom`，数据经过内核socket层、UDP/IP、路由、
网络设备和Wi-Fi驱动。应用通常不直接控制MAC和射频。

### 13.2 当前端口和职责

| 端口 | 协议/方向 | 用途 |
|---:|---|---|
| 8888/UDP | 手机→A35/HUD | 导航、路况、预警消息 |
| 8890/UDP | `radar_fusion`→本机HUD | IMU事件转发 |
| 8889/UDP广播 | HUD→手机 | IMU/应急事件 |
| 8080/TCP | Dashboard HTTP | 状态、事件、控制面板 |
| 8090/TCP | OTA HTTP | 版本、上传、安装、状态 |

### 13.3 UDP的语义

UDP不建立连接，不保证到达、顺序或去重。`sendto`成功通常只证明数据交给本机内核，
不能证明对端应用收到，更不能证明手机短信发送成功。

项目通过事件ID、CSV和不同链路日志定位消息到达哪一层，但手机协议当前没有最终ACK。
如果业务要求端到端确认，需要在应用协议中增加序号、ACK、重发、超时和幂等处理。

### 13.4 hostapd和dnsmasq

- hostapd建立Wi-Fi AP；
- dnsmasq为连接设备提供DHCP等服务；
- 当前dnsmasq只等待hostapd，不再等待全局`network-online.target`。

`network-online`不是“所有网络业务都能访问互联网”的绝对保证，只是发行版定义的
一个同步点。产品应等待真正需要的接口、地址或服务条件。

### 13.5 Dashboard在架构中的位置

Dashboard是观测和控制界面，不参与核心融合判断。它读取`radar_state.json`、CSV、
systemd和`/proc`、`/sys`状态。拆成独立服务后，即使融合主程序退出，Dashboard仍可
展示故障；但旧状态可能变成stale，所以不能把“页面能打开”当成业务正常。

## 14. 用户态程序结构

### 14.1 构建和运行文件

主机使用AArch64交叉工具链：

- C：`aarch64-linux-gnu-gcc`；
- C++17：`aarch64-linux-gnu-g++`；
- 主要库：`stai_mpu`、`libjpeg`、`pthread`、`dl`；
- `make`同时编译`radar_fusion`和HUD，并检查Dashboard、Shell和OTA Python代码。

部署后主要程序：

| 程序/服务 | 语言 | 作用 |
|---|---|---|
| `radar_fusion` | C/C++ | 摄像头、NPU、雷达、DVR、RPMsg、告警主程序 |
| `hud` | C | OLED/HUD和IMU事件转发 |
| `radar_dashboard.py` | Python | HTTP状态、实验标注和控制界面 |
| `helmet_ota_server.py` | Python | A35应用OTA HTTP服务 |
| `start_dvr.sh` | Bash | 设备等待、进程启动、兼容管理和清理 |

### 14.2 `radar_fusion`为什么不是“所有逻辑都多线程”

核心摄像头、雷达和融合决策位于一个主循环，避免多个线程直接争用核心状态。独立线程
处理LED、RPMsg、导航接收、watchdog和音频。这样结构相对简单，但也意味着主循环中
不能放长时间阻塞操作，所以编码被移到子进程。

### 14.3 主循环的事件模型

主循环通过定时差控制摄像头采集，并使用`select`等待雷达串口。每轮还会：

- 检查编码子进程是否结束；
- 更新NPU连续帧状态；
- 解析雷达数据；
- 形成融合告警；
- 写CSV和JSON状态；
- 触发DVR、音频和灯光。

这是一种“单线程状态机 + 辅助线程”的架构，不是完整事件框架。优点是状态关系直观；
缺点是主循环职责较多，任何长耗时步骤都会影响其他任务。

### 14.4 全局状态和同步

项目使用多个全局告警状态，CSV写入使用mutex保护。需要理解：

- mutex保护的是临界区和内存可见性；
- `volatile`只限制编译器部分优化，不等于线程安全；
- 多线程读写普通变量理论上可能产生data race；
- 一个状态如果由多个线程更新，应明确所有者，或使用mutex/atomic/message queue。

当前代码能运行不代表并发模型已经形式化验证。后续重构可按“单写者、多读者”或事件
队列减少共享变量。

### 14.5 状态机比一堆if更值得理解

项目中至少有这些状态机：

- NPU未确认→连续确认→确认→连续丢失→否定；
- 雷达无目标→目标存在→达到风险→风险解除；
- DVR空闲→预缓存→已触发→后录制→编码→重新武装；
- OTA空闲→安装中→健康检查→成功/回滚/失败；
- systemd inactive→activating→active→failed→restart。

分析问题时先画状态和转换条件，比直接在几千行代码里搜索某个变量更有效。

## 15. 业务数据流

### 15.1 融合告警

```text
摄像头MJPEG → NPU道路使用者状态 ─┐
                                  ├→ 融合风险 → LED/音频/方向灯/DVR
雷达BSD目标 → 风险条件/方向/TTC ──┘
```

当前属于状态/决策级融合，不是雷达目标与图像框的空间标定级融合。

### 15.2 IMU事件

```text
M33 IMU判断
  → RPMsg
  → A35本地LED/音频/DVR
  → UDP 8890
  → HUD记录和同类冷却
  → UDP 8889广播
  → 手机App
```

每一箭头都有独立成功条件。A35成功不能代替手机端验收。

### 15.3 导航和路况

```text
手机UDP 8888 JSON
  → nav_tts线程
  ├→ OLED/HUD方向和距离
  └→ 本地WAV骨传导播放
```

开发板无网络时优先使用本地缓存，在线TTS默认关闭，避免把网络不可达变成核心业务
依赖。

### 15.4 事件录像

```text
可能目标出现 → 开始MJPEG预缓存
风险确认     → 标记触发时间
继续采集     → 保留后置时间
停止缓存     → 子进程H.264/MP4编码
编码完成     → 回收子进程并重新武装
```

连续测试中曾出现第一段录像后无法重新启动缓存，根因是NPU状态未重新形成期望边沿。
这说明业务不应只依赖一次0→1边沿，还要检查“当前有目标、当前未缓存”这样的稳态条件。

## 16. systemd和启动依赖

### 16.1 systemd不是按文件顺序执行脚本

systemd根据unit依赖图并发启动。常见关系：

- `Wants=`：弱依赖，尝试一起启动；
- `Requires=`：强依赖，一个失败会影响另一个；
- `After=`：只定义顺序，不自动拉起；
- `Before=`：反向顺序；
- target：一组启动状态，不是传统脚本阶段；
- `WantedBy=`：enable时建立依赖链接。

### 16.2 当前服务关系

```text
sysinit.target
  └── dvr-m33.service (oneshot, early remoteproc)

multi-user.target
  ├── hostapd.service
  │   └── dnsmasq.service
  ├── radar-dashboard.service
  ├── helmet-ota.service
  └── dvr.service
      ├── After local-fs
      ├── After dvr-m33
      ├── Wants Dashboard
      └── ExecStart start_dvr.sh
```

### 16.3 `Type=oneshot`和`Type=simple`

- M33早期脚本执行一次初始化后退出，使用`oneshot`和`RemainAfterExit=yes`；
- 主业务脚本长期运行，使用`simple`；
- `RemainAfterExit=active`只表示初始化动作完成，不持续证明M33健康。

### 16.4 udev settle为什么通常过宽

`systemd-udev-settle`等待整个udev事件队列清空，可能被项目不使用的设备拖慢。业务
真正需要的是具体设备，例如雷达串口、摄像头、GPIO和RPMsg。

当前脚本对实际设备做有界等待：

- 雷达是硬前提，超时退出，由systemd重启；
- 摄像头、BLE灯、GPIO等保留原降级行为；
- RPMsg由M33启动和既有超时负责。

精确等待不是取消设备同步，而是把同步条件从“全系统都完成”收窄为“我的依赖完成”。

### 16.5 服务active为什么不够

如果systemd跟踪的是父脚本，而真正业务子进程退出，父脚本又卡在日志管道，unit仍
可能显示active。健康检查应分层：

1. unit状态；
2. 主业务PID；
3. 设备fd和线程是否工作；
4. `radar_state.json`时间戳是否更新；
5. 端到端执行器或数据是否产生。

## 17. 启动优化的通用方法

### 17.1 先定义终点

不同终点会得到不同“启动时间”：

- kernel ready；
- userspace/systemd ready；
- M33 ready；
- 基础告警ready；
- 摄像头融合ready；
- DVR存储ready；
- Wi-Fi/手机连接ready。

不能通过把初始化推迟到第一次事件来制造更好数字，否则首个告警会变慢。

### 17.2 建立统一时间轴

```text
UART主机时间戳：完整上电链
dmesg printk time：内核入口以后
journal monotonic：systemd和服务
CLOCK_BOOTTIME里程碑：应用内部
```

墙钟可能校时，不适合直接拼接启动阶段。

### 17.3 找关键路径，不看最大数字猜测

正确顺序：

1. `systemd-analyze time`看阶段；
2. `blame`找候选慢项；
3. `critical-chain`看阻塞路径；
4. journal单调时间确认重叠；
5. 应用里程碑拆内部初始化；
6. 协议ACK或设备事件证明等待可以结束；
7. 冷启动多次看范围和中位数；
8. 做功能回归和回退。

### 17.4 当前大项的处理逻辑

- SNMP：确认项目不使用，停用失败超时服务；
- M33：前移到sysinit，保留回退；
- 雷达：匹配ACK结束等待，保留超时；
- udev：全局等待改具体设备；
- 网络：全局online改等待hostapd；
- fsck：修复数据和竞态，不能关闭；
- U-Boot：缺少安全接管和恢复条件，测试后撤回。

### 17.5 下一阶段应该考虑“分级ready”

早期完整业务把摄像头、TF、融合和部分网络准备绑得较紧。当前已实现存储动态接入，
并将主录像迁到板载ext4；该分级模型仍可用于解释故障隔离：

```text
Level 1：M33 + RPMsg + 雷达基础告警
Level 2：摄像头 + NPU融合
Level 3：板载ext4 + 事件录像（TF仅人工导入导出）
Level 4：Wi-Fi + Dashboard + 手机协同
```

但拆分后要处理晚到设备、启动期间事件缓存、状态切换和失败降级。没有现场回归时只做
设计，不应直接改生产路径。

## 18. 运行可靠性

### 18.1 先划分故障域

| 故障域 | 例子 | 是否应拖垮核心告警 |
|---|---|---|
| M33/RPMsg | 固件未启动、endpoint未出现 | 会影响IMU/V2X，但雷达可考虑保留 |
| 摄像头/NPU | 节点晚到、模型失败 | 可退化为纯雷达 |
| 雷达 | 串口不存在、协议错误 | 核心融合无法工作，应明确失败 |
| 存储 | userfs未挂载、空间不足 | 告警可继续，录像不可用 |
| 音频/灯光 | PCM或GPIO失败 | 其他输出可继续 |
| Dashboard | HTTP进程失败 | 不应停止核心融合 |
| OTA | 服务失败 | 不影响当前release运行 |

故障隔离不是把所有错误都忽略，而是明确哪些是硬前提、哪些允许降级，并让状态可见。

### 18.2 日志策略

不同数据有不同语义：

- system journal：服务生命周期和系统事件；
- 文本日志：人类排障；
- CSV：结构化实验和时间线；
- JSON：Dashboard当前快照；
- MP4：事故业务数据；
- OTA状态JSON：事务状态。

不能用一个“定期rm旧文件”策略处理全部类型。正式事故视频可能需要人工保护，CSV
轮转要保留表头和写入一致性，状态JSON适合临时文件+rename。

### 18.3 原子性、幂等和回退

- 原子性：外部只看到切换前或切换后，不看到半完成状态；
- 幂等：脚本重复执行不会不断破坏状态；
- 回退：改动失败后能恢复已知正常版本；
- 健康检查：不能只看进程是否存在，要检查关键功能入口。

启动优化脚本保存原unit状态，OTA使用release目录和符号链接切换，都是这些原则的
具体应用。

## 19. OTA的系统视角

### 19.1 当前事务

```text
上传
  → 包和MANIFEST校验
  → staging安全解压
  → 继承持久化radar_config
  → stop dvr.service
  → 切换/xxl/camera_detect符号链接
  → start dvr.service
  → 检查VERSION、unit、进程和Dashboard
  → success或rollback
```

安装进程脱离HTTP请求运行，避免手机断开直接中止事务。启动时发现`installing`状态会
优先恢复上一稳定版本。

### 19.2 “原子切换”能保证什么

符号链接rename能让路径切换在目录项层面接近原子，但不能自动保证：

- 新程序一定能运行；
- 所有旧进程都已退出；
- 打开的旧文件描述符自动指向新release；
- 掉电时所有文件已经落盘；
- 新旧版本配置和数据格式兼容。

所以还需要stop/start、健康检查、持久化配置和回滚。

### 19.3 安全边界

SHA-256回答“文件传输后是否相同”，不能回答“谁发布了这个包”。当前HTTP和私有AP
适合第一版联调，量产还需：签名、可信公钥、认证授权、防重放、防降级、HTTPS、密钥
保护和审计。

## 20. 观测和排障框架

### 20.1 通用七层检查

遇到任何“功能不工作”，按以下顺序缩小范围：

1. 供电、线序和硬件状态；
2. DT/pinctrl/clock/reset/DMA/IRQ；
3. 驱动probe和设备节点；
4. 权限、占用和用户态打开参数；
5. 字节/帧/消息协议；
6. 业务状态机和线程；
7. 执行器、远端设备和最终回执。

不要从第7层现象直接猜第2层根因。

### 20.2 工具与能证明的事情

| 工具 | 适用层 | 能看到什么 | 不能单独证明什么 |
|---|---|---|---|
| UART抓取 | bootloader/内核 | 完整上电日志 | 业务功能正确 |
| `dmesg` | 内核/驱动 | probe、IRQ、DMA、文件系统 | 用户业务状态 |
| `systemctl/journalctl` | 服务 | unit、退出码、依赖、日志 | 传感器真实效果 |
| `udevadm` | 设备 | 节点事件和属性 | 数据内容正确 |
| `media-ctl/v4l2-ctl` | 摄像头 | 拓扑、格式、流能力 | NPU和融合正确 |
| `strace` | 进程 | 系统调用、阻塞、错误码 | 内核函数内部细节 |
| `perf` | CPU/调度 | 热点、采样、统计 | 低频外设协议全部语义 |
| `ftrace/trace-cmd` | 内核 | probe、调度、函数和事件 | bootloader时间 |
| `ss` | 网络 | socket监听和连接 | UDP对端已处理 |
| `lsof/fuser` | 文件/设备 | 谁持有fd | 协议是否正常 |
| `/proc/interrupts` | IRQ | 中断是否增长 | 数据一定正确 |
| `vmstat/iostat` | 系统负载 | CPU、内存、I/O压力 | 某个业务状态的原因 |
| CSV/JSON事件 | 业务 | 状态机和跨模块时间线 | 物理环境标签一定正确 |

### 20.3 按症状定位

**`/dev/video7`不存在**

先看`lsusb -t`、sysfs驱动路径、`dmesg`中的USB/`uvcvideo`日志和V4L2节点，
再检查供电、Hub连接及驱动加载，不要先改应用重试次数。

**串口存在但没有雷达数据**

检查占用、termios、RX/TX、供电和原始字节；设备节点存在只证明驱动注册。

**服务active但页面状态不更新**

检查`radar_fusion` PID、状态文件mtime、主循环日志和Dashboard stale，不只看systemd。

**有LED但手机没收到**

沿RPMsg→A35→UDP8890→HUD→UDP8889→手机逐段看事件ID和CSV；LED只证明A35本地分支。

**录像文件没有生成**

分别检查预缓存状态、触发状态、后录制时间、编码子进程、GStreamer退出码、TF挂载和
空间，不能把“没有MP4”统一归因于摄像头。

## 21. 源码导航

### 21.1 从问题找到文件

| 问题 | 首先看什么 |
|---|---|
| 主业务入口和状态机 | `radar_fusion.cpp`的`main`和主循环 |
| V4L2采集 | `camera.c/.h` |
| NPU模型和NMS | `npu_detect.cpp/.h` |
| 导航、OLED和骨传导 | `nav_tts.c/.h` |
| BLE方向灯协议 | `ble_risk_output.cpp/.h` |
| WBA BLE Central和方向灯固件 | `../../../../E04-2G4M10S1AX/STM32_WPAN/App/`、`Core/Src/` |
| HUD/手机转发 | `hud_project/main.c`、`udp.c`、`oled.c` |
| systemd启动关系 | `dvr.service.example`、`dvr-m33.service.example` |
| M33早期启动 | `scripts/start_m33_early.sh` |
| 总启动脚本 | `start_dvr.sh` |
| 启动优化脚本 | `scripts/boot_optimize.sh` |
| Dashboard | `dashboard/radar_dashboard.py`、`static/*` |
| OTA事务 | `ota/helmet_ota_installer.py`、`helmet_ota_common.py` |
| OTA HTTP | `ota/helmet_ota_server.py` |
| 设备树 | `board/myb-stm32mp257x-2GB.dts` |
| 编译部署 | `Makefile` |

### 21.2 `radar_fusion.cpp`的阅读顺序

不要从第一行顺序读到最后。建议：

1. 顶部宏：设备路径、阈值、端口、DVR参数；
2. `main`初始化顺序和主循环；
3. 雷达结构体、解析和`radar_init`；
4. NPU状态更新和融合触发分支；
5. `dvr_start/dvr_trigger_save/dvr_encode_mp4`；
6. `rpmsg_thread`和IMU/V2X处理；
7. CSV/JSON状态写入；
8. 信号和退出清理。

每读一个函数，记录四件事：输入、输出、共享状态、失败路径。

### 21.3 配置、运行状态和业务数据要分开

- 配置：`radar_config`、systemd unit、DTB；
- 运行状态：PID、fd、remoteproc state、Dashboard JSON；
- 业务数据：CSV、MP4、人工labels；
- 软件版本：release目录、VERSION、OTA状态。

不要为了清理日志误删配置或事故视频，也不要用旧CSV判断当前进程状态。

## 22. 当前已知边界和代码审查候选

以下内容用于后续学习和设计，不表示已经在板端造成故障，也不应直接写成简历成果：

1. V4L2缓冲在调用者处理前已经QBUF，存在数据被驱动覆盖的潜在竞态；
2. 多线程进程fork后在exec前执行大量非安全函数，存在继承锁状态的理论风险；
3. 多个全局状态跨线程读写，`volatile`不能替代atomic或mutex；
4. DVR帧索引有界但TF原始文件持续append，写入量未真正有界；
5. 实机已确认当前`/dev/video7`为USB UVC，但`video7`编号仍依赖枚举顺序，产品配置
   最好使用稳定udev链接或按设备属性匹配；
6. RPMsg ready仍依赖固定1秒等待，缺少显式双向握手；
7. UDP手机链路缺少端到端ACK；
8. 完整业务ready仍与摄像头和TF较强耦合；
9. OTA只校验完整性，没有发布者签名和公网安全能力；
10. WBA当前按名称/FFF0识别CH9140，应用层身份认证和抗伪造能力尚未建立；
11. SSH和日志检查不能代替真实雷达目标、画面、骨传导、方向灯和手机验收。

任何后续修复都应先构造可复现测试，再做单项改动和回退，不能仅凭静态代码推断直接
修改生产板。

## 23. 给后续GPT的使用方式

如果能够上传文件，把本文和下面专项文档一起提供：

- [DATA_FLOW.md](DATA_FLOW.md)：业务数据流；
- [BOOT_OPTIMIZATION.md](BOOT_OPTIMIZATION.md)：启动实测和决策；
- [RUNTIME_STORAGE.md](RUNTIME_STORAGE.md)：进程、日志和存储；
- [OTA.md](OTA.md)：OTA事务；
- [DEVICE_TREE.md](../board/DEVICE_TREE.md)：板级音频和PWM；
- [E04 WBA README](../../../../E04-2G4M10S1AX/README.md)：BLE Central、GATT和方向灯；
- [INTERVIEW_PREPARATION.md](INTERVIEW_PREPARATION.md)：表达和问题清单。
- [COMPETITION_PREPARATION.md](COMPETITION_PREPARATION.md)：按现场事件梳理逻辑链、
  演示证据和排查节点。

建议提示词：

```text
这是一份真实STM32MP257嵌入式Linux项目知识库。请先区分“Linux通用原理”和“项目
当前实现”，不要新增文档没有的功能或实测结果。

我接下来会指定一个主题。请按以下顺序展开：
1. 这个主题在Linux分层中的位置；
2. 内核和用户态分别负责什么；
3. 对应到本项目的设备节点、服务、线程、源码文件和数据流；
4. 正常生命周期；
5. 常见失败模式和观测工具；
6. 当前实现的边界；
7. 用3到5个问题检查我是否理解。

如果材料之间有冲突，请指出冲突和验证方法，不要擅自选择一个结论。对尚未实测的
代码风险使用“潜在风险”而不是“已发生故障”。
```

可以逐章提问，例如：

- “从`open /dev/video7`开始，讲到一帧MJPEG进入NPU，中间经过哪些内核对象？”
- “结合DTS中的memory-region，解释remoteproc和RPMsg如何使用共享内存。”
- “从`fwrite`到TF卡flash，解释页缓存、fsync、FAT和异常断电风险。”
- “从`aplay`开始，解释ALSA、ASoC、SAI、DMA和MAX98357A的关系。”
- “用本项目的90秒SNMP问题解释systemd并发和关键路径。”
- “分析服务active但业务退出时，PID 1到底在跟踪谁。”

## 24. 学习顺序

### 第一阶段：建立纵向链路

先掌握第2、4、5、6章。能够从“业务调用”一路说到“内核驱动和硬件资源”。

### 第二阶段：掌握四条关键I/O链路

1. V4L2摄像头和NPU；
2. UART雷达协议；
3. remoteproc/RPMsg；
4. ALSA/ASoC音频。

每条链路都画出设备、内核子系统、设备节点、用户函数和业务状态。

### 第三阶段：理解Linux工程问题

学习进程/线程、fd、select、信号、VFS、页缓存、systemd和udev，再回看启动优化、
服务恢复和录像存储。

### 第四阶段：学会故障定位

选择一个现象，按第20章七层检查法写出命令、预期结果和下一步，不直接猜根因。

### 第五阶段：回到面试表达

最后再看面试手册。先有技术框架，再压缩成30秒或2分钟回答，表达会更自然，也不容易
因为追问而断层。
