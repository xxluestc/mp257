# 本次重构的主机检查

日期：2026-10-04。对应本次提交中的 `apps/a35` 源码与工程分布。

## 已完成

| 检查 | 结果和范围 |
|---|---|
| C/C++ 格式 | clang-format 18.1.8，37 个自有源码文件通过；检查末尾换行。厂商 STAI 头文件和上游 cJSON 不参与统一格式。 |
| AArch64 Linux 编译 | 使用 Zig 0.13.0 所带 Clang，18 个 `src` 源文件编译为目标对象；C11/C++17，`-Wall -Wextra -Wpedantic -Werror`，SDK 头文件作为系统头文件。 |
| HUD | OLED 源文件通过严格目标编译；HUD 的 4 个 C 文件完成 AArch64 Linux 编译与链接。 |
| 帧与并发行为 | Windows 原生 C++ 检查通过：帧引用回收、共享内容不被覆盖、池对象生命周期、队列满载策略、关闭唤醒、并发传递、ring 快照、事件窗口上限及视觉状态新鲜度。 |
| DVR | 7 项 Python 测试通过：记录分片读取、格式与边界校验、强制结束标记、中断事件不可提交、TF 挂载检查，以及真实 FFmpeg 生成 MJPEG、编码 MP4、检查容器并完整解码。 |
| Dashboard | 9 项测试通过，包含雷达不可用时的新鲜心跳不能被解释为正常数据，以及不同启动记录的确定性选择。 |
| 脚本 | Python 模块语法检查及 17 个 Shell 文件语法检查通过。 |
| 工程迁移 | 核对已跟踪文件的迁移范围，保持模型、音频、厂商库、固件源文件和历史发布包内容；检查文档中的本地文件链接与 Git 空白错误。 |

## 持续检查入口

仓库的 [Source quality 工作流](../.github/workflows/code-quality.yml) 配置了格式、Shell/Python、Dashboard、帧队列、DVR 及 Linux 对象编译检查。Linux CI 安装 `ffmpeg` 和 `ffprobe`，DVR 集成测试会在两者均可用时执行完整的生产校验函数。本次本地主机没有 `ffprobe` 可执行文件，因此本地真实视频测试覆盖编码、容器与完整解码，未执行该集成用例的 `ffprobe` 分支。

CI 配置已更新，其远端执行结果需以 GitHub Actions 为准。
