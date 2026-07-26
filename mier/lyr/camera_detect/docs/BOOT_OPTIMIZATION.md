# STM32MP257 启动优化与回退

本文记录 STM32MP257 骑行辅助项目的一次完整启动优化，既用于后续维护，也用于
理解 Linux 启动链路、systemd 依赖、remoteproc/RPMsg 和嵌入式系统稳定性取舍。
文中的数据均来自开发板冷启动实测，不是理论估算。

相关实现：

- [`dvr-m33.service.example`](../dvr-m33.service.example)：M33 早期启动单元
- [`start_m33_early.sh`](../scripts/start_m33_early.sh)：M33 remoteproc 启动与状态确认
- [`dvr.service.example`](../dvr.service.example)：主业务 systemd 单元
- [`start_dvr.sh`](../start_dvr.sh)：设备、TF 卡、M33 和业务进程的启动编排
- [`boot_optimize.sh`](../scripts/boot_optimize.sh)：可重复执行、可回退的优化脚本

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

最终结果从 103.005 秒缩短到 13.572 秒，M33 提前到 4.42 秒运行，且保留了
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

目前仍保留 `systemd-networkd-wait-online`（约 2.3 秒）和完整 udev settle
（约 4.8 秒）：前者保护 WiFi AP/DHCP 启动顺序，后者保护 OV5640/CSI/DCMIPP
媒体拓扑和串口设备节点。没有为继续压缩几秒而牺牲可重复启动。

## 内核模块和设备树是否需要裁剪

需要，但应放在最后一阶段。当前 90 秒级问题来自失败的 systemd 服务，不是
内核；直接裁剪内核或设备树收益较小，却可能破坏摄像头媒体拓扑、NPU/GPU共享
内存、WiFi、音频、remoteproc reserved-memory 或板级电源时序。

当前不能裁掉的关键部分包括：

- STM32 remoteproc/RPMsg
- OV5640、CSI、DCMIPP、V4L2 media controller
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
7. 通过冷启动、设备节点、API、网络和数据完整性做回归验证。

### Result（结果）

- systemd 总启动时间：`103.005 s → 13.572 s`，下降约 86.8%；
- userspace：`100.792 s → 11.499 s`，下降约 88.6%；
- M33 running：最早约 11.7 秒提前到 4.42 秒；
- 主雷达融合业务 ready：`24.38 s → 17.79 s`；
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
> 手工挂载存在竞态，我先备份数据再离线修复，并修正状态判断。最终整机 systemd
> 启动从 103 秒降到 13.6 秒，M33 在 4.42 秒运行，同时保留了功能验证和回退能力。

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

### 7. 为什么保留完整 udev settle？

摄像头由 OV5640、CSI、DCMIPP 和 V4L2 media controller 组成异步枚举拓扑，
雷达串口等节点也依赖 udev。当前约 4.8 秒存在进一步优化空间，但在没有把等待
收窄到明确设备节点并完成多轮冷启动验证前，直接删除会制造偶发竞态。

### 8. 下一步如何继续优化？

先把完整 udev settle 改为等待项目必需的具体设备或 udev tag，再评估网络
wait-online 是否能改为只等待 AP 所需接口。之后用 bootchart/ftrace 定位内核
阶段，再制作独立 Image/DTB 做 A/B 测试。每一步都应记录功能 ready 时间和
失败率，而不是只看一次最好成绩。

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
  STM32MP257 Linux 启动链路，将整机启动时间从 103.0 秒降至 13.6 秒，
  userspace 耗时下降 88.6%。
- 设计 A35/M33 的 Linux early-boot remoteproc 启动与 RPMsg 就绪检查机制，
  将 M33 running 提前至 4.42 秒，并保留失败自动回退路径。
- 定位并修复 systemd fsck 与业务手工挂载并发导致的 FAT 数据风险，完成 DVR
  数据备份、离线修复与冷启动回归。
- 将无关服务裁减、业务前移、状态备份、rollback 和验收命令脚本化，在不修改
  内核、设备树和业务算法的前提下实现可复现、可回退的启动优化。
