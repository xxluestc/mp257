# camera_detect / radar_fusion

## 项目说明

`radar_fusion` 是主程序，集成：

- 摄像头采集 + NPU 目标检测
- 毫米波雷达数据融合
- HUD 导航信息接收（UDP 8888）
- OLED 导航显示
- 骨传导音频播报
- 碰撞/跌倒检测与视频保存

## 目录结构

```text
mier/
├── camera_detect/              # 本目录
│   ├── Makefile               # 编译脚本
│   ├── radar_fusion.cpp       # 主程序入口
│   ├── nav_tts.c / nav_tts.h  # 导航接收 + OLED + 骨传导
│   ├── camera.c / camera.h    # 摄像头
│   ├── npu_detect.cpp / .h    # NPU 推理
│   ├── jpeg_decoder.c         # JPEG 解码
│   ├── main.cpp               # camera_detect 目标入口
│   ├── fusion_main.cpp        # fusion_detect 目标入口
│   ├── start_dvr.sh           # 启动脚本
│   ├── dvr.service.example    # systemd 自启动示例
│   ├── bt_shell.sh            # 蓝牙串口脚本
│   ├── scripts/               # TTS / V2X 生成脚本
│   ├── models/                # NPU 模型 + 标签
│   ├── sounds/                # V2X / 碰撞 / 跌倒提示音
│   ├── nav_tts_cache/         # 预生成导航语音
│   └── stai_mpu/              # NPU 库 + 头文件
└── hud_project/               # 必须与 camera_detect 同级
    ├── common.h               # NavData 定义
    ├── oled.c / oled.h        # OLED 显示逻辑
    └── cJSON.c / cJSON.h      # JSON 解析
```

## 依赖关系

本目录**没有**把 `hud_project` 的源码复制进来，`Makefile` 通过相对路径引用：

```makefile
HUD_DIR = ../hud_project
```


## 编译

```bash
cd /home/alientek/dvr_project/mier/camera_detect
make clean
make radar-fusion
```

生成可执行文件 `radar_fusion`（ARM aarch64）。

## 部署到开发板

修改 `Makefile` 里的 `BOARD_IP` 和 `BOARD_DIR` 为你的开发板地址，然后：

```bash
make deploy-radar
```

这会拷贝：
- `radar_fusion` 可执行文件
- `models/` 下的 NPU 模型和标签
- `scripts/gen_nav_tts.sh`
- `nav_tts_cache/` 下的预生成导航语音

## 运行

在开发板上：

```bash
cd /xxl/camera_detect
export LD_LIBRARY_PATH=/usr/lib:/vendor/lib
./radar_fusion
```

或用启动脚本：

```bash
cd /xxl/camera_detect
./start_dvr.sh
```

## 上电自启动

```bash
cp /xxl/camera_detect/dvr.service.example /etc/systemd/system/dvr.service
systemctl daemon-reload
systemctl enable dvr.service
systemctl start dvr.service
```

## 日志查看

启动后日志写入：

```text
/xxl/camera_detect/dvr_system.log
```

实时查看：

```bash
tail -f /xxl/camera_detect/dvr_system.log
```

只看导航相关：

```bash
tail -f /xxl/camera_detect/dvr_system.log | grep "\[NAV\]"
```

## 导航数据协议

手机 APP 通过 UDP 8888 发送 JSON：

```json
{"type":"navi","turn":2,"distance":100}
{"type":"navi_tts","tts_type":1,"seq":12,"text":"前方100米右转进入人民路"}
{"type":"alert","message":"前方车辆靠近"}
```

- `type=navi`：刷新 OLED，并在 `distance <= 50m` 且转向时播报语音
- `type=navi_tts`：完整路名播报；无网络时只后台尝试生成，不播兜底
- `type=alert`：播放固定预警提示音

## 摔倒短信通知

检测到 IMU 摔倒事件后，`radar_fusion` 会异步调用外部脚本发送短信通知：

```bash
/xxl/camera_detect/scripts/send_fall_sms.sh "IMU_ALERT type=fall ..."
```

脚本由仓库中 `scripts/send_fall_sms.sh` 提供模板，包含：
- 解析摔倒事件中的 `reason`、`gps_valid`、`lat_1e7`、`lon_1e7` 等字段
- 构建短信内容并记录到 `/xxl/camera_detect/fall_sms.log`
- 预留 HTTP API 调用示例，需根据实际短信平台填写

使用方法：

1. 将 `scripts/send_fall_sms.sh` 部署到开发板并赋予执行权限。
2. 在脚本中替换为你的短信平台接口（阿里云、腾讯云、Twilio 等），或设置环境变量：

```bash
export SMS_API_URL="https://your-sms-api"
export SMS_PHONE="13800138000"
```

3. 触发摔倒事件后查看日志：

```bash
tail -f /xxl/camera_detect/fall_sms.log
```

> 该功能为新增功能，不影响原有的摔倒告警音频、DVR 保存和 LED 提示。

## 注意事项

- 交叉编译器：`aarch64-linux-gnu-gcc` / `aarch64-linux-gnu-g++`
- 开发板需预装：`aplay`、`ffmpeg`、`python3`、`pip`（可选，用于在线 edge-tts）
- 骨传导功放使能：`start_dvr.sh` 会自动导出 PB11 GPIO 并置高
- 日志循环刷屏问题已修复：`dvr.service` 不要把 stdout 重定向回 `dvr_system.log`

## DVR 视频文件

紧急视频保存在 TF 卡：

```text
/run/media/mmcblk0p1/dvr/emergency_YYYYMMDD_HHMMSS.mp4
```

- 编码格式：**H.264 / AVC，1280x720**
- 容器格式：**标准 MP4**（`ftypmp42` / `ftypisom`）
- 触发时会保存触发前 15 秒 + 触发后 15 秒

> 旧版本使用 GStreamer `qtmux` 生成的是 QuickTime 容器（`ftypqt`），部分播放器会提示“格式错误”。当前版本已改用 `mp4mux` 生成标准 MP4。若旧视频无法播放，可在开发板上用 ffmpeg 转封装：
>
> ```bash
> ffmpeg -i emergency_YYYYMMDD_HHMMSS.mp4 -c copy -movflags +faststart -f mp4 emergency_YYYYMMDD_HHMMSS_fixed.mp4
> ```
