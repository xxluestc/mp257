# M33 IMU 摔倒触发 DVR 测试指南

## 一、前置准备

1. 开发板上电，确认 A35 Linux 已启动
2. 确认摄像头 `/dev/video7`、雷达 `/dev/ttySTM1`、TF 卡 `/run/media/mmcblk0p1` 可用
3. 确认告警音频文件存在：
   ```bash
   ls -la /xxl/camera_detect/sounds/
   # 应有 fall_alert.wav / collision_alert.wav / v2x_alert.wav
   ```

## 二、一键启动（推荐）

已提供整合脚本 `start_dvr.sh`，自动完成：
- 清理摄像头/雷达占用
- 启动 M33 固件
- 等待 `/dev/ttyRPMSG0` 就绪
- 启动 `radar_fusion`
- 退出时自动停止 M33 固件

终端**只显示关键事件**，完整日志写入 `/xxl/camera_detect/dvr_system.log`。

### 2.1 真实室外运行

```bash
ssh root@192.168.88.10
cd /xxl/camera_detect
./start_dvr.sh
```

终端会实时显示：
- `[关键] NPU: ROAD USER DETECTED` — 出现目标
- `[关键] *** NPU CONFIRMED: Real road user! ***` — NPU 确认
- `[关键] *** FALL DETECTED! ***` — IMU 摔倒
- `[关键] [AUDIO] Playing fall alert` — 音频提示
- `[保存] [DVR] Child: Saved /run/media/mmcblk0p1/dvr/emergency_...` — 视频保存路径

按 `Ctrl+C` 停止，脚本会在几秒种内清理并停止 M33 固件。

完整日志可另开终端查看：
```bash
ssh root@192.168.88.10 'tail -f /xxl/camera_detect/dvr_system.log'
```

### 2.2 模拟测试模式（无需物理晃动 IMU）

```bash
# 启动 10 秒后自动模拟摔倒
ssh root@192.168.88.10 'cd /xxl/camera_detect && ./start_dvr.sh -t 10'
```

> 模拟模式同样会触发 LED、音频、DVR 保存，适合出门前快速验证端到端流程。

## 三、手动分步启动（调试用）

如需单独调试某个模块，可按以下步骤手动执行：

### 3.1 启动 M33 固件

```bash
ssh root@192.168.88.10
cd /home/root/project
./fw_cortex_m33.sh start
ls -la /dev/ttyRPMSG0   # 确认设备节点存在
```

### 3.2 启动 radar_fusion

```bash
ssh root@192.168.88.10 'cd /xxl/camera_detect && LD_LIBRARY_PATH=/usr/lib:/vendor/lib:/xxl/camera_detect/stai_mpu ./radar_fusion -t 5'
```

预期输出关键日志：
```
[TEST] Simulating IMU fall after 5 seconds...
[TEST] Injecting simulated FALL event
[IMU] *** FALL DETECTED! *** Triggering emergency save
[AUDIO] Playing fall alert
[DVR] Save triggered! (pre=15s, post=15s)
...
[DVR] Post-trigger recording complete (XXXX ms)
[DVR] Child: Saved /run/media/mmcblk0p1/dvr/emergency_YYYYMMDD_HHMMSS.mp4
```

## 四、验证结果

### 4.1 检查生成的视频

```bash
ssh root@192.168.88.10 'ls -lh /run/media/mmcblk0p1/dvr/emergency_*.mp4 && ffprobe -v error -show_entries format=duration -of default=noprint_wrappers=1 /run/media/mmcblk0p1/dvr/emergency_*.mp4'
```

时长预期：
- 摔倒时已有目标缓冲 ≥15s：约 30s（15s 前 + 15s 后）
- 摔倒时无目标缓冲：约 15s（仅摔倒后 15s）
- 缓冲不足 15s：介于 15s ~ 30s 之间（本次测试约为 19-20s）

### 4.2 拷贝视频到虚拟机

```bash
# 单个文件
scp root@192.168.88.10:/run/media/mmcblk0p1/dvr/emergency_YYYYMMDD_HHMMSS.mp4 ~/

# 当天所有视频
scp root@192.168.88.10:/run/media/mmcblk0p1/dvr/emergency_*.mp4 ~/dvr_videos/
```

### 4.3 单独监听 M33 RPMSG 输出

```bash
ssh root@192.168.88.10
cd ~
./a35_read_v2x_alerts.sh
```

正常应能看到 IMU 状态/告警文本，例如：
```
IMU_ALERT type=fall ...
```

## 五、清理与停止

```bash
# 停止 M33 固件
ssh root@192.168.88.10 'cd /home/root/project && ./fw_cortex_m33.sh stop'

# 清理测试视频
ssh root@192.168.88.10 'rm -f /run/media/mmcblk0p1/dvr/emergency_*.mp4'
```

## 六、关于保存延迟的说明

触发后到 MP4 文件可用，中间有固定延迟：

1. **必须等待 15 秒 post-trigger 录制**（设计需求）
2. **ffmpeg 编码耗时**（通常几秒到十几秒）

正常工作时主进程不会被阻塞：编码在子进程中异步执行，主进程继续跑摄像头/NPU/雷达检测。但在编码完成前，新的 DVR 缓冲不会启动，避免多个 ffmpeg 同时运行压垮系统。

## 七、常见问题

1. **`/dev/ttyRPMSG0` 不存在**：M33 固件未启动，执行 `./fw_cortex_m33.sh start`
2. **没有声音**：检查 `/xxl/camera_detect/sounds/fall_alert.wav` 是否存在，以及 `aplay -l` 能否看到 MAX98357A 声卡
3. **没有生成 MP4**：检查 TF 卡是否挂载到 `/run/media/mmcblk0p1`，以及 `which ffmpeg` 是否有输出
4. **程序启动失败**：确认摄像头 `/dev/video7` 未被其他程序占用，必要时执行 `fuser -k /dev/video7`
5. **`Ctrl+C` 后卡住**：旧版脚本可能出现，新版 `start_dvr.sh` 已加入 3 秒超时 + SIGKILL 强制清理，通常 4-5 秒内退出
