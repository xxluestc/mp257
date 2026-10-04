# DVR 提交与故障边界

当前录像链为 RAM JPEG 池 → 15 秒 ring → 有界事件队列 → Encoder 管道 → 工作进程 → TF MP4。详见[应用设计](../../../docs/VIDEO_PIPELINE_DESIGN.md)。

编码输入有完整事件结束标记。数据截断、半帧、非法长度、时间戳倒退、超长事件或事件队列溢出均使该录像失败；完整结束后才验证 MP4 box、ffprobe 与整段解码，再 fsync、rename 和目录 fsync。正式文件按 12 个 / 2 GiB 轮转，失败诊断日志最多保留 36 个。

TF 不承载持续预录 JPEG，编码中的文件为 emergency_*.mp4.part。挂载不存在或编码器忙时，DVR 明确拒绝新事件，RAM ring 继续滚动，风险提醒继续工作。Camera/NPU 等待与重连不会占用 Fusion 线程。

```bash
scripts/verify_dvr_videos.sh --full
```

实际选项以脚本帮助为准。只有正式 MP4 且完整验证通过才报告保存成功。RAM 帧在退出或掉电时丢失；本次没有实现断电恢复原始录像。进行中的事件在退出时中止，编码进程组有超时清理。

历史版本的 TF 原始缓冲和修复记录可从 Git 历史与启动记录查找。1.0.8 包保持历史内容，不包含当前实现。
