# 音频系统完整链路文档

## 一、系统总览

本音频系统用于**头盔行车记录仪项目**，实现从文字到骨传导振子振动的完整音频输出链路。

```
┌─────────────┐     ┌──────────────┐     ┌──────────┐     ┌─────────┐
│  文字内容    │ ──→ │  edge-tts    │ ──→ │  ffmpeg  │ ──→ │  .wav   │
│ "录制开始"   │     │ 微软在线TTS   │     │ 格式转换  │     │ 文件    │
└─────────────┘     └──────────────┘     └──────────┘     └────┬────┘
                                                                │
┌─────────────┐     ┌──────────────┐     ┌──────────┐          │
│ 骨传导振子   │ ←── │  MAX98357A   │ ←── │ SAI1B    │ ←────────┤
│ (振动发声)   │     │ I2S功放芯片  │     │ I2S接口  │     ALSA播放
└─────────────┘     └──────────────┘     └──────────┘
                          ↑                    ↑
                       5.5V供电            STM32MP257内核
```

### 各层职责

| 层级 | 组件 | 职责 |
|------|------|------|
| **TTS生成层** | edge-tts + ffmpeg | 将中文文字转换为WAV音频文件 |
| **应用层** | alsa_player.c | 读取WAV文件，通过ALSA API写入PCM数据 |
| **ALSA中间层** | asound.conf | 软件音量控制(softvol)、格式转换(plug) |
| **内核驱动层** | simple-audio-card + max98357a + SAI1B | I2S DMA传输、时钟配置 |
| **硬件层** | MAX98357A + 骨传导振子 | D类功放放大 → 振子振动产生声音 |

---

## 二、硬件层详解

### 2.1 硬件平台

- **主控芯片**: STM32MP257 (Cortex-A35, 1GB DDR)
- **功放芯片**: MAX98357A (I2S输入, Class D数字功放)
- **输出设备**: 骨传导振子 (Bone Conduction Transducer)
- **供电电压**: VDD = 5.5V

### 2.2 MAX98357A 关键特性

MAX98357A 是一款 **I2S输入、无需I2C控制** 的数字D类功放芯片：

| 特性 | 说明 |
|------|------|
| 输入接口 | I2S (标准数字音频接口) |
| 输出类型 | Class D (高效率，>90%) |
| 控制方式 | 纯引脚控制（SD_MODE, GAIN），不需要I2C/SPI |
| 输出功率 | 3.2W @ 5V, 4Ω负载 |
| 增益档位 | 3dB / 6dB / 9dB / 12dB (由GAIN引脚电阻选择) |
| 待机功耗 | <1μA |

### 2.3 引脚连接表

| MAX98357A引脚 | 开发板连接 | STM32功能 | 功能说明 |
|-------------|-----------|----------|---------|
| **VDD** | 5.5V电源 | - | 芯片主供电 (范围: 2.5V~5.5V) |
| **GND** | GND | - | 电源地 |
| **BCLK** | PD6 | SAI1_SCK_B | 位时钟 (Bit Clock) |
| **LRCLK** | PD5 | SAI1_FS_B | 帧时钟 / 左右声道选择 |
| **DIN** | PD4 | SAI1_SD_B | I2S串行数据输入 |
| **SD_MODE** | **VDD (高电平)** | - | **关机/使能控制** (高=使能, 低=关机) |
| **GAIN** | VDD (3.3V) | - | 增益设置 (接VDD=6dB, 可串100kΩ降为3dB) |
| **OUT+ / OUT-** | 骨传导振子 | - | 差分音频输出 |

### 2.4 SD_MODE 引脚的重要性 ⚠️

这是整个调试过程中**最关键**的硬件问题：

| SD_MODE电平 | 芯片状态 | 表现 |
|------------|---------|------|
| **GND (低)** | 关机模式 (Shutdown) | 完全无声音，无论软件怎么配 |
| **VDD (高)** | 正常工作 | 正常输出音频 |

**教训**: 数据手册 Table 5 明确写了 SD_MODE=Low 时芯片进入 shutdown 模式，
但实际接线时容易忽略这个引脚，导致"软件一切正常但完全无声"的诡异现象。

### 2.5 骨传导振子特殊要求

骨传导振子和普通扬声器不同：
- **需要足够的驱动幅度**才能产生可感知的振动
- 低音量时信号太弱，振子几乎不振动
- 这就是为什么 softvol 的 min_dB 不能设太小（见软件层说明）

---

## 三、设备树配置 (内核驱动层)

### 3.1 设备树节点要点

MAX98357A 在 Linux 中通过 `simple-audio-card` 和 `max98357a` codec 驱动支持。
关键配置项：

```dts
&i2c2 {                           /* 实际使用的是SAI1B的codec节点 */
};

&sai1b {
    status = "okay";
    clocks = <&rcc CK_KER_SAI1>; /* 必须指定SAI时钟源 */

    sai1b_port: port {
        sai1b_endpoint: endpoint {
            remote-endpoint = <&codec_endpoint>;
            format = "i2s";              /* I2S标准格式 */
            mclk-fs = <256>;             /* MCLK = 采样率 × 256 */
            dai-tdm-slot-num = <2>;      /* 2个时隙(TDM slot) */
            dai-tdm-slot-width = <32>;    /* 每个slot 32bit */
        };
    };
};
```

### 3.2 关键参数解释

#### dai-tdm-slot-num = 2
- 表示每帧包含 **2 个 TDM 时隙**（对应左右立体声声道）
- 如果不设或设错，会导致只有单声道或静音

#### dai-tdm-slot-width = 32
- 每个 TDM 时隙 **32 bit 宽度**
- MAX98357A 要求 I2S 数据宽度为 32bit
- 总帧长 = 2 slots × 32 bits = **64 bits per frame**
- 这与标准 I2S 的 64-bit 帧长度一致

#### clocks = <&rcc CK_KER_SAI1>
- 为 SAI1 外设提供内核时钟
- 缺少此项会导致 "No soundcards found" 错误

---

## 四、ALSA 软件层

### 4.1 asound.conf 配置解析

```bash
# 配置文件位置: /etc/asound.conf 或 ~/.asoundrc
pcm.softvol {
    type softvol                  # 软件音量插件
    slave.pcm "hw:0,0"            # 底层直通硬件设备 card0 device0
    control {
        name "PCM"                # amixer 中显示的控制名称
        card 0
    }
    max_dB 0.0                    # 最大音量 = 0dB (不衰减)
    min_dB -20.0                  # 最小音量 = -20dB (衰减20dB)
    resolution 256               # 音量调节精度 (步数)
}

pcm.!default {                   # 默认PCM设备
    type plug                     # 自动格式转换插件
    slave.pcm "softvol"           # 转换后送给 softvol
}

ctl.!default {                   # 默认控制设备
    type hw
    card 0
}
```

### 4.2 音频数据流经ALSA的路径

```
应用写入 (任意格式)
    ↓
plug 插件: 自动格式转换
    ├── 采样率转换 (如 22050→48000)
    ├── 声道数转换 (如 mono→stereo)
    └── 位深转换 (如 8bit→16bit)
    ↓
softvol 插件: 软件音量衰减
    ├── 100% → 0dB 衰减 (原样输出)
    ├── 50%  → 约 -11dB 衰减
    └── 5%   → 约 -19dB 衰减
    ↓
hw:0,0: 直写硬件DMA缓冲区
    ↓
内核 SAI1B 驱动 → I2S总线 → MAX98357A
```

### 4.3 softvol 范围选择的原理 ⚠️

这是第二个关键调试问题：

| 参数 | 旧值 (有问题) | 新值 (正确) |
|------|--------------|------------|
| max_dB | 0 dB | 0 dB |
| min_dB | **-51 dB** | **-20 dB** |
| 范围 | 51 dB | 20 dB |
| 5%音量时的衰减 | **~49 dB** | **~19 dB** |
| 5%音量时的信号幅度 | **0.35%** (推不动振子) | **11.2%** (可以振动) |

**计算过程**:
- 旧方案: 5% × 51dB ≈ 49dB衰减 → 幅度 = 10^(-49/20) = 0.0035 = **0.35%**
- 新方案: 5% × 20dB ≈ 19dB衰减 → 幅度 = 10^(-19/20) = 0.112 = **11.2%**

骨传导振子在信号幅度低于约5%时就几乎不振动了，所以旧方案的5%音量等于没声音。

### 4.4 音量控制命令

```bash
amixer -c 0 sset PCM 100%    # 0dB,   最大音量
amixer -c 0 sset PCM 70%     # -6dB,  大音量
amixer -c 0 sset PCM 50%     # -11dB, 中等音量
amixer -c 0 sset PCM 30%     # -15dB, 较小音量
amixer -c 0 sset PCM 10%     # -18dB, 小音量
amixer -c 0 sset PCM 5%      # -19dB, 最小可用音量
```

---

## 五、应用层 (alsa_player)

### 5.1 播放器工作流程

```
打开WAV文件 → 跳过44字节WAV头 → 打开ALSA PCM设备
    → 设置硬件参数(48000Hz/stereo/16bit) → 循环读取并写入PCM
    → 播放完毕 → 释放资源
```

### 5.2 核心代码逻辑

```c
// 1. 打开默认PCM设备 (走 asound.conf 的 default → plug → softvol → hw:0,0)
snd_pcm_open(&handle, "default", SND_PCM_STREAM_PLAYBACK, 0);

// 2. 设置硬件参数 - 必须与WAV文件格式匹配!
snd_pcm_hw_params_set_format(handle, params, SND_PCM_FORMAT_S16_LE);  // 16bit LE
snd_pcm_hw_params_set_channels(handle, params, 2);                     // stereo
snd_pcm_hw_params_set_rate_near(handle, params, &rate, 0);            // 48000Hz

// 3. 跳过WAV文件头 (标准RIFF WAVE头 = 44字节)
fseek(fp, 44, SEEK_SET);

// 4. 循环读取并写入PCM
while ((n = fread(buffer, 1, size, fp)) > 0) {
    snd_pcm_writei(handle, buffer, n / (channels * 2));
}
```

### 5.3 为什么跳过44字节？

WAV文件采用 RIFF 文件格式，前44字节是文件头信息：
- Bytes 0-3: "RIFF" 标识
- Bytes 4-7: 文件大小 - 8
- Bytes 8-11: "WAVE" 标识
- Bytes 12-15: "fmt " 子块标识
- Bytes 16-19: fmt子块大小 (通常16)
- Bytes 20-21: 音频格式 (1=PCM)
- Bytes 22-23: 声道数
- Bytes 24-27: 采样率
- Bytes 28-31: 字节率
- Bytes 32-33: 块对齐
- Bytes 34-35: 位深
- Bytes 36-39: "data" 子块标识
- Bytes 40-43: 数据区大小

从第44字节开始才是**原始PCM音频数据**。

---

## 六、TTS语音生成层

### 6.1 方案选型历程

| 方案 | 结果 | 原因 |
|------|------|------|
| espeak-ng | ❌ 失败 | 中文语音质量极差，听起来"叽里咕噜"完全听不懂 |
| 讯飞离线SDK | ❌ 失败 | 免费装机量已用完(剩余0)，HMAC签名验证失败(error 18714) |
| **edge-tts** | ✅ 采用 | 微软免费在线TTS，高质量中文语音，无需激活授权 |

### 6.2 espeak-ng 为什么不行？

espeak-ng 生成的WAV文件存在两个问题：

**问题1: 采样率不匹配**
- espeak-ng 默认输出 **22050 Hz / mono / 16bit**
- MAX98357A 配置为 **48000 Hz / stereo / 16bit**
- 当22050Hz的文件以48000Hz播放时：速度变为 **48000/22050 = 2.17倍**
- 听起来就像快进播放，完全听不懂

**问题2: 中文语音质量差**
- espeak-ng 的中文合成引擎基于规则拼接，不是神经网络
- 即使采样率对了，语音也缺乏自然韵律，一字一顿感明显

### 6.3 讯飞离线SDK为什么失败？

讯飞SDK初始化流程需要三步认证：
1. 本地填入 appID / apiKey / apiSecret
2. SDK启动时通过HTTPS连接讯飞服务器做 HMAC 签名验证
3. 验证通过后检查设备激活次数（装机量）

失败原因：
- **HMAC签名不匹配** (错误码18714): 可能凭证复制有误，或SDK版本与控制台不对应
- **装机量为0**: 免费体验版只允许有限次数的设备激活，已全部用完
- 日志显示: `"http status:401 errmsg: {"message":"HMAC signature does not match"}"`

### 6.4 edge-tts 方案详情

#### 什么是 edge-tts？
微软 Edge 浏览器内置的"大声朗读"(Read Aloud)功能的TTS引擎，
被开源社区逆向封装为命令行工具。使用微软Azure云端的TTS服务。

#### 技术特点
- **完全免费**: 无需API Key，无调用限制
- **高质量**: 基于神经网络的中文语音合成
- **多发音人**: 支持晓晓、云希、云健等多个中文声音
- **输出格式**: MP3 (24kHz, 通常为mono)

#### 生成流程

```bash
# 步骤1: edge-tts 合成MP3
edge-tts --voice zh-CN-XiaoxiaoNeural --text "录制开始" --write-media tmp.mp3

# 步骤2: ffmpeg 转码为目标WAV格式
ffmpeg -y -i tmp.mp3 -ar 48000 -ac 2 -sample_fmt s16 output.wav
```

#### ffmpeg 参数含义

| 参数 | 值 | 含义 |
|------|-----|------|
| `-ar` | 48000 | 重采样到48000 Hz (匹配MAX98357A) |
| `-ac` | 2 | 转为双声道立体声 |
| `-sample_fmt` | s16 | 16-bit 有符号小端整数 PCM |

#### 一键生成脚本

```bash
cd scripts/
./gen_tts_wav.sh "要合成的文字" [文件名] [发音人]
# 示例:
./gen_tts_wav.sh "注意安全，请佩戴好头盔" safety_alert
./gen_tts_wav.sh "电量不足，请及时充电" low_battery zh-CN-YunxiNeural
```

详见 [TTS_GENERATION.md](./TTS_GENERATION.md)

---

## 七、WAV文件格式规范

本项目所有WAV文件必须遵循以下规范：

| 参数 | 要求值 | 说明 |
|------|--------|------|
| 采样率 (Sample Rate) | **48000 Hz** | 与ALSA/SAI1B/MAX98357A一致 |
| 声道数 (Channels) | **2 (Stereo)** | 立体声 |
| 位深 (Bits Per Sample) | **16 bit** | 有符号小端 (S16_LE) |
| 编码格式 | **PCM** | 未压缩原始音频 |
| 字节序 | **Little Endian** | ARM/x86通用 |

### 格式验证方法

```bash
# 方法1: file命令
file alert.wav
# 期望输出: RIFF (little-endian) data, WAVE audio, Microsoft PCM, 16 bit, stereo 48000 Hz

# 方法2: ffprobe
ffprobe alert.wav
# 期望: Audio: pcm_s16le, 48000 Hz, 2 channels, s16
```

### 格式不匹配的后果

| 不匹配项 | 后果 |
|---------|------|
| 采样率过低 (如22050Hz) | 播放速度变快 (2.17x)，像快进 |
| 采样率过高 | 播放速度变慢，声音沉闷 |
| 单声道 (mono) | 只有左声道有声音，右声道静音 |
| 位深不对 | 噪音或完全无法播放 |

---

## 八、完整操作手册

### 8.1 生成新的语音提示

```bash
cd /home/alientek/dvr_project/MAX98357A_audio/scripts
./gen_tts_wav.sh "你的提示文字" output_filename
# 输出文件: ../assets/output_filename.wav
```

### 8.2 在开发板上播放

```bash
# 拷贝WAV文件到开发板
scp assets/*.wav root@<开发板IP>:/tmp/

# 在开发板上播放
/tmp/alsa_player /tmp/safety_alert.wav
```

### 8.3 调节音量

```bash
# 开发板上执行
amixer -c 0 sset PCM 80%
```

### 8.4 编译部署 (如果修改了C代码)

```bash
# PC上交叉编译
unset LD_LIBRARY_PATH
source /opt/st/stm32mp2/5.0.3-snapshot/environment-setup-cortexa35-ostl-linux
cd src && make clean && make

# 部署到开发板
scp alsa_player root@<开发板IP>:/
```

### 8.5 更换设备树 (如果修改了dts)

```bash
source /opt/st/stm32mp2/5.0.3-snapshot/environment-setup-cortexa35-ostl-linux
make stm32mp257_atk_defconfig
make st/stm32mp257d-atk-ddr-1GB.dtb
scp arch/arm/boot/dts/st/stm32mp257d-atk-ddr-1GB.dtb root@<板IP>:/boot/

# 开发板重启生效
reboot
```

---

## 九、调试历史时间线

| 时间 | 事件 | 状态 |
|------|------|------|
| 初始 | 连线完成，软件配置SAI1B + MAX98357A | - |
| 问题1 | 完全无声音 | ✅ 已解决: SD_MODE改接VDD |
| 问题2 | dmesg报SAI帧异常 | ✅ 已解决: 添加tdm-slot配置 |
| 问题3 | No soundcards found | ✅ 已解决: 添加clocks配置 |
| 问题4 | 100%有声音但5%/10%无振动 | ✅ 已解决: softvol改为-20dB范围 |
| 问题5 | espeak-ng生成的语音听不懂 | ✅ 已解决: 改用edge-tts |
| 问题6 | 讯飞离线SDK初始化失败(18714) | ⚠️ 已记录: 凭证/装机量问题 |

---

## 十、项目文件结构

```
MAX98357A_audio/
├── assets/                      # 音频素材 (48000Hz/stereo/16bit WAV)
│   ├── rec_start.wav            # 录制开始
│   ├── rec_stop.wav             # 录制结束
│   ├── safety_alert.wav         # 注意安全，请佩戴好头盔
│   ├── low_battery.wav          # 电量不足，请及时充电
│   ├── storage_full.wav         # 存储空间不足，请清理文件
│   ├── connect_ok.wav           # 连接成功
│   ├── connect_lost.wav         # 连接断开，请检查设备
│   └── booting.wav             # 系统启动中
├── config/
│   └── asound.conf              # ALSA配置 (softvol + plug)
├── docs/
│   ├── README.md                # 项目总览
│   ├── AUDIO_PIPELINE.md        # 本文档 - 完整链路说明
│   └── TTS_GENERATION.md        # TTS生成详细说明
├── dts/
│   └── max98357a_dts_reference.dts  # 设备树参考配置
├── scripts/
│   ├── gen_tts.sh               # (旧) espeak-ng脚本 (已废弃)
│   └── gen_tts_wav.sh           # (新) edge-tts一键生成脚本
├── src/
│   ├── alsa_player.c            # ALSA播放器源码
│   └── Makefile                 # 交叉编译Makefile
└── Linux_awaken_esr_xtts_aisound_v2.2.15-rc5/  # 讯飞离线SDK (备用参考)
```
