# MS60-1211S80M-BSD 雷达调试手册

## 1. 修改代码参数（需重新编译）

编辑 `radar_init.c`，修改以下宏定义：

```c
#define TTC_THRESHOLD    10.0f   /* TTC阈值(秒)，推荐: 车辆3~5, 人员5~10 */
#define DIST_THRESHOLD   3       /* 距离阈值(米)，推荐: 2~5 */
#define LED_BLINK_ON_MS  200     /* LED亮灯时间(ms)，推荐: 100~200 */
#define LED_BLINK_OFF_MS 200     /* LED灭灯时间(ms)，推荐: 100~200 */
```

编译并上传：
```bash
export PATH=/home/alientek/Phytium_syscode/GCC编译器/gcc-arm-10.2-2020.11-x86_64-aarch64-none-linux-gnu/bin:$PATH
aarch64-none-linux-gnu-gcc -Wall -O2 -o radar_init radar_init.c -lpthread
scp radar_init root@192.168.88.10:/xxl/radar/
```

## 2. 动态调节雷达参数（无需重新编译）

开发板上使用 `/xxl/radar/send_radar_cmd.sh` 脚本：

```bash
# 查询当前感应等级
/xxl/radar/send_radar_cmd.sh 03

# 设置感应等级 (0~15, 越小越灵敏)
/xxl/radar/send_radar_cmd.sh 02 00   # 最灵敏
/xxl/radar/send_radar_cmd.sh 02 05   # 中等
/xxl/radar/send_radar_cmd.sh 02 10   # 较迟钝

# 设置检测最远距离 (单位: cm, 小端)
/xxl/radar/send_radar_cmd.sh D2 88 13   # 5000cm = 50m
/xxl/radar/send_radar_cmd.sh D2 D0 07   # 2000cm = 20m

# 查询雷达状态
/xxl/radar/send_radar_cmd.sh D0

# System Reset (异常时恢复)
/xxl/radar/send_radar_cmd.sh 09
```

## 3. 室外调试步骤

```
1. 启动程序
   cd /xxl/radar && ./radar_init

2. 观察无目标时 LED 是否误亮
   → 误亮: 调高感应等级 (02 05 或 02 08)

3. 人员从盲区方向靠近
   → 不亮: 调低感应等级 (02 00 或 02 01)
           或调小 TTC_THRESHOLD / DIST_THRESHOLD

4. 人员距离车辆 2~3m 时
   → 应稳定触发 LED 闪烁

5. 人员远离后
   → LED 应在 2~3 秒内熄灭
```

## 4. 常见问题

| 现象 | 原因 | 解决方法 |
|------|------|----------|
| LED 常亮/频繁误触发 | 环境杂波多，灵敏度过高 | 调高感应等级 (5~10) |
| 靠近不触发 | 灵敏度太低或阈值太大 | 调低感应等级 (0~2)，减小阈值 |
| 远离后还亮很久 | 雷达持续上报旧目标 | BSD 算法正常保持，稍等即可 |
| 速度始终为 0 | 固件限制 | 主要依赖 DIST_THRESHOLD 判断 |
| 保存设置超时 | 当前固件不支持 Flash 保存 | 每次上电后重新发送配置 |

## 5. 接线参考

| 开发板引脚 | 功能 | 接雷达/LED |
|-----------|------|-----------|
| PG14 (AF6) | USART1_TX | 雷达 RX |
| PG15 (AF6) | USART1_RX | 雷达 TX |
| PD11 (GPIO) | LED 输出 | LED 正极 (串联限流电阻) |
