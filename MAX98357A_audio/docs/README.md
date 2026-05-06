# MAX98357A 骨传导音频系统 - 调试总结

## 硬件平台
- **主控**: STM32MP257 (1GB DDR)
- **功放**: MAX98357A (I2S输入, Class D, 无需I2C)
- **输出**: 骨传导振子
- **供电**: 5.5V (VDD)

## 引脚连接

| MAX98357A模组 | 开发板引脚 | STM32功能 | 说明 |
|--------------|-----------|----------|------|
| VDD | 5.5V电源 | - | 芯片供电 (2.5V~5.5V) |
| GND | GND | - | 地 |
| BCLK | PD6 | SAI1_SCK_B | 位时钟 |
| LRCLK | PD5 | SAI1_FS_B | 帧时钟(左右声道) |
| DIN | PD4 | SAI1_SD_B | I2S数据 |
| SD_MODE | VDD (高电平) | - | **必须接高电平!** 接GND=关机 |
| GAIN | VDD (3.3V) | - | 增益6dB, 可串100kΩ电阻降至3dB |

## 关键问题与解决

### 1. SD_MODE接GND导致无声音 (根因)
- **现象**: 软件播放正常, SAI寄存器配置正确, 但完全无声
- **原因**: MAX98357A数据手册Table 5: SD_MODE接GND = 关机模式
- **解决**: SD_MODE改接VDD (高电平), 功放使能

### 2. SAI帧长度不匹配
- **现象**: dmesg显示SAI配置异常
- **原因**: 默认32位帧, MAX98357A需要64位帧(32bit×2声道)
- **解决**: 设备树添加 `dai-tdm-slot-num = <2>` 和 `dai-tdm-slot-width = <32>`

### 3. 时钟配置缺失
- **现象**: "No soundcards found"
- **解决**: SAI1B节点添加 `clocks = <&rcc CK_KER_SAI1>`

### 4. softvol范围过大导致低音量无声
- **现象**: 100%有声音, 5%/10%完全无震动
- **原因**: 旧范围0~-51dB, 10%时信号幅度仅0.5%, 推不动骨传导振子
- **解决**: 改为0~-20dB, 5%时仍有11%幅度

## 软件架构

```
应用层: alsa_player (C语言, libasound)
    ↓
ALSA层: default PCM → plug → softvol → hw:0,0
    ↓
内核层: simple-audio-card → max98357a codec驱动 → SAI1B DMA
    ↓
硬件层: SAI1B I2S → MAX98357A → 骨传导振子
```

## 音量控制

```bash
# 软件音量 (0~-20dB范围)
amixer -c 0 sset PCM 100%   # 最大
amixer -c 0 sset PCM 50%    # 中等 (-11dB)
amixer -c 0 sset PCM 10%    # 低 (-18dB)
amixer -c 0 sset PCM 5%     # 最低 (-19dB)

# 播放
/tmp/alsa_player /tmp/test_1khz.wav
```

## 编译与部署

### 设备树编译
```bash
source /opt/st/stm32mp2/5.0.3-snapshot/environment-setup-cortexa35-ostl-linux
make stm32mp257_atk_defconfig
make st/stm32mp257d-atk-ddr-1GB.dtb
# 部署: scp dtb到开发板 /boot/
```

### 应用程序编译
```bash
unset LD_LIBRARY_PATH
source /opt/st/stm32mp2/5.0.3-snapshot/environment-setup-cortexa35-ostl-linux
cd src && make
```

## TTS语音生成

```bash
# 使用espeak-ng生成中文语音
cd scripts
./gen_tts.sh "你好世界" hello
# 输出: ../assets/hello.wav (立体声WAV)
```

## 文件结构

```
MAX98357A_audio/
├── src/            # 应用层源码
│   ├── alsa_player.c
│   └── Makefile
├── dts/            # 设备树参考
│   └── max98357a_dts_reference.dts
├── assets/         # 音频素材
│   ├── test_hello.wav
│   ├── welcome.wav
│   └── alert.wav
├── config/         # 配置文件
│   └── asound.conf
├── scripts/        # 工具脚本
│   └── gen_tts.sh
└── docs/           # 文档
    └── README.md
```
