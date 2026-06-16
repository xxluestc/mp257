# MAX98357A 音频系统 (MYiR STM32MP257x)

> 适用于 MYiR STM32MP257x Evaluation Board 2GB
> SAI4B + MAX98357A，引脚 PB4/PB5/PB6

## 硬件接线

| 功能 | 引脚 | 说明 |
|------|------|------|
| SAI4_FS_B | PB4 | AF4，帧同步 |
| SAI4_SD_B | PB5 | AF4，数据输出 |
| SAI4_SCK_B | PB6 | AF4，位时钟 |
| SD_MODE | **PB11** | GPIO 输出高电平使能，低电平=关机 |
| GAIN | **悬空** | 默认 9dB 增益（推荐） |

## 设备树修改 (基于原始dtb反编译修改)

**必须从开发板原始dtb反编译修改**，不能用内核源码里的dts，否则保留内存地址等硬件配置可能对不上。

### 关键修改点

1. **使能 SAI4 父节点**
   ```dts
   sai@40340000 {
       status = "okay";
       pinctrl-names = "default";
       pinctrl-0 = <&sai4b-0>;
   };
   ```

2. **配置 SAI4B 为 TX (播放)**
   ```dts
   audio-controller@40340024 {
       compatible = "st,stm32-sai-sub-b";
       dma-names = "tx";
       dmas = <&hpdma 80 0x43 0x21>;
       status = "okay";
   };
   ```

3. **禁用占用引脚的外设**
   ```dts
   serial@40100000 { status = "disabled"; };  /* UART4 */
   i2c@40130000 { status = "disabled"; };     /* I2C2 */
   ```

4. **引脚复用配置 (注意AF4对应数值是0x05，不是0x04)**
   ```dts
   sai4b-0 {
       pins {
           pinmux = <0x1405>,  /* PB4 = SAI4_FS_B (AF4) */
                    <0x1505>,  /* PB5 = SAI4_SD_B (AF4) */
                    <0x1605>;  /* PB6 = SAI4_SCK_B (AF4) */
           bias-disable;
           drive-push-pull;
           slew-rate = <0x01>;
       };
   };
   ```

5. **添加 MAX98357A 和 sound 节点**
   ```dts
   max98357a {
       compatible = "maxim,max98357a";
       #sound-dai-cells = <0x00>;
       status = "okay";
   };

   sound {
       compatible = "simple-audio-card";
       label = "MAX98357A";
       simple-audio-card,format = "i2s";
       simple-audio-card,mclk-fs = <0x100>;
       simple-audio-card,cpu {
           sound-dai = <&sai4b>;
           bitclock-master;
           frame-master;
           dai-tdm-slot-num = <0x02>;
           dai-tdm-slot-width = <0x20>;
       };
       simple-audio-card,codec {
           sound-dai = <&max98357a>;
       };
   };
   ```

6. **禁用 FDCAN1（释放 PB11 为 GPIO）**
   ```dts
   can@402d0000 {
       status = "disabled";
   };
   ```

7. **禁用 ES8388 相关节点**
   ```dts
   ES8388-Sound { status = "disabled"; };
   /* i2c@40130000 下的 es8328@10 状态因 i2c disabled 已失效 */
   ```

### 编译dtb

```bash
# 从开发板拷贝原始dtb反编译
scp root@192.168.88.10:/boot/myb-stm32mp257x-2GB.dtb /tmp/board.dtb
dtc -I dtb -O dts /tmp/board.dtb -o /tmp/board.dts

# 修改后编译回dtb
dtc -I dts -O dtb /tmp/board.dts -o /tmp/board_new.dtb

# 部署
scp /tmp/board_new.dtb root@192.168.88.10:/boot/myb-stm32mp257x-2GB.dtb
ssh root@192.168.88.10 'sync; reboot'
```

## 项目结构

```
/xxl/audio/                     # 开发板目录
├── bin/
│   └── alsa_player             # 播放器可执行文件
└── assets/
    ├── xxl_test.wav            # 测试音频
    ├── booting.wav
    ├── connect_ok.wav
    ├── connect_lost.wav
    ├── low_battery.wav
    ├── rec_start.wav
    ├── rec_stop.wav
    ├── safety_alert.wav
    └── storage_full.wav

~/dvr_project/mier/audio/       # 虚拟机维护目录
├── assets/                     # 音频WAV文件 (48kHz/stereo/16bit)
├── config/
│   └── asound.conf             # ALSA配置 (已同步到 /etc/asound.conf)
├── docs/
│   └── README.md               # 本文档
├── scripts/
│   ├── gen_tts_wav.sh          # edge-tts 一键生成脚本
│   └── gen_tts.sh              # (旧) espeak-ng脚本(已废弃)
└── src/
    ├── alsa_player.c           # ALSA播放器源码
    └── Makefile                # 交叉编译Makefile
```

## 编译

```bash
cd ~/dvr_project/mier/audio/src
make clean && make
# 编译产物需为 ARM aarch64：file alsa_player
```

## 测试方法

### 1. 检查声卡是否注册
```bash
cat /proc/asound/cards
# 应显示：0 [MAX98357A]: simple-card - MAX98357A
```

### 2. 检查引脚复用
```bash
cat /sys/kernel/debug/pinctrl/pinctrl-maps | grep -A2 -B2 "40340000"
# 应显示 function af4 (不是 af3)
```

### 3. 检查PB11是否释放为GPIO
```bash
cat /sys/kernel/debug/pinctrl/pinctrl-maps | grep -i fdcan1
# 应无输出（FDCAN1已禁用）
gpioinfo | grep PB11
# 应显示 line 11: "PB11" input
```

### 4. 控制功放使能（SD_MODE接PB11）
```bash
# 拉高 PB11 使能功放
gpioset -c gpiochip1 11=1 &

# 拉低 PB11 关闭功放
killall gpioset
gpioset -c gpiochip1 11=0 &
```

### 5. 检查DMA通道
```bash
cat /sys/kernel/debug/dmaengine/summary | grep sai
cat /proc/interrupts | grep dma0chan10
# 播放时中断计数应增加
```

### 6. 播放测试
```bash
# 先使能功放
gpioset -c gpiochip1 11=1 &

# 使用系统aplay
aplay -D hw:0,0 /xxl/audio/assets/xxl_test.wav

# 或使用项目播放器
/xxl/audio/bin/alsa_player /xxl/audio/assets/xxl_test.wav

# 生成正弦波测试
speaker-test -D hw:0,0 -c 2 -t sine -f 1000
```

### 7. 调节音量
```bash
amixer -c 0 sset PCM 100%   # 最大
amixer -c 0 sset PCM 50%    # 中等
amixer -c 0 sset PCM 10%    # 较小
```

## 踩坑记录

| # | 问题 | 根因 | 解决方案 |
|---|------|------|---------|
| 1 | 完全无声音 | SD_MODE接GND=关机模式 | 改接VDD高电平 |
| 2 | Input/output error | pinmux值错误，AF4配成了AF3 | pinmux改为0x1405/0x1505/0x1605 |
| 3 | pinctrl未生效 | pinctrl放在sai4b子节点 | 移到sai4父节点 |
| 4 | DMA中断为0 | 曾误用dma1导致CID不匹配 | 使用dma0 (hpdma) |
| 5 | 启动失败/变砖 | 使用内核源码dts编译，保留内存地址不匹配 | 必须用开发板原始dtb反编译修改 |
| 6 | 无声音(旧板) | softvol范围太大(-51dB) | 改为-20dB |
| 7 | 中文语音差(旧板) | espeak-ng中文质量差 | 改用edge-tts |

## TTS语音生成

```bash
cd ~/dvr_project/mier/audio/scripts
./gen_tts_wav.sh "录制开始" rec_start
# 第三个参数可选发音人，默认 zh-CN-YunxiNeural
```

需要安装：`edge-tts`, `ffmpeg`
