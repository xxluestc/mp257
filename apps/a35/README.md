# A35 Linux 应用

STM32MP257 骑行辅助系统的 Linux 业务工程，负责雷达视觉融合、事件录像、导航音频、HUD 转发、Dashboard 与应用 OTA。生产程序为 radar_fusion，入口在 `src/app/main.cpp`。

Camera、NPU、Radar、DVR 分线程处理。Camera 把 V4L2 JPEG 复制进 RAM 帧池，DVR 持续维护最近 15 秒 ring；触发后流式编码前段与后段，TF 保存最终视频。Fusion 统一维护风险状态。

## 工程分布

```text
apps/a35/
├── include/                 # 自有接口、队列、帧池与融合状态
├── src/
│   ├── app/                 # 入口、配置与进程状态
│   ├── camera/              # V4L2 C 接口
│   ├── vision/              # JPEG、STAI 与 SSD 后处理
│   ├── radar/               # BSD、方向滤波与接收线程
│   ├── runtime/             # 流水线与各工作线程
│   ├── output/              # BLE 方向灯输出
│   ├── events/              # RPMsg 接收与转发
│   ├── navigation/          # 导航、OLED 与有界音频队列
│   ├── platform/            # GPIO、串口基础操作
│   └── telemetry/           # CSV、状态与存储初始化
├── tests/                   # 资源、状态和编码流行为测试
├── hud/                     # IMU 事件转发、共用 OLED 接口与固定版本 cJSON
├── dashboard/               # Web 状态服务
├── ota/                     # 包构建、安装与回滚
├── scripts/                 # 编码、启停与维护工具
├── board/                   # DTS 与板级资料
├── models/、stai_mpu/      # 模型、标签与厂商运行库
├── sounds/、nav_tts_cache/ # 本地音频
├── docs/                    # 模块与维护资料
└── dist/                    # 明确发布的历史包
```

## 构建与检查

Linux 主机需要 GNU AArch64 工具链、make、Python 3，可按 OpenSTLinux SDK 覆盖编译器：

```bash
cd apps/a35
make CC=aarch64-linux-gnu-gcc CXX=aarch64-linux-gnu-g++
make core-check runtime-check lifecycle-check json-check encoder-check dashboard-check script-check ota-check
make object-check CC=gcc CXX=g++
python3 tools/check_format.py
```

make 构建主业务与 hud/hud，对象与依赖进入 build。C 使用 C11，C++ 使用 C++17，自有源码按 clang-format 18.1.8 检查。STAI、cJSON 与固件厂商代码独立保留。

导航 UDP 8888 与 OLED 由 `radar_fusion` 中的导航模块独占；独立 `hud` 只监听 `127.0.0.1:8890` 并向手机转发 IMU JSON。`WorkerGroup` 统一管理主应用工作线程，`VideoPipeline` 的生命周期接口由 Main 调用且只启动一次。配置在帧池分配前完成校验；停机取消队列中尚未处理的数据并释放引用。

NPU 后处理与 STAI 资源封装分别维护，类别内执行 NMS，异常分数/坐标在输出前过滤。cJSON 来源和校验值见 [第三方说明](hud/THIRD_PARTY.md)。

板端录像需要 ffmpeg/ffprobe。硬件编码另需 Python GI、GStreamer 与 BSP H.264 插件，自动选择结果记入日志。默认 JPEG 池为 224 MiB，单帧上限 256 KiB；总内存还包含 NPU、RGB、V4L2 和编码器，见[流水线设计](../../docs/VIDEO_PIPELINE_DESIGN.md)。

## 运行与交付

板端目录继续使用 /xxl/camera_detect，可执行文件与 systemd 名称保持原部署接口。源码路径同步到了 Makefile、OTA 和文档。

```bash
make deploy-runtime BOARD_IP=192.168.88.10
make ota-package VERSION=<新的发布版本>
```

部署命令会写入开发板并重启应用，按需要手动执行。dist 中的 1.0.8 包保持历史内容与 SHA-256，不包含当前改造，应以新版本构建、验收再交付。

## 资料入口

- [当前应用链路](docs/DATA_FLOW.md)
- [线程与 RAM 缓存](../../docs/VIDEO_PIPELINE_DESIGN.md)
- [代码约定](../../docs/CODING_CONVENTIONS.md)
- [工程分布](../../docs/REPOSITORY_LAYOUT.md)
- [DVR 边界](docs/DVR_RELIABILITY.md)
- [蓝牙方向灯](docs/BLUETOOTH_DIRECTION_LED.md)
- [RPMsg 摔倒链路](docs/FALL_SMS_PIPELINE.md)
- [OTA](docs/OTA.md)
- [板端操作](docs/BOARD_PROGRAMS_AND_OPERATIONS.md)
