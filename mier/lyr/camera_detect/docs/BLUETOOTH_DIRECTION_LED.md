# MP257 雷达风险联动 WBA 左右方向灯

本文记录 STM32MP257、CH9140 和 STM32WBA54 之间的蓝牙方向灯功能，包含硬件
接线、通信协议、软件链路、编译烧录、无雷达环境测试方法和 2026-08-07
实机联调结果。

## 1. 功能目标

MP257 根据现有雷达与摄像头 NPU 融合逻辑选出当前最危险目标。只有最终确认
碰撞风险后，才根据该目标方向通知 WBA：

| 最终风险方向 | MP257 命令 | WBA 输出 |
|---|---|---|
| LEFT（左后方） | `RISK LEFT` | PA7 左灯亮，PA5 右灯灭 |
| RIGHT（右后方） | `RISK RIGHT` | PA5 右灯亮，PA7 左灯灭 |
| CENTER（正后方） | `RISK CENTER` | PA7、PA5 同时亮 |
| 无最终风险 | `RISK CLEAR` | 两灯同时熄灭 |

该功能没有修改已有的雷达阈值、危险目标选择、NPU、DVR、M33、摔倒、Audio
或 MP257 本机 LED 逻辑，只消费已有的 `g_radar_npu_alert` 最终状态。

## 2. 整体链路

```text
AT6010 后向雷达
      │ /dev/ttySTM1，921600
      ▼
MP257 radar_fusion
  危险目标 + 方向滤波 + NPU 最终确认
      │ RISK LEFT/RIGHT/CENTER/CLEAR
      │ /dev/ttySTM0，115200 8N1
      ▼
CH9140（BLE Peripheral，FFF0/FFF1/FFF2）
      ))) BLE (((
STM32WBA54（BLE Central）
      ├── PA7：左方向灯
      └── PA5：右方向灯
```

手机不参与这条控制链路。MP257 与 WBA 直接通过 CH9140 BLE 透明串口通信。

## 3. 硬件与供电

### 3.1 WBA 外接 LED

EWT04 测试底板排针对外提供 3.3 V，没有官方支持的 5 V 输出。Type-C 的 5 V
VBUS 是底板输入并经过稳压器生成 3.3 V，不应从焊盘私自引出为负载供电。

| 用途 | MCU 引脚 | EWT04 排针序号 | 配置 |
|---|---|---:|---|
| 左后方风险灯 | PA7 | 3 | 推挽输出，低电平点亮 |
| 右后方风险灯 | PA5 | 5 | 推挽输出，低电平点亮 |

当前实物采用低电平有效的 LED/MOS 输入，接线逻辑为：

```text
PA7/PA5 输出低电平 ── 对应 LED/MOS 通道点亮
PA7/PA5 输出高电平 ── 对应 LED/MOS 通道熄灭
```

如果以后改为 `GPIO -> 限流电阻 -> LED -> GND` 的普通高电平有效接法，必须
同步修改固件输出极性。不能省略限流电阻。5 V 灯具或大功率 LED 必须使用
独立 5 V 电源和 MOSFET，WBA 与外部电源共地，PA7/PA5 只连接 MOSFET 控制输入。

WBA 上电、复位以及 BLE 断开时，软件均把两个方向灯设为熄灭，避免保留过期
告警。

### 3.2 MP257 与 CH9140

| MP257 J15 | CH9140 | 说明 |
|---|---|---|
| PA4 / USART2_TX | RX | MP257 发命令 |
| PA8 / USART2_RX | TX | MP257 收 WBA ACK |
| 3.3 V | VCC | CH9140 供电 |
| GND | GND | 必须共地 |

Linux 设备为 `/dev/ttySTM0`。该串口已从 Linux console/getty 中释放，不能
再配置为系统调试控制台。

CH9140 使用从机/Peripheral 模式，串口波特率引脚保持默认 `111`（115200）。
WBA 主动扫描名称 `CH9140BLE2U` 或 FFF0 服务并作为 Central 建立连接。

## 4. 蓝牙文本协议

命令为 ASCII 文本，必须以 `\n` 或 `\r` 结束，单条长度不超过 20 字节。

| 命令 | 动作 | ACK |
|---|---|---|
| `PING` | 链路检查 | `PONG` |
| `LEFT ON` | 单独点亮 PA7 | `ACK LEFT ON` |
| `LEFT OFF` | 单独熄灭 PA7 | `ACK LEFT OFF` |
| `RIGHT ON` | 单独点亮 PA5 | `ACK RIGHT ON` |
| `RIGHT OFF` | 单独熄灭 PA5 | `ACK RIGHT OFF` |
| `RISK LEFT` | 仅左灯亮 | `ACK RISK LEFT` |
| `RISK RIGHT` | 仅右灯亮 | `ACK RISK RIGHT` |
| `RISK CENTER` | 两灯亮 | `ACK RISK CENTER` |
| `RISK CLEAR` | 两灯灭 | `ACK RISK CLEAR` |

未知命令返回 `ERR UNKNOWN CMD`。旧 WBA 固件不认识 `RISK ...`，出现该返回
说明 WBA 尚未烧录本功能对应的新固件。

## 5. 软件实现

### 5.1 MP257 A35

- `ble_risk_output.cpp/.h`
  - 非阻塞打开 `/dev/ttySTM0`
  - 配置 115200 8N1
  - 串口暂时不可用时每 2 秒重试，不阻塞雷达主循环
  - 风险状态改变时发送对应命令
  - 读取并记录 WBA ACK
  - 进程退出时发送 `RISK CLEAR`
- `radar_fusion.cpp`
  - 继续使用原有危险目标和方向滤波结果
  - 有摄像头时只使用雷达告警且 NPU 已确认的最终状态
  - 无摄像头的 radar-only 模式沿用原有雷达最终告警
  - LEFT/RIGHT 分别控制单灯，CENTER 或尚未稳定的危险方向控制双灯
- `start_dvr.sh`
  - 从 `radar_config` 读取蓝牙方向灯配置
  - 默认启用 `/dev/ttySTM0`
- `scripts/ble_led_test.sh`
  - 无雷达环境下进行 PING、单灯和风险方向测试
  - 检测到 `radar_fusion` 正在占用串口时拒绝并发测试

现场配置：

```ini
BLE_LED_ENABLED=1
BLE_LED_UART=/dev/ttySTM0
```

如需临时关闭方向灯输出：

```ini
BLE_LED_ENABLED=0
```

### 5.2 STM32WBA

- `Core/Src/main.c`：将 PA7、PA5 配置为低速推挽输出，初始高电平（灯灭）。
- `STM32_WPAN/App/ch9140_client.c`：解析方向灯命令、控制 GPIO 并回传 ACK。
- BLE 断开回调会自动清除两个方向灯。
- 原有 `PING` 和 PA2 板载 `LED ON/OFF` 诊断命令保留。

## 6. 编译、烧录与部署

### 6.1 WBA Keil 工程

在 Windows/Keil 打开：

```text
E04-2G4M10S1AX/MDK-ARM/02_test.uvprojx
```

选择 `E04_BLE_UART`，执行 Rebuild，通过 ST-LINK 烧录。烧录后建议按一次 WBA
RESET，观察重新扫描和 GATT ready 的完整过程。

### 6.2 MP257 A35

主机交叉编译：

```bash
cd /home/alientek/dvr_project/mier/lyr/camera_detect
make radar-fusion
```

部署：

```bash
make deploy-radar BOARD_IP=192.168.88.10
```

只手工部署相关文件时：

```bash
systemctl stop dvr.service
scp radar_fusion start_dvr.sh radar_config.example \
    root@192.168.88.10:/xxl/camera_detect/
scp scripts/ble_led_test.sh \
    root@192.168.88.10:/xxl/camera_detect/scripts/
ssh root@192.168.88.10 'systemctl start dvr.service'
```

## 7. 无雷达环境测试

直接测试时必须先停止 `dvr.service`，否则 `radar_fusion` 与测试脚本会同时
访问 `/dev/ttySTM0`：

```bash
systemctl stop dvr.service

/xxl/camera_detect/scripts/ble_led_test.sh ping
/xxl/camera_detect/scripts/ble_led_test.sh left
/xxl/camera_detect/scripts/ble_led_test.sh right
/xxl/camera_detect/scripts/ble_led_test.sh center
/xxl/camera_detect/scripts/ble_led_test.sh clear

systemctl start dvr.service
systemctl is-active dvr.service
```

预期依次观察到左灯、右灯、双灯亮，最后全部熄灭。

也可以分别测试单灯，不改变另一侧状态：

```bash
/xxl/camera_detect/scripts/ble_led_test.sh left-on
/xxl/camera_detect/scripts/ble_led_test.sh left-off
/xxl/camera_detect/scripts/ble_led_test.sh right-on
/xxl/camera_detect/scripts/ble_led_test.sh right-off
```

## 8. 日志与排障

MP257：

```bash
journalctl -u dvr.service -f
tail -f /xxl/camera_detect/dvr_system.log
```

正常输出包含：

```text
[系统] [BLE-LED] CH9140 UART ready: /dev/ttySTM0 @ 115200
[BLE-LED] Collision indication -> CLEAR
[BLE-LED] WBA reply: ACK RISK CLEAR
```

WBA 调试 UART1 为 115200 8N1，虚拟机当前设备为 `/dev/ttyUSB0`。正常连接过程：

```text
[BLE] scanning for CH9140 name/FFF0
[BLE] connected to CH9140
[GATT] FFF0 found; discover FFF1/FFF2
[BLE] CH9140 link ready
[GATT] transparent link READY
[HEARTBEAT] main loop alive
[STATUS] GAP=connected GATT=ready
```

常见问题：

1. `RX: timeout`
   - 先确认 CH9140 3.3 V、TX/RX 和共地。
   - 查看 WBA 是否持续输出 heartbeat，而不只是曾经出现过一次 GATT ready。
   - WBA 刚烧录后若表面 ready 但没有心跳或双向数据，按一次 WBA RESET，使其
     完整断开、扫描、发现服务并重新连接。
2. `ERR UNKNOWN CMD`
   - WBA 仍是旧固件，需要重新 Rebuild 并烧录当前工程。
3. 测试脚本提示 `radar_fusion 正在使用`
   - 先执行 `systemctl stop dvr.service`，测试后务必重新启动。
4. WBA 一直 `GAP=scanning`
   - 首先确认 CH9140 已接 3.3 V；本项目早期联调曾因 CH9140 未供电停留在扫描。

## 9. 2026-08-07 实机验证记录

测试硬件：STM32MP257、J15 USART2、CH9140、EWT04-2G4M10S1AX、
PA7/PA5 两个外接 LED。

| 检查项 | 实机结果 |
|---|---|
| WBA 启动与 UART 日志 | 通过 |
| WBA 扫描并连接 CH9140 | 通过 |
| FFF0/FFF1/FFF2 发现与 Notify | 通过 |
| MP257 `PING` | 收到 `PONG` |
| `RISK LEFT` | 收到 ACK，左灯亮 |
| `RISK RIGHT` | 收到 ACK，右灯亮 |
| `RISK CENTER` | 收到 ACK，两灯亮 |
| `RISK CLEAR` | 收到 ACK，两灯灭 |
| `dvr.service` 启动接管串口 | 通过 |
| 业务启动自动清灯 | 收到 `ACK RISK CLEAR` |

首次烧录后测试曾出现 `GAP=connected GATT=ready` 但没有心跳、双向数据均超时。
按 WBA RESET 后观察到完整的扫描、连接、GATT 发现流程，所有命令随即通过。
该记录说明排障时不能只看一次 `GATT=ready`，还应确认持续 heartbeat 和实际
`PING/PONG`。

当前已完成软件、蓝牙和 GPIO 实机闭环。真实道路环境下由雷达/NPU 自动触发
LEFT/RIGHT/CENTER 的动态验证仍应随下一轮室外雷达测试一起进行。
