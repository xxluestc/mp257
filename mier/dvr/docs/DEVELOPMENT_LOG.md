# USB摄像头行车记录开发日志

## 项目概述

在 `/home/alientek/dvr_project/mier/dvr/` 目录下开发基于USB摄像头的行车记录功能。

### 开发板信息

| 项目 | 详情 |
|------|------|
| 开发板型号 | myd-ld25x (米尔科技) |
| CPU | aarch64 (ARM 64-bit) |
| 内核 | Linux 6.6.48 |
| 系统 | OpenSTLinux Weston 5.0.3 (scarthgap) |
| IP地址 | 192.168.88.10 |
| 摄像头 | USB 2.0 Camera (UVC), /dev/video7 |
| 摄像头格式 | MJPEG, 最高 1920x1080@30fps |

### 与原始 recorder 的区别

| 项目 | 原始 recorder (STM32MP257D-ATK) | 当前 mier/dvr (myd-ld25x) |
|------|--------------|---------------|
| 摄像头 | DCMIPP 并行摄像头 (`/dev/video-camera0`) | USB UVC 摄像头 (`/dev/video7`) |
| ISP | 需要 `dcmipp-isp-ctrl` | 不需要 |
| 显示 | LCD (framebuffer/Qt) | 无LCD |
| M核通信 | RPMSG (`/dev/ttyRPMSG0/1`) | 暂不需要（注释掉） |
| 触发方式 | M核 + 命名管道 | A核Linux自触发 |
| 存储 | SD卡 (`/run/media/mmcblk0p1`) | SD卡 (`/run/media/mmcblk0p1/dvr`) |
| 编译方式 | 板载gcc | PC交叉编译 (`aarch64-linux-gnu-gcc`) |

### 文件结构

```
mier/dvr/
├── Makefile             # 编译脚本 (支持VM编译 + 交叉编译 + board编译)
├── .gitignore           # Git忽略规则
├── src/                 # 源码目录
│   ├── dvr_types.h      # 数据类型定义 (状态机、触发事件、配置、NPU配置)
│   ├── dvr_engine.h/c   # DVR 核心引擎 (状态机、环形缓冲、编码保存、NPU融合)
│   ├── dvr_main.c       # 主入口 (命令行参数解析、信号处理)
│   ├── usb_camera.h/c   # USB摄像头V4L2接口
│   ├── ring_buffer.h/c  # 环形缓冲区 (异步写线程、时间戳索引)
│   ├── trigger_receiver.h/c # 命名管道触发接收器
│   ├── frame_decoder.h/c    # 帧解码器 (MJPEG/YUYV → RGB, 用于NPU)
│   ├── npu_fusion.h/cpp     # NPU融合模块 C API (封装C++ NpuDetector)
│   ├── npu_detector.hpp/cpp # 纯C++ NPU检测器 (SSD MobileNet V2, 无Qt)
├── scripts/             # 脚本目录
│   └── test_dvr.sh      # 测试脚本 (自动/手动模式)
├── output/              # 测试输出 (视频、日志) — 不提交git
└── docs/                # 文档目录
    └── DEVELOPMENT_LOG.md
```

---

## 开发记录

### 2026-06-16: 创建基础录制程序 — 验收通过

**目标：** 验证USB摄像头能正常采集视频并保存为MP4文件

**实现：**
1. `usb_camera.c/h` - 从原始 `camera_v4l2.c` 改写，去除ISP控制，增加MJPEG/YUYV等USB常用格式支持
2. `simple_recorder.c` - 简易录制程序，通过V4L2采集帧，pipe给ffmpeg编码为MP4

**测试结果：**
- 摄像头: USB 2.0 Camera (uvcvideo), MJPEG 1280x720@15fps
- 录制: 5秒 → 52帧, MJPEG原始3.4MB → MP4编码0.7MB
- SCP: 成功传输到PC (`/home/alientek/test_record.mp4`)
- 文件: 有效MP4文件 (ISO Media, MP4 Base Media v1)
- **验证通过**

**交叉编译注意事项：**
- 使用 `aarch64-linux-gnu-gcc` (Ubuntu 24.04自带)
- 需要 `-D_GNU_SOURCE` 解决kernel header兼容性问题
- 用 `gettimeofday()` 替代 `clock_gettime()` 避免跨平台问题

**待完成：**
- [x] 添加环形缓冲（参考原 recorder 的 ring_buffer）
- [x] 触发保存功能
- [x] 分段录制
- [x] 自动触发模式 (--auto)
- [x] 插TF卡后适配SD卡路径
- [ ] 代码Review和安全性检查

---

### 2026-06-16: 实现DVR引擎 + 环形缓冲区 + 自动触发模式

**目标：** 基于原 recorder 的行车记录逻辑，实现完整的环形缓冲、状态机和触发保存功能。

**实现：**
1. `dvr_engine.c/h` - 核心状态机: IDLE → BUFFERING → (COLLISION) → SAVING
   - TARGET_ON: 开始环形缓冲录制
   - WARNING/FALL/COLLISION: 保存前15s+后15s视频 (COLLISION/FALL为保护片段)
   - TARGET_OFF: 停止录制
   - 动态帧率计算: `fps = frame_count / window_sec`，解决摄像头实际帧率偏低导致的时长不准问题
   - fork子进程 + ffmpeg异步编码 (MJPEG: 临时文件方式; YUYV: 管道方式)
2. `ring_buffer.c/h` - SD卡单文件环形缓冲区
   - 异步写线程 + pending队列, 避免阻塞摄像头采集
   - 12字节帧头 [uint32_t size | int64_t timestamp_us]
   - 暂停/恢复/清除操作，编码完成后延迟清除避免竞态
3. `trigger_receiver.c/h` - 命名管道 /tmp/dvr_trigger_pipe
4. `dvr_main.c` - 命令行解析 + 信号处理

**视频时长准确性修复：**
- 摄像头标称帧率可能高于实际帧率 (如25fps实际只有15fps)
- 保存视频时动态计算实际帧率: `fps_for_ffmpeg = frame_count / window_sec`
- ffmpeg编码使用 `-fps_mode cfr` 强制恒定帧率，确保视频时长 = 窗口时长

**自动触发模式 (--auto)：**
- 通过 `--auto` 参数启动，无需命名管道
- 参数:
  - `--target-delay N`: 启动后N秒触发TARGET_ON (默认3)
  - `--collision-delay N`: TARGET_ON后N秒触发COLLISION (默认25)
  - `--auto-event TYPE`: 触发类型 warning|collision (默认collision)
- 时间线: 启动 → target_delay秒 → TARGET_ON → collision_delay秒 → COLLISION → 补录15s → 编码保存 → 自动退出
- 安全保护: collision_delay < save_before_seconds 时自动等待到至少15s，确保完整保存

**开发板部署路径变更：**
- 开发板程序目录: `/xxl/dvr/` (旧路径 `/home/root/` 需清理)
- PC视频存放目录: `/home/alientek/dvr_project/mier/dvr/`
- Makefile新增: `make deploy` (部署到开发板), `make fetch-video` (拉取视频)

**测试方式：**
- 自动模式: `./test_dvr.sh` (默认) 或 `./dvr --auto --target-delay 5 --collision-delay 25`
- 手动模式: `./test_dvr.sh -m manual` (通过命名管道触发)

**编译验证：**
- 本地gcc编译: 0错误 0警告 ✓
- aarch64-linux-gnu-gcc交叉编译: 0错误 0警告 ✓

**待验证 (上电后)：**
- [x] 端到端自动模式测试 (验证30s视频 = 15s前 + 15s后)
- [x] 视频时长正确性 (通过动态帧率)
- [x] 编码输出文件有效性
- [x] MJPEG临时文件方式编码是否正常

---

### 2026-06-16: 存储路径迁移到SD卡 — 验收通过

**背景：** 插入58GB SD卡(TF卡)，需要将缓冲区文件和视频文件从eMMC迁移到SD卡。

**方案分析：**

| 方案 | 缓冲区位置 | 视频位置 | 评估 |
|------|-----------|---------|------|
| A | eMMC | SD卡 | eMMC仅占4GB，2GB缓冲占50%，Wear Leveling空间不足；eMMC损坏不可更换 |
| B (选中) | SD卡 | SD卡 | SD卡58GB，2GB缓冲仅占3.4%，Wear Leveling空间大；SD卡损坏可更换 |
| C | RAM(tmpfs) | SD卡 | 零磨损，但38秒缓冲约2GB超出/tmp 884MB限制；需缩小缓冲区 |

**选中方案B的原因：**
1. 与原始recorder项目一致（`ring_buffer_create(config->sd_card_path, ...)`）
2. SD卡容量大，循环缓冲的磨损可分散到大量空闲块
3. SD卡可更换，eMMC是系统盘损坏代价更高
4. 不需要大幅改动代码

**修改内容：**
1. `dvr_main.c` - 默认存储路径: `/usr/local/dvr` → `/run/media/mmcblk0p1/dvr`
2. `dvr_engine.c` - `dvr_engine_create()` 中自动 `mkdir` 创建存储目录（SD卡路径可能不存在）
3. `test_dvr.sh` - `OUTPUT_DIR` 改为 `/run/media/mmcblk0p1/dvr`，Cleanup阶段 `mkdir -p`
4. `Makefile` - `BOARD_VIDEO_DIR` 改为 `/run/media/mmcblk0p1/dvr`，`fetch-video`/`distclean` 适配新路径

**测试结果：**
- 缓冲区文件: `/run/media/mmcblk0p1/dvr/dvr_buffer.bin` (SD卡)
- 视频文件: `/run/media/mmcblk0p1/dvr/emergency_20260614_001147_COLLISION.mp4` (SD卡)
- 时长: 30.000秒 (750帧 × 25fps)，精确
- 文件大小: 5.3MB
- ffprobe验证: duration=30.000000 ✓

---

### 2026-06-16: 目录结构整理

**目标：** 清理 `/home/alientek/dvr_project/mier/dvr/` 目录，源码/脚本/输出/文档分类存放。

**整理前：** 所有文件混放在根目录，包含编译产物(`*.o`, `dvr`)、旧测试代码(`simple_recorder.c`, `test_record.sh`, `test_pipe.py`)、输出文件(`.mp4`, `.log`)和源码。

**整理后：**
```
mier/dvr/
├── Makefile
├── .gitignore
├── src/          # 源码 (.c/.h)
├── scripts/      # 测试脚本
├── output/       # 视频输出、日志 (gitignore)
└── docs/         # 文档
```

**删除文件：**
- `simple_recorder.c`, `simple_recorder` — 早期简易录制，已被DVR取代
- `test_record.sh`, `test_pipe.py` — 旧测试脚本
- `*.o`, `dvr` — 编译产物

**Makefile修改：** 源码路径添加 `src/` 前缀，`deploy` 目标引用 `scripts/test_dvr.sh`，`fetch-video` 目标输出到 `output/`。

---

### 2026-06-17: NPU融合验证 — 摄像头+雷达+DVR联调

**目标：** 集成NPU摄像头目标检测，验证雷达触发是否为真实道路用户，减少雷达误触发。

**架构设计：**

```
雷达进程 (radar_link)                       DVR进程 (dvr + NPU)
─────────────────────                      ──────────────────────
  UART读取雷达数据                             持有 /dev/video6 (独占)
  │                                              │
  ├─ 目标出现 → TARGET_ON ──管道──→ 收到TARGET_ON → 启动NPU验证
  │                                    │
  ├─ TTC<10s → COLLISION ──管道──→    ├─ 每5帧解码MJPG→RGB → NPU推理
  │                                    │   连续2帧确认 → NPU_CONFIRMED
  ├─ 目标消失 → TARGET_OFF ──管道──→    │   连续3帧否认 → NPU_DENIED
  │                                    │
  │                                    ├─ 收到COLLISION
  │                                    │   NPU_CONFIRMED → 保存视频
  │                                    │   NPU_DENIED → 跳过保存
  │                                    │
  │                                    └─ 收到TARGET_OFF → 停止录制
```

**关键设计决策：**

1. **NPU在DVR侧运行**：避免雷达进程和DVR进程争抢同一个 `/dev/video6` 摄像头
2. **条件编译**：VM 编译时定义 `NO_NPU_SUPPORT`，NPU代码编译为桩（无NPU时信任雷达）
3. **连续验证机制**：避免单帧NPU误判，需连续2帧确认道路用户/连续3帧否认误触发
4. **帧解码复用**：DVR已持有MJPEG/YUYV帧，通过 `frame_decoder` 解码为RGB再送NPU
5. **每5帧推理一次**：减少NPU负载，不影响主录制帧率

**道路用户类别 (COCO数据集)：**

| 类别ID | 名称 | 说明 |
|--------|------|------|
| 1 | person | 行人 |
| 2 | bicycle | 自行车 |
| 3 | car | 汽车 |
| 4 | motorcycle | 摩托车 |
| 6 | bus | 公交车 |
| 8 | truck | 卡车 |

**新增文件：**

| 文件 | 说明 |
|------|------|
| `src/npu_detector.hpp/cpp` | 纯C++ NPU检测器，从 `ssd_mobilenet_v2` 项目剥离Qt依赖 |
| `src/npu_fusion.h/cpp` | C API封装，道路用户过滤逻辑 |
| `src/frame_decoder.h/c` | MJPEG/YUYV→RGB解码 (条件编译支持libjpeg) |

**修改文件：**

| 文件 | 改动 |
|------|------|
| `src/dvr_types.h` | 新增 `npu_model_path`, `npu_labels_path`, `npu_confidence`, `npu_enabled` |
| `src/dvr_engine.c` | 集成NPU验证状态机 + 帧解码 + 推理；`on_trigger` 增加NPU过滤 |
| `src/dvr_main.c` | 新增 `--npu-model`, `--npu-labels`, `--npu-confidence` CLI参数 |
| `Makefile` | 支持C/C++混合编译，`make`=VM无NPU，`make board`=开发板含NPU |

**编译验证：**

| 平台 | 编译命令 | 结果 |
|------|----------|------|
| VM (gcc) | `make` | 0错误 |
| aarch64交叉编译 | `make CC=aarch64-linux-gnu-gcc CXX=aarch64-linux-gnu-g++` | 0错误 |

**开发板 NPU 编译 (需板上执行)：**

```bash
# 需要有 stai_mpu 库和 libjpeg
cd /xxl/dvr
make board CC=gcc CXX=g++ HAS_LIBJPEG=1
```

**pro/ 目录变更：**

| 动作 | 文件 | 原因 |
|------|------|------|
| 删除 | `pro/fusion_radar.c` | NPU融合已移入DVR，不再需要 |
| 删除 | `pro/camera_capture.c/h` | 帧解码已移入DVR的 `frame_decoder` |
| 保留 | `pro/radar_link.c` | 纯雷达程序，UART → 管道事件 |
| 简化 | `pro/Makefile` | 只编译 `radar_link` |
| 更新 | `pro/start_pro.sh` | 自动检测NPU模型文件 |

**待验证 (上电后)：**

- [ ] NPU模型加载和推理 (需要 `stai_mpu` 库和 `.nb` 模型文件)
- [ ] MJPEG→RGB解码 + NPU 端到端
- [ ] 连续验证机制 (确认/否认)
- [ ] 雷达误触发被NPU过滤
- [ ] 雷达正确触发被NPU确认后正常保存

---

## 使用说明

### 交叉编译 + 部署（在PC上）
```bash
cd /home/alientek/dvr_project/mier/dvr

# VM编译 (无NPU)
make

# 交叉编译 (无NPU, 可部署到开发板)
make CC=aarch64-linux-gnu-gcc CXX=aarch64-linux-gnu-g++

# 部署到开发板
make deploy
```

### 在开发板上运行

```bash
ssh root@192.168.88.10
cd /xxl/dvr

# === 无NPU模式 (信任雷达直接触发) ===
./dvr -d /dev/video6 -s /run/media/mmcblk0p1/dvr

# === NPU融合模式 (摄像头AI验证雷达目标) ===
./dvr -d /dev/video6 -s /run/media/mmcblk0p1/dvr \
  --npu-model /usr/local/share/npu/ssd_mobilenet_v2_fpnlite_10_256_int8_per_tensor.nb \
  --npu-labels /usr/local/share/npu/labels_coco_dataset_80.txt

# === 自动模式 (测试用) ===
./test_dvr.sh
./dvr --auto --target-delay 5 --collision-delay 25
```

### 联调模式 (雷达 + DVR + NPU)

```bash
# 在开发板上
cd /xxl/pro
./start_pro.sh          # 启动DVR(后台) + radar_link(前台)
./start_pro.sh stop     # 停止所有
```

### 自动模式时间线
```
启动 → target_delay秒 → TARGET_ON → collision_delay秒 → COLLISION → 15s补录 → 编码 → 退出
         (默认3s)         (开始缓冲)       (默认25s)       (触发保存)   (15s后)
```

### 手动模式触发命令
```bash
echo "TARGET_ON"  > /tmp/dvr_trigger_pipe   # 开始录制
echo "WARNING"    > /tmp/dvr_trigger_pipe   # 触发保存
echo "COLLISION"  > /tmp/dvr_trigger_pipe   # 碰撞保护保存
echo "TARGET_OFF" > /tmp/dvr_trigger_pipe   # 停止录制
```

### 传输视频到PC
```bash
# 在PC上执行
scp root@192.168.88.10:/run/media/mmcblk0p1/dvr/emergency_*.mp4 /home/alientek/dvr_project/mier/dvr/

# 或使用 Makefile
make fetch-video
```