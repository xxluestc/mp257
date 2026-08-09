# STM32MP257 主动安全骑行辅助系统

当前仓库维护两个可直接构建和部署的工程：

```text
mp257/
├── mier/lyr/camera_detect/   # STM32MP257 A35 Linux核心业务
└── E04-2G4M10S1AX/           # STM32WBA54 + CH9140 BLE方向灯固件
```

旧的`A_Core`、`recorder`、`camera`等目录是早期实验遗留，不是当前业务入口。
当前录像、雷达、NPU、HUD、Dashboard、OTA与运维代码均以
[`mier/lyr/camera_detect`](mier/lyr/camera_detect/README.md)为准。

## 从新电脑开始

```bash
git clone https://github.com/xxluestc/mp257.git
cd mp257/mier/lyr/camera_detect
make clean
make
make deploy-radar BOARD_IP=192.168.88.10
```

A35主机需要AArch64交叉编译器、Python 3与make，并能够通过SSH/SCP登录开发板。
现场`/xxl/camera_detect/radar_config`不会被部署目标覆盖。

WBA固件在Windows中用Keil打开
[`E04-2G4M10S1AX/MDK-ARM/02_test.uvprojx`](E04-2G4M10S1AX/MDK-ARM/02_test.uvprojx)，
安装STM32WBA54 Device Family Pack后选择`E04_BLE_UART`执行Rebuild，再用
ST-LINK烧录。完整接线、LED低有效极性和联调方法见
[WBA工程说明](E04-2G4M10S1AX/README.md)。

## 当前运行结论

- A35录像与实验CSV使用板载`/usr/local/helmet` userfs/ext4，不依赖外置TF。
- 当前TF卡已确认持久写入不可靠并安全卸载，应更换；新卡默认只用于人工导入导出。
- Dashboard监听`0.0.0.0:8080`，网线和WiFi均使用8080端口，IP随接口变化。
- Dashboard提供TF挂载/弹出、安全停止项目和安全关机；命令行也有等价脚本。
- 风险录像在正式提交前经过MP4结构、ffprobe和整段解码校验。
- 雷达LEFT/CENTER/RIGHT会经MP257 USART2、CH9140 BLE传给WBA方向灯。

## 主要文档

- [A35核心工程](mier/lyr/camera_detect/README.md)
- [日常操作指南](mier/lyr/camera_detect/docs/操作指南.md)
- [录像可靠性与TF故障复盘](mier/lyr/camera_detect/docs/DVR_RELIABILITY.md)
- [比赛演示与事件链](mier/lyr/camera_detect/docs/COMPETITION_PREPARATION.md)
- [雷达实验与人工标注](mier/lyr/camera_detect/docs/RADAR_EXPERIMENT.md)
- [A35 OTA](mier/lyr/camera_detect/docs/OTA.md)
- [WBA蓝牙方向灯](E04-2G4M10S1AX/README.md)

运行日志、录像、实验CSV、现场配置、构建产物与IDE临时文件不提交；需要回看旧实现时
使用Git历史，不在当前工程中新增`old`、`bak`或版本副本。
