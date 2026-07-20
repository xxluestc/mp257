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

## 注意事项

- 交叉编译器：`aarch64-linux-gnu-gcc` / `aarch64-linux-gnu-g++`
- 开发板需预装：`aplay`、`ffmpeg`、`python3`、`pip`（可选，用于在线 edge-tts）
- 骨传导功放使能：`start_dvr.sh` 会自动导出 PB11 GPIO 并置高
- 日志循环刷屏问题已修复：`dvr.service` 不要把 stdout 重定向回 `dvr_system.log`
