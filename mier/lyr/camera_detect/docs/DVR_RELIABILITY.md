# DVR录像可靠性、存储与比赛检查

## 1. 当前结论

比赛录像和雷达/传感器日志使用板载`userfs` ext4，不再写入外置TF卡：

```text
/usr/local/helmet/dvr/                 # emergency_*.mp4
/usr/local/helmet/radar_experiments/   # CSV、labels、radar_state.json
```

当前这张`/dev/mmcblk0p1` TF卡不能用于比赛数据。实测同一文件在写入后可读取、
哈希正确，但卸载并重新挂载后内容和哈希发生变化，MP4丢失`moov`。`fsync`、FAT
修复以及`sync,flush`挂载均不能解决，属于介质/卡接口持久写入不可靠。
2026-08-09已通过Dashboard维护接口安全卸载，当前业务在TF未挂载时正常运行。
`start_dvr.sh`还会识别旧版本现场配置中的TF日志路径，并仅把运行时CSV目录迁移到
板载ext4，防止OTA继承旧配置后重新写回故障卡或未挂载目录。

## 2. 故障根因与已修复的软件缺陷

本次排查确认了三个相互叠加的问题：

1. 旧代码只检查编码器退出码为0且文件非空，随机数据也会被误报为“保存成功”；
2. 多线程`radar_fusion`在`fork()`后没有立即`exec()`，而是在子进程继续执行
   malloc、stdio、JPEG提取和GStreamer，属于未定义行为；
3. 当前TF卡写入结果只在页缓存中暂时正确，卸载重挂后介质内容改变。

现在由`radar_fusion`关闭原始缓冲、写入帧索引任务，然后用`posix_spawn`直接
启动全新的`dvr_encode_worker.py`。worker执行：

```text
原始MJPEG缓冲
  → /tmp提取JPEG
  → /tmp编码MP4
  → 检查ftyp/mdat/moov
  → ffprobe
  → ffmpeg整段解码
  → 顺序复制到/usr/local/helmet/dvr/*.mp4.part
  → 再次检查结构、ffprobe和整段解码
  → 原子改名为正式emergency_*.mp4
```

只有全部通过才打印：

```text
[DVR-WORKER] VALIDATED /usr/local/helmet/dvr/emergency_....mp4
[保存] [DVR] Encoder finished (exit=0)
```

失败时不会播放保存完成提示，也不会留下正式`.mp4`；原始帧、JPEG和日志保存在：

```text
/usr/local/helmet/dvr/.buffer.failed_<pid>/
```

## 3. 连续左/中/右比赛演示

第一次风险触发后继续保存15秒。该窗口内的新风险会延长录像结束时间，最长允许
连续演示60秒，因此LEFT、CENTER、RIGHT可以放在同一段录像中，相互重叠不会被
第一个事件吞掉。最终录像范围为：

```text
第一次风险前15秒 → 最后一次风险后15秒
```

编码完成之前不要停止服务或断电。以`DVR-WORKER VALIDATED`作为真正完成标志，
不能只看“Save triggered”或文件大小。

对于隔开较长时间的事件，worker结束后主进程会清除触发状态并重新武装。下一次
风险会重新建立缓冲并生成独立MP4。测试参数可在维护时模拟该过程：

```bash
./radar_fusion -t 5 --test-fall-count 2 --test-fall-interval 50 --no-ble-led
```

这些参数仅用于无实测环境时的录像回归，正常`dvr.service`不传入它们。

## 4. 容量策略

`/usr/local`来自板载`/dev/mmcblk1p9`，容量约4GB，当前可用约3.5GB。worker自动
管理`emergency_*.mp4`：

- 最多保留最近12段；
- 正式录像合计最多约2GiB；
- 任一条件超限时先删除最旧的正式事件录像；
- `.part`永远不算正式录像；失败恢复目录需人工确认后清理。

## 5. 比赛前检查

```bash
systemctl is-active dvr.service radar-dashboard.service
pgrep -a radar_fusion
df -h /usr/local /tmp
ls -lht /usr/local/helmet/dvr
find /usr/local/helmet/dvr -maxdepth 1 \
  \( -name '*.part' -o -name '.buffer.failed_*' \) -print
```

快速检查全部正式录像：

```bash
/xxl/camera_detect/scripts/verify_dvr_videos.sh
```

比赛前至少执行一次整段解码：

```bash
/xxl/camera_detect/scripts/verify_dvr_videos.sh --full
```

一次真实触发后应看到`VALIDATED`，再运行`--full`。任何失败、`.part`或
`.buffer.failed_*`都视为录像链路未通过，不能开始比赛演示。

## 6. 复制到虚拟机

```bash
mkdir -p /home/alientek/dvr_project/mier/dvr_videos
scp 'root@192.168.88.10:/usr/local/helmet/dvr/*.mp4' \
  /home/alientek/dvr_project/mier/dvr_videos/
```

复制后对比开发板和虚拟机的`sha256sum`。

## 7. 更换TF卡后的验收

新TF卡不能只做一次在线读写。必须先备份、格式化并完成以下验收：

1. 写入至少1GB可校验测试数据；
2. `sync`并安全卸载；
3. 重新插入/挂载；
4. 全量SHA-256必须一致；
5. 重复至少三轮并检查`dmesg`无MMC/I/O错误。

即使新卡通过，默认仍建议板载ext4作为比赛主存储，TF只作为赛后导出或冗余副本。

## 8. 2026-08-09板端回归证据

最终端到端测试文件：

```text
/usr/local/helmet/dvr/emergency_20260727_111719_389.mp4
H.264, 1280x720, 20fps, 15.05s, 45,047,560 bytes
SHA-256: f92949eecb6465d4b030f921647c3050ece94949090b61ac0436c21ab99c6c78
```

验证结果：worker双重整段解码通过；卸载并重新挂载`userfs`后SHA-256不变；
`fsck.ext4 -fn /dev/mmcblk1p9`返回0；重挂后再次整段解码返回0。

同一`radar_fusion`进程的双轮间隔触发回归：

```text
emergency_20260727_113950_320.mp4  H.264 1280x720 14.95s
SHA-256 71c05a8bcaec1f2e91fce2af95b175bf21d5ecd5a86997d73f5709c9000f5b56

emergency_20260727_114040_363.mp4  H.264 1280x720 14.95s
SHA-256 41d4725be927a9a4da80cfd985cdff97ab85381b23d313afc32a6e00d9fcbf21
```

两次触发相隔50秒，日志均出现`VALIDATED`、`Encoder finished (exit=0)`和
`State reset, ready for next trigger`。三段现存正式录像执行`verify_dvr_videos.sh
--full`结果为`检查=3 失败=0 未提交part=0 恢复目录=0`。

## 9. 安全停止与异常断电

不要在仍有`.buffer`或`.part`时直接断电。Dashboard“设备运维”可安全停止项目或
安全关机；命令行等价操作为：

```bash
/xxl/camera_detect/scripts/project_safe_stop.sh stop
/xxl/camera_detect/scripts/project_safe_stop.sh poweroff
```

直接切断电源可能丢失正在写入的临时缓冲或CSV。已经原子提交并通过整段解码的正式
MP4风险较低，但比赛仍应使用有序关机。
