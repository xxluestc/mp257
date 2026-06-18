# 雷达+摄像头融合+DVR 调试经验记录

## 项目概述

将雷达目标检测、摄像头 NPU 目标识别、LED 告警、DVR 行车记录融合为一个程序 `radar_fusion`，运行在米尔 MYD-LD25X 开发板上。

### 融合逻辑

```
摄像头始终采集 (25fps)
    │
    ├─ NPU 推理 (每 5 帧) — 始终运行
    │   ├─ 检测到道路用户 → npu_has_target=1 → 开始 DVR 缓冲
    │   ├─ 连续 N 帧确认 → npu_confirmed=1
    │   └─ 丢失目标 → npu_has_target=0 → 无触发则清理缓冲
    │
    ├─ 雷达数据
    │   ├─ 检测到目标 → target_active=1
    │   └─ 目标消失 3s → target_active=0
    │
    └─ 触发条件: radar.should_alert AND npu_confirmed AND !npu_denied
        ├─ DVR 保存触发 → 继续录制 15s → 异步编码 MP4
        └─ LED 闪烁
```

### 关键文件

| 文件 | 说明 |
|------|------|
| `radar_fusion.cpp` | 主程序，融合雷达、NPU、LED、DVR |
| `camera.c` | USB 摄像头采集 (MJPEG) |
| `npu_detect.cpp` | NPU SSD MobileNet V2 推理 |
| `stai_mpu/` | 正点原子 NPU 库文件 |

---

## 一、编译与部署

### 交叉编译

```bash
cd /home/alientek/dvr_project/mier/camera_detect
make clean
make radar-fusion CC=aarch64-linux-gnu-gcc CXX=aarch64-linux-gnu-g++
```

### 部署到开发板

```bash
scp radar_fusion root@192.168.88.10:/xxl/camera_detect/
```

### 开发板运行

```bash
cd /xxl/camera_detect
LD_LIBRARY_PATH=/usr/lib:/vendor/lib ./radar_fusion
```

---

## 二、关键调试经验

### 1. 设备冲突处理 (Device or resource busy)

**现象**: 打开摄像头或雷达设备时报 `Device or resource busy`。

**原因**: 之前的进程未正常退出，仍占用设备文件描述符。

**解决**: 在打开设备前使用 `fuser -k` 强制释放。

```cpp
static void kill_device_holders(const char *device) {
    char cmd[256];
    snprintf(cmd, sizeof(cmd), "fuser -k %s 2>/dev/null", device);
    system(cmd);
    usleep(500000);  // 等 500ms 让内核释放
}
```

### 2. NPU 库的交叉编译

**问题**: 正点原子的 NPU 库 `libstai_mpu.so` 需要从正点原子 rootfs 中提取。

**操作步骤**:
1. 解压根文件系统镜像: `sudo mount -o loop rootfs.ext4 /mnt/rootfs`
2. 复制库文件: `cp /mnt/rootfs/usr/lib/libstai_mpu.so* ./stai_mpu/`
3. 复制头文件: `cp /mnt/rootfs/usr/include/stai_mpu*.h ./stai_mpu/include/`

**编译链接**:
```makefile
radar_fusion: camera.o npu_detect.o radar_fusion.o
    $(CXX) -o $@ $^ -L./stai_mpu -lstai_mpu -ljpeg -lstdc++ -lm -lpthread -ldl
```

### 3. ffmpeg 编码器兼容性

**问题**: `Unrecognized option 'preset'` — ffmpeg 编码失败。

**原因**: 开发板 ffmpeg 编译时 `--disable-libx264`，不支持 `libx264` 编码器和 `-preset` 参数。

**修复**: 改用 `mpeg4` 编码器。

```cpp
// 错误
snprintf(cmd, sizeof(cmd),
    "ffmpeg -y -r %d -f concat -safe 0 -i %s "
    "-c:v libx264 -preset ultrafast -crf 28 ...");

// 正确
snprintf(cmd, sizeof(cmd),
    "ffmpeg -y -r %d -f concat -safe 0 -i %s "
    "-c:v mpeg4 -q:v 5 ...");
```

**注意**: `-r` 参数必须在 `-i` 之前，否则输入的帧率不会被正确解析。

### 4. JPEG 解码错误抑制

**现象**: 大量 `Corrupt JPEG data` 警告刷屏。

**原因**: 摄像头采集的 MJPEG 帧偶尔不完整，`libjpeg` 的默认错误处理会打印到 stderr。

**修复**: 覆盖 libjpeg 的错误处理函数，静默抑制。

```cpp
static void jpeg_error_silent(j_common_ptr cinfo) {
    // 不打印任何错误信息
}
// 在解压 JPEG 前设置
cinfo.err->error_exit = jpeg_error_silent;
cinfo.err->emit_message = jpeg_error_silent;
```

---

### 5. ffmpeg 编码输出刷屏

**现象**: 编码时 ffmpeg 的进度信息（frame=xx fps=xx bitrate=xx）与主程序日志混在一起，刷屏严重。

**原因**: `execlp("ffmpeg", ...)` 的子进程继承父进程 stdout/stderr。

**修复**: 在孙进程 `execlp` 之前，将 stdout 和 stderr 重定向到 `/dev/null`。

```cpp
pid_t ff_pid = fork();
if (ff_pid == 0) {
    /* 重定向 stdout/stderr, 避免刷屏 */
    int devnull = open("/dev/null", O_WRONLY);
    if (devnull >= 0) {
        dup2(devnull, STDOUT_FILENO);
        dup2(devnull, STDERR_FILENO);
        close(devnull);
    }
    execlp("ffmpeg", "ffmpeg", ...);
    exit(1);
}
```

主进程只打印关键结果：`[DVR] Child: Saved xxx.mp4`。

---

## 三、致命 Bug 记录

### Bug 1: NPU 死锁 — npu_denied 永不重置 (最严重)

**现象**: DVR 缓冲反复启停，但触发保存从未执行。

**根因**: 
```
启动时 NPU 无目标 → 3 帧后 npu_denied = 1  ← 锁定
NPU 检测到人 → npu_has_target = 1 → 开始缓冲
npu_confirm_cnt 计数到阈值 → 条件检查:
    if (npu_confirm_cnt >= 2 && !npu_confirmed && !npu_denied)
                                                 ^^^^^^^^^^^^
                                                 FALSE！死锁！
npu_denied 只在编码完成后重置 → 编码永远不触发 → 循环死锁
```

**修复**: 新目标出现时重置所有状态。

```cpp
if (!npu_has_target) {
    npu_has_target = 1;
    npu_denied = 0;       // ← 打破死锁
    npu_confirmed = 0;
    npu_confirm_cnt = 0;
    npu_deny_cnt = 0;
    // 开始 DVR 缓冲...
}
```

### Bug 2: DVR 保存触发不检查雷达

**现象**: NPU 确认后立即触发 DVR 保存，即使雷达完全没有检测到目标。

**根因**: DVR 保存触发条件只有 `npu_confirmed && !npu_denied`，没有 `radar.should_alert`。

**修复**: 将 DVR 保存触发和 LED 控制都移到雷达处理段，加上 `radar.should_alert` 条件。

```cpp
// 雷达段
if (radar.should_alert && npu_confirmed && !npu_denied) {
    g_led_alert = 1;
    if (dvr_recording && !dvr_save_triggered && !dvr_encoding) {
        dvr_trigger_save(trigger_ts);
    }
} else {
    g_led_alert = 0;
}
```

### Bug 3: 时间戳基准不一致 — Post-trigger 计时溢出

**现象**: `Post-trigger recording complete (18445034998473565 ms)` — 天文数字，post-trigger 15 秒等待被跳过。

**根因**: 两个时间戳使用了不同的基准。

```
帧缓冲段 ts_us    = 相对 t_start 的时间 (如 7300000 μs)
雷达段   trigger_ts = 绝对 gettimeofday 时间 (约 1736 亿 μs)

ts_us - trigger_ts = 7300000 - 1736000000000 = 负数溢出 → 巨大正数
```

**修复**: 统一使用相对于 `t_start` 的时间。

```cpp
// 错误 (绝对时间)
dvr_trigger_save(tv_now.tv_sec * 1000000ULL + tv_now.tv_usec);

// 正确 (相对时间)
dvr_trigger_save((uint64_t)(tv_now.tv_sec - t_start.tv_sec) * 1000000ULL
                 + (uint64_t)tv_now.tv_usec);
```

### Bug 4: LED 不亮

**现象**: 触发告警后 LED 不闪烁。

**原因**: 最初使用 `green:heartbeat` 内核虚拟 LED；后改用 sysfs 接口操作 PD11 引脚，路径错误且权限问题。

**修复**: 使用 GPIO 字符设备 API (`/dev/gpiochip3` line 11 = PD11)。

```cpp
static int led_init(void) {
    int chip_fd = open("/dev/gpiochip3", O_RDONLY);
    struct gpio_v2_line_request req = {
        .offsets = {11},
        .num_lines = 1,
        .consumer = "radar-fusion",
        .config = {
            .flags = GPIO_V2_LINE_FLAG_OUTPUT,
            .num_attrs = 0,
        },
    };
    ioctl(chip_fd, GPIO_V2_GET_LINE_IOCTL, &req);
    // ...
}
```

---

## 四、DVR 行车记录模块

### 目录结构

```
/run/media/mmcblk0p1/dvr/           # TF 卡 DVR 根目录
├── .buffer/                         # 帧缓冲临时目录
│   ├── dvr_raw.bin                  # 原始 MJPEG 帧
│   ├── dvr_index.bin                # 帧索引
│   └── filelist.txt                 # ffmpeg 输入列表
└── emergency_YYYYMMDD_HHMMSS.mp4   # 保存的 MP4 文件
```

### 关键设计

1. **缓冲启动**: NPU 检测到道路用户时开始缓冲，不是雷达检测到目标时
2. **保存触发**: `radar.should_alert && npu_confirmed && !npu_denied`
3. **异步编码**: `fork()` 子进程执行 ffmpeg，父进程继续采集和推理
4. **状态重置**: 编码完成后 `npu_confirmed/denied/confirm_cnt/deny_cnt` 全部清零，支持连续触发

### 道路用户类别

```cpp
static bool is_road_user(const char *label) {
    return (strcmp(label, "person") == 0   ||
            strcmp(label, "bicycle") == 0  ||
            strcmp(label, "car") == 0      ||
            strcmp(label, "motorbike") == 0||
            strcmp(label, "bus") == 0      ||
            strcmp(label, "truck") == 0);
}
```

---

## 五、开发板环境信息

| 项目 | 值 |
|------|-----|
| 开发板 | 米尔 MYD-LD25X (STM32MP257) |
| 摄像头 | USB 摄像头 (MJPEG, 1280x720) |
| 雷达 | 毫米波雷达，串口 `/dev/ttySTM1`，波特率 921600 |
| NPU | NPU-4 (STM32MP25 内置)，SSD MobileNet V2 模型 |
| LED | PD11 (gpiochip3 line 11) |
| TF 卡 | `/run/media/mmcblk0p1/` |
| ffmpeg | 6.1.1 (mpeg4 编码器) |
| 编译器 | aarch64-linux-gnu-gcc/g++ |

---

## 六、常见问题快速排查

### 雷达无数据

```bash
# 检查设备是否存在
ls -la /dev/ttySTM1

# 检查是否有进程占用
fuser /dev/ttySTM1

# 检查是否在发送数据
timeout 1 cat /dev/ttySTM1 | xxd | head
```

### 摄像头无画面

```bash
# 检查设备
ls -la /dev/video*

# 释放占用
fuser -k /dev/video0
```

### NPU 初始化失败

```bash
# 检查库文件
ls -la /usr/lib/libstai_mpu.so* /vendor/lib/libstai_mpu.so*

# 检查模型文件
ls -la /xxl/camera_detect/ssd_mobilenet_v2*
```

### 查看已保存的 DVR 文件

```bash
ls -la /run/media/mmcblk0p1/dvr/emergency_*.mp4
```

---

## 七、架构演进历程

| 阶段 | 描述 |
|------|------|
| 1 | 独立雷达检测程序 (`radar_link.c`) |
| 2 | 独立摄像头 NPU 检测程序 (`camera_detect`) |
| 3 | 雷达+NPU 融合，NPU 只在雷达激活时运行 |
| 4 | NPU 始终运行，NPU 检测到目标开始缓冲 |
| 5 | DVR 集成，NPU 触发缓冲+雷达确认触发保存 |
| 当前 | 完整融合：NPU 缓冲 → 雷达+NPU 触发 → 异步编码 → 连续触发 |