# 雷达模块 (MS60-1211S80M-BSD / AT6010)

## 系统架构

```
雷达进程 (radar_link)                  DVR进程 (dvr + NPU)
───────────────────                   ──────────────────────
  /dev/ttySTM1 (UART)                    /dev/video6 (摄像头, 独占)
  │                                       │
  ├─ 目标出现 → TARGET_ON ──管道──→  收到TARGET_ON → 启动缓冲 + NPU验证
  ├─ TTC<10s → COLLISION  ──管道──→  收到COLLISION → NPU确认后保存视频
  ├─ LED闪烁控制                       │
  └─ 目标消失 → TARGET_OFF ──管道──→  收到TARGET_OFF → 停止录制
         │                                       │
         └────── /tmp/dvr_trigger_pipe (命名管道) ──┘
```

- **雷达程序** (`pro/radar_link.c`)：纯雷达 UART 解析 + TTC 判断 + LED 控制 + 命名管道事件发送
- **DVR程序** (`dvr/`)：摄像头录制 + NPU 目标验证 + 视频保存
- **NPU融合**：集成在DVR进程内，避免摄像头冲突

## 开发板信息

- **开发板 IP**: `192.168.88.10`
- **SSH 用户**: `root` (免密登录)
- **雷达部署路径**: `/xxl/pro/radar_link`
- **联调启动脚本**: `/xxl/pro/start_pro.sh`

## 硬件接口

| 开发板引脚 | 功能 | 连接 |
|-----------|------|------|
| PG14 (AF6) | USART1_TX | 雷达 RX |
| PG15 (AF6) | USART1_RX | 雷达 TX |

- 设备树已正确配置，映射到串口 `/dev/ttySTM1`
- 波特率: **921600**

## 设备树配置

设备树文件: `/home/alientek/dvr_project/mier/dts/myb-stm32mp257x-2GB.dts`

USART1 节点已配置:
```dts
&usart1 {
    pinctrl-names = "default", "sleep";
    pinctrl-0 = <&usart1_pins_a>;
    pinctrl-1 = <&usart1_sleep_pins_a>;
    status = "okay";
};
```

## 编译

### 雷达程序 (pro/radar_link.c)

```bash
cd /home/alientek/dvr_project/mier/pro
export PATH=/home/alientek/Phytium_syscode/GCC编译器/gcc-arm-10.2-2020.11-x86_64-aarch64-none-linux-gnu/bin:$PATH
make                        # 编译
make deploy                 # 部署到开发板
```

### 雷达原始程序 (radar/radar_init.c)

```bash
cd /home/alientek/dvr_project/mier/radar
export PATH=/home/alientek/Phytium_syscode/GCC编译器/gcc-arm-10.2-2020.11-x86_64-aarch64-none-linux-gnu/bin:$PATH
aarch64-none-linux-gnu-gcc -Wall -O2 -o radar_init radar_init.c
```

## 运行

### 联调模式 (雷达 + DVR)

```bash
ssh root@192.168.88.10
cd /xxl/pro
./start_pro.sh              # 启动DVR(后台) + radar_link(前台)
./start_pro.sh stop         # 停止所有
```

### 独立运行雷达

```bash
ssh root@192.168.88.10 '/xxl/pro/radar_link'
```

程序会先发送初始化命令序列，然后进入持续接收循环，按 **Ctrl+C** 退出。

## 雷达协议参考

- 手册位置: `/home/alientek/radar/60GBSD汽车检测AT6010 SOC HCI Protocol_V1.4.pdf`
- 发送帧头: `0x58`
- 回复帧头: `0x59`
- 上报帧头: `0x5A`
- BSD 上报 TYPE: `7`

## 触发逻辑

| 条件 | 雷达动作 | DVR响应 |
|------|---------|--------|
| 目标出现 (obj_count > 0) | 发送 `TARGET_ON` | 开始缓冲录制 + NPU验证 |
| TTC < 10s | 发送 `COLLISION` + LED闪烁 | NPU确认后保存前后15s视频 |
| 目标消失 (obj_count == 0) | 发送 `TARGET_OFF` | 停止录制 |

## 相关文件

| 文件 | 说明 |
|------|------|
| `pro/radar_link.c` | 联调雷达程序 (UART + TTC + 管道事件) |
| `pro/Makefile` | 编译脚本 |
| `pro/start_pro.sh` | 联调启动脚本 |
| `radar/radar_init.c` | 原始雷达测试程序 (仅打印) |
| `dvr/` | DVR 行车记录程序 (含NPU融合) |