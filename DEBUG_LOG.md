# DVR 行车记录系统 - 调试记录

## 测试日期: 2026-05-01

## 系统配置
- 硬件: STM32MP2 开发板 + DCMIPP 摄像头
- LCD: 800x480, RGB565 (16bpp)
- 摄像头设备: /dev/video-camera0
- 存储: SD卡 /run/media/mmcblk0p1
- 交叉编译器: aarch64-ostl-linux-gcc (STM32MP2 SDK 5.0.3)

## 已修复问题

### 1. LCD显示灰白/颜色淡
- 原因: 缺少ISP(图像信号处理器)配置
- 修复: 在camera_v4l2.c中添加dcmipp-isp-ctrl调用
- 参考: systemui_src/mediaengine/camera.cpp 中的 ISPCtrolThread
- 关键: ISP控制必须在VIDIOC_STREAMON之后调用

### 2. LCD显示不全(右边缺一块)
- 原因: 直接用分辨率计算内存偏移，未考虑framebuffer stride
- 修复: 使用finfo.line_length作为stride计算偏移
- 文件: display/display_lcd.c

### 3. 录制时LCD卡顿
- 原因: ffmpeg编码在主线程执行，阻塞摄像头采集和LCD刷新
- 修复: fork()子进程异步编码
  - 父进程fork前收集帧偏移量(持锁)
  - 子进程独立打开buffer文件，pread()读取帧，管道送ffmpeg
  - 父进程立即返回，不阻塞主循环
- 文件: recorder/dvr_engine.c

### 4. 刷屏黑条纹
- 原因: 每帧全屏memset(0)清空导致闪烁
- 修复: 移除全屏清空，只绘制摄像头画面区域

### 5. 内存OOM
- 原因: 环形缓冲区全部在内存中(~800MB)
- 修复: 改为SD卡单文件缓冲，内存仅存索引
- 文件: common/ring_buffer.c (使用pwrite/pread避免lseek竞争)

## 测试结果

### 触发流程测试
```
TARGET_ON → 状态: IDLE → BUFFERING
WARNING   → 紧急保存触发，等待15秒缓冲
→ 异步编码启动 (pid=3282), 440帧
→ 保存完成: 440帧, 598KB, 14.67秒
```

### 视频参数
- 分辨率: 640x480 @ 30fps
- 编码: MPEG-4 Simple Profile
- 容器: MP4
- 码率: ~331 kb/s

### 内存占用
- DVR进程: ~7.5MB VSZ
- 系统可用: ~497MB

## 待解决问题

### 摄像头分辨率
- 摄像头支持 16x16 ~ 4096x2160 (Continuous)
- v4l2-ctl可设置800x480并成功流传输
- 但V4L2 API直接设置800x480时STREAMON失败(EINVAL)
- 可能原因: DCMIPP驱动对单平面API有分辨率约束
- 临时方案: 使用640x480，LCD居中显示

## 关键文件
- A_Core/dvr_main.c       - 主程序入口
- camera/camera_v4l2.c    - V4L2摄像头采集 + ISP控制
- display/display_lcd.c   - Framebuffer LCD显示
- common/ring_buffer.c    - SD卡环形缓冲区
- recorder/dvr_engine.c   - DVR状态机 + 异步编码
- ipc/trigger_receiver.c  - 命名管道触发接收
