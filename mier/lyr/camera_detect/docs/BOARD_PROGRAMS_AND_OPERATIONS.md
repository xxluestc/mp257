# STM32MP257 开发板程序位置、运行维护与 OTA 操作手册

本文将以下两部分内容合并为一份可独立使用的现场手册：

1. 项目相关程序在开发机仓库和开发板上的位置、作用、启动关系及维护方法；
2. 通过 SSH 或串口进行 A35 应用 OTA 升级、回退和版本核验的方法。

本文以 2026-08-09 对 `192.168.88.10` 开发板的在线检查和当前仓库代码为依据。
当前板端 A35 应用仍为 `1.0.7`，上一稳定版本为 `1.0.6`；本轮界面与TF修复采用
开发态直接部署，没有修改VERSION，也没有生成新OTA包。等全部功能定稿后再统一
选择新版本号并把最终包交给手机/云端队友。文中的 PID、USB 设备编号和磁盘占用
会随重启或设备连接发生变化，排查时应以现场命令结果为准。

> 本文中的 OTA 只覆盖 Cortex-A35 Linux 应用，不更新 M33 固件、WBA 固件、U-Boot、
> Linux 内核、设备树或根文件系统。

---

## 1. 先区分四类程序

这个项目不是一个单独的 Linux 可执行文件，而是由三种处理器上的程序和 Linux 系统服务
共同组成。

| 层次 | 运行位置 | 当前主要职责 | 更新方式 |
|---|---|---|---|
| Cortex-A35 Linux | STM32MP257 Linux | 雷达、摄像头、NPU、录像、HUD、语音、面板、OTA | A35 OTA 包 |
| Cortex-M33 | STM32MP257 M33 核 | IMU事件、V2X事件、与A35的RPMsg通信 | CubeIDE构建、remoteproc加载 |
| STM32WBA | 外接WBA板 | CH9140 BLE Central和方向灯控制 | Keil构建、ST-LINK烧录 |
| Linux系统组件 | A35 Linux | systemd、ALSA、hostapd、dnsmasq、驱动和文件系统 | 系统镜像或板端配置 |

因此：

- `radar_fusion`、HUD和Dashboard可以在Linux中看到进程；
- M33固件只能通过remoteproc状态确认，不能用`ps`找到；
- WBA固件在另一块MCU上，Linux只能通过串口命令检查通信；
- 摄像头采集、NPU推理、录像和语音不是独立systemd服务，而是主业务程序内部模块。

---

## 2. 当前板端总结构

### 2.1 启动和运行关系

```text
Linux systemd
├── dvr-m33.service
│   └── remoteproc加载M33固件
│       └── IMU/V2X事件 -> RPMsg -> A35
│
├── dvr.service
│   └── /xxl/camera_detect/start_dvr.sh
│       ├── log_maintenance.sh
│       ├── hud
│       └── radar_fusion
│           ├── 60GHz毫米波雷达
│           ├── USB UVC摄像头和NPU推理
│           ├── 风险融合及告警
│           ├── 行车记录
│           ├── M33 RPMsg输入
│           ├── WBA/BLE方向灯输出
│           └── 骨传导语音
│
├── radar-dashboard.service -> TCP 8080状态和控制面板
├── helmet-ota.service      -> TCP 8090 OTA服务
├── hostapd.service         -> Wi-Fi热点
└── dnsmasq.service         -> DHCP
```

### 2.2 当前版本目录

```text
/xxl/camera_detect       -> /xxl/releases/1.0.7
/xxl/releases/.previous -> /xxl/releases/1.0.6
/xxl/persistent/camera_detect/radar_config
/opt/helmet-ota/
/var/lib/helmet-ota/
```

含义如下：

- `/xxl/releases/<version>/`保存每一个A35应用release；
- `/xxl/camera_detect`是指向当前release的软链接；
- `.previous`记录上一稳定release；
- `radar_config`放在持久化目录，不跟随release切换；
- OTA服务位于`/opt/helmet-ota`，不会因为业务版本切换而被替换；
- OTA状态、上传包和安装日志位于`/var/lib/helmet-ota`。

不要直接覆盖`/xxl/camera_detect`或手工替换当前release中的文件。正式版本更新应统一
生成OTA包并由安装器切换。

---

## 3. A35 Linux程序位置与职责

当前A35源码的唯一正式维护入口是本目录的上一级，即
`mier/lyr/camera_detect/`。根目录中的`A_Core`、`M_Core`、`camera`、`recorder`等目录是
旧实现或参考代码，不是当前板端生产入口。

| 功能 | 板端文件 | 仓库源码 | 启动和管理 |
|---|---|---|---|
| 主业务 | `/xxl/camera_detect/radar_fusion` | [`radar_fusion.cpp`](../radar_fusion.cpp) | `dvr.service` |
| 摄像头采集 | 编译进`radar_fusion` | [`camera.c`](../camera.c)、[`camera.h`](../camera.h) | 随主业务运行 |
| NPU推理 | 编译进`radar_fusion` | [`npu_detect.cpp`](../npu_detect.cpp)、[`npu_detect.h`](../npu_detect.h) | 随主业务运行 |
| 导航和语音 | 编译进`radar_fusion` | [`nav_tts.c`](../nav_tts.c)、[`nav_tts.h`](../nav_tts.h) | 随主业务运行 |
| BLE风险输出 | 编译进`radar_fusion` | [`ble_risk_output.cpp`](../ble_risk_output.cpp) | 随主业务运行 |
| HUD | `/xxl/camera_detect/hud` | [`hud_project/main.c`](../hud_project/main.c) | `dvr.service` |
| 业务启动总控 | `/xxl/camera_detect/start_dvr.sh` | [`start_dvr.sh`](../start_dvr.sh) | `dvr.service`直接执行 |
| 日志维护 | `/xxl/camera_detect/scripts/log_maintenance.sh` | [`scripts/log_maintenance.sh`](../scripts/log_maintenance.sh) | `dvr.service`子进程 |
| Web面板 | `/xxl/camera_detect/dashboard/` | [`dashboard/`](../dashboard/) | `radar-dashboard.service` |
| OTA服务 | `/opt/helmet-ota/` | [`ota/`](../ota/) | `helmet-ota.service` |

### 3.1 `radar_fusion`包含的主要功能

`radar_fusion`是当前业务核心进程，主要完成：

1. 通过`/dev/ttySTM1`接收并解析60GHz毫米波雷达目标；
2. 通过V4L2打开USB UVC摄像头并采集图像；
3. 加载SSD模型和STAI运行库执行NPU推理；
4. 接收雷达、视觉和M33事件，形成业务风险状态；
5. 通过`/dev/ttyRPMSG0`接收M33的IMU和V2X事件；
6. 通过`/dev/ttySTM0`向WBA发送方向灯和风险命令；
7. 调用ALSA/aplay播放骨传导提示音；
8. 管理板载ext4中的事件触发录像、雷达和传感器日志；
9. 向HUD和Dashboard输出当前状态。

当前摄像头是Sonix/Microdia USB UVC摄像头，现场设备节点为`/dev/video7`。设备编号可能
随USB枚举变化。项目当前没有OV5640，不应在说明和排查记录中继续使用该名称。

### 3.2 HUD程序

HUD当前负责：

- 接收手机导航消息；
- 接收主业务发送的IMU和风险信息；
- 驱动OLED/HUD显示；
- 通过UDP广播部分状态；
- 记录IMU消息投递情况。

当前涉及的UDP端口：

| 端口 | 方向和用途 |
|---|---|
| UDP 8888 | 手机导航/危险消息输入，HUD和导航模块使用 |
| UDP 8890 | `radar_fusion`向HUD发送本地IMU消息 |
| UDP 8889 | HUD向手机广播状态 |

### 3.3 Dashboard

Dashboard由独立的`radar-dashboard.service`管理，默认监听TCP 8080。它读取：

- `radar_state.json`和业务CSV；
- `/proc`、`/sys`中的CPU、内存和设备状态；
- systemd服务状态；
- 启动关键时间和业务关键事件；
- 允许控制的业务服务状态。

Dashboard独立于`dvr.service`。停止主业务不会自动停止面板，停止面板也不会停止雷达、
录像和HUD业务。

### 3.4 OTA服务

OTA服务由`helmet-ota.service`管理，默认监听TCP 8090，职责包括：

- 接收并校验A35应用OTA包；
- 解压到新的release目录；
- 停止并重新启动`dvr.service`；
- 原子切换`/xxl/camera_detect`软链接；
- 检查VERSION、`radar_fusion`、HUD和Dashboard；
- 新版本失败时自动恢复上一稳定版本；
- 上电时恢复被断电中断的安装事务。

---

## 4. M33固件位置与职责

### 4.1 板端文件

```text
/home/root/project/fw_cortex_m33.sh
/home/root/project/lib/firmware/project_CM33_NonSecure.elf
/usr/lib/firmware/project_CM33_NonSecure.elf
/sys/class/remoteproc/remoteproc0/state
```

当前在线检查时，remoteproc状态为`running`，板端两份ELF的SHA256一致。

M33源码主要位于：

```text
mier/v2x/STM32Cube_ATK_FW_MP2_V1.0.0/
  Projects/STM32MP257D-ATK/Applications/CM33_OpenAMP_DEMO/
```

核心业务入口是：

[`app_freertos.c`](../../../v2x/STM32Cube_ATK_FW_MP2_V1.0.0/Projects/STM32MP257D-ATK/Applications/CM33_OpenAMP_DEMO/project/STM32CubeIDE/CM33/NonSecure/Application/User/FREERTOS/App/app_freertos.c)

M33当前涉及：

- IMU数据和事件处理；
- 急刹、摔倒等事件；
- V2X协同危险事件；
- OpenAMP/RPMsg消息发送；
- 与A35业务状态联动。

### 4.2 当前版本管理风险

`mier/.gitignore`当前忽略了`/v2x/`，并且本地没有找到可以与板端ELF直接校验对应的构建
产物。因此，当前GitHub版本管理不能完整证明“某次M33源码提交对应板端哪一个ELF”。

修改M33之前应先补齐：

1. M33源码的正式Git仓库或解除忽略；
2. 固件版本号、构建时间和对应commit；
3. 板端ELF和发布ELF的SHA256；
4. M33升级和回退步骤；
5. RPMsg协议兼容版本。

在这些内容补齐以前，不建议为了启动时间或小功能调整随意替换M33固件。

---

## 5. WBA方向灯固件位置与职责

当前正式WBA源码位于仓库根目录：

[`E04-2G4M10S1AX`](../../../../E04-2G4M10S1AX/README.md)

当前固件主要完成：

- CH9140 BLE Central通信；
- 接收A35发送的`PING`和`RISK`命令；
- 处理`LEFT`、`RIGHT`、`CENTER`和`CLEAR`；
- 驱动左右方向灯；
- 返回ACK。

WBA程序不在Linux文件系统中，不能用systemd启动或OTA更新。它需要通过Keil工程构建并用
ST-LINK烧录。Linux只能通过`/dev/ttySTM0`进行通信测试。

`mier/v2x/E04-2G4M10S1AX`是另一套旧版或实验性V2V代码，不应和根目录的当前WBA方向灯
固件混为同一个生产版本。

当前仓库中没有找到完整手机App源码。板端目前提供UDP导航/HUD接口以及8080、8090的
HTTP接口，手机端不属于本手册中的板端可执行程序。

---

## 6. 设备、端口和数据位置

### 6.1 设备对应关系

| 硬件 | 当前现场节点 | 主要使用程序 |
|---|---|---|
| 60GHz毫米波雷达 | `/dev/ttySTM1`，921600波特率 | `radar_fusion` |
| USB UVC摄像头 | 当前为`/dev/video7` | `radar_fusion` |
| A35与M33通信 | `/dev/ttyRPMSG0` | `radar_fusion` |
| WBA/BLE方向灯 | `/dev/ttySTM0`，115200波特率 | `radar_fusion` |
| 骨传导音频 | `/dev/snd/pcmC0D0p` | ALSA/aplay，由主业务调用 |
| 告警GPIO | `/dev/gpiochip3` | `radar_fusion` |
| 板载userfs | `/dev/mmcblk1p9`，ext4，挂载到`/usr/local` | 比赛录像和业务数据 |
| 外置TF卡 | `/dev/mmcblk0`或`/dev/mmcblk0p1` | 自动识别，仅用于导入/导出，不参与业务写入 |

### 6.2 网络端口

| 端口 | 程序 | 作用 |
|---|---|---|
| TCP 22 | sshd | SSH运维 |
| TCP 8080 | Dashboard | 状态和控制面板 |
| TCP 8090 | OTA服务 | 版本、上传、安装和状态查询 |
| UDP 67 | dnsmasq | DHCP |
| UDP 8888 | HUD/导航模块 | 导航和危险消息输入 |
| UDP 8889 | HUD | 向手机广播状态 |
| UDP 8890 | HUD | 本地IMU消息输入 |

### 6.3 录像和业务数据

可靠主存储位于板载 ext4：

```text
/usr/local/helmet
```

主要数据目录：

```text
/usr/local/helmet/dvr/*.mp4
/usr/local/helmet/radar_experiments/radar_data.csv
/usr/local/helmet/radar_experiments/sensor_events.csv
/usr/local/helmet/radar_experiments/imu_delivery.csv
/usr/local/helmet/radar_experiments/control_events.csv
/usr/local/helmet/radar_experiments/labels.csv
/usr/local/helmet/radar_experiments/radar_state.json
```

主业务和系统日志：

```text
/xxl/camera_detect/dvr_system.log
/xxl/camera_detect/radar_dashboard.log
/tmp/hud.log
/var/lib/helmet-ota/install.log
/var/lib/helmet-ota/status.json
/var/lib/helmet-ota/last_success.json
systemd journal
```

---

## 7. 上电启动顺序

### 7.1 M33早期启动

`dvr-m33.service`在`sysinit.target`阶段调用：

```text
/xxl/camera_detect/scripts/start_m33_early.sh
```

脚本逻辑是：

1. 等待`remoteproc0`出现；
2. M33已经`running`时直接保留，不重复加载；
3. M33未运行时调用`/home/root/project/fw_cortex_m33.sh start`；
4. 启动失败时记录错误，让后续`start_dvr.sh`保留回退检查能力。

### 7.2 A35业务启动

`dvr.service`执行`/xxl/camera_detect/start_dvr.sh`，主要顺序为：

1. 读取持久化`radar_config`；
2. 启动日志维护和存储准备；
3. 检查必要的雷达串口；
4. 检查M33和RPMsg；
5. 启动HUD；
6. 确认Dashboard状态；
7. 启动`radar_fusion`；
8. 由主业务异步完成摄像头和存储相关准备。

当前M33、Dashboard、OTA和Wi-Fi均独立管理。`dvr.service`重启不会重启这些独立服务。

---

## 8. 日常状态检查

### 8.1 一组命令完成基础检查

```bash
systemctl is-active dvr-m33 dvr radar-dashboard helmet-ota hostapd dnsmasq
systemctl is-enabled dvr-m33 dvr radar-dashboard helmet-ota hostapd dnsmasq

cat /sys/class/remoteproc/remoteproc0/state
pgrep -a radar_fusion
pgrep -a hud
/xxl/camera_detect/scripts/tf_card_control.sh status

test -e /dev/ttySTM0 && echo 'BLE UART OK'
test -e /dev/ttySTM1 && echo 'RADAR UART OK'
test -e /dev/ttyRPMSG0 && echo 'RPMSG OK'
test -e /dev/video7 && echo 'CAMERA NODE OK'

curl -fsS http://127.0.0.1:8080/api/state
curl -fsS http://127.0.0.1:8090/api/ota/version
```

设备节点存在和进程在线只表示基础运行条件成立，不等于所有场景功能已经通过验收。比赛或
正式交付前仍应测试雷达目标、摄像头、风险告警、方向灯、HUD、骨传导、录像和V2X事件。

### 8.2 查看服务详情

```bash
systemctl status dvr-m33.service --no-pager -l
systemctl status dvr.service --no-pager -l
systemctl status radar-dashboard.service --no-pager -l
systemctl status helmet-ota.service --no-pager -l
systemctl status hostapd.service dnsmasq.service --no-pager -l
```

### 8.3 查看日志

```bash
journalctl -b -u dvr-m33.service -u dvr.service --no-pager
journalctl -u dvr.service -f
journalctl -u radar-dashboard.service -f
journalctl -u helmet-ota.service -f

tail -F /xxl/camera_detect/dvr_system.log
tail -F /tmp/hud.log
tail -n 100 /var/lib/helmet-ota/install.log
```

---

## 9. 服务启动、停止和重启

### 9.1 主业务

```bash
systemctl status dvr.service
systemctl restart dvr.service
systemctl stop dvr.service
systemctl start dvr.service
```

停止`dvr.service`会停止：

- `radar_fusion`；
- HUD；
- `log_maintenance.sh`及其子进程。

不会停止：

- M33固件；
- Dashboard；
- OTA服务；
- hostapd和dnsmasq。

不要在`dvr.service`运行时手工执行`/xxl/camera_detect/start_dvr.sh`。否则可能创建重复进程，
并造成摄像头、串口、音频或文件锁冲突。

### 9.2 Dashboard和OTA

```bash
systemctl restart radar-dashboard.service
systemctl restart helmet-ota.service
```

### 9.3 Wi-Fi热点

```bash
systemctl status hostapd.service dnsmasq.service
systemctl restart hostapd.service dnsmasq.service
```

如果SSH连接通过板载Wi-Fi建立，重启hostapd或dnsmasq可能中断当前连接。除非正在排查网络，
不要把网络服务重启加入普通业务恢复流程。

---

## 10. 特殊维护操作

### 10.1 修改雷达业务参数

实际持久化文件：

```text
/xxl/persistent/camera_detect/radar_config
```

`/xxl/camera_detect/radar_config`只是指向该文件的软链接。推荐步骤：

```bash
cp -a /xxl/persistent/camera_detect/radar_config \
  /root/radar_config.before-change
vi /xxl/persistent/camera_detect/radar_config
systemctl restart dvr.service
```

不要把现场参数直接放进release包，否则升级时容易覆盖实际标定值。

### 10.2 M33维护性重启

只有在M33或RPMsg确实异常时才执行。过程中IMU和V2X功能会中断：

```bash
systemctl stop dvr.service
cd /home/root/project
./fw_cortex_m33.sh stop
systemctl restart dvr-m33.service
systemctl start dvr.service
```

`dvr-m33.service`是一次性启动服务，没有负责停止固件的`ExecStop`。仅执行
`systemctl stop dvr-m33.service`不能等同于停止M33。

### 10.3 WBA方向灯测试

测试脚本会占用`/dev/ttySTM0`，必须先停止主业务：

```bash
systemctl stop dvr.service
/xxl/camera_detect/scripts/ble_led_test.sh ping
/xxl/camera_detect/scripts/ble_led_test.sh left
/xxl/camera_detect/scripts/ble_led_test.sh right
/xxl/camera_detect/scripts/ble_led_test.sh center
/xxl/camera_detect/scripts/ble_led_test.sh clear
systemctl start dvr.service
```

### 10.4 安全移除TF卡

当前主业务不再向 TF 写入，但换新卡或人工导出数据后仍应安全卸载：

```bash
/xxl/camera_detect/scripts/tf_card_control.sh eject
```

重新插入后先确认挂载。主业务使用板载 ext4，不必因 TF 插拔而重启：

```bash
/xxl/camera_detect/scripts/tf_card_control.sh status
```

Dashboard“设备运维”页提供等价的TF挂载/弹出、安全停止项目和安全关机按钮。
无网页时执行`/xxl/camera_detect/scripts/project_safe_stop.sh stop`；需要关机时执行
`/xxl/camera_detect/scripts/project_safe_stop.sh poweroff`。
前者仅停止`dvr.service`并保留Dashboard、M33、网络和OTA；后者会在刷盘后请求
systemd关闭整机，不需要先执行前者。
项目停止后，设备运维区的同一按钮会变为“安全启动项目”，启动并确认服务正常后再
恢复为“安全停止项目”。

### 10.5 清理运行日志

项目提供`clear_runtime_logs.sh`，会停止业务、清理文本日志和部分CSV状态，再根据执行前状态
恢复业务。它会改变和删除数据，不能把它当作普通查看命令。执行前先确认是否需要保留实验
证据，并备份相关目录。录像、标签和systemd journal的保留范围应以脚本当前实现为准。

---

## 11. SSH和串口能否切换OTA版本

可以。SSH和串口只是进入Linux Shell的不同方式。只要已经进入开发板Linux的root Shell，
两者都能调用同一套OTA安装器。

如果串口中看到类似：

```text
root@myd-ld25x:~#
```

说明已经进入Linux，可以执行本文OTA命令。

如果看到：

```text
STM32MP>
```

这是U-Boot控制台，不能直接运行Python OTA安装器，需要先启动Linux。

### 11.1 “切换版本”和“修改VERSION”不是一回事

真正的版本切换需要：

```text
校验OTA包
-> 停止dvr.service
-> 原子切换/xxl/camera_detect
-> 启动dvr.service
-> 执行健康检查
-> 记录当前和上一稳定版本
```

直接编辑`/xxl/camera_detect/VERSION`只会改变显示文字，不会改变程序，还会破坏OTA版本
判断、安装记录和回退逻辑，因此禁止用这种方式“改版本”。

---

## 12. 回退到上一稳定版本

当前现场状态是：

```text
current  -> 1.0.7
previous -> 1.0.6
```

### 12.1 回退前检查

```bash
cat /var/lib/helmet-ota/status.json
cat /var/lib/helmet-ota/last_success.json
readlink -f /xxl/camera_detect
readlink -f /xxl/releases/.previous
cat /xxl/camera_detect/VERSION
systemctl is-active dvr.service helmet-ota.service
```

如果`status.json`中的状态为`installing`，不要启动第二次安装、回退或手工修改软链接。应先
等待当前事务完成，或由OTA恢复器处理异常中断。

### 12.2 执行回退

```bash
/usr/bin/python3 /opt/helmet-ota/helmet_ota_installer.py --rollback
```

安装器会：

1. 获取OTA排他锁；
2. 读取上一稳定版本记录；
3. 停止`dvr.service`；
4. 原子切换`/xxl/camera_detect`；
5. 启动`dvr.service`；
6. 检查版本、`radar_fusion`、HUD和Dashboard；
7. 更新`.previous`、`last_success.json`和`status.json`。

回退会造成A35核心业务短暂停止，但M33、Wi-Fi、Dashboard和OTA服务不会随
`dvr.service`一起停止。

### 12.3 回退后核验

```bash
readlink -f /xxl/camera_detect
cat /xxl/camera_detect/VERSION

systemctl is-active dvr.service helmet-ota.service
pgrep -a radar_fusion
pgrep -a hud

curl -fsS http://127.0.0.1:8080/api/state
curl -fsS http://127.0.0.1:8090/api/ota/status
journalctl -u dvr.service -n 100 --no-pager
```

运行状态核验后，还应按功能验收表检查雷达、摄像头、方向灯、HUD、音频、录像和M33事件。

---

## 13. 通过SSH安装之后的新版本

### 13.1 在开发机生成新版本包

```bash
cd /home/alientek/dvr_project/mier/lyr/camera_detect
make
make dashboard-check
make script-check
make ota-package VERSION=1.0.8
```

生成：

```text
dist/helmet-a35-1.0.8.tar.gz
dist/helmet-a35-1.0.8.tar.gz.sha256
```

版本号必须是尚未安装过的新版本。相同VERSION不能重复安装，目标release目录已经存在时
安装器也会拒绝覆盖。

### 13.2 推荐方式：使用OTA HTTP接口

开发机执行：

```bash
BOARD=192.168.88.10
PKG=dist/helmet-a35-1.0.8.tar.gz
SHA=$(sha256sum "$PKG" | awk '{print $1}')

curl "http://${BOARD}:8090/api/ota/version"

curl -X POST \
  -H 'Content-Type: application/gzip' \
  -H "X-OTA-SHA256: ${SHA}" \
  --data-binary "@${PKG}" \
  "http://${BOARD}:8090/api/ota/upload"
```

保存上传响应中的`package_id`，再执行：

```bash
curl -X POST \
  -H 'Content-Type: application/json' \
  -d '{"package_id":"<package_id>"}' \
  "http://${BOARD}:8090/api/ota/install"

watch -n 1 "curl -s http://${BOARD}:8090/api/ota/status"
```

`install`返回HTTP 202只表示后台安装任务已经受理。网络中断后不要直接重复提交，应恢复连接
后先查询`/api/ota/status`。

### 13.3 SSH复制后直接调用安装器

需要避开HTTP上传流程时，可以先复制完整OTA包：

```bash
scp dist/helmet-a35-1.0.8.tar.gz \
  root@192.168.88.10:/var/lib/helmet-ota/uploads/ssh-1.0.8.tar.gz
```

SSH进入开发板后执行：

```bash
PKG=/var/lib/helmet-ota/uploads/ssh-1.0.8.tar.gz
SHA=$(sha256sum "$PKG" | awk '{print $1}')

/usr/bin/python3 /opt/helmet-ota/helmet_ota_installer.py \
  --install \
  --package "$PKG" \
  --sha256 "$SHA" \
  --package-id ssh-manual-1.0.8
```

这种方式仍经过包校验、原子切换、服务重启、健康检查和失败自动回退，但不会经过HTTP服务
生成上传元数据。因此日常正式发布优先使用OTA接口，直接安装器适合串口/SSH恢复和现场调试。

---

## 14. 能否切换到任意旧版本

当前安装器正式支持：

- 安装一个未使用过的新VERSION；
- 回退到`.previous`记录的上一稳定版本。

它没有提供`--activate-version 1.0.3`一类“选择任意已有release”的命令。虽然板端可能保留
`/xxl/releases/1.0.0`到`1.0.3`，也不应直接执行：

```bash
ln -sfn /xxl/releases/1.0.3 /xxl/camera_detect
```

手工切换会绕过：

- OTA安装锁；
- 停止和启动顺序；
- 配置继承；
- 健康检查；
- 失败自动恢复；
- `last_success.json`和`.previous`维护。

如果需要恢复`1.0.3`的代码，推荐从对应Git提交检出旧代码，再以一个未使用的新版本号，
例如`1.0.9`，重新构建OTA包。这样代码内容可以来自旧版本，但发布过程仍受完整事务保护。

后续如果确实需要经常选择任意历史release，应扩展安装器，增加带健康检查和自动恢复的
`activate-version`操作，而不是提供手工修改软链接的运维命令。

---

## 15. OTA安装后的检查和故障处理

### 15.1 基础检查

```bash
readlink -f /xxl/camera_detect
cat /xxl/camera_detect/VERSION
cat /xxl/camera_detect/radar_config

systemctl is-active dvr.service radar-dashboard.service helmet-ota.service
pgrep -a radar_fusion
pgrep -a hud

curl -fsS http://127.0.0.1:8080/api/state
curl -fsS http://127.0.0.1:8090/api/ota/version
curl -fsS http://127.0.0.1:8090/api/ota/status
```

### 15.2 安装记录

```bash
cat /var/lib/helmet-ota/status.json
cat /var/lib/helmet-ota/last_success.json
tail -n 100 /var/lib/helmet-ota/install.log
journalctl -u helmet-ota.service -n 100 --no-pager
```

状态含义：

| 状态 | 含义 |
|---|---|
| `installing` | 正在校验、切换、启动或检查，不要重复操作 |
| `success` | 新版本或手工回退版本通过健康检查 |
| `rolled_back` | 新版本失败，已自动恢复上一版本 |
| `failed` | 安装或恢复未能确认成功，需要人工检查 |

### 15.3 掉电恢复

如果OTA过程中掉电，`helmet-ota.service`下次启动时会检查`status.json`。若发现上一次状态
仍为`installing`，恢复脚本会根据事务阶段清理staging或尝试恢复上一稳定版本。

不要在这种状态下直接删除`/var/lib/helmet-ota`、修改状态JSON或手工更改release软链接。

---

## 16. 编译、部署和版本管理边界

### 16.1 A35应用

日常开发入口：

```bash
cd /home/alientek/dvr_project/mier/lyr/camera_detect
make
```

正式发布入口：

```bash
make ota-package VERSION=<未使用的新版本>
```

进入release/软链接模式后，`make deploy-radar`只应作为初次部署或临时开发调试工具，不能
代替正式OTA发布，否则会破坏release不可变性和回退依据。

### 16.2 M33和WBA

A35 OTA不会更新：

- `/home/root/project/...elf` M33固件；
- WBA板上的烧录固件；
- U-Boot、TF-A、OP-TEE；
- Linux内核和设备树；
- rootfs系统服务和驱动。

如果一次功能变更同时修改A35、M33和WBA，应为三个产物分别记录：

```text
组件版本
源码commit
构建工具和配置
二进制SHA256
部署/烧录时间
协议兼容版本
回退方法
```

否则只回退A35可能造成RPMsg协议或BLE命令不兼容。

---

## 17. 当前在线检查发现的维护问题

### 17.1 开发板系统时间不准确

开发主机日期为2026-08-09时，开发板显示2026-07-27。板端RTC读取失败，
`systemd-timesyncd`虽然为active，但没有完成有效网络校时。

这不会改变systemd使用单调时钟计算的本次启动耗时，但会影响：

- 录像文件名；
- 事件和实验日志时间；
- OTA安装时间；
- 跨设备故障时间线；
- 证书或网络服务的时间判断。

在比赛记录和长期运行前应修复RTC或NTP同步，并明确“上电后时间同步完成”这一业务就绪
条件。

### 17.2 两个非项目服务失败

在线检查发现：

```text
eeprom-pnsn.service
rc-local.service
```

处于failed状态。当前项目核心服务均正常，这两个单元不是已确认的骑行辅助业务链，但应
单独判断是厂商遗留服务、无效兼容配置，还是仍有硬件用途，不能直接删除。

### 17.3 版本回退不能替代功能验收

Git和OTA可以恢复A35应用文件，但不能证明硬件、M33、WBA、外设标定和现场配置一定一致。
每次版本切换至少应保存：

1. 当前版本、目标版本和Git commit；
2. `radar_config`备份；
3. OTA状态和安装日志；
4. 服务、进程、设备节点和TF卡状态；
5. 雷达、摄像头、方向灯、HUD、语音、录像和V2X功能结果。

---

## 18. 推荐现场操作顺序

### 18.1 普通故障恢复

```text
先查服务和日志
-> 确认设备节点
-> 仅重启dvr.service
-> 再验证功能
-> 只有RPMsg/M33明确异常时才维护性重启M33
```

### 18.2 OTA升级

```text
构建并测试新包
-> 保存当前状态和配置
-> 上传包
-> 触发安装一次
-> 持续查询status
-> 检查服务/进程/设备
-> 完成功能验收
-> 保存版本和验收记录
```

### 18.3 OTA回退

```text
确认无installing事务
-> 确认.previous目标
-> 执行--rollback
-> 查看status和install.log
-> 检查A35业务
-> 检查M33/WBA协议和完整功能
```

这个顺序的核心不是“能把软链接改过去”，而是让每次版本变化都有明确目标、可验证状态、
失败恢复路径和现场记录。
