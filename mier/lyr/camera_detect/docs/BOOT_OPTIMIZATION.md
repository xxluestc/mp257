# STM32MP257 启动优化与回退

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
