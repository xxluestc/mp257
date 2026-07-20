# `danger_tts` 接收解析接口说明

## 1. 本次修改范围

本次只修改并交付以下两个源文件：

```text
nav_tts.c
nav_tts.h
```

未修改以下内容：

```text
Makefile
radar_fusion.cpp
start_dvr.sh
scripts/
nav_tts_cache/
models/
stai_mpu/
```

替换 `nav_tts.c` 和 `nav_tts.h` 后，原有导航接收、OLED 显示和导航语音逻辑保持不变，并新增 `danger_tts` 异常提醒的接收、解析和回调接口。

---

## 2. 已完成的功能

开发板已实测完成以下链路：

```text
手机通过 UDP 8888 发送 danger_tts
→ nav_tts.c 接收 JSON
→ 解析 phase / alert_type / text / distance
→ preload 只接收和记录，不触发播放
→ trigger 将最终 text 提交给注册的处理函数
```

`text` 使用手机发送的完整文本，不会在开发板端重新拼接距离。

例如：

```text
前方25米停车场出口，请减速观察
```

---

## 3. 文件替换方式

进入原工程目录：

```bash
cd ~/myir257/camera_detect
```

建议先备份原文件：

```bash
cp nav_tts.c nav_tts.c.bak
cp nav_tts.h nav_tts.h.bak
```

然后使用新的文件覆盖：

```text
新的 nav_tts.c → camera_detect/nav_tts.c
新的 nav_tts.h → camera_detect/nav_tts.h
```

不要复制其他工程文件，也不要复制已经生成的 `.o` 文件或 `radar_fusion` 可执行文件。

---

## 4. 新增接口位置

### `nav_tts.h`

搜索：

```c
DangerTextHandler
```

新增接口定义：

```c
typedef void (*DangerTextHandler)(const char *text);

void nav_tts_set_danger_text_handler(DangerTextHandler handler);
```

接口作用：注册一个文本处理函数。当收到 `phase=trigger` 的异常提醒时，系统会将最终完整文本传入该函数。

### `nav_tts.c`

可搜索以下函数：

```c
nav_tts_set_danger_text_handler
dispatch_danger_text
parse_danger_tts_json
```

各函数作用：

```text
parse_danger_tts_json
    解析 danger_tts JSON

dispatch_danger_text
    将 trigger 阶段的最终 text 交给已注册的处理函数

nav_tts_set_danger_text_handler
    保存外部注册的文本处理函数
```

---

## 5. 接口使用方法

本次没有修改 `radar_fusion.cpp`。需要在原工程的 `radar_fusion.cpp` 中手动完成一次接口注册。

搜索：

```cpp
nav_tts_start();
```

在其前面注册处理函数。

### 快速联调

直接复用工程现有的 `nav_tts_speak()`：

```cpp
nav_tts_set_danger_text_handler(nav_tts_speak);
nav_tts_start();
```

推荐写法：

```cpp
nav_tts_set_danger_text_handler(nav_tts_speak);

if (nav_tts_start() != 0) {
    fprintf(stderr, "nav_tts_start failed\n");
}
```

### 接入其他骨传导处理函数

```cpp
#include "nav_tts.h"

static void play_danger_text(const char *text)
{
    if (text == nullptr || text[0] == '\0') {
        return;
    }

    /* 在此调用实际的骨传导播放或音频队列接口 */
    bone_audio_enqueue_text(text);
}
```

启动时注册：

```cpp
nav_tts_set_danger_text_handler(play_danger_text);
nav_tts_start();
```

回调运行在 UDP 接收线程中，因此处理函数不要长时间阻塞。推荐只进行文本复制或提交到音频队列。

---

## 6. 协议与处理规则

### preload 阶段

示例：

```json
{
  "type": "danger_tts",
  "phase": "preload",
  "alert_type": "其他异常",
  "text": "前方停车场出口，请减速观察",
  "distance": 78
}
```

处理规则：

```text
接收并解析
记录日志
不调用文本处理函数
不播放
```

### trigger 阶段

示例：

```json
{
  "type": "danger_tts",
  "phase": "trigger",
  "alert_type": "其他异常",
  "text": "前方26米停车场出口，请减速观察",
  "distance": 26
}
```

处理规则：

```text
接收并解析
提取最终 text
调用已注册的文本处理函数
```

处理函数收到的 `text` 已经包含距离和完整提示语，不需要再次解析 JSON，也不要根据 `distance` 重新拼接文本。

---

## 7. 正常日志

收到 preload：

```text
[DANGER_TTS][解析成功] phase=preload ...
[DANGER_TTS][PRELOAD] 已接收，不播放
```

收到 trigger 且未注册处理函数：

```text
[DANGER_TTS][解析成功] phase=trigger ...
[DANGER_TTS][TRIGGER] 最终文本已就绪
[DANGER_TTS][接口] 文本已解析，等待接入骨传导
```

注册处理函数后：

```text
[DANGER_TTS][接口] 骨传导文本接口已注册
[DANGER_TTS][TRIGGER] 最终文本已就绪
[DANGER_TTS][接口] 提交最终文本: 前方26米停车场出口，请减速观察
```

---

## 8. 重新编译

沿用原工程的 OpenSTLinux SDK 环境：

```bash
cd ~/myir257/camera_detect

source ~/myir257/tool_chain/environment-setup-cortexa35-ostl-linux

make clean
make radar-fusion CC="$CC" CXX="$CXX"
```

不要只指定裸编译器名称：

```bash
CC=aarch64-ostl-linux-gcc
CXX=aarch64-ostl-linux-g++
```

因为这样会丢失 SDK 中的 `--sysroot` 等参数。

---

## 9. 注意事项

```text
1. 只替换 nav_tts.c 和 nav_tts.h。
2. 必须重新编译 radar_fusion。
3. 不要复制旧的 .o 文件和可执行文件。
4. 不要创建第二个 UDP 8888 接收线程。
5. preload 不播放，trigger 才调用处理函数。
6. trigger 的 text 已是最终完整文本，直接使用即可。
7. 未注册处理函数时，接收解析仍然正常，只是不执行播放。
```
