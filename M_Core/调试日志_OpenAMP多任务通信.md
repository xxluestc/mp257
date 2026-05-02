# OpenAMP 异构多核通信调试日志

## 项目概述

- **平台**: STM32MP257D-ATK
- **A核**: Cortex-A35, Linux (OpenSTLinux)
- **M核**: Cortex-M33, FreeRTOS
- **通信方式**: OpenAMP / RPMsg / VIRT_UART
- **目标**: A核循环发送 1-10，M核循环发送 11-20，双向持续通信

---

## 一、最终正确架构

### 1.1 M核任务架构（4任务 + 2消息队列）

```
┌─────────────────────────────────────────────────────────────┐
│                    M核 FreeRTOS                              │
│                                                             │
│  ┌──────────────────────┐                                   │
│  │ OpenAMP_Task          │ ← 唯一调用 OPENAMP_check_for_message()
│  │ 优先级: osPriorityHigh│    的任务。通过信号量阻塞等待接收，  │
│  │                       │    通过 SendQueue 获取待发送数据。  │
│  │ 循环:                 │                                   │
│  │  1.OPENAMP_check_for  │                                   │
│  │    _message()         │                                   │
│  │  2.尝试获取信号量(100ms│                                   │
│  │    超时) → 收到数据    │                                   │
│  │    → 放入 RecvQueue   │                                   │
│  │  3.从 SendQueue 取数据 │                                   │
│  │    → VIRT_UART_       │                                   │
│  │    TransmitNB() 发送  │                                   │
│  └──────┬──────────┬─────┘                                   │
│         │RecvQueue │SendQueue                                │
│         ▼          ▲                                         │
│  ┌──────────┐  ┌──────────┐  ┌──────────┐                   │
│  │M_Recv_Task│  │M_Send_Task│  │M_Main_Task│                  │
│  │优先级:    │  │优先级:    │  │优先级:    │                  │
│  │AboveNormal│  │AboveNormal│  │Normal    │                  │
│  │           │  │           │  │           │                  │
│  │阻塞等待   │  │每秒用     │  │每2秒打印  │                  │
│  │RecvQueue  │  │vTaskDelay │  │运行状态   │                  │
│  │→ 打印接收 │  │Until发送  │  │           │                  │
│  │  数据     │  │11-20到    │  │           │                  │
│  │           │  │SendQueue  │  │           │                  │
│  └──────────┘  └──────────┘  └──────────┘                   │
│                                                             │
│  ★ 关键设计:                                                │
│  - 只有 OpenAMP_Task 调用 OPENAMP_check_for_message()       │
│  - M_Recv_Task 和 M_Send_Task 不直接操作 OpenAMP            │
│  - 所有任务间数据通过消息队列传递                             │
└─────────────────────────────────────────────────────────────┘
```

### 1.2 A核架构（3线程）

```
A核 Linux 用户空间
═══════════════════════════════════════════════════════════════

main() 主线程:
  - 打开 /dev/ttyRPMSG0 (非阻塞模式 O_NONBLOCK)
  - 配置串口参数 (raw mode, 115200)
  - 创建 recv_thread 和 send_thread
  - 每 2 秒打印 "[A-MAIN] A-core main thread is running..."

recv_thread (接收线程):
  - 循环 read() 读取 M核发来的数据
  - 打印 "[A-RECV] <- M: ..."

send_thread (发送线程):
  - 循环发送 1-10
  - 每次 write() 后 sleep(1)
  - 如果 write 返回 -1 (EAGAIN/EWOULDBLOCK):
    打印 "Buffer full, retrying..."
    usleep(500000) 后重试
```

---

## 二、调试过程（按时间顺序）

### 阶段1：初始实现 — M核发送一次后停止

**现象**:
```
[M-RECV] <- A: "1" (1 bytes)     ← 只收到一次
[M-SEND] -> A: "11" (2 bytes)    ← 只发送一次
[M-MAIN] Task started             ← 之后所有M核日志停止
```

A核侧:
```
[A-SEND] -> M: "1" ... "7"       ← 前7条正常
[A-SEND] FAILED: "8" (ret=-1, errno=12: Cannot allocate memory)
                                  ← 之后全部失败
```

**当时的代码结构**:
- M_Recv_Task 和 M_Send_Task **都在各自循环中调用 OPENAMP_check_for_message()**
- M_Recv_Task 优先级 osPriorityHigh，用 `osSemaphoreAcquire(Semaphore, 0)` 轮询
- M_Send_Task 优先级 osPriorityNormal

**根因分析**:

1. **OpenAMP 线程安全冲突（主要原因）**:
   - `OPENAMP_check_for_message()` → `MAILBOX_Poll()` 操作全局变量 `msg_received_ch1`、`msg_received_ch2`
   - 这些全局变量同时被 IPCC 中断回调修改
   - 两个不同优先级的任务同时调用 → 竞态条件
   - `MAILBOX_Poll()` 内部调用 `rproc_virtio_notified()` → `virtqueue_notification()` 操作共享内存中的 Virtio 描述符环
   - 并发访问导致 Virtio 队列状态机崩溃或死锁

2. **高优先级任务空转**:
   - M_Recv_Task (osPriorityHigh) 用 `osSemaphoreAcquire(Semaphore, 0)` 不等待轮询
   - 虽然有 osDelay(1)，但高频率轮询 + OpenAMP 调用抢占其他任务

3. **A核发送失败**:
   - M核死锁后不再调用 OPENAMP_check_for_message() → 不再释放 RPMsg 接收缓冲区
   - A核持续发送直到占满所有 RPMsg 缓冲区（通常 16 或 32 个）
   - Linux write() 返回 -1 (ENOMEM)

### 阶段2：尝试修复 — 增加栈大小、添加 fault handler 日志

**修改**:
- 任务栈从 1024/2048 增加到 4096
- 在 HardFault/MemManage/BusFault/UsageFault 中添加 loc_printf 打印
- 添加 configCHECK_FOR_STACK_OVERFLOW 和 configASSERT 日志

**结果**: 问题依旧。确认不是栈溢出，也不是 configASSERT 触发。

**回退**: 还原栈溢出检测和 configASSERT 相关修改。

### 阶段3：关键发现 — osDelay 导致任务永久阻塞

**现象**:
- 所有调用 osDelay() 的任务全部卡死
- 只有 BusyWait_Task（永不阻塞的忙等任务）能运行

**根因**: SysTick 中断处理函数未调用 FreeRTOS 的 tick 处理。

**SysTick 与 FreeRTOS tick 的关系**:

```
硬件 SysTick 定时器 (每1ms触发)
        │
        ▼
SysTick_Handler()  ← 硬件中断入口 (在 stm32mp2xx_it.c)
        │
   ┌────┴────┐
   ▼         ▼
HAL_IncTick()   xPortSysTickHandler()
(HAL库用)       (FreeRTOS用)
                    │
                    ▼
              xTaskIncrementTick()
                    │
              ├── xTickCount++ (全局tick计数器+1)
              ├── 检查延时任务链表 (有任务延时到了吗?)
              └── 如果需要切换 → 触发 PendSV
```

**FreeRTOSConfig.h 中的宏**:
```c
#define SysTick_Handler xPortSysTickHandler
```
这个宏将 port.c 中的 `SysTick_Handler` 函数重命名为 `xPortSysTickHandler`，避免和 stm32mp2xx_it.c 中的 ISR 冲突。

**修复前** (stm32mp2xx_it.c):
```c
void SysTick_Handler(void)
{
    HAL_IncTick();   // 只更新 HAL tick，FreeRTOS tick 永远不增加
}
```

**修复后**:
```c
extern void xPortSysTickHandler(void);

void SysTick_Handler(void)
{
    HAL_IncTick();
    xPortSysTickHandler();  // ★ 让 FreeRTOS tick 正常递增
}
```

**osDelay 调用链**:
```
osDelay(1000)
  → vTaskDelay(1000)
    → prvAddCurrentTaskToDelayedList(1000)
      → 唤醒时间 = xTickCount + 1000
      → 任务进入 Blocked 状态
      → 等待 SysTick 中断唤醒...

每次 SysTick 中断:
  → xPortSysTickHandler()
    → xTaskIncrementTick()
      → xTickCount++
      → 检查延时链表: 当前tick >= 唤醒时间?
        → 是: 任务移回就绪链表
        → 触发 PendSV 做上下文切换
```

**修复前**: xTickCount 永远是 0，唤醒时间 = 0 + 1000 = 1000，永远达不到 → 任务永久阻塞。

### 阶段4：重构为单 OpenAMP 任务 — 通信恢复正常

**修改**:
- 创建单一的 OpenAMP_Task，只有它调用 OPENAMP_check_for_message()
- 用 `osSemaphoreAcquire(Semaphore, pdMS_TO_TICKS(100))` 带超时阻塞等待
- 不再使用 osDelay(1)（因为信号量超时本身就是阻塞点，会自动让出 CPU）

**结果**: A核和M核双向通信正常，但只有一个任务在工作。

### 阶段5：扩展为多任务架构 — 最终方案

**新增**:
- 两个消息队列: RecvQueue (接收队列), SendQueue (发送队列)
- M_Recv_Task: 从 RecvQueue 阻塞读取，打印接收数据
- M_Send_Task: 每秒用 vTaskDelayUntil 发送 11-20 到 SendQueue
- M_Main_Task: 每 2 秒打印运行状态

**数据流**:
```
A核 → RPMsg → M核 VIRT_UART 中断
  → VIRT_UART0_RxCpltCallback()
    → osSemaphoreRelease(Semaphore)  // 通知 OpenAMP_Task
    → OpenAMP_Task 收到信号量
      → 读 VirtUart0ChannelBuffRx
      → osMessageQueuePut(RecvQueue, ...)
        → M_Recv_Task 从 RecvQueue 取出 → 打印

M_Send_Task
  → osMessageQueuePut(SendQueue, ...)
    → OpenAMP_Task 从 SendQueue 取出
      → VIRT_UART_TransmitNB() → RPMsg → A核
```

---

## 三、修改文件清单

### 3.1 stm32mp2xx_it.c — SysTick 中断修复

**路径**: `CM33/NonSecure/Core/Src/stm32mp2xx_it.c`

**修改内容**:
```c
// 添加 extern 声明
extern void xPortSysTickHandler(void);

// SysTick_Handler 中添加 FreeRTOS tick 处理
void SysTick_Handler(void)
{
    HAL_IncTick();
    xPortSysTickHandler();  // ★ 新增
}
```

### 3.2 app_freertos.c — 任务架构重构

**路径**: `CM33/NonSecure/FREERTOS/App/app_freertos.c`

**注意**: 需要同步修改两个位置:
- `CM33/NonSecure/FREERTOS/App/app_freertos.c` (源码，Makefile 编译用)
- `STM32CubeIDE/CM33/NonSecure/Application/User/FREERTOS/App/app_freertos.c` (IDE 副本)

**修改内容**:

1. 添加头文件:
```c
#include "FreeRTOS.h"
#include "task.h"
```

2. 定义消息结构体:
```c
typedef struct {
    uint8_t data[MAX_BUFFER_SIZE];
    uint16_t len;
} Msg_t;
```

3. 新增消息队列和任务句柄:
```c
static osMessageQueueId_t RecvQueue;
static osMessageQueueId_t SendQueue;
static osThreadId_t M_RecvTaskHandle;
static osThreadId_t M_SendTaskHandle;
```

4. MX_FREERTOS_Init() 中创建队列和任务:
```c
RecvQueue = osMessageQueueNew(8, sizeof(Msg_t), NULL);
SendQueue = osMessageQueueNew(8, sizeof(Msg_t), NULL);
M_RecvTaskHandle = osThreadNew(M_Recv_Task, NULL, &M_Recv_attributes);
M_SendTaskHandle = osThreadNew(M_Send_Task, NULL, &M_Send_attributes);
```

5. OpenAMP_Task — 唯一调用 OPENAMP_check_for_message():
```c
static void OpenAMP_Task(void *argument)
{
    while (1) {
        OPENAMP_check_for_message();

        // 带超时的信号量获取 (100ms)，超时自动让出CPU
        if (osOK == osSemaphoreAcquire(Semaphore, pdMS_TO_TICKS(100))) {
            Msg_t msg;
            msg.len = VirtUart0ChannelRxSize;
            memcpy(msg.data, VirtUart0ChannelBuffRx, msg.len);
            osMessageQueuePut(RecvQueue, &msg, 0, 0);
        }

        // 非阻塞取发送队列，有数据就发
        Msg_t tx_msg;
        while (osOK == osMessageQueueGet(SendQueue, &tx_msg, NULL, 0)) {
            VIRT_UART_TransmitNB(&huart0, tx_msg.data, tx_msg.len);
        }
    }
}
```

6. M_Recv_Task — 阻塞等待接收队列:
```c
static void M_Recv_Task(void *argument)
{
    while (1) {
        Msg_t msg;
        if (osOK == osMessageQueueGet(RecvQueue, &msg, NULL, osWaitForever)) {
            loc_printf("[M-RECV] <- A: \"%.*s\" (%d bytes)\r\n",
                       msg.len, msg.data, msg.len);
        }
    }
}
```

7. M_Send_Task — 精确周期发送:
```c
static void M_Send_Task(void *argument)
{
    TickType_t xLastWakeTime = xTaskGetTickCount();
    int send_num = 11;

    while (1) {
        vTaskDelayUntil(&xLastWakeTime, pdMS_TO_TICKS(1000));

        char buf[16];
        int len = snprintf(buf, sizeof(buf), "%d", send_num);
        Msg_t msg;
        msg.len = len;
        memcpy(msg.data, buf, len);

        osMessageQueuePut(SendQueue, &msg, 0, osWaitForever);

        send_num++;
        if (send_num > 20) send_num = 11;
    }
}
```

### 3.3 FreeRTOSConfig.h — 保持原始配置

**路径**: `CM33/NonSecure/FREERTOS/App/FreeRTOSConfig.h`

**关键配置**:
```c
#define configTICK_RATE_HZ       ((TickType_t)1000)  // 1ms tick
#define configMAX_PRIORITIES     ( 56 )
#define configTOTAL_HEAP_SIZE    ((size_t)32768)

#define SysTick_Handler xPortSysTickHandler  // ★ 重命名 port.c 中的函数

#define configASSERT( x ) if ((x) == 0) {taskDISABLE_INTERRUPTS(); for( ;; );}
```

### 3.4 mbox_ipcc.c — 未修改（仅分析用）

**路径**: `CM33/NonSecure/OPENAMP/mbox_ipcc.c`

**关键全局变量**（无锁保护，必须单线程访问）:
```c
int msg_received_ch1 = MBOX_NO_MSG;  // IPCC通道1状态
int msg_received_ch2 = MBOX_NO_MSG;  // IPCC通道2状态
```

**MAILBOX_Poll 函数**:
```c
int MAILBOX_Poll(struct virtio_device *vdev)
{
    if (msg_received_ch1 == MBOX_BUF_FREE) {
        rproc_virtio_notified(vdev, VRING0_ID);  // 处理缓冲区释放
        msg_received_ch1 = MBOX_NO_MSG;
    }
    if (msg_received_ch2 == MBOX_NEW_MSG) {
        rproc_virtio_notified(vdev, VRING1_ID);  // 处理新消息
        msg_received_ch2 = MBOX_NO_MSG;
    }
}
```

### 3.5 Linux/rpmsg_tty_app.c — A核应用

**路径**: `Linux/rpmsg_tty_app.c`

**关键设计**:
- 非阻塞模式打开 `/dev/ttyRPMSG0` (`O_NONBLOCK`)
- 发送失败时 (EAGAIN/EWOULDBLOCK) 等待 500ms 后重试
- 3 线程: main (状态打印), recv (接收), send (发送)

---

## 四、核心设计原则总结

| 原则 | 说明 |
|---|---|
| **OpenAMP 单线程访问** | 只有一个任务调用 OPENAMP_check_for_message()，其他任务通过消息队列间接通信 |
| **信号量超时阻塞** | `osSemaphoreAcquire(Semaphore, pdMS_TO_TICKS(100))` 替代 osDelay(1)，更高效的阻塞点 |
| **消息队列解耦** | SendQueue/RecvQueue 将数据生产和消费分离，任务间不直接共享数据 |
| **SysTick 必须调用 FreeRTOS** | stm32mp2xx_it.c 的 SysTick_Handler 必须调用 xPortSysTickHandler() |
| **vTaskDelayUntil 精确周期** | 比 osDelay 更适合周期性任务，补偿执行时间偏差 |
| **非阻塞发送** | VIRT_UART_TransmitNB() 使用 rpmsg_trysend()，不会阻塞 OpenAMP_Task |
| **A核非阻塞 + 重试** | O_NONBLOCK 打开设备，write 失败时等待重试，避免死等 |

---

## 五、文件同步注意事项

STM32CubeIDE 项目中存在**两套文件**需要同步:

| 用途 | 路径 |
|---|---|
| 源码（Makefile 编译用） | `CM33/NonSecure/FREERTOS/App/app_freertos.c` |
| IDE 编辑副本 | `STM32CubeIDE/CM33/NonSecure/Application/User/FREERTOS/App/app_freertos.c` |

修改代码时**两个文件都要改**，否则可能出现 IDE 显示的内容和实际编译的内容不一致。

---

## 六、正常运行日志示例

```
[M] Starting OpenAMP application (May  1 2026: 18:05:23)
[M] Virtual UART0 OpenAMP-rpmsg channel creation
[M] Waiting for rpmsg endpoint to be ready...
[M] RPMSG endpoint ready!
[MAIN] About to call osKernelStart()
[M-RECV] Task started
[M-SEND] Task started, sending 11-20 in loop
[M-MAIN] Task started

[A-MAIN] Opening /dev/ttyRPMSG0...
[A-RECV] Thread started
[A-MAIN] All threads started, main loop running
[A-MAIN] A-core main thread is running...
[A-SEND] Thread started, sending 1-10 in loop
[A-SEND] -> M: "1" (1 bytes)
[A-RECV] <- M: "11" (2 bytes)
[A-SEND] -> M: "2" (1 bytes)
[A-MAIN] A-core main thread is running...
...
（持续运行，不会中断）
```
