# 项目文档总览

[返回项目介绍](../README.md)

本页集中整理系统架构、工程说明、实验验证和设备维护资料。文档随对应工程源码维护，以下链接指向各模块的现有说明。

## 架构与事件链

| 文档 | 内容 |
|---|---|
| [系统数据流](../mier/lyr/camera_detect/docs/DATA_FLOW.md) | 摄像头、NPU、雷达、IMU、导航与 DVR 的数据流转。 |
| [事件链与演示说明](../mier/lyr/camera_detect/docs/COMPETITION_PREPARATION.md) | 典型事件的入口、判断、输出、验证证据及实现边界。 |
| [摔倒与手机短信链路](../mier/lyr/camera_detect/docs/FALL_SMS_PIPELINE.md) | M33 → A35 → HUD → 手机的事件投递及确认范围。 |

## 工程入口

| 工程 | 内容 |
|---|---|
| [A35 核心工程](../mier/lyr/camera_detect/README.md) | `radar_fusion`、HUD、Dashboard、运行资源与编译部署说明。 |
| [WBA 蓝牙方向灯](../E04-2G4M10S1AX/README.md) | STM32WBA54 Central、CH9140、左右灯控制及 Keil 工程。 |
| [双 WBA V2V 实验](../V2V_keil/E04-2G4M10S1AX/README.md) | GPS/IMU、BLE 广播与扫描、相对风险和 MP3 语音。 |
| [跨电脑编译部署交接](../mier/lyr/camera_detect/docs/跨电脑编译部署交接.md) | 主机环境、A35 部署与两套 WBA 固件的烧录范围。 |

## 实验与可靠性

| 文档 | 内容 |
|---|---|
| [雷达实验与人工标注](../mier/lyr/camera_detect/docs/RADAR_EXPERIMENT.md) | 危险目标选择、方向滤波、CSV 字段及场景标定。 |
| [蓝牙方向灯联调](../mier/lyr/camera_detect/docs/BLUETOOTH_DIRECTION_LED.md) | MP257 → CH9140 → WBA 的接线、协议和实机记录。 |
| [DVR 可靠性与 TF 存储](../mier/lyr/camera_detect/docs/DVR_RELIABILITY.md) | 录像校验、TF 挂载、故障隔离和恢复验证。 |
| [运行可靠性与存储](../mier/lyr/camera_detect/docs/RUNTIME_STORAGE.md) | 日志容量、数据轮转和运行存储策略。 |
| [启动优化与回退](../mier/lyr/camera_detect/docs/BOOT_OPTIMIZATION.md) | 启动链、M33 启动方案、优化记录和恢复方式。 |

## 部署与日常维护

| 文档 | 内容 |
|---|---|
| [日常操作指南](../mier/lyr/camera_detect/docs/操作指南.md) | 开机、状态检查、录像、实验和现场排障。 |
| [板端程序与操作](../mier/lyr/camera_detect/docs/BOARD_PROGRAMS_AND_OPERATIONS.md) | 开发板程序、服务和操作入口。 |
| [设备树说明](../mier/lyr/camera_detect/board/DEVICE_TREE.md) | 板级 DTS 与音频硬件配置。 |
| [OTA 操作指南](../mier/lyr/camera_detect/docs/OTA.md) | A35 应用打包、安装事务、健康检查和回滚。 |
| [OTA HTTP API](../mier/lyr/camera_detect/docs/OTA_API.md) | 手机侧调用的升级服务接口。 |
| [OTA 联调交接](../mier/lyr/camera_detect/docs/OTA_HANDOFF.md) | Android、云端与板端的升级分工。 |
| [1.0.8 发布说明](../mier/lyr/camera_detect/docs/RELEASE_1.0.8.md) | 已跟踪发布包、校验值与演示验收记录。 |

OTA 覆盖 A35 应用和运行资源，M33 固件、内核、设备树及系统镜像由各自的部署流程管理。

## 开发参考

- [项目技术知识库](../mier/lyr/camera_detect/docs/PROJECT_TECHNICAL_KNOWLEDGE_BASE.md)
- [项目讲解与面试准备](../mier/lyr/camera_detect/docs/INTERVIEW_PREPARATION.md)

## 仓库维护约定

- 根目录 README 用于项目介绍，文档总览用于查找工程与维护资料。
- 当前 A35 代码统一维护在 `mier/lyr/camera_detect`，两套 WBA 固件分别维护。
- 历史实现和实验性变更通过 Git 历史或分支管理。
- 运行日志、录像、实验 CSV、现场配置和 IDE 临时文件由各工程的忽略规则排除。
- 普通构建产物不提交；明确交付的发布包及校验文件按发布文档管理。
- 厂商依赖及其许可证随工程保留，设备树保存可复现的 DTS 源码。
