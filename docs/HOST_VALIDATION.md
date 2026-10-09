# 本次重构的主机检查

日期：2026-10-09。对应本次提交中的 `apps/a35` 源码与工程分布。

## 已完成

| 检查 | 结果和范围 |
|---|---|
| C/C++ 格式 | clang-format 18.1.8，49 个自有源码文件通过；检查末尾换行。厂商 STAI 头文件和上游 cJSON 不参与统一格式。 |
| AArch64 Linux 编译 | 使用 Zig 0.13.0 所带 Clang，20 个 `src` 源文件与 5 个 HUD 源文件编译为目标对象；C11/C++17，`-Wall -Wextra -Wpedantic -Werror`，SDK 头文件作为系统头文件。 |
| HUD | `main.c`、`udp.c`、`imu_message.c`、`cJSON.c` 完成 AArch64 Linux 编译与链接。OLED 由主应用使用，其源码另行通过严格目标编译。 |
| 帧与并发行为 | Windows 原生 C++ 检查通过：帧引用回收、池对象生命周期、队列满载策略、关闭与取消、并发传递、ring 时间范围快照、事件窗口上限、延迟触发和视觉状态新鲜度。 |
| C++ 资源与边界 | 线程组部分启动失败时的回收、fd 移动与释放、重复启停保护、退出清空帧引用、配置校验先于大内存分配、路径边界、RPMsg 字段及超长行处理、按类别 NMS 均通过检查。生命周期测试连接生产 `VideoPipeline`，设备工作函数由测试替身提供。 |
| JSON 与 IMU 转发 | 检查转义字符、Unicode、非法输入与嵌套上限；生产 `imu_message_to_json` 保留 GPS、短信等原字段并更新来源与时间。上游 cJSON 文件与记录的 SHA-256 一致。 |
| DVR | 12 项 Python 测试通过：分片读取、流边界、强制结束标记、中断事件、TF 挂载、临时文件独占、清理异常、提交后的维护异常、容器尾部与保留策略；真实 FFmpeg 测试覆盖 MJPEG 生成、MP4 编码、容器检查和完整解码。 |
| Dashboard | 9 项测试通过，包含雷达不可用时的新鲜心跳不能被解释为正常数据，以及不同启动记录的确定性选择。 |
| 脚本 | `scripts`、`dashboard`、`ota`、`tools`、`tests` 中的 Python 模块及应用 Shell 脚本通过语法检查。 |
| 工程维护 | 同步构建依赖、持续检查入口、线程与端口职责文档；保留第三方来源和许可证，检查 Git 空白错误。 |

## 持续检查入口

仓库的 [Source quality 工作流](../.github/workflows/code-quality.yml) 配置了格式、Shell/Python、Dashboard、帧队列、C++ 资源、管线生命周期、JSON、DVR 及 Linux 对象编译检查。Linux CI 安装 `ffmpeg` 和 `ffprobe`，DVR 集成测试在两者均可用时执行完整的生产校验函数。本地主机的真实视频测试覆盖编码、容器与完整解码；`ffprobe` 分支由配置了该工具的检查环境执行。

在 `apps/a35` 中运行：

```bash
python3 tools/check_format.py
make core-check runtime-check lifecycle-check json-check encoder-check
make dashboard-check script-check ota-check
make object-check CC=gcc CXX=g++
make -C hud CC=gcc
```

CI 配置已更新，其远端执行结果需以 GitHub Actions 为准。
