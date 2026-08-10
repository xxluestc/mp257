# DVR录像可靠性与TF主存储

## 1. 当前存储策略

事件录像必须保存到外置TF卡，不写板载`/usr/local/helmet/dvr`：

```text
整盘文件系统：/dev/mmcblk0   -> /run/media/mmcblk0/dvr/
带首分区：    /dev/mmcblk0p1 -> /run/media/mmcblk0p1/dvr/
```

`start_dvr.sh`调用`scripts/tf_card_control.sh`自动识别两种布局，并把实际挂载点通过
`--dvr-mount-dir`和`--dvr-dir`传给`radar_fusion`。路径不是由C++猜测，也不依赖
某一种固定分区形式。

雷达、传感器和人工标注CSV仍保存到板载：

```text
/usr/local/helmet/radar_experiments/
```

这样人工安全弹出TF只影响录像业务，不会带走尚在分析的实验标注。

## 2. 为什么不会误写根文件系统

仅判断目录存在是不安全的：TF未挂载时，`/run/media/mmcblk0p1`也可能只是根文件系统
里的普通空目录。当前链路有三道检查：

1. 启动脚本确认块设备存在并完成真实挂载；
2. 在`<TF挂载点>/dvr`创建并同步写入探测文件，失败则`dvr.service`退出；
3. `radar_fusion`比较挂载点和父目录的`st_dev`，每次新建录像缓冲前重新检查。

因此TF缺失、未挂载、只读或已被物理拔出时，不会悄悄写入同名根目录。服务会失败并
由systemd重试，日志中可看到明确原因。

## 3. 触发和提交条件

碰撞录像要求现有融合逻辑最终成立：

```text
摄像头可用 + 雷达危险 + NPU确认道路目标 -> 触发事件录像
```

摔倒事件也触发事件录像。RPMsg线程只投递原子事件，所有DVR文件状态统一由主线程
处理，避免摔倒与摄像头写帧或雷达触发同时操作同一缓冲文件。

第一次风险前最多保留15秒，最后一次风险后继续15秒；连续LEFT/CENTER/RIGHT可合并
到同一段录像，连续事件窗口最长60秒。LED闪烁只说明告警成立，正式文件必须以以下
日志为准：

```text
[DVR-WORKER] VALIDATED <TF挂载点>/dvr/emergency_....mp4
[保存] [DVR] Encoder finished (exit=0)
```

## 4. 编码和原子提交

```text
TF/.buffer/dvr_raw.bin
  -> /tmp提取JPEG
  -> /tmp编码MP4
  -> 检查ftyp/mdat/moov
  -> ffprobe
  -> ffmpeg整段解码
  -> 顺序复制到TF/dvr/*.mp4.part
  -> 再次结构检查、ffprobe和整段解码
  -> fsync并原子改名为emergency_*.mp4
```

任何步骤失败都不会把`.part`冒充正式录像。原始帧和诊断日志保留在：

```text
<TF挂载点>/dvr/.buffer.failed_<pid>/
```

容量策略：最多保留最近12段正式录像，合计最多约2GiB，超限时先删除最旧录像。

## 5. 安全停止和TF弹出

`radar_fusion`收到SIGTERM后会关闭原始缓存、编码并等待worker完成。启动脚本允许最长
120秒，`dvr.service`的`TimeoutStopSec`为150秒，避免过去3秒强杀导致的录像丢失。

Dashboard“安全弹出TF”按以下顺序执行：

```text
停止dvr.service -> 等待编码完成 -> sync -> umount -> 返回成功
```

命令行等价操作：

```bash
/xxl/camera_detect/scripts/project_safe_stop.sh stop
/xxl/camera_detect/scripts/tf_card_control.sh eject
```

直接执行`tf_card_control.sh eject`时，如果`dvr.service`仍在运行，脚本会拒绝卸载。
不要直接拔卡，也不要在`.buffer`或`.part`存在时切断电源。

## 6. 板端检查

```bash
/xxl/camera_detect/scripts/tf_card_control.sh status
systemctl is-active dvr.service radar-dashboard.service
grep -E 'TF 录像主存储|Save triggered|DVR-WORKER|Encoder finished' \
  /xxl/camera_detect/dvr_system.log | tail -100
/xxl/camera_detect/scripts/verify_dvr_videos.sh --full
```

查询实际录像目录：

```bash
TF_MOUNT=$(/xxl/camera_detect/scripts/tf_card_control.sh status |
  sed -n 's/.* mount=\([^ ]*\).*/\1/p')
ls -lht "$TF_MOUNT/dvr"
```

复制到虚拟机：

```bash
mkdir -p /home/alientek/dvr_project/mier/dvr_videos
scp 'root@192.168.88.10:/run/media/mmcblk0/dvr/*.mp4' \
  /home/alientek/dvr_project/mier/dvr_videos/
```

如果`status`显示`mmcblk0p1`，将命令中的挂载目录相应改成
`/run/media/mmcblk0p1/dvr`。

## 7. TF卡验收

比赛卡不能只看剩余容量。至少执行：

1. `fsck.fat -n`或与文件系统对应的只读检查；
2. 写入大文件并记录SHA-256；
3. `sync`、安全卸载、重新挂载；
4. 重新计算SHA-256，至少重复三轮；
5. 检查`dmesg`无MMC、超时或I/O错误；
6. 完成一次真实触发，等待`VALIDATED`，再执行`verify_dvr_videos.sh --full`。

2026-08-09曾发现一张旧卡在线读取正常但重挂后内容变化，因此当时临时迁移到板载
ext4。后续新卡已通过多轮重挂和哈希检查。当前需求已经明确恢复TF主存储；旧卡故障
经验保留为验收标准，而不是改变录像保存位置的理由。
