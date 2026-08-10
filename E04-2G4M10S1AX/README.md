# STM32MP257（CH9140）与 STM32WBA54 蓝牙通信

本工程用于实现以下专用链路：

```text
MP257 USART2 ⇄ CH9140（BLE Peripheral）~~~ BLE ~~~
STM32WBA54（BLE Central）⇄ PA2 板载 LED + PA7/PA5 外接方向灯
```

旧工程中的 GNSS、IMU、语音、V2X 和手机日志业务已经移除；STM32 HAL、
CMSIS、STM32_WPAN、启动文件和链接脚本是编译及无线协议栈依赖，必须保留。

## 为什么 WBA 必须作为 Central

MP257 J15 外接模块的准确型号是 **CH9140**。根据 CH9140 手册：

- CH9140 默认/从机模式是标准 BLE Peripheral，可供通用 BLE Central 连接。
- CH9140 的主机模式只用于连接部分 WCH BLE 芯片，不能作为通用主机连接
  STM32WBA。
- CH9140 不使用串口 AT 指令配置角色；角色和波特率由硬件引脚决定。

所以第一版不能让两块板都做 Peripheral，也不能把 CH9140 切到主机模式。
正确方式是让 WBA 主动扫描并连接 CH9140。

## CH9140 硬件状态

CH9140 需保持：

- `BLE_MODE`（24 脚）：高电平或悬空（内部上拉），即从机模式。
- `BPS2/BPS1/BPS0`（26/27/28 脚）：`111`（默认 115200）。
- 默认广播名称：`CH9140BLE2U`。

CH9140 默认串口透传服务：

| UUID | 作用 |
|---|---|
| `FFF0` | BLE 串口服务 |
| `FFF1` | CH9140 UART RX → BLE Notify |
| `FFF2` | BLE Write → CH9140 UART TX |

WBA 上电后自动循环扫描 CH9140。识别时同时检查广播包、扫描响应中的
`CH9140BLE2U` 名称以及 `FFF0` 服务 UUID，因此名称被修改后仍可通过服务
识别。连接后发现 FFF0、订阅 FFF1，并通过 FFF2 回传，不需要手机参与配对
或转发。

## 已实现指令

MP257 发送的文本命令必须以 `\n` 或 `\r` 结束：

| MP257 命令 | WBA 动作 | 返回 |
|---|---|---|
| `PING` | 链路检查 | `PONG` |
| `LED ON` | 点亮 EWT04 底板 D1（PA2，低有效） | `ACK LED ON` |
| `LED OFF` | 熄灭 D1 | `ACK LED OFF` |
| `LEFT ON` / `LEFT OFF` | 单独控制 PA7 左灯 | `ACK LEFT ...` |
| `RIGHT ON` / `RIGHT OFF` | 单独控制 PA5 右灯 | `ACK RIGHT ...` |
| `RISK LEFT` | 左灯执行告警时序，右灯灭 | `ACK RISK LEFT` |
| `RISK RIGHT` | 右灯执行告警时序，左灯灭 | `ACK RISK RIGHT` |
| `RISK CENTER` | 两灯同步执行告警时序 | `ACK RISK CENTER` |
| `RISK CLEAR` | 保持当前风险灯1秒后熄灭 | `ACK RISK CLEAR` |

其他命令返回 `ERR UNKNOWN CMD`。收到的 BLE 数据也会转发到 WBA UART1，
所以虚拟机的 `/dev/ttyUSB0` 能看到 MP257 发出的命令。连接状态会输出：

```text
[BLE] CH9140 link ready
[BLE] CH9140 link down
```

## WBA 串口和 ST-LINK

WBA UART1：`115200 8N1`，无硬件流控。

- PB12：USART1_TX
- PA8：USART1_RX
- GND：必须共地
- I/O 电平：3.3 V，不能接 RS-232 电平

裸 E04-2G4M10S1AX 模块的 ST-LINK 接线：

| ST-LINK | E04 模块脚 | MCU 信号 |
|---|---:|---|
| SWCLK | 22 | PA14 / SWCLK |
| SWDIO | 23 | PA13 / SWDIO |
| GND | 任一 GND（例如 27） | GND |
| VTref/VAPP | 13 的 3.3 V 电源域 | VDD 参考 |
| NRST（推荐） | 26 | NRST |

EWT04 测试底板 2×15 排针：

| ST-LINK | 测试底板排针 |
|---|---:|
| SWCLK | 20（PA14） |
| SWDIO | 21（PA13） |
| GND | GND |
| NRST（推荐） | 24 |

目标板若已由 Type-C/底板供电，不要再用 ST-LINK 3.3 V 强行供电。至少连接
VTref、SWDIO、SWCLK、GND，推荐再接 NRST。

## 外接左右 LED

EWT04 测试底板的公开排针只提供 3.3 V，没有可供外设使用的 5 V 输出。板上
Type-C 的 VBUS 会先经过稳压电路生成 3.3 V，不建议从焊盘私自引出 VBUS。

| 用途 | MCU 引脚 | 测试底板排针序号 | 电平 |
|---|---|---:|---|
| 左后方风险灯 | PA7 | 3 | 高电平点亮 |
| 右后方风险灯 | PA5 | 5 | 高电平点亮 |

2026-08-10更换后的左右风险灯与MP257的PD11告警灯同款，按高电平有效配置：
PA7/PA5输出高电平时点亮，输出低电平时熄灭。PA2底板D1没有更换，仍为低电平
有效。不要省略限流电阻，也不要由GPIO直接驱动大功率LED。若灯具必须使用5V，
请使用独立5V电源和两路MOSFET开关，WBA与5V电源必须共地，PA7/PA5只接
支持3.3V高电平触发的MOSFET控制输入。

`RISK LEFT/RIGHT/CENTER`不是持续常亮，而是执行一次非阻塞时序：先常亮1秒，
再以200ms灭/200ms亮闪烁3次，短暂熄灭200ms后常亮1秒，最后自动熄灭，
总时长约3.4秒。新风险命令会立即按新方向重新开始；`RISK CLEAR`会把当前风险灯
保持1秒再熄灭，期间若收到新风险则立即切换并重新开始。BLE断开或复位仍立即熄灯，
避免断链后保留陈旧告警。`LEFT/RIGHT ON/OFF`保留为人工GPIO测试命令，不执行该时序。

蓝牙断开或 WBA 复位时，两路外接方向灯默认熄灭。

## 编译和烧录 WBA

从新电脑获取完整仓库：

```bash
git clone https://github.com/xxluestc/mp257.git
cd mp257/E04-2G4M10S1AX
```

1. 在Windows安装Keil MDK及支持STM32WBA54的STM32WBA Device Family Pack。
2. 打开`MDK-ARM/02_test.uvprojx`；不要只复制单个`.c`文件。
3. 选择目标 `E04_BLE_UART`，执行 Rebuild。
4. 通过 ST-LINK 下载新固件。

之前烧录的 `WBA-UART` Peripheral 版不能连接 CH9140，必须重新编译并烧录
当前 Central 版。工程沿用 Ebyte 官方示例的芯片、Flash、时钟和无线协议栈
设置。

仓库保留了Keil工程、启动文件、链接脚本、CMSIS/HAL、无线协议栈与应用源码；
忽略`Objects/`、`Listings/`、`.hex`等本机生成物。新电脑Rebuild后会重新生成
可烧录文件，因此不依赖旧电脑的构建目录。

## MP257 当前状态

MP257 J15：

- PA4 / USART2_TX → CH9140 RX
- PA8 / USART2_RX ← CH9140 TX
- Linux 设备：`/dev/ttySTM0`
- 参数：`115200 8N1`

实机已完成并验证：

- 从当前实际 U-Boot 启动项中删除 `console=ttySTM0,115200`。
- 从当前实际 DTB `/chosen` 中删除 `stdout-path=serial0`。
- 停止并屏蔽 `serial-getty@ttySTM0.service`。
- 重启后 `/sys/class/tty/console/active` 仅为 `tty0`。

板端备份仍保留在：

```text
/boot/mmc1_extlinux/myb-stm32mp257x-2GB_extlinux.conf.before-bluetooth-20260807
/boot/myb-stm32mp257x-2GB.dtb.before-bluetooth-console-20260807
```

## 联调

先在虚拟机观察 WBA 串口（当前识别为 `/dev/ttyUSB0`）：

```bash
python3 -m serial.tools.miniterm /dev/ttyUSB0 115200
```

若没有安装 pyserial，也可以：

```bash
picocom -b 115200 /dev/ttyUSB0
```

WBA 烧录完成、两板上电后，应先看到 `[BLE] CH9140 link ready`。然后在
MP257 SSH 中运行：

```sh
/root/ble_link/check_uart.sh
python3 /root/ble_link/ble_uart_test.py --send 'PING' --line --listen 5
python3 /root/ble_link/ble_uart_test.py --send 'LED ON' --line --listen 5
python3 /root/ble_link/ble_uart_test.py --send 'LED OFF' --line --listen 5
python3 /root/ble_link/ble_uart_test.py --send 'RISK LEFT' --line --listen 5
python3 /root/ble_link/ble_uart_test.py --send 'RISK RIGHT' --line --listen 5
python3 /root/ble_link/ble_uart_test.py --send 'RISK CENTER' --line --listen 5
python3 /root/ble_link/ble_uart_test.py --send 'RISK CLEAR' --line --listen 5
```

预期分别收到对应ACK；`LED ON/OFF`控制PA2底板D1，三条`RISK`命令应让左灯、
右灯、双灯依次完成“常亮→闪3次→常亮→熄灭”的时序；在风险时序进行中发送
`RISK CLEAR`，当前方向灯应再保持1秒后熄灭。
如果 WBA 一直显示 link down，首先检查 CH9140 的 `BLE_MODE` 是否为高/悬空、
波特率引脚是否为 `111`，以及天线和供电。

诊断固件每 3 秒输出一次当前阶段，例如：

```text
[STATUS] GAP=scanning
[STATUS] GAP=connecting
[STATUS] GAP=connected GATT=discover-FFF0
[STATUS] GAP=connected GATT=discover-FFF1-FFF2
[STATUS] GAP=connected GATT=discover-CCCD
[STATUS] GAP=connected GATT=ready
```

同时会输出扫描命中、连接、GATT 发现、Notify 订阅、错误阶段以及断线重连
事件。这些日志由项目自己的 UART 环形缓冲输出，不依赖可能被关闭的
`LOG_INFO_APP`。

扫描状态还会区分射频层是否收到过任意 BLE 广播：

```text
[STATUS] GAP=scanning RF=no-advertisements
[STATUS] GAP=scanning RF=advertisements-seen target=no
```

前者表示扫描命令运行但尚未收到任何广播报告；后者表示 WBA 射频接收正常，
但没有找到名称为 `CH9140BLE2U` 或带 `FFF0` 服务 UUID 的目标。

启动诊断还会直接使用轮询 UART 输出，不经过 BLE 调度器：

```text
[BOOT] USART1 PB12/PA8 ready
[BOOT] ICACHE ready
[BOOT] RAMCFG ready
[BOOT] RNG ready
[BOOT] RTC ready
[BOOT] UART RX DMA ready
[BOOT] entering BLE init
[BOOT] BLE init returned
[HEARTBEAT] main loop alive
```

如果某个 HAL 初始化失败，会持续输出 `[FATAL] halted` 并翻转 D1。这样可以
区分初始化失败、BLE 初始化卡住、错误烧录和 CH340/UART 硬件通路问题。

## 硬件资料

- E04-2G4M10S1AX：<https://www.ebyte.com/product/2687.html>
- EWT04-2G4M10S1AX：<https://www.ebyte.com/product/2688.html>
- CH9140 数据手册：<https://www.wch.cn/downloads/CH9140DS1_PDF.html>

## 实机验证记录

2026-08-07 在真实 STM32MP257、CH9140 和 E04-2G4M10S1AX 上验证：

| 项目 | 结果 |
|---|---|
| WBA UART `/dev/ttyUSB0` | 115200 8N1 日志正常 |
| WBA 扫描 CH9140 | CH9140 接通 3.3 V 后成功 |
| GAP/GATT | `GAP=connected GATT=ready` |
| MP257 `PING` | WBA 收到 `PING`，MP257 收到 `PONG` |
| `LED ON` | MP257 收到 `ACK LED ON` |
| `LED OFF` | MP257 收到 `ACK LED OFF` |
| 断线情况 | 指令测试后仍保持 GATT ready |
| MP257 USART2 | 无 console/getty 占用，检查结果 READY |

本次最初一直停留在 `GAP=scanning` 的直接原因是 CH9140 未接 3.3 V，而不是
WBA 射频、BLE Central 实现或 GATT 客户端故障。

2026-08-10因外接灯更换为与PD11同款，固件已由低电平有效改为高电平有效，并增加
有限时长告警时序。该版需要重新用Keil Rebuild、烧录后复测PA7/PA5；上表属于
旧灯实测记录，不能替代新灯极性与时序验收。

MP257 雷达自动联动、外接方向灯接线、无雷达测试、排障过程和完整实机验证
记录见
[`mier/lyr/camera_detect/docs/BLUETOOTH_DIRECTION_LED.md`](../mier/lyr/camera_detect/docs/BLUETOOTH_DIRECTION_LED.md)。
