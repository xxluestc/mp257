# MAX98357A 骨传导音频系统

> 头盔行车记录仪项目 — 从文字到骨传导振动的完整音频解决方案

## 快速开始

```bash
# 1. 生成语音文件 (在PC上)
cd scripts
./gen_tts_wav.sh "录制开始" rec_start

# 2. 播放 (在开发板上)
./alsa_player /path/to/rec_start.wav

# 3. 调节音量 (在开发板上)
amixer -c 0 sset PCM 80%
```

## 系统架构

```
文字 → [edge-tts] → MP3 → [ffmpeg] → WAV(48kHz/stereo/16bit)
    → [alsa_player] → ALSA(softvol+plug) → SAI1B(I2S) → MAX98357A → 骨传导振子
```

## 硬件清单

| 组件 | 型号 | 说明 |
|------|------|------|
| 主控 | STM32MP257 | Cortex-A35, 1GB DDR |
| 功放 | MAX98357A | I2S输入, Class D, 无需I2C控制 |
| 输出 | 骨传导振子 | 差分驱动, 振动发声 |
| 供电 | 5.5V | VDD范围: 2.5V~5.5V |

### 关键接线 ⚠️

- **SD_MODE 必须接 VDD (高电平)** — 接GND = 芯片关机，完全无声音！
- **GAIN 接 VDD = 6dB 增益**（可串100kΩ电阻降为3dB）
- BCLK/LRCLK/DIN 分别接 PD6/PD5/PD4 (SAI1B)

详细接线表和设备树配置见 [AUDIO_PIPELINE.md](./AUDIO_PIPELINE.md)

## 已生成的语音文件

位于 `assets/` 目录，全部为 **48000Hz / 立体声 / 16bit PCM WAV**：

| 文件 | 内容 | 用途 |
|------|------|------|
| rec_start.wav | 录制开始 | 开始录像提示 |
| rec_stop.wav | 录制结束 | 停止录像提示 |
| safety_alert.wav | 注意安全，请佩戴好头盔 | 安全提醒 |
| low_battery.wav | 电量不足，请及时充电 | 低电量告警 |
| storage_full.wav | 存储空间不足，请清理文件 | 存储满告警 |
| connect_ok.wav | 连接成功 | 外设连接成功 |
| connect_lost.wav | 连接断开，请检查设备 | 连接丢失告警 |
| booting.wav | 系统启动中 | 开机提示 |

## TTS语音生成

使用微软 edge-tts (免费高质量中文TTS) + ffmpeg 格式转换。

**一键生成新语音：**
```bash
cd scripts
./gen_tts_wav.sh "要说的文字" 文件名
# 可选第三个参数指定发音人: zh-CN-YunxiNeural (男声)
```

详细说明见 [TTS_GENERATION.md](./TTS_GENERATION.md)

## 软件音量控制 (ALSA softvol)

范围: **0dB ~ -20dB** (256级精度)，通过 amixer 控制：

```bash
amixer -c 0 sset PCM 100%   # 最大 (0dB)
amixer -c 0 sset PCM 50%    # 中等 (-11dB)
amixer -c 0 sset PCM 10%    # 小 (-18dB)
amixer -c 0 sset PCM 5%     # 最小可用 (-19dB)
```

> 为什么是 -20dB 而不是 -51dB？因为骨传导振子需要足够的驱动幅度，
> 旧方案(-51dB)在低音量时信号仅0.35%，推不动振子。详见链路文档。

## 编译与部署

### 应用程序 (alsa_player)
```bash
unset LD_LIBRARY_PATH
source /opt/st/stm32mp2/5.0.3-snapshot/environment-setup-cortexa35-ostl-linux
cd src && make clean && make
scp alsa_player root@<开发板IP>:/
```

### 设备树
```bash
source /opt/st/stm32mp2/5.0.3-snapshot/environment-setup-cortexa35-ostl-linux
make stm32mp257_atk_defconfig && make st/stm32mp257d-atk-ddr-1GB.dtb
scp arch/arm/boot/dts/st/stm32mp257d-atk-ddr-1GB.dtb root@<板IP>:/boot/
reboot
```

## 项目结构

```
MAX98357A_audio/
├── assets/                 # 音频WAV文件 (48kHz/stereo/16bit)
├── config/asound.conf      # ALSA配置 (softvol + plug)
├── docs/
│   ├── README.md            # 本文档
│   ├── AUDIO_PIPELINE.md    # 完整链路技术文档 ★
│   └── TTS_GENERATION.md    # TTS生成说明
├── dts/                    # 设备树参考
├── scripts/
│   ├── gen_tts_wav.sh       # edge-tts 一键生成脚本
│   └── gen_tts.sh           # (旧) espeak-ng脚本(已废弃)
└── src/
    ├── alsa_player.c        # ALSA播放器源码
    └── Makefile             # 交叉编译Makefile
```

## 踩过的坑 (调试历史)

| # | 问题 | 根因 | 解决方案 |
|---|------|------|---------|
| 1 | 完全无声音 | SD_MODE接GND=关机模式 | 改接VDD高电平 |
| 2 | SAI帧异常 | 缺少tdm-slot配置 | slot-num=2, slot-width=32 |
| 3 | No soundcards found | 缺少SAI时钟配置 | clocks=<&rcc CK_KER_SAI1> |
| 4 | 低音量无振动 | softvol范围太大(-51dB) | 改为-20dB |
| 5 | espeak-ng语音听不懂 | 中文质量差 + 采样率不匹配 | 改用edge-tts |
| 6 | 讯飞SDK初始化失败18714 | HMAC签名不匹配 + 装机量为0 | 放弃，用edge-tts替代 |

每个问题的详细分析和技术原理见 [AUDIO_PIPELINE.md](./AUDIO_PIPELINE.md)

## 相关文档

- **[AUDIO_PIPELINE.md](./AUDIO_PIPELINE.md)** — 完整音频链路技术文档（硬件→内核→ALSA→应用→TTS 全栈详解）
- **[TTS_GENERATION.md](./TTS_GENERATION.md)** — TTS语音生成方法、发音人列表、批量生成示例
