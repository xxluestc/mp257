# TTS语音生成说明

## 概述

本项目使用 **微软 edge-tts** 在线TTS引擎生成高质量的中文语音WAV文件，
用于MAX98357A骨传导音频输出（头盔行车记录仪项目）。

## 为什么不用espeak-ng / 讯飞离线SDK

| 方案 | 问题 |
|------|------|
| espeak-ng | 中文语音质量极差，听起来"叽里咕噜"完全听不懂 |
| 讯飞离线SDK | 需要设备激活授权，免费体验装机量已用完(剩余0)，HMAC签名验证失败(错误码18714) |
| **edge-tts (当前方案)** | 免费、高质量中文语音、无需激活、在线即可用 |

## 输出音频规格

所有生成的WAV文件统一为以下规格（匹配MAX98357A + ALSA配置）：

- 采样率: **48000 Hz**
- 声道: **立体声 (2 channels)**
- 位深: **16 bit PCM**
- 格式: **RIFF WAVE**

## 环境依赖

```bash
# edge-tts (微软在线TTS)
pip3 install edge-tts --break-system-packages

# ffmpeg (格式转换: mp3 -> wav)
sudo apt-get install ffmpeg
```

## 使用方法

### 快速生成单个文件

```bash
cd MAX98357A_audio/scripts
./gen_tts_wav.sh "录制开始" rec_start
./gen_tts_wav.sh "注意安全，请佩戴好头盔" safety_alert
```

### 参数说明

```bash
./gen_tts_wav.sh "中文文本" [输出文件名] [发音人]
```

| 参数 | 必填 | 说明 | 默认值 |
|------|------|------|--------|
| 中文文本 | ✅ | 要合成的语音文字 | - |
| 输出文件名 | ❌ | 保存到assets/下的文件名(不含.wav) | 文本md5前8位 |
| 发音人 | ❌ | TTS发音人ID | zh-CN-XiaoxiaoNeural |

### 可用中文发音人

| 发音人ID | 名称 | 性别 | 特点 |
|----------|------|------|------|
| zh-CN-XiaoxiaoNeural | 晓晓 | 女 | 默认，年轻甜美女声 |
| zh-CN-YunxiNeural | 云希 | 男 | 年轻男声 |
| zh-CN-XiaoyiNeural | 晓伊 | 女 | 温柔女声 |
| zh-CN-YunjianNeural | 云健 | 男 | 成熟男声 |

### 批量生成示例

```bash
cd MAX98357A_audio/scripts

./gen_tts_wav.sh "录制开始" rec_start
./gen_tts_wav.sh "录制结束" rec_stop
./gen_tts_wav.sh "注意安全，请佩戴好头盔" safety_alert
./gen_tts_wav.sh "电量不足，请及时充电" low_battery
./gen_tts_wav.sh "存储空间不足，请清理文件" storage_full
./gen_tts_wav.sh "连接成功" connect_ok
./gen_tts_wav.sh "连接断开，请检查设备" connect_lost
./gen_tts_wav.sh "系统启动中" booting
```

## 已生成的语音文件

位于 `assets/` 目录：

| 文件 | 内容 | 时长 |
|------|------|------|
| rec_start.wav | 录制开始 | ~1.6s |
| rec_stop.wav | 录制结束 | ~1.6s |
| safety_alert.wav | 注意安全，请佩戴好头盔 | ~2.9s |
| low_battery.wav | 电量不足，请及时充电 | ~2.9s |
| storage_full.wav | 存储空间不足，请清理文件 | ~3.2s |
| connect_ok.wav | 连接成功 | ~1.6s |
| connect_lost.wav | 连接断开，请检查设备 | ~2.9s |
| booting.wav | 系统启动中 | ~1.8s |

## 开发板播放验证

将WAV文件拷贝到开发板后，使用alsa_player播放：

```bash
# 在开发板上执行
./alsa_player assets/safety_alert.wav
```

## 历史记录

- 2026-05-06: 初始创建，从espeak-ng迁移到edge-tts方案
- espeak-ng生成的22050Hz mono文件在48000Hz stereo设备上播放会变快2.17倍导致听不懂
