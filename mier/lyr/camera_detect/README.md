# camera_detect 核心项目

这是仓库当前重点维护的主项目，面向 STM32MP257 开发板，整合摄像头
NPU 识别、毫米波雷达、DVR、IMU/V2X 告警、HUD/OLED 与导航语音功能。

## 目录

```text
camera_detect/
├── camera_detect/   # 融合检测、DVR 和音频主程序
├── hud_project/     # HUD/OLED 显示程序
└── AI写的文档.md    # 设计参考
```

主要入口：

- `camera_detect/radar_fusion.cpp`：融合检测主程序
- `camera_detect/start_dvr.sh`：板端启动脚本
- `camera_detect/Makefile`：编译与部署入口
- `camera_detect/README.md`：功能、参数和部署说明
- `hud_project/oled.c`：OLED/HUD 显示逻辑

## 仓库约定

- 源码、脚本、模型、必要的板端运行库和文档纳入 Git。
- 编译生成的 `radar_fusion`、`hud` 等可执行文件不纳入 Git。
- 录像、日志、DVR 临时缓冲和本地 SDK 不纳入 Git。
- 当前核心代码以本目录为准；`mier/camera_detect/` 仅作为历史/对照副本，
  后续修改应优先落在本目录。

具体编译、部署和运行方式见
[`camera_detect/README.md`](camera_detect/README.md)。
