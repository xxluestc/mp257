# 雷达数据增强、Dashboard 与实测标注

## 数据流

```text
BSD 串口帧
  └─ radar_fusion
      ├─ 保留本帧最多 8 个 objId
      ├─ 每个目标计算 distance / velocity / angle / TTC / direction
      ├─ 选择当前危险目标
      ├─ 追加 radar_data.csv
      └─ 原子更新 radar_state.json
                         │
                         └─ radar_dashboard.py
                             ├─ 浏览器实时状态与曲线
                             └─ 人工开始/结束标签 → labels.csv
```

Dashboard 同时消费雷达、摄像头/NPU、M33 IMU 和 HUD 投递日志；NPU、DVR、
LED、Audio 与 M33 摔倒决策本身没有重构。

## 危险目标选择

每个有效目标计算：

```text
distance_ratio = distance / DIST
ttc_ratio      = TTC / TTC_THRESHOLD     # 仅速度 < 0 的靠近目标有效
risk           = min(distance_ratio, ttc_ratio)
```

`risk` 最小的 `objId` 是当前危险目标。告警仍严格沿用原条件：
`TTC < TTC_THRESHOLD` 或 `distance <= DIST`。这样距离、速度、角度和
TTC 始终来自同一个目标，不会再把不同目标的最短距离和最小 TTC 混在一起。

方向先把雷达协议坐标转换为骑行者坐标：

```text
rider_angle = sensor_angle × ANGLE_SIGN
rider_angle <= LEFT_ANGLE    → LEFT
rider_angle >= RIGHT_ANGLE   → RIGHT
其他                         → CENTER
```

角度先经过按 `objId` 独立维护的 EMA，再经过 2° 边界滞回和连续帧确认，
避免阈值附近频繁跳变。

## 配置

将 [`radar_config.example`](../radar_config.example) 复制为开发板上的
`/xxl/camera_detect/radar_config`：

```ini
TTC=2.5
DIST=1.2
ANGLE_SIGN=-1
LEFT_ANGLE=-10
RIGHT_ANGLE=10
ANGLE_ALPHA=0.35
DIRECTION_SAMPLES=3
DASHBOARD_PORT=8080
RADAR_LOG_DIR=/run/media/mmcblk0p1/dvr/radar_experiments
```

其中 `LEFT_ANGLE` 必须小于 `RIGHT_ANGLE`。当前后向安装方式经实测确认：
雷达协议正角位于骑行者左后方、负角位于骑行者右后方，因此
`ANGLE_SIGN=-1`。CSV 中的 `angle_deg/filtered_angle_deg` 保留雷达协议原值，
页面扇形、方向字段和左右阈值使用转换后的骑行者坐标。

## 2026-07-31 室外数据结论

分析 `radar_experiments/labels.csv` 中 29 组完整事件，并仅统计危险目标在
4 米内的数据：

| 人工场景 | 完整组数 | 每组角度中位数的总体中位数（雷达协议） |
|---|---:|---:|
| 正后方快速碰撞 | 8 | +3.1° |
| 左后方快速碰撞 | 8 | +26.4° |
| 右后方快速碰撞 | 7 | -17.7° |
| 安全接近 | 6 | -4.3° |

采用 `ANGLE_SIGN=-1` 和骑行者坐标 `-10°/+10°` 边界后，23 组有方向标签的
快速碰撞事件中，按事件区间角度中位数统计可正确区分 22 组（95.7%）：
正后方 8/8、左后方 7/8、右后方 7/7。唯一偏差样本的运动轨迹大部分仍在
中心区，不建议为了单个样本扩大中心区。下一轮继续保持相同安装位置和阈值，
每类补测 10 组以上即可。

“安全接近”只表示预期不告警，不代表目标一定在正后方，因此不用于左右方向
准确率统计。

## 编译与部署

在交叉编译环境执行：

```bash
cd /home/alientek/dvr_project/mier/lyr/camera_detect
make clean
make radar-fusion
make dashboard-check
```

开发板恢复在线后：

```bash
make deploy-radar
```

`deploy-radar` 会同步二进制、模型、Dashboard、静态页面和配置示例。

## 启动

开发板执行：

```bash
cd /xxl/camera_detect
chmod +x start_dvr.sh
./start_dvr.sh
```

电脑连接开发板 WiFi 后访问：

```text
http://<开发板IP>:8080
```

也可以单独启动 Dashboard：

```bash
python3 /xxl/camera_detect/dashboard/radar_dashboard.py \
  --host 0.0.0.0 \
  --port 8080 \
  --data-dir /run/media/mmcblk0p1/dvr/radar_experiments
```

## 输出文件

```text
/run/media/mmcblk0p1/dvr/radar_experiments/
├── radar_data.csv      # 每帧、每个 objId 的雷达数据
├── radar_state.json    # Dashboard 最新状态；无目标时保持 1 Hz 在线心跳
├── sensor_events.csv   # 摄像头/NPU、M33 IMU、A35→HUD 同步事件
├── imu_delivery.csv    # HUD 接收、冷却与 App UDP 广播结果
└── labels.csv          # 人工开始/结束标注
```

`radar_data.csv` 主要字段：

- `timestamp`、`timestamp_ms`
- `objId`
- `distance_m`、`velocity_mps`
- `angle_deg`、`filtered_angle_deg`
- `TTC_s`、`direction`
- `dangerous_objId`、`is_current_dangerous`
- `radar_alert`

`labels.csv` 中每次点击都会写一行。开始和结束共享同一个 `event_id`，
结束行额外保存 `duration_ms`。所有 CSV 都包含 `timestamp_ms`，可以直接
按时间范围对齐。

`sensor_events.csv` 统一字段为 `timestamp_ms/source/event_type/status/event_id`
以及 NPU 的 `label/score/count`、IMU 的 `seq/reason/details`。`imu_delivery.csv`
用同一个 `event_id` 记录 `hud_received`、`app_broadcast` 及 sent/failed/cooldown。
手机短信尚无 ACK，面板不会把 UDP sent 显示成 SMS 成功。

其中 `source=a35_dvr` 新增记录 DVR 生命周期：

- `event_type=buffer,status=started/failed`：预缓存启动结果及失败原因；
- `event_type=recording,status=triggered`：摔倒或雷达+NPU 融合触发保存。

本批 23 组快速碰撞事件都同时出现过雷达告警和 NPU 确认，说明“不生成视频”
不是本次雷达阈值或 NPU 融合条件不足。旧代码只在 NPU 第一次从无目标变成有
目标时尝试一次 `dvr_start()`，且忽略失败返回值；目标持续存在时不会重试，
碰撞分支又要求 `dvr_recording=1` 才保存，因此 TF 卡目录短暂未就绪时会静默
漏录。摔倒分支会主动启动 DVR，所以会表现为“只有摔倒能录像”。当前版本已
增加有目标时每 2 秒重试，以及碰撞告警时强制补启动并写入上述诊断事件。

## 当前实测方法

1. 启动系统并打开 Dashboard，确认右上角显示“雷达数据在线”。
2. 选择一种场景，在动作发生前点击对应的“开始标记”。
3. 完成正后方、左后方、右后方快速接近，或“安全接近（预期不告警）”、
   “危险接近（预期告警）”动作。
4. 动作完全结束后点击“结束标记”。
5. 每种场景至少重复 10 次，并包含不同速度、距离和横向角度。
6. 下载 `radar_data.csv` 与 `labels.csv`，按同一 `event_id` 的
   `start/end timestamp_ms` 截取雷达数据。
7. 统计事件区间内的最小 TTC、最小距离、角度分布、方向稳定性以及
   `radar_alert` 是否符合人工标注，再调整 `radar_config`。

建议先做低速、空旷、封闭区域测试，并安排一人操作网页、一人负责车辆；
不要在开放道路上边骑行边操作 Dashboard。
