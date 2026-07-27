# STM32MP257 骑行辅助项目

本目录只维护当前有效的 A35 Linux 侧核心工程：

```text
mier/
├── .gitignore
├── README.md
└── lyr/
    └── camera_detect/       # 唯一开发、编译和部署入口
```

核心代码位于 [`lyr/camera_detect`](lyr/camera_detect/README.md)，主程序为
`radar_fusion`，集成摄像头、NPU、雷达、DVR、HUD、音频、LED、IMU/V2X
以及本地雷达 Dashboard。

## 仓库维护规则

- 只在 `mier/lyr/camera_detect` 维护当前版本，不在 `mier` 下复制工程。
- 实验性改动使用 Git 分支或提交记录管理，不新增 `old`、`bak`、`v2` 副本。
- 编译产物、运行日志、录像、CSV、现场配置和大型厂商 SDK 不提交。
- 根目录 README 保持为索引；编译、部署、测试说明随核心代码维护。
- 设备树只保存可复现的 `.dts` 源文件，不提交生成的 `.dtb`。

本机的 `myir-st-linux/`、`v2x/` 和 `dvr_videos/` 属于 SDK 或运行数据，
已被忽略，不会出现在 GitHub 仓库中。

## 快速入口

```bash
cd /home/alientek/dvr_project/mier/lyr/camera_detect
make
```

详细说明：

- [核心工程 README](lyr/camera_detect/README.md)
- [日常操作指南](lyr/camera_detect/docs/操作指南.md)
- [数据流](lyr/camera_detect/docs/DATA_FLOW.md)
- [雷达实验与标注](lyr/camera_detect/docs/RADAR_EXPERIMENT.md)
- [M33 摔倒与短信投递链](lyr/camera_detect/docs/FALL_SMS_PIPELINE.md)
- [启动优化与回退](lyr/camera_detect/docs/BOOT_OPTIMIZATION.md)
- [A35 应用层 OTA](lyr/camera_detect/docs/OTA.md)
- [Android OTA HTTP API](lyr/camera_detect/docs/OTA_API.md)
- [设备树说明](lyr/camera_detect/board/DEVICE_TREE.md)
