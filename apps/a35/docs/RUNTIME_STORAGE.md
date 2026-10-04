# 当前运行存储

JPEG 预录与事件帧留在 RAM。默认帧池 896 槽 × 256 KiB = 224 MiB，ring 最多 375 帧 / 15 秒，事件输入最多 439 帧。编码消费后及时释放引用，持续事件不增长成无限原始帧文件。

JPEG 池预算之外，RGB 工作区约 25.3 MiB，另有 V4L2 MMAP、STAI 模型与运行库、编码器、进程与系统内存。改变 --pool-mib / --max-jpeg-kib 时同时校验槽数，不能用平均 JPEG 大小代替上限预算。超限与池满计入 video_counters。

TF 保存 emergency_*.mp4.part、已提交 emergency_*.mp4 与编码诊断日志。正式视频按 12 个 / 2 GiB 轮转；诊断日志最多 36 个。雷达与传感器 CSV 保留现有 20 MiB × 当前文件及 4 个备份策略，尚未就绪的传感器事件使用容量 128 的 RAM 队列。

板端配置、日志与录像不进入源码仓库。源码调整不改变 /xxl/camera_detect 安装接口。帧池、队列与提交细节见[流水线设计](../../../docs/VIDEO_PIPELINE_DESIGN.md)，检查记录见[主机报告](../../../docs/HOST_VALIDATION.md)。
