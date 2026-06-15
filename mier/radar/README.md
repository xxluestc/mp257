# 雷达模块 (MS60-1211S80M-BSD / AT6010)

## 开发板信息

- **开发板 IP**: `192.168.88.10`
- **SSH 用户**: `root` (免密登录)
- **目标部署路径**: `/xxl/radar/`

## 硬件接口

| 开发板引脚 | 功能 | 连接 |
|-----------|------|------|
| PG14 (AF6) | USART1_TX | 雷达 RX |
| PG15 (AF6) | USART1_RX | 雷达 TX |

- 设备树已正确配置，映射到串口 `/dev/ttySTM1`
- 波特率: **921600**

## 交叉编译器

```bash
export PATH=/home/alientek/Phytium_syscode/GCC编译器/gcc-arm-10.2-2020.11-x86_64-aarch64-none-linux-gnu/bin:$PATH
```

编译器版本: `aarch64-none-linux-gnu-gcc (GNU Toolchain 10.2.1)`

## 编译命令

```bash
cd /home/alientek/dvr_project/mier/radar
export PATH=/home/alientek/Phytium_syscode/GCC编译器/gcc-arm-10.2-2020.11-x86_64-aarch64-none-linux-gnu/bin:$PATH
aarch64-none-linux-gnu-gcc -Wall -O2 -o radar_init radar_init.c
```

## 传输到开发板

```bash
scp /home/alientek/dvr_project/mier/radar/radar_init root@192.168.88.10:/xxl/radar/
ssh root@192.168.88.10 'chmod +x /xxl/radar/radar_init'
```

## 运行

```bash
ssh root@192.168.88.10 '/xxl/radar/radar_init'
```

程序会先发送初始化命令序列，然后进入持续接收循环，按 **Ctrl+C** 退出。

## 雷达协议参考

- 手册位置: `/home/alientek/radar/60GBSD汽车检测AT6010 SOC HCI Protocol_V1.4.pdf`
- 发送帧头: `0x58`
- 回复帧头: `0x59`
- 上报帧头: `0x5A`
- BSD 上报 TYPE: `7`
