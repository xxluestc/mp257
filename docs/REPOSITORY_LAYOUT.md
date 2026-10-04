# 工程分布与源码迁移

| 目录 | 职责 | 入口 |
|---|---|---|
| apps/a35 | MP257 A35 Linux 感知、融合、录像与维护 | Makefile；src/app/main.cpp |
| firmware/ble_direction | WBA54 Central、CH9140 风险命令与方向灯 | MDK-ARM/02_test.uvprojx |
| firmware/v2v | 双 WBA54 BLE/GPS/IMU/MP3 协同实验 | MDK-ARM/02_test.uvprojx |
| docs | 总览、工程约定与应用设计 | README.md |

原 mier/lyr/camera_detect 对应 apps/a35；原 E04-2G4M10S1AX 对应 firmware/ble_direction；原 V2V_keil/E04-2G4M10S1AX 对应 firmware/v2v。Keil 内部相对路径保持一致，厂商生成布局与许可证保留。

A35 业务源在 src，接口在 include，主机测试在 tests，构建产物在 build。HUD、Dashboard 与 OTA 分别构建或运行。现场配置、日志、录像和 IDE 状态被忽略。板端 /xxl/camera_detect、可执行文件与服务名保持现有接口。

Git 历史保留迁移前源码；当前构建只有一套生产入口。历史包和实机记录保留其版本归属，本次源码见[主机检查报告](HOST_VALIDATION.md)。
