# V2V 双终端实验工程

当前目录只维护一套STM32WBA54双终端V2V实验固件：

- 工程入口：[`E04-2G4M10S1AX/MDK-ARM/02_test.uvprojx`](E04-2G4M10S1AX/MDK-ARM/02_test.uvprojx)
- 完整说明：[`E04-2G4M10S1AX/README.md`](E04-2G4M10S1AX/README.md)

它让两个WBA终端同时广播和扫描GPS/IMU状态，并在本端判断相对风险后通过UART2控制
MP3模块。它不是仓库根目录的CH9140方向灯固件，也不是MP257 A35业务程序。

仓库保留源码、Keil工程、头文件和工程明确引用的ST预编译库；不保留旧HEX、AXF、对象
文件、构建日志和Keil用户配置。换电脑后必须执行Clean和Rebuild，不能使用本地残留固件。

若由MP257的M33侧向WBA提供可选外部状态，MP257端源码归属
`/home/alientek/STM32Cube_ATK_FW_MP2_V1.0.0`。当前该BSP中尚未找到与WBA外部状态协议
匹配的发送实现，不能把通用OpenAMP回显示例当作已完成的V2V对接代码。
