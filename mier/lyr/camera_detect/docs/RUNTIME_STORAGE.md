# 运行可靠性、日志容量与 DVR 缓存

本文记录开发板实机排查和修复结果，覆盖主进程异常恢复、日志轮转、可靠存储和
后续RAM预缓存设计。

> 当前状态：事件录像已经按项目要求恢复为外置TF主存储，CSV保留在板载
> `/usr/local/helmet` ext4；编码使用独立worker和双重整段解码。当前实现以
> [DVR_RELIABILITY.md](DVR_RELIABILITY.md)为准。

## radar_fusion 异常恢复

原 `start_dvr.sh` 在 `radar_fusion` 退出后尝试用负 PID 结束日志过滤管道，
但后台 Bash 子 shell 不一定是进程组 leader。`kill -${READER_PID}` 没有结束
`tail/grep/awk`，主脚本随后永久等待，导致：

- `dvr.service` 仍显示 `active (running)`；
- `Restart=on-failure` 不会触发；
- Dashboard 仍可访问，但 `radar_state.json` 已停止更新；
- 摄像头、NPU、雷达和 DVR 核心功能实际离线。

修复后的行为：

1. 按父 PID 结束日志过滤子进程；
2. 有界等待并回收过滤子 shell；
3. 清理 HUD 和日志维护；独立 Dashboard 不随融合业务退出；
4. 主脚本以非零状态退出；
5. systemd 根据 `Restart=on-failure` 在 5 秒后重启完整业务。

实机使用 `SIGKILL` 结束 `radar_fusion` 验证：

```text
old radar_fusion PID: 2828
exit status:          137
systemd NRestarts:    1
new start_dvr PID:    2912
new radar_fusion PID: 3047
Dashboard stale:      false
```

## 日志容量策略

### 进程文本日志

`scripts/log_maintenance.sh` 每 60 秒检查一次。因为这些文件仍被进程打开，使用
copy-truncate，轮转后原文件 inode 不变，现有文件描述符可以继续追加。

| 文件 | 单文件上限 | 历史份数 | 最大占用 |
|---|---:|---:|---:|
| `dvr_system.log` | 10 MiB | 3 | 约 40 MiB |
| `radar_dashboard.log` | 5 MiB | 3 | 约 20 MiB |
| `/tmp/hud.log` | 5 MiB | 2 | 约 15 MiB |

`/tmp/hud.log` 位于 tmpfs，不写 TF 卡，重启后丢失。

手工检查一次：

```bash
/xxl/camera_detect/scripts/log_maintenance.sh --once
```

常驻检查由 `start_dvr.sh` 自动启动和停止。

Dashboard 新版由 `radar-dashboard.service` 独立托管，输出进入有容量上限的
systemd journal，不再依赖 `dvr.service` 内的日志维护进程。旧系统使用
`start_dvr.sh` 兼容路径时仍写入并轮转 `radar_dashboard.log`。

### 板载 ext4 CSV

CSV 不能直接由外部脚本截断，否则可能丢失表头或与写入线程竞争。`radar_fusion`
和 HUD 会在自己的写入上下文中执行 `fflush → fclose → rename → reopen`。

| 文件 | 单文件上限 | 历史份数 | 最大占用 |
|---|---:|---:|---:|
| `radar_data.csv` | 20 MiB | 4 | 约 100 MiB |
| `sensor_events.csv` | 20 MiB | 4 | 约 100 MiB |
| `imu_delivery.csv` | 10 MiB | 4 | 约 50 MiB |

轮转文件使用 `.1` 到 `.4` 后缀，`.1` 最新。`labels.csv` 是人工标定结果，不做
自动轮转；`radar_state.json` 是小型状态快照，通过临时文件和 rename 持续覆盖。

MP4属于业务数据。当前worker最多保留最近12段或约2GiB，达到任一限制时删除
最旧的`emergency_*.mp4`。

## 当前TF DVR缓存

摄像头输出已经是 MJPEG，因此 `.buffer/dvr_raw.bin` 保存的是压缩 JPEG 帧，
不是未压缩 RGB。当前仅在 NPU 检测到道路目标后开始缓存，配置为 25 FPS。

代码的帧索引最多保留约90秒，允许连续左/中/右演示延长同一录像窗口。原始文件
位于外置TF，编码输出先在`/tmp`生成，通过验证后再提交到TF。

```text
逻辑索引：有界，保留最近约90秒
事件窗口：第一次风险前15秒到最后一次风险后15秒，连续事件最长60秒
```

目标长期存在仍会增加临时MJPEG写入量，因此比赛后应观察存储容量和失败恢复目录。

## RAM 容量与推荐方案

板载物理 RAM 不能通过软件真正增加。当前系统实测：

```text
Linux 可见 RAM:  1767 MiB
业务运行时可用: 约 1.5 GiB
Swap:            0
```

修改 tmpfs 大小只是在现有 RAM 中调整允许使用的上限，不会增加物理容量。通过
设备树释放 NPU、GPU 或 remoteproc reserved-memory 风险很高，本项目依赖这些
模块，不应为了录像缓存这样做。

从本次恢复的 49.3 MiB 数据中检测到约 572 个 JPEG 帧，平均约 88 KiB/帧。
按 25 FPS 估算：

```text
15 秒典型预缓存 ≈ 33 MiB
30 秒典型缓存   ≈ 65 MiB
```

画面复杂度会改变 JPEG 大小，因此不能只按秒数分配。建议后续实现：

- 只把触发前 15 秒放在 RAM；
- 使用压缩 MJPEG 帧环形队列；
- 同时限制时间为 15 秒、总字节为 256 MiB；
- 达到任一限制就丢弃最旧帧；
- 触发后立即冻结/引用前置帧，并将后 15 秒流式写入；
- 最终写入 `.partial`，完成后 `fsync` 并 rename 成正式 MP4；
- 内存不足时优先缩短预缓存，不允许触发 OOM。

256 MiB 是软件分配上限，不是启动时立即占满；队列应按实际 JPEG 大小动态增长。
RAM 高频读写不会像 TF/NAND 那样产生擦写寿命损耗，主要风险是内存压力和 OOM，
不是“写坏 RAM”。

## 验收

```bash
systemctl status dvr.service dvr-m33.service
systemctl show dvr.service -p NRestarts -p MainPID
pgrep -a radar_fusion
pgrep -af log_maintenance.sh
curl http://127.0.0.1:8080/api/state
head -1 /usr/local/helmet/radar_experiments/radar_data.csv
head -1 /usr/local/helmet/radar_experiments/sensor_events.csv
head -1 /usr/local/helmet/radar_experiments/imu_delivery.csv
/xxl/camera_detect/scripts/verify_dvr_videos.sh --full
```

`dvr.service=active` 不能单独证明业务正常；必须同时确认 `radar_fusion` 存在且
Dashboard 返回 `stale=false`。
