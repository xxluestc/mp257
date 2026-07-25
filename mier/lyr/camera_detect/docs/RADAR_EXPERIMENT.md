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

`radar_fusion` 的原有 NPU、DVR、LED、Audio、IMU 摔倒触发链路没有改变；
Dashboard 只消费雷达状态文件。

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

方向规则：

```text
angle <= LEFT_ANGLE    → LEFT
angle >= RIGHT_ANGLE   → RIGHT
其他                   → CENTER
```

角度先经过按 `objId` 独立维护的 EMA，再经过 2° 边界滞回和连续帧确认，
避免阈值附近频繁跳变。

## 配置

将 [`radar_config.example`](radar_config.example) 复制为开发板上的
`/xxl/camera_detect/radar_config`：

```ini
TTC=2.5
DIST=1.2
LEFT_ANGLE=-10
RIGHT_ANGLE=10
ANGLE_ALPHA=0.35
DIRECTION_SAMPLES=3
DASHBOARD_PORT=8080
RADAR_LOG_DIR=/run/media/mmcblk0p1/dvr/radar_experiments
```

其中 `LEFT_ANGLE` 必须小于 `RIGHT_ANGLE`。如果现场确认雷达角度正负方向
与页面相反，应根据实测重新配置左右边界。

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
├── radar_state.json    # Dashboard 最新状态
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
结束行额外保存 `duration_ms`。两份 CSV 都包含 `timestamp_ms`，可以直接
按时间范围对齐。

## 当前实测方法

1. 启动系统并打开 Dashboard，确认右上角显示“雷达数据在线”。
2. 选择一种场景，在动作发生前点击对应的“开始标记”。
3. 完成正后方、左后方、右后方快速接近或普通接近动作。
4. 动作完全结束后点击“结束标记”。
5. 每种场景至少重复 10 次，并包含不同速度、距离和横向角度。
6. 下载 `radar_data.csv` 与 `labels.csv`，按同一 `event_id` 的
   `start/end timestamp_ms` 截取雷达数据。
7. 统计事件区间内的最小 TTC、最小距离、角度分布、方向稳定性以及
   `radar_alert` 是否符合人工标注，再调整 `radar_config`。

建议先做低速、空旷、封闭区域测试，并安排一人操作网页、一人负责车辆；
不要在开放道路上边骑行边操作 Dashboard。
