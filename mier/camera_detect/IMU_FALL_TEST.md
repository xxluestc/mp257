# M33 IMU 摔倒触发 DVR 测试指南

## 一、前置准备

1. 开发板上电，确认 A35 Linux 已启动
2. 确认 M33 固件已启动（/dev/ttyRPMSG0 存在）
3. 确认摄像头 /dev/video7、雷达 /dev/ttySTM1、TF 卡 /run/media/mmcblk0p1 可用
4. 确认告警音频文件存在：
   ```bash
   ls -la /xxl/camera_detect/sounds/
   # 应有 fall_alert.wav / collision_alert.wav / v2x_alert.wav
   ```

## 二、启动 M33 固件

```bash
ssh root@192.168.88.10
cd /home/root/project
./fw_cortex_m33.sh start
ls -la /dev/ttyRPMSG0   # 确认设备节点存在
```

## 三、启动 radar_fusion

### 3.1 真实运行（等待 IMU 真实摔倒事件）

```bash
ssh root@192.168.88.10 'cd /xxl/camera_detect && LD_LIBRARY_PATH=/usr/lib:/vendor/lib:/xxl/camera_detect/stai_mpu ./radar_fusion'
```

触发条件：
- M33 检测到 IMU 摔倒（剧烈冲击 + 倾斜）后，通过 RPMSG 发送 `IMU_ALERT ... type=fall`
- A35 收到后触发 DVR 保存

### 3.2 模拟测试模式（无需物理晃动 IMU）

使用 `-t N` 参数，N 秒后自动模拟一次摔倒事件：

```bash
# 5 秒后模拟摔倒触发
ssh root@192.168.88.10 'cd /xxl/camera_detect && LD_LIBRARY_PATH=/usr/lib:/vendor/lib:/xxl/camera_detect/stai_mpu timeout 40 ./radar_fusion -t 5'
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

### 4.2 单独监听 M33 RPMSG 输出

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

## 六、常见问题

1. **/dev/ttyRPMSG0 不存在**：M33 固件未启动，执行 `./fw_cortex_m33.sh start`
2. **没有声音**：检查 `/xxl/camera_detect/sounds/fall_alert.wav` 是否存在，以及 `aplay -l` 能否看到 MAX98357A 声卡
3. **没有生成 MP4**：检查 TF 卡是否挂载到 `/run/media/mmcblk0p1`，以及 `which ffmpeg` 是否有输出
4. **程序启动失败**：确认摄像头 /dev/video7 未被其他程序占用，必要时执行 `fuser -k /dev/video7`
