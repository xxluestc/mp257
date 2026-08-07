# 设备树修改记录

## 开发板信息

- **型号**: MYiR STM32MP257x Evaluation Board (2GB DDR)
- **当前使用设备树**: `myb-stm32mp257x-2GB.dtb`
- **开发板 IP**: `192.168.88.10`
- **SSH 用户**: `root` (免密登录)

> **重要教训**: 必须从开发板原始dtb反编译修改，**不能使用内核源码中的dts**。内核源码dts的保留内存地址与开发板实际镜像不一致，会导致启动失败或DMA异常。

## 编译方法

### 1. 反编译 dtb → dts

```bash
# 从开发板拷贝设备树到虚拟机
scp root@192.168.88.10:/boot/myb-stm32mp257x-2GB.dtb /tmp/board.dtb

# 反编译为 dts
dtc -I dtb -O dts /tmp/board.dtb -o /tmp/board.dts
```

### 2. 修改 dts 后编译回 dtb

```bash
dtc -I dts -O dtb /tmp/board.dts -o /tmp/board_new.dtb
```

> 编译时会出现大量 `phandle reference` / `power_domains` / `resets` 等 Warning，这是 dtb↔dts 反编译再编译的正常现象，不影响功能。

### 3. 传回开发板并重启

```bash
# 备份原 dtb
ssh root@192.168.88.10 'cp /boot/myb-stm32mp257x-2GB.dtb /boot/myb-stm32mp257x-2GB.dtb.bak.$(date +%Y%m%d%H%M%S)'

# 上传新 dtb
scp /tmp/board_new.dtb root@192.168.88.10:/boot/myb-stm32mp257x-2GB.dtb

# 重启生效
ssh root@192.168.88.10 'sync; reboot'
```

## 修改内容汇总

### 修改 0: PF10 配置为 TIM2_CH3 硬件 PWM

- **基线**: 从开发板当时正在运行的
  `/boot/myb-stm32mp257x-2GB.dtb`（SHA256:
  `7cf5ebd3daaf66d13999f3356f8a09a97b5dd01b96bda99a9957ad7bbf058270`）
  反编译后修改，不使用内核源码中的通用 DTS。
- **UART8**: `serial@40380000` 继续保持 `disabled`，释放 PF10/PF11。
- **TIM2**: `timer@40000000` 和其 `pwm` 子节点改为 `okay`。
- **PF10**: `pinmux = <0x5a08>`，对应 TIM2_CH3；休眠态为
  `pinmux = <0x5a11>`。
- **PWM 通道**: Linux `pwmchip` 中的通道号为 `2`（CH3）。
- **当前 DTB SHA256**:
  `bc27687bb01dca448761ea1ceecd288a26960d1f9d2f385a7b0914b12cc0a045`。
- **回退备份**:
  `/boot/myb-stm32mp257x-2GB.dtb.bak-before-pf10-pwm-20260807`。

亮度控制程序位于开发板 `/home/root/pf10_pwm`：

```bash
# 1 kHz、5% / 20% / 50% 亮度
/home/root/pf10_pwm 5
/home/root/pf10_pwm 20
/home/root/pf10_pwm 50

# 关闭
/home/root/pf10_pwm 0

# 可选：指定频率和反相极性
/home/root/pf10_pwm 20 1000 inversed
```

该程序使用 TIM2 硬件 PWM，不通过 Linux 用户态循环翻转 GPIO，因此不会持续占用
CPU，输出抖动也明显小于软件 PWM。

### 修改 1: 禁用 UART4（释放 PB6）

- **节点**: `serial@40100000` (UART4)
- **修改**: `status = "disabled"`
- **释放引脚**:
  - PB6 (原 UART4_RX, AF3)

### 修改 2: 禁用 I2C2（释放 PB4、PB5）

- **节点**: `i2c@40130000` (I2C2)
- **修改**: `status = "disabled"`
- **释放引脚**:
  - PB4 (原 I2C2_SDA)
  - PB5 (原 I2C2_SCL)
- **副作用**: I2C2 上的 eeprom@50、es8328@10 (ES8388)、stusb1600@28 均不可用

### 修改 3: 禁用 ES8388 相关节点

- **节点**: `ES8388-Sound`
- **修改**: `status = "disabled"`
- **说明**: `es8328@10` 节点因父节点 `i2c@40130000` 已禁用，无需单独修改

### 修改 4: 新增 SAI4B 引脚配置（接 MAX98357A 功放）

- **新增节点**: `sai4b-0` (phandle = <0x500>)
- **引脚分配**:
  | 引脚 | pinmux | 功能 | 接功放 |
  |------|--------|------|--------|
  | PB4 | `0x1405` | SAI4_FS_B (AF4) | I2S 帧时钟 (LRCLK) |
  | PB5 | `0x1505` | SAI4_SD_B (AF4) | I2S 数据 |
  | PB6 | `0x1605` | SAI4_SCK_B (AF4) | BCLK 位时钟 |

> **注意**: `AF4` 在 `stm32-pinfunc.h` 中对应数值为 `0x05`，不是 `0x04`。`0x04` 对应的是 `AF3`。

### 修改 5: 启用 SAI4 / SAI4B

- **节点**: `sai@40340000` (SAI4)
  - `status = "okay"`
  - `pinctrl-names = "default"`
  - `pinctrl-0 = <0x500>` (引用 sai4b-0)
  - **注意**: pinctrl 必须在 **父节点** (sai@40340000) 上配置，不能放在子节点 (audio-controller@40340024)

- **子节点**: `audio-controller@40340024` (SAI4B)
  - `status = "okay"`
  - `dma-names = "tx"`
  - `dmas = <&hpdma 80 0x43 0x21>` (使用 dma0 / hpdma)
  - `dai-tdm-slot-num = <2>`
  - `dai-tdm-slot-width = <32>`

### 修改 6: 新增 MAX98357A 音频子系统

- **新增 Codec 节点** (`/` 根节点下):
  ```dts
  max98357a {
      compatible = "maxim,max98357a";
      #sound-dai-cells = <0>;
      status = "okay";
  };
  ```

- **新增 Sound Card 节点** (`/` 根节点下):
  ```dts
  sound {
      compatible = "simple-audio-card";
      label = "MAX98357A";
      simple-audio-card,format = "i2s";
      simple-audio-card,mclk-fs = <256>;

      simple-audio-card,cpu {
          sound-dai = <&sai4b>;
          bitclock-master;
          frame-master;
          dai-tdm-slot-num = <2>;
          dai-tdm-slot-width = <32>;
      };

      simple-audio-card,codec {
          sound-dai = <&max98357a>;
      };
  };
  ```

### 修改 7: 禁用 FDCAN1（释放 PB11 为 GPIO）

- **节点**: `can@402d0000` (FDCAN1)
- **修改**: `status = "disabled"`
- **释放引脚**:
  - PB11 (原 FDCAN1_RX, AF8) → 用作 GPIO 控制 MAX98357A 的 SD_MODE
  - PB9 (原 FDCAN1_TX, AF8) → 同步释放为 GPIO

## 引脚变更总览

| 引脚 | 原功能 | 新功能 |
|------|--------|--------|
| PB4 | I2C2_SDA | SAI4_FS_B (AF4) |
| PB5 | I2C2_SCL | SAI4_SD_B (AF4) |
| PB6 | UART4_RX (AF3) | SAI4_SCK_B (AF4) |
| PB11 | FDCAN1_RX (AF8) | GPIO 控制 MAX98357A SD_MODE |
| PF10 | UART8_TX (AF6) | TIM2_CH3 硬件 PWM |

## 关键踩坑记录

| # | 问题 | 根因 | 解决方案 |
|---|------|------|---------|
| 1 | Input/output error，DMA中断为0 | pinmux值错误，`AF4`写成了`0x04`（实际应为`0x05`） | 修正为 `0x1405/0x1505/0x1605` |
| 2 | pinctrl未生效 | 把 `pinctrl-0` 放在了 `sai4b` 子节点上 | 移到 `sai@40340000` 父节点 |
| 3 | DMA传输失败 | 曾尝试使用 `dma1`，CID不匹配导致请求被过滤 | 使用 `dma0` (`hpdma`) |
| 4 | 开发板启动失败/变砖 | 使用内核源码dts编译，保留内存地址与镜像不匹配 | **必须用开发板原始dtb反编译修改** |
| 5 | GAIN增益选择困惑 | 不清楚不同dB对应的放大倍数 | 悬空=9dB（推荐），接VDD=6dB，接GND=12dB |

## 测试验证

```bash
# 检查声卡注册
cat /proc/asound/cards
# 应显示: 0 [MAX98357A]: simple-card - MAX98357A

# 检查引脚复用
cat /sys/kernel/debug/pinctrl/pinctrl-maps | grep -A2 -B2 "40340000"
# 应显示 function af4

# 检查DMA中断
cat /proc/interrupts | grep dma0chan10
# 播放时中断计数应持续增加

# 播放测试
aplay -D hw:0,0 /xxl/camera_detect/sounds/collision_alert.wav
speaker-test -D hw:0,0 -c 2 -t sine -f 1000
```

## 文件位置

- 当前修改版 dts: `/home/alientek/dvr_project/mier/lyr/camera_detect/board/myb-stm32mp257x-2GB.dts`
- 开发板 dtb: `/boot/myb-stm32mp257x-2GB.dtb`
- 开发板 dtb 备份: `/boot/myb-stm32mp257x-2GB.dtb.bak.*`
