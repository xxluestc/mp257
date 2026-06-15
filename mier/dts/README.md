# 设备树修改记录

## 开发板信息

- **型号**: MYiR STM32MP257x Evaluation Board (2GB DDR)
- **当前使用设备树**: `myb-stm32mp257x-2GB.dtb`
- **开发板 IP**: `192.168.88.10`
- **SSH 用户**: `root` (免密登录)

## 编译方法

### 1. 反编译 dtb → dts

```bash
# 从开发板拷贝设备树到虚拟机
scp root@192.168.88.10:/boot/myb-stm32mp257x-2GB.dtb /tmp/

# 反编译为 dts
dtc -I dtb -O dts /tmp/myb-stm32mp257x-2GB.dtb -o /tmp/myb-stm32mp257x-2GB.dts
```

### 2. 修改 dts 后编译回 dtb

```bash
dtc -I dts -O dtb /tmp/myb-stm32mp257x-2GB.dts -o /tmp/myb-stm32mp257x-2GB.dtb
```

> 编译时会出现大量 `phandle reference` / `power_domains` / `resets` 等 Warning，这是 dtb↔dts 反编译再编译的正常现象，不影响功能。

### 3. 传回开发板并重启

```bash
# 备份原 dtb
ssh root@192.168.88.10 'cp /boot/myb-stm32mp257x-2GB.dtb /boot/myb-stm32mp257x-2GB.dtb.bak.$(date +%Y%m%d%H%M%S)'

# 上传新 dtb
scp /tmp/myb-stm32mp257x-2GB.dtb root@192.168.88.10:/boot/myb-stm32mp257x-2GB.dtb

# 重启生效
ssh root@192.168.88.10 'reboot'
```

## 修改内容汇总

### 修改 1: 禁用 UART4（释放 PB6、PD11）

- **节点**: `serial@40100000` (UART4)
- **修改**: `status = "disabled"`
- **释放引脚**:
  - PB6 (原 UART4_TX, AF4)
  - PD11 (原 UART4_RX, AF5)

### 修改 2: 禁用 I2C2（释放 PB4、PB5）

- **节点**: `i2c@40130000` (I2C2)
- **修改**: `status = "disabled"`
- **释放引脚**:
  - PB4 (原 I2C2_SDA, AF10)
  - PB5 (原 I2C2_SCL, AF10)
- **副作用**: I2C2 上的 eeprom@50、es8328@10 (ES8388)、stusb1600@28 均不可用

### 修改 3: 禁用 ES8388 Codec

- **节点**: `es8328@10`
- **修改**: `status = "disabled"`

### 修改 4: 新增 SAI4B 引脚配置（接 MAX98357A 功放）

- **新增节点**: `sai4b-0` (phandle = <0x500>)
- **引脚分配**:
  | 引脚 | pinmux | 功能 | 接功放 |
  |------|--------|------|--------|
  | PB4 | `0x1404` | SAI4_FS_B (AF4) | I2S 帧时钟 (LRCLK) |
  | PB5 | `0x1504` | SAI4_SD_B (AF4) | I2S 数据 |
  | PB6 | `0x1604` | SAI4_SCK_B (AF4) | BCLK 位时钟 |

### 修改 5: 启用 SAI4 / SAI4B

- **节点**: `sai@40340000` (SAI4)
  - `status = "okay"`
- **子节点**: `audio-controller@40340024` (SAI4B)
  - `status = "okay"`
  - `pinctrl-0 = <0x500>` (引用 sai4b-0)

### 修改 6: 新增 MAX98357A 音频子系统

- **新增 Codec 节点** (`/` 根节点下):
  ```dts
  max98357a_codec: max98357a {
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
          clocks = <&rcc CK_KER_SAI4>;
          bitclock-master;
          frame-master;
          dai-tdm-slot-num = <2>;
          dai-tdm-slot-width = <32>;
      };

      simple-audio-card,codec {
          sound-dai = <&max98357a_codec>;
      };
  };
  ```

### 修改 7: I2C3 引脚改为 PG1/PG2 AF9（SCL/SDA 互换）

- **节点**: `i2c3-0` / `i2c3-sleep-0`
- **原配置**:
  - `pinmux = <0x610a 0x620a>` (PG1=AF10-SCL, PG2=AF10-SDA)
- **新配置**:
  - `pinmux = <0x6209 0x6109>` (PG2=AF9-SCL, PG1=AF9-SDA)
- **时序参数**改为与原 I2C2 一致:
  - `i2c-scl-rising-time-ns = <0x64>` (100ns)
  - `i2c-scl-falling-time-ns = <0x0d>` (13ns)

## 引脚变更总览

| 引脚 | 原功能 | 新功能 |
|------|--------|--------|
| PB4 | I2C2_SDA (AF10) | SAI4_FS_B (AF4) |
| PB5 | I2C2_SCL (AF10) | SAI4_SD_B (AF4) |
| PB6 | UART4_TX (AF4) | SAI4_SCK_B (AF4) |
| PD11 | UART4_RX (AF5) | 释放为 GPIO |
| PG1 | I2C3_SCL (AF10) | I2C3_SDA (AF9) |
| PG2 | I2C3_SDA (AF10) | I2C3_SCL (AF9) |

## 文件位置

- 当前修改版 dts: `/home/alientek/dvr_project/mier/dts/myb-stm32mp257x-2GB.dts`
- 开发板 dtb: `/boot/myb-stm32mp257x-2GB.dtb`
- 开发板 dtb 备份: `/boot/myb-stm32mp257x-2GB.dtb.bak.*`
