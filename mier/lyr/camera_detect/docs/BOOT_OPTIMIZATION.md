# STM32MP257 启动优化与回退

本文记录 STM32MP257 骑行辅助项目的一次完整启动优化，既用于后续维护，也用于
理解 Linux 启动链路、systemd 依赖、remoteproc/RPMsg 和嵌入式系统稳定性取舍。
文中的数据均来自开发板冷启动实测，不是理论估算。

> 2026-08-09状态更新：本文中的TF/fsck数据保留为启动优化和故障排查历史。
> 当前`start_dvr.sh`不再自动挂载TF，录像和CSV使用板载`/usr/local/helmet`
> userfs/ext4；TF不再位于业务关键链。当前存储结论见
> [DVR录像可靠性](DVR_RELIABILITY.md)。

相关实现：

- [`dvr-m33.service.example`](../dvr-m33.service.example)：M33 早期启动单元
- [`start_m33_early.sh`](../scripts/start_m33_early.sh)：M33 remoteproc 启动与状态确认
- [`dvr.service.example`](../dvr.service.example)：主业务 systemd 单元
- [`start_dvr.sh`](../start_dvr.sh)：设备、板载存储、M33 和业务进程的启动编排
- [`boot_optimize.sh`](../scripts/boot_optimize.sh)：可重复执行、可回退的优化脚本
- [`collect_boot_metrics.sh`](../scripts/collect_boot_metrics.sh)：单调时钟和健康状态采集
- [`dnsmasq.service.override.example`](../dnsmasq.service.override.example)：AP/DHCP 精确依赖
- [`helmet_ota_recover_if_needed.sh`](../ota/helmet_ota_recover_if_needed.sh)：OTA 恢复快速检查

## 优化目标与约束

目标不是单纯追求最短的 `systemd-analyze` 数字，而是在保证摄像头、NPU、雷达、
M33、RPMsg、LED、音频、WiFi AP、Dashboard 和 DVR 数据完整的前提下：

1. 找出真正阻塞启动的服务；
2. 将 M33 和主业务尽可能前移；
3. 停用产品不需要的通用发行版服务；
4. 为所有改动保留回退路径；
5. 用多次冷启动和业务功能检查证明优化有效。

本次明确不做大规模代码重构，不直接裁剪内核或设备树，也不保留无法证明可靠的
U-Boot 启动 M33 方案。对于 fsck、udev 等涉及数据和硬件枚举的步骤，稳定性
优先于少量时间收益。

## 优化前基线

开发板实测：

```text
kernel:                2.212 s
userspace:             1 min 40.792 s
multi-user.target:     1 min 40.733 s
```

主要慢项：

| 项目 | 耗时 | 结论 |
|---|---:|---|
| `snmpd.service` | 89.922 s | 启动超时并失败，项目不使用 |
| `snmptrapd.service` | 89.788 s | 启动超时并失败，项目不使用 |
| TF 卡 fsck | 20.707 s | 已发现 FAT 损坏项；数据安全相关，不能禁用或绕过 |
| Weston 图形会话 | 7.455 s | 项目使用 Web Dashboard/OLED，不使用本地桌面 |
| udev settle | 5.672 s | 摄像头、串口等设备节点需要，保留 |
| network wait online | 2.663 s | 首阶段保留，避免影响 WiFi AP/DHCP |

原业务在开机约 12.7 秒进入 `start_dvr.sh`，脚本再次停止、启动 M33，并串行
等待 HUD 和 Dashboard，主程序约 24.4 秒进入运行状态。

审计还发现旧脚本可能在 `systemd-fsck@dev-mmcblk0p1.service` 运行期间手工
挂载 TF 卡，导致 DVR 与 fsck 并发修改 FAT。新脚本会等待 fsck/自动挂载结束，
只有确认 fsck 不活动时才允许手工挂载。若卡需要较长时间修复，业务就绪时间会
相应延后；这是必要的数据安全等待，不应通过禁用 fsck 换取表面启动速度。

## 优化过程复盘

### 1. 建立基线，而不是凭感觉优化

首先用下面的命令分别观察总时间、服务耗时、关键链和单调时钟日志：

```bash
systemd-analyze time
systemd-analyze blame --no-pager
systemd-analyze critical-chain
journalctl -b -o short-monotonic --no-pager
```

`blame` 表示单个单元从启动到完成所花的时间，但不能直接相加。例如
`snmpd` 和 `snmptrapd` 都接近 90 秒，但二者并行等待，所以整机增加约 90 秒，
不是 180 秒。判断真实阻塞链时必须结合 `critical-chain` 和单调时钟日志。

业务“可用时间”也不能只看 `multi-user.target`。本项目另外记录 M33 running、
`radar_fusion` ready、Dashboard API 可访问等业务里程碑，避免出现系统显示启动
完成、实际功能仍不可用的假优化。

### 2. 对服务做分类

服务按以下三类处理：

- 核心依赖：WiFi AP、ALSA、udev、TF 卡挂载、remoteproc/RPMsg 等，保留；
- 产品无关：SNMP、Weston、Netdata、Bluetooth、Avahi 等，禁用；
- 暂不确定：网络在线等待、完整 udev settle、板级 EEPROM 等，先保留并继续观察。

禁用服务前先检查业务代码、systemd 依赖和运行日志是否引用它；无法证明安全的
单元不因为“看起来没用”就删除。这里选择 `disable` 而不是 `mask`，便于现场
诊断时临时启动，也能通过优化脚本恢复原状态。

### 3. 确认 M33 实际由谁启动

不能因为系统里存在 `st-m33firmware-load.service` 就认定 M33 已在早期启动。
实际检查了服务脚本、设备树 compatible、remoteproc 状态和启动日志，发现厂商
服务因板名不匹配直接退出；真正启动项目固件的是稍后的 `start_dvr.sh`。

这一步说明，排查启动问题要验证“实际执行路径”，不能只看文件名或单元状态。

### 4. 对 U-Boot 方案做有限、可回退的实机试验

现有 `boot.scr.uimg` 支持发现 `/boot/rproc-m4-fw.elf` 后调用
`rproc init/load/start`，而且失败后仍会继续启动 Linux，因此具备有限试验条件。
将固件放入 bootfs 冷启动后，Linux 仍记录自己执行 `powering up m33`，并且内核
报告缺少 detach mailbox，无法证明 U-Boot 启动的 M33 能被 Linux 安全接管。

因此立即撤回测试文件，不修改 `bootcmd`，也不把“偶尔能启动”当成可交付方案。
试验文件被转移到板端优化备份目录，保留问题现场和回退能力。

### 5. 在 Linux sysinit 阶段提前启动 M33

新增 `dvr-m33.service`，使用 `Type=oneshot`，在 remoteproc 驱动可用后运行
`start_m33_early.sh`。脚本等待 remoteproc sysfs 节点出现，调用原厂固件脚本，
并检查状态是否变为 `running`。随后由 `start_dvr.sh` 等待 RPMsg 设备出现，
将“核已运行”和“通信通道已就绪”分成两层验证。

主业务启动时先检查 M33 和 RPMsg：

- 若早期启动成功，直接复用，不再停止、重启 M33；
- 若早期启动失败，回退到原 `fw_cortex_m33.sh` 启动路径；
- 若最终仍失败，记录明确日志，便于定位 remoteproc、固件或 RPMsg 问题。

启动关系可以概括为：

```text
remoteproc 驱动就绪
        |
        v
dvr-m33.service --成功--> M33 running
        |                         |
        |失败                     v
        +-----------------> start_dvr.sh 检查
                                  |
                       等待 RPMsg / 原路径回退启动
                                  |
                                  v
                         radar_fusion 等业务进程
```

### 6. 调整主业务依赖，但不破坏设备前提

`dvr.service` 被前移到业务所需设备和基础服务就绪之后。没有简单删除所有
`After=`，而是在摄像头、雷达串口、RPMsg、网络和 TF 卡之间维持必要顺序。
`start_dvr.sh` 对关键设备节点继续做有界等待，使“服务调度得早”不会变成
“程序过早失败”。

### 7. 修复 TF 卡检查与业务挂载并发

首次优化重启时发现 TF 卡 fsck 明显变慢。继续检查后确认，旧脚本可能在
`systemd-fsck@dev-mmcblk0p1.service` 仍运行时手工挂载分区，造成两个写入者
并发操作 FAT。

修复时没有禁用 fsck，而是：

1. 修改脚本，仅在 fsck 的 `ActiveState=activating` 时等待；
2. 先备份正式 DVR 数据，并排除可重建的 `.buffer` 临时缓存；
3. 卸载分区后离线执行 fsck；
4. 仅丢弃严重损坏的临时缓存目录；
5. 再次以只读检查确认文件系统无剩余错误。

这里有一个容易踩到的 systemd 细节：fsck 单元完成后可能保持
`active (exited)`，所以只用 `systemctl is-active` 判断会永远等待。应读取
`ActiveState`/`SubState`，只等待真正处于执行中的状态。

### 8. 冷启动验证和回归检查

每次重要修改后都执行完整断电或重启验证，而不是只手工重启单个服务。验收同时
覆盖：

- systemd 总时间、关键链和失败单元；
- M33 remoteproc 状态和 RPMsg 节点；
- 摄像头、雷达串口、GPIO 等设备节点；
- WiFi AP、DHCP 和手机访问 Dashboard；
- Dashboard 数据是否新鲜；
- TF 卡正式录像、CSV 和实验数据是否保留。

第一阶段结果从 103.005 秒缩短到 13.572 秒，M33 提前到 4.42 秒运行，且保留了
失败回退和原始状态恢复方式。

## M33 启动结论与实机验证

原状态不是 U-Boot 启动。系统自带 `st-m33firmware-load.service` 虽在开机约
11.3 秒执行，但它只识别 ST 官方板名，当前设备树首个 compatible 是
`myir,myb-stm32mp257x`，因此默认脚本直接退出。真正的
`project_CM33_NonSecure.elf` 由 `start_dvr.sh` 在 Linux userspace 启动。

当前 U-Boot 的 `boot.scr.uimg` 虽内置以下路径：

```text
发现 bootfs 根目录 /rproc-m4-fw.elf
  → load 到 m4fw_addr
  → rproc init/load/start
  → 无论文件不存在或启动失败，继续正常启动 Linux
```

实机放入固件并重启后，Linux 仍在 11.5 秒才执行 `powering up m33`，没有形成
可验证的 U-Boot→Linux 接管。当前设备树也没有 detach mailbox，内核明确记录：

```text
remoteproc remoteproc0: cannot get detach mbox
```

因此已撤回 `/boot/rproc-m4-fw.elf`，不修改 `bootcmd`，也不把未经验证的
U-Boot 方案留在产品中。

最终方案是新增 `dvr-m33.service`，在 Linux `sysinit.target`、remoteproc
驱动出现后立即启动项目固件；`start_dvr.sh` 检测到 M33 与 RPMsg 正常后直接
复用。若早期服务失败，业务脚本仍自动回退到原有 Linux remoteproc 启动方式。

## 第一阶段改动

执行：

```bash
/xxl/camera_detect/scripts/boot_optimize.sh apply
reboot
```

该脚本会保存原状态到 `/etc/dvr-boot-optimization/`，安装早期
`dvr-m33.service` 和提前启动的 `dvr.service`，并禁用以下与当前骑行辅助
产品无关的服务：

- SNMP、Netdata、Sysstat
- Weston、本地图形 HMI、启动画面
- Bluetooth（手机链路使用 WiFi AP）
- Avahi、RPC、TCF 调试代理
- TSN/MSTP
- kdump

保留：

- `hostapd`、`dnsmasq`、`systemd-networkd`：手机 WiFi AP
- `iptables/ebtables`：网络配置
- `alsa-state-stm32mp`：声音
- `mount-partitions` 和 TF 卡 fsck：DVR/实验数据
- `tee-supplicant`：平台安全服务
- udev：摄像头、雷达串口、GPIO、RPMsg
- 日志服务和 `dvr.service`

回退：

```bash
/xxl/camera_detect/scripts/boot_optimize.sh rollback
reboot
```

## 重启验收

```bash
systemd-analyze time
systemd-analyze blame --no-pager | head -30
journalctl -b -u dvr.service -o short-monotonic --no-pager
dmesg | grep -Ei 'remoteproc|m33|rpmsg'
systemctl --failed
```

功能检查：

```bash
systemctl is-active dvr.service hostapd.service dnsmasq.service
test -e /dev/video7
test -e /dev/ttySTM1
test -e /dev/ttyRPMSG0
curl http://127.0.0.1:8080/api/state
```

还需确认摄像头/NPU、雷达、LED、音频、OLED、WiFi AP、Dashboard 和 TF 卡
CSV 持续工作。

## 本次实机结果

第一阶段优化、TF 卡修复并完成最终冷启动后：

```text
kernel:                  2.212 s → 2.072 s
userspace finished:      100.792 s → 11.499 s
total systemd startup:   103.005 s → 13.572 s
multi-user.target:       100.733 s → 11.414 s
M33 running:             约 11.7~17 s → 4.42 s
radar_fusion ready:      24.38 s → 17.79 s
```

第一次优化重启时 userspace 仍需 22.97 秒，原因是 TF 卡 fsck 占用 16.64 秒。
检查发现旧 `/dvr/.buffer` 临时目录严重损坏；正式 MP4、CSV 和配置先备份到
`/home/root/tf-recovery-20260726/dvr-before-fat-repair.tar`，随后离线
fsck 仅丢弃该临时缓存目录。最终冷启动 fsck 降至 1.615 秒，验证结果为
22 个有效文件、无剩余错误。正式录像和 `radar_experiments` 均保留。

第一阶段仍保留 `systemd-networkd-wait-online`（约 2.3 秒）和完整 udev settle
（约 4.8 秒）：前者保护 WiFi AP/DHCP 启动顺序，后者保护当时尚未逐项确认的
摄像头和串口设备节点。没有为继续压缩几秒而牺牲可重复启动。

## 第二阶段：持续启动优化

第二阶段遵循以下约束：先测量、一次只改一个点、每次冷启动并检查完整功能，
不能以摄像头、NPU、雷达、M33/RPMsg、WiFi AP、Dashboard、DVR 或数据完整性
回归来换取启动数字。所有板端配置修改必须先保存原状态并提供回退方式。

只读采集脚本为：

```bash
/xxl/camera_detect/scripts/collect_boot_metrics.sh
```

### 2026-07-27 第二阶段基线（冷启动 1）

本次开发板 RTC 初值不正确，启动中途又发生校时，因此所有结论使用内核单调时钟，
不使用墙钟时间。板端内核为 `6.6.48-gbebcf479fd77`，OpenSTLinux
`5.0.3-snapshot-20250320`。

| 里程碑/项目 | 单调时间或耗时 | 结论 |
|---|---:|---|
| kernel | 2.061 s | 当前不是主要瓶颈 |
| userspace | 11.234 s | `multi-user.target` 在 11.179 s 到达 |
| systemd 总启动 | 13.296 s | 本轮对照基线 |
| M33 running | 3.912 s | 正常，early service 耗时 588 ms |
| `/dev/ttyRPMSG0` udev ready | 4.117 s | 正常 |
| 雷达 `/dev/ttySTM1` udev ready | 4.670 s | 正常 |
| 摄像头 `/dev/video7` udev ready | 8.675 s | 当前最晚的业务设备 |
| TF fsck | 1.272 s | 约 9.19 s 完成，检测并清除了 dirty bit |
| TF 挂载 | 约 9.28 s | 仍是完整 DVR 的前置条件 |
| udev settle | 4.713 s | 9.358 s 完成；由 `dvr` 和 `iiod` 共同拉起 |
| `dvr.service` ExecStart | 9.473 s | 在 udev settle、local-fs 和 M33 之后 |
| `radar_fusion` 进程启动 | 11.384 s | 启动脚本自身约耗时 1.9 s |
| Fusion 初始化完成 | 16.935 s | 应用内部初始化约耗时 5.5 s |
| RPMsg ready 消息 | 17.937 s | 线程中另有固定 1 s 等待 |
| network wait-online | 2.790 s | 阻塞 `dnsmasq` 和 `multi-user.target` |

基线功能检查全部通过：M33 为 `running`，RPMsg、雷达、BLE LED 和摄像头设备
节点存在；`radar_fusion`、HUD、Dashboard、hostapd、dnsmasq 正常；Dashboard
`stale=false`；TF 卡以读写方式挂载。板端仍有两个既有失败单元：
`eeprom-pnsn.service` 连续重试后失败，`rc-local.service` 因 Exec format error
失败。它们不在本次关键链上，在确认产品职责前不直接禁用。

### 第二阶段候选点及初步判断

1. 全局 `systemd-udev-settle`：服务已经被 systemd 标记为 deprecated。不过完整
   业务仍需等待 8.675 s 才出现的 V4L2 摄像头节点，所以仅删除 settle 预计只能让完整
   DVR 提前约 0.7 s；需要同时将等待收窄到具体设备，并审计产品不使用的 `iiod`。
2. TF 卡（当时状态）：主脚本曾要求TF完成fsck/挂载；后续已经将M33/RPMsg/雷达
   与可移除介质解耦，并把DVR/CSV迁移到板载ext4。该项现已完成，不再是候选。
3. 应用初始化：从进程启动到 Fusion ready 约 5.5 s，当前为编码器探测、GPIO、
   雷达、摄像头和 NPU 模型串行初始化。已增加 `[启动] [boot=...]` 里程碑，下一次
   冷启动后再依据精确数据调整顺序。
4. RPMsg：接收线程打开设备后固定等待 1 s 才发送 ready 消息。必须先确认 M33
   协议和多次冷启动稳定性，再考虑缩短或用握手替代。
5. 启动脚本：HUD、Dashboard、固定 sleep、日志和资源清理均位于主程序之前，
   可在保持进程生命周期和事件投递顺序的前提下拆分或并行。
6. 网络：只有 dnsmasq 明确依赖 `network-online.target`。应确认 AP 静态地址和
   DHCP 启动条件后，再决定是否改为等待 `wlan0`/指定地址，而不是全局 online。

### 优化 1：收到匹配 ACK 后结束雷达命令等待

启动里程碑证明，NPU 模型加载约 61 ms、摄像头启动约 289 ms，而
`radar_init()` 耗时约 4.92 s。原实现依次发送四条配置命令；每条命令即使已经
读到回复，仍继续等待完整的 1000 ms 超时，之后再保留 200 ms 稳定间隔。

只读观测到的四条完整回复如下：

| 命令 | 回复命令字 | 首次收到数据 |
|---|---:|---:|
| `group7_cmd1e` | `0xFE` | 0~30 ms |
| `group6_cmd11` | `0xD1` | 0 ms |
| `group0_cmd02` | `0x02` | 0 ms |
| `group6_cmd12` | `0xD2` | 0~29 ms |

优化后的等待器按协议帧长度解析串口流，只在收到“完整且命令字匹配”的
`HEAD_REPLY` 后提前结束；异步 `HEAD_REPORT` 会被识别并跳过。每条命令原有的
1000 ms 超时和 200 ms 稳定间隔均保留，因此雷达不回复时仍沿用原来的最坏等待
和继续启动行为。

首次服务重启对比：

| 指标 | 优化前 | 优化后 | 收益 |
|---|---:|---:|---:|
| 雷达初始化 | 4.923 s | 0.930 s | 3.993 s |
| 主程序入口到 Fusion core ready | 5.326 s | 1.341 s | 3.985 s |

服务重启后四条 ACK 均匹配，摄像头首帧、NPU 推理、M33/RPMsg、BLE LED 串口、
HUD、Dashboard、hostapd、dnsmasq 和 TF 挂载检查通过，Dashboard
`stale=false`。雷达串口的 ACK 证明模块通信正常；现场目标检测仍需在实物进入
雷达视场时做最终功能回归。

板端回退文件：

```text
/etc/dvr-boot-optimization/stage2-baseline/radar_fusion.before-radar-ack-opt
```

### 优化 2：减少启动脚本的纯管理开销

原脚本每读取一个配置项都启动一组 `grep | cut`，每条日志又启动 `date | tee`；
此外在主程序自身的设备占用恢复之前，脚本重复执行两次 `fuser -k` 并固定等待
200 ms，HUD 和 Dashboard 启动后也分别固定等待 200 ms。

本次保持现场配置格式、启动顺序、PID 检查和主程序设备占用恢复不变，仅做：

1. 单次 shell 循环解析白名单配置键，不 `source` 现场文件；
2. 使用 Bash 内建时间格式和 `printf` 同时写终端/日志；
3. 删除脚本层重复的 `fuser` 与 200 ms 等待，保留主程序打开设备前的恢复；
4. 删除 HUD、Dashboard 的固定等待，保留可执行文件检查、PID 检查和失败日志。

服务重启分步测量：

| 状态 | `dvr.service` 启动到主程序入口 |
|---|---:|
| 冷启动基线 | 2.010 s |
| 单次配置解析 + 内建日志 | 约 0.837 s |
| 再删除重复设备清理 | 约 0.535 s |
| 再删除两个固定等待 | 约 0.131 s |

现场 `radar_config` SHA-256 前后均为
`bfad967d6c5567f35a7bb215f06d365e9c993572035878b7d31e238472cff326`，
最终主程序参数与基线完全一致。每一步均检查了 M33、RPMsg、雷达 ACK、摄像头
首帧、NPU、HUD、Dashboard 和 TF 状态。

### 优化 3：禁用空闲的 IIO 网络守护进程

`iiod.service` 是全局 udev settle 的两个拉起者之一。板端审计结果：

- `/sys/bus/iio/devices` 下没有 IIO 设备；
- `radar_fusion`、HUD 没有打开 IIO/sysfs 文件；
- 仓库没有 `iiod`、libiio 或 `/dev/iio:*` 业务引用；
- 没有其他服务依赖 `iiod`，它只被 `multi-user.target` 拉起。

因此将 `iiod.service` 纳入 `boot_optimize.sh` 的可回退可选服务列表。禁用守护
进程不会禁用内核 IIO 子系统；若将来接入 Linux IIO 传感器或远程 IIO 客户端，
可由 rollback 恢复。单独禁用它不会消除 settle，因为当前 `dvr.service` 仍在
拉起 settle；收益要与下一项精确设备等待配合并在重启后测量。

### 优化 4：用精确设备等待替代全局 udev settle

移除 `dvr.service` 对 `systemd-udev-settle.service` 的 `Wants/After`。为避免
设备枚举竞态，`start_dvr.sh` 改为只等待实际使用的设备：

- `/dev/ttySTM1`：雷达硬前提，10 s 超时后失败并由 systemd 重启；
- `/dev/video7`：USB UVC摄像头节点，等待后仍沿用radar-only降级。2026-08-09
  实机确认其为Sonix/Microdia `0c45:636b`，由`uvcvideo`驱动；
- `/dev/ttySTM0`：启用 BLE 方向灯时等待，超时后沿用现有串口失败处理；
- `/dev/gpiochip3`：本机告警 GPIO，超时后沿用现有 LED disabled 处理；
- `/dev/ttyRPMSG0`：继续由原有 M33/RPMsg 15 s 有界等待负责。

基线中这些设备最晚的`/dev/video7`在8.675~8.857s完成udev初始化，早于当时的
TF挂载和`dvr.service`启动，因此该阶段硬件组合没有新增等待。当前主脚本已不再
自动挂载TF。这个改动的
主要系统收益是：在产品未使用的 `iiod` 也被禁用后，`sysinit.target`、网络和
其他 userspace 服务不必等整个 udev 队列清空。

首次整机重启结果：settle 未启动，`sysinit.target` 从 7.280 s 提前到
6.301 s，`dvr.service` ExecStart 从 9.441 s 提前到 8.499 s。Fusion core
在 12.669 s ready，相对第二阶段基线 16.935 s 累计提前 4.266 s。由于本次
`networkd-wait-online` 从 1.669 s 波动到 2.888 s，systemd 总时间不能用于单独
评价本项收益。

### 优化 5：dnsmasq 只等待产品 AP，不等待全局 network-online

板端反向依赖确认，只有 dnsmasq 拉起 `network-online.target`。当前 WLAN 使用
静态地址 `192.168.152.119/24`，hostapd 成功记录 `AP-ENABLED` 后 AP 已可用；
全局 wait-online 还会继续等待 networkd 的链路状态和 IPv6LL，对 DHCP 服务没有
额外价值。

将 dnsmasq drop-in 从 `After/Wants=network-online.target` 改为明确
`After/Wants=hostapd.service`，仍保留 `network.target`。原 drop-in 由
`boot_optimize.sh` 首次保存，rollback 可恢复。预期 `systemd-networkd-wait-online`
在没有其他消费者后不再进入启动事务，同时 dnsmasq 不早于 WiFi AP 启动。

首次整机重启中 wait-online 未启动，dnsmasq 在 hostapd 之后正常启动；systemd
总时间为 12.648 s。新的关键链转移到 `helmet-ota.service`。

### 优化 6：OTA 无待恢复事务时不加载完整安装器

`helmet-ota.service` 原先每次启动都执行 Python `--recover`。当前状态为
`success` 时，恢复器读取 833 字节的 `status.json` 后立即返回，但导入
tarfile、urllib、dataclasses 和完整 OTA 公共模块在板上热缓存仍需约 0.88 s，
冷启动 I/O 竞争时本次实测为 2.523 s。

新增 `helmet_ota_recover_if_needed.sh`：只用 grep 检查事务状态，只有明确出现
`"state": "installing"` 时才 `exec` 原 Python 恢复器。状态文件缺失、损坏或
处于其他状态时，原 `read_json()` 同样不会执行恢复，因此快速路径不改变事务
判断。完整的安装中断回滚逻辑、锁和 Python 安装器均保留。

板端无待恢复事务时，原 Python 检查热缓存约 0.87~0.88 s，轻量检查连续五次
均低于 `time` 的 0.01 s 显示精度。完整 `make ota-test` 已覆盖上传、安装、版本
切换、旧版迁移、手工回滚、健康检查失败和自动回滚。部署前后 `status.json`
SHA-256 均为
`1f4230f3dfcf972aeef5efe004aefdf8387a66caeb42d4085f3fcf2ac4447b41`，
证明快速检查没有改写事务状态。

### 第二阶段最终结果（连续冷启动 3 次）

最终配置连续冷启动三次，均等待板端重新建立 SSH 后再读取本次 boot 的单调时钟
日志。SSH 从主机侧恢复约需 50 s，只代表管理网络可达时间，不能作为系统或业务
启动耗时；下表使用 `systemd-analyze` 和应用 `CLOCK_BOOTTIME` 里程碑。

| 指标 | 第二阶段基线 | 最终 3 次范围 | 最终中位数 | 累计收益 |
|---|---:|---:|---:|---:|
| systemd 总启动 | 13.296 s | 11.837~11.945 s | 11.841 s | 1.455 s |
| M33 running | 3.912 s | 3.881~3.945 s | 3.908 s | 基本不变 |
| Fusion core ready | 16.935 s | 12.386~12.638 s | 12.602 s | 4.333 s |
| RPMsg ready 已发送 | 17.937 s | 13.389~13.639 s | 13.605 s | 4.332 s |

最终三次中 `systemd-udev-settle` 和 `systemd-networkd-wait-online` 均未进入启动
事务。`multi-user.target` 最终两次分别在 userspace 9.488 s、9.407 s 到达；
摄像头设备枚举仍有约 8.50~9.20 s 波动，但精确等待覆盖了该波动，没有发生
偶发打开失败。当前关键链为 WLAN hostapd 启动后再启动 dnsmasq，这是产品热点
和 DHCP 功能所需，未为缩短 target 时间而解除依赖。

#### 2026-08-09：USB摄像头晚就绪专项排查

开发板恢复在线后进行了纯只读复核。`/dev/video7`的sysfs路径位于
`.../482f0000.usb/usb3/3-1/3-1.1/...`，驱动为`uvcvideo`；`v4l2-ctl`和
`media-ctl -d /dev/media2 -p`确认它是Sonix/Microdia `0c45:636b` USB 2.0 UVC
摄像头，输出1280×720 MJPEG 25 FPS。

本次冷启动关键时间：

| 里程碑 | 内核单调时间 | 判断 |
|---|---:|---|
| EHCI USB 2.0主控启动 | 0.576 s | 主控不是慢项 |
| 外接USB Hub枚举 | 0.999 s | 正常 |
| 摄像头USB设备发现 | 1.288 s | 硬件并未等待8秒才上电 |
| `mc`媒体核心加载 | 7.148 s | udev冷插拔/模块加载较晚 |
| `videodev`加载 | 7.206 s | UVC依赖开始就绪 |
| `uvcvideo`识别摄像头 | 8.276 s | 驱动绑定完成 |
| `/dev/video7` udev完成 | 8.288 s | `USEC_INITIALIZED=8287671` |
| `dvr.service`脚本开始 | 8.843 s | 摄像头提前约0.56秒就绪 |

因此8.29秒是从内核入口计算的绝对时间，不是摄像头枚举函数连续运行8.29秒。
真正的延后位于大规模udev冷插拔与`mc`、`videodev`、`uvcvideo`按需加载，USB硬件
本身约1.29秒已被发现。可以把`uvcvideo`加入`modules-load.d`制作可回退冷启动A/B，
但本次它不在业务关键路径上，提前节点预计不会改善Fusion ready，还可能与TF、Wi-Fi
模块加载争用存储I/O。因此当前只记录候选，不修改生产板配置。

#### 2026-08-09：`dvr.service` 到8.84秒才执行的直接原因

本节使用同一次冷启动的内核单调时钟。`systemd-analyze critical-chain`显示的
userspace相对时间容易与内核绝对时间混用，以下统一采用`journalctl -o
short-monotonic`和`systemctl show`中的单调时间。

| 里程碑 | 内核单调时间 | 含义 |
|---|---:|---|
| PID 1排队并开始userspace | 约2.17 s | systemd开始组织启动事务 |
| systemd开始等待`/dev/hwrng` | 2.698 s | `rng-tools.service`是`sysinit.target`前置项 |
| 内核CRNG初始化完成 | 5.220 s | 早于`rngd`启动 |
| systemd确认`/dev/hwrng` | 8.258 s | 当前随机源为`optee-rng` |
| `rng-tools.service`启动 | 8.299 s | `rngd -f -r /dev/hwrng` |
| `sysinit.target`到达 | 8.303 s | 上游主要阻塞解除 |
| `basic.target`到达 | 8.377 s | 普通服务开始并行调度 |
| `radar-dashboard.service`进程启动 | 8.622 s | `dvr.service`显式排在它之后 |
| systemd标记DVR已启动 | 8.726 s | `ExecStart`已经派生 |
| `start_dvr.sh`首条日志 | 8.843 s | 所谓“DVR到8.84秒才执行” |

当前`dvr.service`的显式顺序是：

```text
basic.target + local-fs.target → radar-dashboard.service ─┐
local-fs.target + dvr-m33.service ────────────────────────┼→ dvr.service
```

其中`local-fs.target`在4.330秒完成，M33服务在3.868秒完成，都不是本次最后阻塞项。
Dashboard采用普通systemd默认依赖，必须等`basic.target`；DVR又显式
`After=radar-dashboard.service`。真正把`basic.target`拖到8.377秒的是
`rng-tools.service`：其vendor unit配置了`DefaultDependencies=no`、
`Before=sysinit.target`和`After=dev-hwrng.device`，因此OP-TEE随机数设备未被systemd
确认前，整个`sysinit.target`不能完成。USB摄像头在8.276秒绑定只是时间相邻，
`dvr.service`没有依赖`/dev/video7`设备单元，不能把它当成服务晚启动的原因。

该次历史测试中，DVR脚本启动后还有第二段独立等待：TF卡dirty bit触发FAT检查，fsck从7.832秒
运行到9.570秒，挂载在10.068秒完成；脚本从8.939秒等待到10.518秒后才继续，
`radar_fusion`主入口在10.896秒，runtime ready在13.126秒。因此要区分：

1. 8.84秒之前是systemd基础启动链，主要受`rng-tools`与`/dev/hwrng`影响；
2. 8.84～10.52秒是脚本为避免FAT检查与业务写盘并发而等待TF挂载；
3. 10.90～13.13秒是雷达、摄像头、NPU和融合线程的真实应用初始化。

当时没有直接禁用`rng-tools`，也没有跳过TF fsck。前者关系到系统熵源，后者保护当时
仍在TF上的录像和CSV。当前TF已退出业务链，但仍不应在挂载使用时绕过介质检查。
`optee_rng`当前是模块，未出现在默认`modules-load.d`中；结合模块
TEE modalias和板端配置，可判断它由udev冷插拔过程按设备事件加载。

##### `optee_rng`预加载A/B：基础target提前，但业务无收益，已回退

为验证能否安全解除上游阻塞，新增独立、可回退的`apply-optee-rng`动作，仅向
`modules-load.d`加入`optee_rng`。实验没有关闭OP-TEE，没有禁用`rng-tools`，也没有
改变`After=dev-hwrng.device`和`Before=sysinit.target`关系。配置连续重启三次后，
再执行`rollback-optee-rng`并重启确认恢复。

| 指标 | 改动前本次基线 | 预加载三次范围 | 预加载中位数 | 结果 |
|---|---:|---:|---:|---|
| `/dev/hwrng` udev完成 | 6.773 s | 3.755～3.912 s | 3.858 s | 设备事件明显提前 |
| `sysinit.target` | 8.301 s | 4.791～5.060 s | 4.801 s | 提前约3.5 s |
| `dvr.service` active | 8.724 s | 5.213～5.421 s | 5.241 s | 提前约3.48 s |
| USB摄像头节点 | 8.288 s | 9.988～10.659 s | 10.556 s | 反而延后约2.27 s |
| Fusion runtime ready | 13.126 s | 13.082～13.466 s | 13.246 s | 没有业务收益 |
| systemd总启动 | 12.250 s | 12.301～12.427 s | 12.381 s | 没有整机收益 |

现象表明，提前加载随机数驱动确实消除了`rng-tools`对基础target的表面阻塞，但也让
普通用户态服务约提前3.5秒进入启动，与尚未完成的udev冷插拔和存储I/O并发。三次
实验中摄像头节点均比原基线晚；结合回退后摄像头恢复到8.719秒，可以合理推断启动
资源争用抵消了target提前收益，但不能仅凭这组数据把所有波动都归因于单一模块。

工程结论是：本项目以`radar_fusion_runtime_ready`和首帧为优化目标，不以
`sysinit.target`单项为目标，因此拒绝把该预加载纳入默认`apply`。板端已经回退，
仓库保留独立A/B及回退动作，便于以后在不同镜像上复测：

```bash
/xxl/camera_detect/scripts/boot_optimize.sh apply-optee-rng
# 重启并采集至少三次
/xxl/camera_detect/scripts/boot_optimize.sh rollback-optee-rng
```

也不继续尝试取消`rng-tools`的`Before=sysinit.target`：刚才的实验已经证明，即使DVR
service提前约3.5秒，当时单体脚本仍要等待摄像头和TF，完整业务不会因此提前。
后续已经完成“雷达/RPMsg告警初始化”和“可移除存储”的解耦，并将业务主存储迁至
板载ext4；TF侧仍需正常卸载，不能在fsck期间并发手工挂载。

##### 业务初始化顺序重构：风险链先运行，摄像头与存储动态接入

2026-08-09按上述结论完成了业务层拆分，没有修改内核、设备树、Bootloader、
OP-TEE或`rng-tools`。这次不是把初始化推迟到第一次事件，而是在后台继续主动初始化
全部设备，同时让已经具备条件的风险链先进入主循环。

原顺序为：

```text
basic.target → Dashboard → dvr.service
  → 等摄像头 → 等TF/fsck/挂载 → 雷达初始化 → 摄像头/NPU → RPMsg → 主循环
```

调整后为：

```text
local-fs.target + M33 → dvr.service → 等雷达串口 → 雷达初始化 → 风险主循环
                                      ├→ RPMsg/HUD/NAV
                                      ├→ 摄像头出现后自动接入V4L2/NPU
                                      └→ TF完成fsck/挂载后接入DVR/CSV/日志轮转

basic.target → Dashboard（独立按普通服务顺序启动，不反向阻塞DVR）
```

关键实现与保护措施：

1. `dvr.service`只提前到`local-fs.target`和`dvr-m33.service`之后，显式补回
   `shutdown.target`顺序/冲突；Dashboard仍是`Wants`，但移除双向顺序阻塞。
2. 启动脚本只把雷达串口作为硬前提；TF检查、自动挂载失败回退和日志轮转放到后台。
   后台仍等待`systemd-fsck@dev-mmcblk0p1.service`结束，绝不与fsck并发挂载。
3. `radar_fusion`先完成雷达协议初始化并进入主循环。摄像头线程等待真实
   `/dev/video7`，V4L2或NPU初始化失败会清理后重试，成功后以原子状态切换到融合模式，
   不是本次开机永久退化为radar-only。
4. 不能用“目录存在”判断TF已挂载。程序比较挂载点与父目录的`st_dev`，防止
   Dashboard或旧目录把根文件系统误判为TF；Dashboard也不会在未挂载时创建隐藏目录。
5. TF未就绪时传感器事件进入128条有界RAM环形队列，挂载后按原顺序补写CSV；队列满
   时保留最新事件并记录`dropped`计数，避免无限占用内存。
6. RPMsg设备可以提前打开，但保留原1秒端点稳定时间，并等待
   `/dev/snd/pcmC0D0p`出现后才向M33发送ready；5秒超时后才按“音频降级”继续，
   避免早到的摔倒/V2V事件因ALSA尚未枚举而丢失语音。

部署前板端完整备份位于：

```text
/root/dvr-backups/init-order-20260809-141650/
```

可用其中的`radar_fusion`、`start_dvr.sh`、`radar_dashboard.py`、`dvr.service`和
`radar-dashboard.service`恢复本阶段前状态。部署前已完成AArch64交叉编译、shell
语法、Python编译、Dashboard单元测试与`systemd-analyze verify`。

加入“ALSA输出就绪后才向M33发送ready”的保护后，最终连续两次冷启动结果如下，
全部为内核单调时间：

| 指标 | 本阶段前同配置基线 | 最终冷启动1 | 最终冷启动2 | 两次中位数/结论 |
|---|---:|---:|---:|---:|
| `start_dvr.sh`首条日志 | 9.084 s | 4.368 s | 4.397 s | 4.383 s |
| 雷达风险核心进入主循环 | 13.259 s（原完整runtime） | 6.705 s | 6.788 s | 6.747 s |
| ALSA输出与RPMsg ready | 13.896 s（原RPMsg） | 9.216 s | 9.104 s | 9.160 s |
| TF/DVR/CSV就绪 | 约10.40 s | 11.933 s | 10.668 s | 11.301 s；未绕过fsck |
| 摄像头首帧/视觉融合就绪 | 13.297 s | 10.529/10.522 s | 11.362/11.360 s | 首帧约10.946 s |
| 完整业务（取各分支最晚项） | 13.896 s | 11.933 s | 11.362 s | 11.648 s |
| systemd总启动 | 12.226 s | 13.040 s | 12.919 s | 12.980 s；没有改善 |

这里必须区分指标含义：旧`radar_fusion_runtime_ready`是在雷达、摄像头、NPU和存储
串行完成后打印；新版本把它定义为“风险主循环已运行”，并新增
`fusion_vision_initialized`与`business_storage_initialized`分别表示视觉融合和存储
就绪。因此风险核心约提前6.51秒；按可比的摄像头首帧衡量，完整视觉业务约提前
2.35秒，取视觉、RPMsg/音频和存储三条分支最晚项的完整业务约提前2.25秒。
systemd总时间没有下降，说明收益来自解除业务串行等待，不是隐藏或删除
基础系统工作。

两次冷启动均验证：M33/RPMsg、四条雷达ACK、雷达首个上报、摄像头首帧、NPU、
BLE串口、HUD、Dashboard新鲜状态、TF挂载及CSV持续写入正常；`radar_fusion`、HUD和
Dashboard均只有一个实例。第一轮还实际出现了“事件先到、TF后挂载”的窗口，程序
在存储就绪后报告`Flushed 1 boot events from RAM (dropped=0)`。远程没有触发真实
碰撞、摔倒、音频或录像事件，所以仍需
在有人板旁时补做画面质量、真实雷达目标、LED/OLED、骨传导和事件录像验收，不能把
SSH健康检查写成“所有实物功能已证明无影响”。

最终功能回归如下：

| 功能 | SSH 可验证结果 |
|---|---|
| M33 / RPMsg | remoteproc 为 `running`，`/dev/ttyRPMSG0` 存在，每次均发送 ready |
| 雷达 | `/dev/ttySTM1` 存在，四条初始化命令每次均收到匹配完整 ACK |
| 摄像头 / NPU | `/dev/video7` 存在，每次有首帧里程碑和 NPU 推理日志 |
| BLE 方向灯 | `/dev/ttySTM0` 存在，CH9140 UART ready |
| HUD / Dashboard | 两个进程均存活，Dashboard `stale=false`、版本 1.0.5 |
| WiFi AP / DHCP | hostapd、dnsmasq 均 active，dnsmasq 严格在 hostapd 后启动 |
| TF / 数据记录 | TF 读写挂载，fsck 保留，状态与传感器 CSV 持续更新 |
| OTA | 服务 active，版本和状态 API 正常，当前 1.0.5 / success |

SSH 能证明接口、进程和数据链路正常，但无法代替现场实物验证：仍应由人在板旁
完成一次雷达真实目标、摄像头画面质量、骨传导音频、LED/OLED 显示、手机连接
AP 并获取 DHCP 地址的验收。本阶段没有触发告警来制造录像或声音，避免把远程
检查本身误当成无副作用操作。

### 暂缓的进一步优化点

1. 雷达四条命令之间仍保留 200 ms 稳定间隔，约占 0.8 s。没有协议手册和多种
   雷达固件/温度条件验证前不继续缩短。
2. RPMsg 线程打开设备后仍固定等待 1 s。缺少 M33 对端源码和明确握手协议时，
   缩短可能让 ready 早于对端端点可用。
3. TF fsck 约1.3~1.8s是历史数据；当前TF已退出业务链，主脚本也不自动挂载。
   后续重点是验证启动窗口事件补写，以及板载ext4不可用时的录像降级提示。
4. USB摄像头硬件较早被发现，但本阶段两次`/dev/video7`仍在约9.94~10.00秒才完成
   udev。它已不阻塞雷达/RPMsg；若继续优化完整视觉ready，可单独对`uvcvideo`预加载
   做A/B，但必须防止模块加载与TF、WiFi争用导致整体反而变慢。

板端原配置和每阶段二进制均保存在
`/etc/dvr-boot-optimization/`。`boot_optimize.sh rollback` 可恢复其管理的服务、
dvr unit 和 dnsmasq drop-in；雷达与启动脚本的阶段备份位于
`/etc/dvr-boot-optimization/stage2-baseline/`，便于单项比对或回退。

## 第三阶段：Bootloader、Linux 工具与响应时间方法

这一阶段先做只读审计，不修改 U-Boot 环境、FIP、TF-A、OP-TEE、DTB、内核或
生产 extlinux 配置。原因是当前无法持续进行板旁实物回归，而 bootloader 错误
可能让 SSH 和正常 Linux 回退路径同时消失。

只读审计脚本：

```bash
/xxl/camera_detect/scripts/collect_bootloader_audit.sh
```

### Linux 能测到什么，不能测到什么

`systemd-analyze` 的 kernel 起点已经晚于 BootROM、TF-A、OP-TEE 和 U-Boot，
所以它报告的 11.8 s 不包含 bootloader。`dmesg` 的 0.000 s 也只是 Linux 内核
入口，不能反推出上电到内核入口的耗时。

完整的上电时间线应使用 UART 采集并在主机侧给每行加单调时间戳，至少标记：

```text
上电
  → TF-A / OP-TEE 首条输出
  → U-Boot banner
  → extlinux 菜单
  → Starting kernel
  → Linux [0.000000]
  → M33 running
  → Fusion ready
  → RPMsg ready
```

U-Boot 已编入 bootstage 相关代码，但当前没有可供 Linux 读取的 stash 区域；下次
接 UART 时，应先在 U-Boot 命令行确认 `bootstage report` 是否可用。官方建议先用
bootstage 获取总体阶段时间，需要函数级定位时再使用 U-Boot trace；trace 本身会
改变耗时，最终端到端数据仍应在关闭 trace 后测量：
[U-Boot bootstage](https://docs.u-boot.org/en/latest/develop/pytest/test_bootstage.html)、
[U-Boot tracing](https://docs.u-boot.org/en/latest/develop/trace.html)。

### 2026-08-08 Bootloader 只读审计结果

| 项目 | 当前状态 | 判断 |
|---|---|---|
| U-Boot | `2023.10-stm32mp-r1` | FIP 中只读识别，未修改 |
| `bootdelay` | `0` | 已无 U-Boot 倒计时收益 |
| `boot_targets` | 仅 `mmc1` | 已排除 USB/PXE/多介质扫描 |
| boot 分区 | eMMC `mmc1p6`，ext4 | 当前有效分区已缓存为 `devplist=6` |
| extlinux | 板级配置，`TIMEOUT 20` | 按 U-Boot 语义为 2.0 s 菜单等待 |
| Kernel | `Image.gz` 约 12 MiB，解压后约 29 MiB | 压缩/非压缩需 A/B，不能凭文件大小判断 |
| initramfs | 压缩约 8 MiB，解压后约 24 MiB | 内核解包实测约 0.420 s |
| resize 状态 | `/etc/.resized` 存在 | 一次性扩容已完成，但 initramfs 仍负责挂载根分区 |
| DTB | 当前明确加载 2GB 板 DTB | 不允许为启动时间盲目替换或裁节点 |
| 串口采集 | SSH 主机当前没有 `/dev/ttyUSB*`/`ttyACM*` | 暂时无法给出 bootloader 总耗时 |

U-Boot extlinux 文档明确说明 `timeout` 单位为 0.1 s，因此 `TIMEOUT 20` 是 2 s：
[U-Boot PXE/extlinux 配置语义](https://docs.u-boot.org/en/stable/usage/pxe.html)。

当前 `boot_prefixes=/mmc1_`、`boot_syslinux_conf` 已直接指向存在的板级配置，
extlinux 又排在 boot script 扫描之前；成功启动时不会继续执行后面的
`boot.scr.uimg`。所以“改成直接 bootcmd、删除通用扫描”在当前状态很可能只有
毫秒或小数秒收益，不属于本项目当前的大项。

### Bootloader 优化会不会导致内核崩溃

需要区分三类后果：

| 修改类型 | 典型后果 | 当前策略 |
|---|---|---|
| 菜单 timeout、搜索顺序、启动画面 | 通常不改变内核本身，但可能失去恢复入口 | 有 UART/备用项后才 A/B |
| initramfs、Image 格式、bootargs、DTB | 可能无法挂载 rootfs、kernel panic 或驱动异常 | 只新增备用启动项，不覆盖已知正常项 |
| DDR/时钟、电源、TF-A、OP-TEE、FIP、secure boot、M33 接管 | 可能随机崩溃、数据损坏、安全能力丢失或完全无法启动 | 本阶段禁止修改 |

因此“bootloader 优化不会影响内核”并不成立。菜单等待本身不会让运行中的内核
崩溃，但错误 DTB、内存训练、电源时序、reserved-memory 或 M33 生命周期修改
完全可能在 Linux 中表现为 panic、驱动 probe 失败、DMA 越界或偶发死机。

当前 M33 已验证不适合从 U-Boot 提前启动：Linux 缺少可靠 detach mailbox，
再次尝试会扩大 remoteproc/RPMsg 双重接管风险。TF-A/OP-TEE 和镜像校验也不能
为了速度关闭。

### 大收益候选及安全实施顺序

#### 候选 A：缩短 extlinux 菜单等待

- 理论上限约 2 s，是当前最明确的大项；
- 但 `bootdelay=0`，extlinux 菜单也是当前主要人工恢复窗口；
- 无 UART、bootcount/altbootcmd 或硬件恢复验证时，不改生产配置；
- 有板旁条件后先从 `TIMEOUT 20 → 5` 做 A/B，保留 0.5 s 窗口，不直接改 0；
- 若输入窗口不可靠，应先建立 bootcount + 已知正常启动项回退。U-Boot 的
  bootcount 在连续失败超过 `bootlimit` 后可执行 `altbootcmd`，但 Linux 成功后
  还必须可靠清零：
  [U-Boot Boot Count Limit](https://docs.u-boot.org/en/latest/api/bootcount.html)。

#### 候选 B：验证不使用 resize initramfs 的启动项

当前内核已将 MMC、MMC block、SDHCI 和 EXT4 编入内核，bootargs 也有明确
`root=PARTUUID=... rootwait rw`，具备直接挂载 rootfs 的必要条件。initramfs
一次性扩容标记也已存在。但它目前还负责挂载根分区和 `switch_root`，所以只能：

1. 保留现有 label 为默认；
2. 新增一个不带 `INITRD` 的测试 label；
3. 在 UART 菜单手工选择测试项；
4. 检查 rootfs、bootfs、vendorfs、userfs、TF、M33、摄像头和网络；
5. 完成多轮冷启动和异常断电恢复后，才讨论切换默认项。

预期收益包括 U-Boot 少读取 8 MiB、内核少解包约 0.420 s，以及省去 initramfs
shell/switch_root；真实收益必须由 UART A/B 决定。

#### 候选 C：压缩与非压缩 Image A/B

压缩 Image 少读约 17 MiB，但需要解压；非压缩 Image 读取更多，却可能减少 CPU
解压时间。结果取决于 eMMC 吞吐和 A35/U-Boot 解压性能，不能直接假设哪种更快。
当前 bootfs 只剩约 30 MiB，几乎无法同时安全保存两份完整 Image，因此在没有
独立测试分区或完整恢复镜像前不做。

#### 当前不做的细节项

关闭少量串口输出、删除 258 KiB splash、把已经收窄的 distro scan 改成硬编码
load/booti，都可能只有毫秒到小数秒收益，却会降低诊断和恢复能力。除非 UART
bootstage 证明它们进入关键路径，否则不投入修改和回归成本。

### Linux 启动问题检测工具

| 阶段 | 工具 | 能回答的问题 | 注意事项 |
|---|---|---|---|
| 全局 | UART 时间戳、示波器/GPIO | 上电到 U-Boot、内核和业务的真实端到端时间 | 唯一能覆盖 BootROM/TF-A/U-Boot 的方法 |
| U-Boot | `bootstage report` | U-Boot 各阶段累计耗时 | 需要串口命令行和编译支持 |
| U-Boot | trace + `proftool`/`trace-cmd` | 慢函数调用路径 | 有明显测量扰动，只用于定位 |
| Kernel | `dmesg`/printk 单调时间 | 大致找出设备枚举与 rootfs 阶段 | 两行间空白不等于 CPU 一直阻塞 |
| Kernel | `initcall_debug` | 每个内建驱动 initcall 的耗时和返回值 | 日志量大，只放在诊断启动项 |
| Kernel | ftrace boot-time tracing、`trace-cmd`、KernelShark | driver probe、调度、I/O 的精确时间线 | 当前生产内核未启用 FTRACE，应使用独立诊断内核 |
| userspace | `systemd-analyze time/blame/critical-chain/plot` | target、unit 和关键依赖链 | blame 不能相加，plot 也不含 bootloader |
| userspace | `journalctl -b -o short-monotonic` | 服务与业务日志的统一单调时间轴 | 避免 RTC/NTP 跳变误判 |
| 设备 | `udevadm info/monitor` | 设备节点何时完成初始化 | 应等待具体设备，不做全局 settle |
| 进程 | `perf stat/record`、`strace -ff -ttT` | CPU 热点、系统调用/I/O 等待 | 工具有开销，只在复现环境使用 |
| 应用 | `CLOCK_BOOTTIME`/`CLOCK_MONOTONIC` 里程碑 | 进程入口到设备、模型、首帧、输出 ready | 本项目已经接入并用于第二阶段 |

Linux 官方支持 `initcall_debug` 内核参数；复杂的启动期设备初始化还可以通过
bootconfig 配置 ftrace 事件和函数过滤：
[内核启动参数](https://docs.kernel.org/admin-guide/kernel-parameters.html)、
[Linux boot-time tracing](https://docs.kernel.org/trace/boottime-trace.html)。当前板端
只有 `CONFIG_PRINTK_TIME=y`，没有启用 FTRACE，因此不能在生产镜像上假装已有
函数级数据。

### Dashboard 常态化观测

Dashboard 的 `TIMING` 栏目把 `systemd-analyze`、本次启动日志、已有
`CLOCK_BOOTTIME` 业务里程碑，以及启动后的雷达/NPU/IMU/录像/人工标注/控制事件
集中展示；`HEALTH` 栏目只读展示关键服务、进程、
CPU、内存、温度、TF 和设备节点。它用于快速发现“服务 active 但业务节点未
ready”或“启动后 CPU 持续繁忙”等大项，不替代完整采集脚本和冷启动测试。

为避免观测工具影响被观测对象，系统状态只在栏目激活时每 5 秒刷新，启动信息
每 60 秒刷新并由后端缓存，运行事件每 10 秒更新，浏览器隐藏时停止轮询；正常 API
访问日志每个接口最多每 60 秒写一条。

控制能力与观测接口隔离：独立 `radar-dashboard.service` 不属于 `dvr.service`
cgroup，暂停融合业务后仍保持在线。POST 控制只对白名单中的 DVR 与 OTA 开放，
Dashboard、M33 和网络服务不可控；每次操作要求二次确认、同源校验和服务端令牌，
并写入审计 CSV。启动优化采集仍是只读操作，不会触发控制动作。

### 启动优化与响应优化要分开验收

启动优化指标是“上电到功能 ready”；响应优化指标应是“事件发生到执行器动作”。
不能把初始化推迟到第一次事件来制造更好的启动数字，否则首个告警会变慢。

建议给响应链增加统一单调时钟：

```text
雷达/IMU/摄像头输入
  → 完整帧接收
  → 解析完成
  → 融合决策
  → GPIO/BLE/音频命令发出
  → Dashboard/录像状态发布
```

统计至少记录 p50、p95、p99 和最大值，并分别测试 CPU 空闲、NPU 推理、录像编码、
TF 写入和网络访问并发时的结果。先定位 100 ms 以上或关键链上 0.5~1 s 以上的
问题；不优先追逐不影响用户感知的几毫秒。

### 无法持续实物测试时的变更门禁

1. SSH 能验证但不改变启动介质的只读采集，可以立即做；
2. systemd/应用改动必须有精确依赖、超时回退、板端备份和自动健康检查；
3. bootloader、DTB、initramfs、Image 只能新增 A/B 项，不覆盖唯一正常启动项；
4. 没有 UART、硬件启动模式或可确认的自动回退时，不部署可能失去 SSH 的改动；
5. 远程检查不能替代雷达实物目标、画面、音频、LED/OLED 和手机 DHCP 验收；
6. 无法完成现场验收的改动只记录为候选，不宣称“功能完全无影响”。

面试讲述时重点不是“删了多少服务”，而是证据链：先定义系统 ready 与业务
ready；再用单调时钟、关键链和协议 ACK 找到真正的大项；区分并行耗时与关键
路径；为高风险假设做 A/B 和回退；最后说明哪些方案因证据不足被主动拒绝。
“没有修改”也可以是有价值的工程决策，例如本次拒绝无回退地取消 extlinux
菜单、删除 initramfs，以及再次让 U-Boot 启动 M33。

## 内核模块和设备树是否需要裁剪

需要，但应放在最后一阶段。当前 90 秒级问题来自失败的 systemd 服务，不是
内核；直接裁剪内核或设备树收益较小，却可能破坏摄像头媒体拓扑、NPU/GPU共享
内存、WiFi、音频、remoteproc reserved-memory 或板级电源时序。

当前不能裁掉的关键部分包括：

- STM32 remoteproc/RPMsg
- USB主控、`uvcvideo`和V4L2
- `galcore` 和 STAI/OpenVX 依赖
- `brcmfmac/cfg80211` WiFi
- STM32 SAI、MAX98357A ALSA
- GPIO、串口、VFAT/NLS、MMC

Bluetooth、Hantro VPU、本地显示/触摸、TSN/交换机等是后续候选，但必须先用
bootchart/启动日志证明它们的耗时，再制作独立内核配置和 DTB，保留旧
Image/DTB 启动项进行 A/B 回退。本阶段不修改内核、DTB 或模块黑名单。

## systemd 面试知识点

### `After`、`Wants`、`Requires` 和 `WantedBy`

| 配置 | 含义 | 常见误区 |
|---|---|---|
| `After=A.service` | 本单元若与 A 同时启动，排在 A 后面 | 不会自动拉起 A |
| `Wants=A.service` | 启动本单元时，弱依赖并拉起 A | A 失败通常不阻止本单元 |
| `Requires=A.service` | 强依赖并拉起 A | 仍需配合 `After` 表达先后 |
| `WantedBy=target` | `enable` 时把本单元挂到该 target | 不是运行时的先后关系 |

依赖关系和顺序关系是两件事。例如同时需要“拉起 M33 服务”与“主业务在 M33
之后启动”，通常需要分别表达 `Wants`/`Requires` 和 `After`。

### 为什么 M33 服务使用 `Type=oneshot`

`start_m33_early.sh` 不是常驻守护进程，它完成的是一次固件加载和状态确认。
`Type=oneshot` 会等脚本执行完再认为服务完成，适合把它作为后续服务的顺序
屏障。`RemainAfterExit=yes` 可让成功执行后的单元保持 `active (exited)`，
表达“初始化动作已完成”，但它不表示 M33 后续永远健康，业务仍需检查
remoteproc/RPMsg 的实际状态。

### 为什么谨慎使用 `DefaultDependencies=no`

它能让单元进入很早的 sysinit 阶段，但也会移除 systemd 自动添加的基础启动和
关机依赖。如果脚本依赖尚未挂载的目录、日志、网络或动态设备，此配置可能制造
竞态。早期单元必须依赖少、等待有界，并具备失败回退。

### “服务已启动”不等于“功能已就绪”

systemd 只能根据服务类型和进程状态判断单元是否完成。对本项目而言，还要验证：

- remoteproc 的 `state` 是否为 `running`；
- `/dev/ttyRPMSG0` 是否出现；
- 雷达串口和摄像头节点是否存在；
- Dashboard API 是否返回新鲜业务数据。

因此评价启动优化应同时给出操作系统启动时间和业务 ready 时间。

## 面试 STAR 讲解模板

### Situation（背景）

STM32MP257 骑行辅助设备集成 Linux A35 与 M33，包含摄像头/NPU、毫米波雷达、
DVR、WiFi Dashboard、LED 和音频。开发板冷启动进入 multi-user 需要约
100.7 秒，业务约 24.4 秒才 ready，影响产品开机体验。

### Task（任务）

在不破坏现有业务、不冒险修改内核和设备树的前提下，定位启动瓶颈，提前 M33
和主业务启动，并提供可重复部署、验证和回退能力。

### Action（行动）

1. 用 `systemd-analyze`、critical chain 和 monotonic journal 建立量化基线；
2. 找到两个并行等待近 90 秒且最终失败的 SNMP 服务，审计后禁用无关服务；
3. 沿 systemd、shell 和 remoteproc 日志追踪 M33 的真实启动者；
4. 有边界地测试 U-Boot remoteproc，因 Linux 接管条件不成立而撤回；
5. 新增 sysinit 阶段的 M33 oneshot 服务，并在主业务中保留原路径回退；
6. 发现业务挂载与 fsck 的竞态，先备份数据，再离线修复 FAT 并修正等待逻辑；
7. 解析雷达完整 ACK，将四个无条件 1 s 等待改为“匹配即结束、超时仍保留”；
8. 将 udev/network-online 全局等待收窄到业务设备和 WiFi AP 的精确依赖；
9. 压缩 shell 管理开销与 OTA 无事务快速路径，并连续冷启动做功能回归。
10. 将风险链、视觉链和存储链拆分，雷达/RPMsg先运行，摄像头与TF在就绪后动态接入。

### Result（结果）

- systemd 总启动时间：`103.005 s → 11.841 s`（最终三次中位数），下降约 88.5%；
- userspace：`100.792 s → 9.820 s`（最终三次中位数），下降约 90.3%；
- M33 running：最早约 11.7 秒提前到最终中位数 3.908 秒；
- 雷达风险主循环：`24.38 s → 6.747 s`（本阶段最终两次中位数）；
- 摄像头首帧/视觉融合约10.95秒就绪，带音频保护的RPMsg ready约9.16秒；
- 取视觉、存储与RPMsg分支最晚项，完整业务约11.65秒就绪；
- 保留 WiFi、摄像头、雷达、RPMsg、音频和数据记录功能；
- 所有服务调整均可由脚本回退，未修改内核、DTB 和 U-Boot `bootcmd`。

### 60 秒口述版本

> 我在 STM32MP257 骑行辅助项目中做过一次启动优化。开发板最初 userspace
> 启动要 100 秒左右。我先用 systemd-analyze、critical chain 和单调时钟日志
> 建立基线，发现两个产品不用的 SNMP 服务并行超时近 90 秒；审计依赖后，我用
> 可回退脚本禁用了无关服务。随后继续追踪 M33，确认它并不是 U-Boot 启动，而是
> 被较晚的业务脚本启动。我测试过 U-Boot remoteproc，但由于内核缺少可靠的
> detach/attach 接管证据，没有冒险保留，而是新增 Linux sysinit 阶段的 M33
> oneshot 服务，并给主业务保留原启动路径回退。优化中还发现 TF 卡 fsck 和业务
> 手工挂载存在竞态，我先备份数据再离线修复，并修正状态判断。第二阶段再按雷达
> 完整 ACK 提前结束无效等待，把 udev 和网络等待收窄到具体设备/AP，并压缩启动
> 脚本与 OTA 快速路径。最后把雷达/RPMsg风险链与摄像头、TF存储的晚就绪解耦，
> 摄像头和存储仍在后台主动初始化。整机systemd三次中位数从103秒降到11.84秒；
> 最新两次冷启动中雷达风险主循环约6.75秒就绪，视觉融合约10.95秒、完整业务
> 约11.65秒就绪，同时
> 保留了fsck、事件暂存、失败重试和板端回退。

## 常见追问与参考回答

### 1. 两个服务各超时 90 秒，为什么总时间不是 180 秒？

systemd 会并行启动互不依赖的单元。两个 SNMP 服务的等待区间重叠，所以关键
路径约增加 90 秒。`systemd-analyze blame` 的数字不能直接求和，需要结合
`critical-chain` 和时间戳判断。

### 2. 为什么不禁用 fsck，再省十几秒？

fsck 是数据完整性保护，不是无用服务。禁用它可能让损坏继续扩大，录像和标定
CSV 都有丢失风险。本次正确做法是消除并发挂载、修复文件系统，并保留启动检查。
修复后 fsck 自然从十几秒下降到约 1.6 秒。

### 3. 为什么没有坚持在 U-Boot 启动 M33？

启动得更早不等于更可靠。U-Boot 能执行 `rproc start`，但当前设备树和 Linux
remoteproc 缺少可验证的 detach/attach 接管条件。若双边重复加载或重置 M33，
可能破坏共享内存和 RPMsg。无法证明生命周期连续时，应选择可验证、可回退的
Linux 早期启动方案。

### 4. remoteproc 和 RPMsg 分别解决什么问题？

remoteproc 管理异构核的固件加载、启动、停止和状态；RPMsg 建立 Linux A 核与
远端 M33 之间的消息通道。remoteproc 显示 `running` 只证明 M33 被启动，RPMsg
设备出现并能通信才更接近业务就绪。

### 5. 为什么不先裁剪内核、模块和设备树？

基线表明主要瓶颈在 userspace 失败服务，而不是内核的 2 秒启动。先改内核的
收益有限，风险却会扩散到媒体拓扑、NPU 共享内存、WiFi、音频和 remoteproc。
工程上应先处理高收益、低风险、可回退的问题，再对剩余毫秒或秒级项目做 A/B。

### 6. 如何证明被禁用的服务真的没用？

检查产品需求、业务源码引用、systemd 依赖、端口/设备占用和运行日志，再做冷
启动回归。不能只凭服务名称判断。对不确定单元选择保留；被禁用的单元用脚本
记录原状态，出现回归时可立即恢复。

### 7. 第一阶段为什么保留、第二阶段为什么能移除完整 udev settle？

第一阶段尚未确认实际业务摄像头归属，也没有证明具体节点的最晚时间，所以连同雷达
串口等设备一起保留全局udev等待。第二阶段加入`/dev/video7`、`ttySTM0/1`、
`gpiochip3`和RPMsg的有界精确等待，并审计、禁用空闲iiod后，才移除settle；连续
冷启动未出现枚举竞态。后续实机确认`/dev/video7`实际为USB UVC，这不改变
“等待业务实际使用节点”的原则。

### 8. 下一步如何继续优化？

雷达/RPMsg与TF已经完成解耦，下一步首先是在有人板旁时验证启动窗口内真实事件、
音频、LED/OLED和录像降级/恢复。之后可对`uvcvideo`加载做独立A/B，以改善约10.9秒
才完成的视觉融合；雷达命令间隔和RPMsg固定等待则需先拿到协议或M33对端证据。
再往后才使用bootchart/ftrace定位内核阶段，并以独立Image/DTB做A/B；每一步仍应
记录业务ready时间和失败率，而不是只看一次最好成绩。

### 9. 为什么没有把所有 failed unit 都禁用？

`failed` 不等于与产品无关。板级 EEPROM、温控或兼容初始化失败可能暴露硬件
配置问题，贸然禁用会隐藏风险。只有确认职责、依赖和替代路径后才能处理。

### 10. 如何保证优化可复现？

把服务文件、启动脚本、优化/回退动作和验收命令纳入 Git；脚本在板端保存原
状态，重复执行保持幂等；每次使用冷启动日志和固定业务检查项验收，并记录固件、
内核、DTB 与应用版本。

## 简历表述示例

可根据岗位侧重点选用：

- 基于 `systemd-analyze`、critical chain 与 monotonic journal 优化
  STM32MP257 Linux 启动链路，将整机启动时间从 103.0 秒降至最终三次中位数
  11.84 秒，userspace 耗时下降 90.3%。
- 设计 A35/M33 的 Linux early-boot remoteproc 启动与 RPMsg 就绪检查机制，
  将 M33 running 提前至最终中位数 3.91 秒，并保留失败自动回退路径。
- 根据毫米波雷达完整 ACK 重构初始化等待，将融合业务 ready 从 24.38 秒提前至
  最终三次中位数 12.60 秒，同时保留原超时与命令稳定间隔。
- 定位并修复 systemd fsck 与业务手工挂载并发导致的 FAT 数据风险，完成 DVR
  数据备份、离线修复与冷启动回归。
- 将无关服务裁减、业务前移、状态备份、rollback 和验收命令脚本化，在不修改
  内核、设备树和业务算法的前提下实现可复现、可回退的启动优化。
