# A35 应用层 OTA 操作指南

本文只覆盖 STM32MP257 A35 Linux 应用，不升级 M33 固件、U-Boot、内核、
设备树或根文件系统。OTA 服务与 `dvr.service` 分离，固定安装在
`/opt/helmet-ota`，默认监听 TCP 8090，运行环境要求 Python 3.9 或更新版本。

## 1. 目录与事务模型

板端最终结构：

```text
/opt/helmet-ota/                    # 稳定 OTA 服务，不随业务版本切换
/var/lib/helmet-ota/                # 上传包、状态、安装日志和成功记录
/xxl/persistent/camera_detect/
└── radar_config                    # 现场配置，所有 release 共用
/xxl/releases/
├── 1.0.7/                         # 当前稳定基线
├── 1.0.8/                         # OTA成功后出现
├── legacy-YYYYmmdd-HHMMSS/         # 首次 OTA 时迁移的旧普通目录
└── .previous -> 上一稳定版本
/xxl/camera_detect -> /xxl/releases/1.0.7（升级前）
```

一次安装由板端独立执行：

```text
校验包 SHA/清单
  -> 安全解压到 staging
  -> 继承 radar_config
  -> 停止 dvr.service
  -> 原子切换 /xxl/camera_detect
  -> 启动 dvr.service
  -> 重启独立 radar-dashboard.service（加载同一release后端）
  -> 健康检查
  -> 成功记录上一稳定版本
       或失败自动切回上一版本并重新启动
```

`POST /api/ota/install` 只负责启动独立安装进程并立即返回 202。安装进程脱离
HTTP 请求和手机连接运行，因此手机断开 WiFi 不会中止已经开始的安装。板子掉电
后，`helmet-ota.service` 下次启动会检查未完成事务并优先恢复上一版本。

进入 release/软链接模式后，正式版本更新统一使用 OTA；不要再用
`make deploy-radar` 直接覆盖当前 release。后者只保留作初次部署和临时开发调试。

现场`radar_config`整体继承、不由OTA包覆盖。为兼容早期版本，若其中
`RADAR_LOG_DIR`仍指向`/run/media/mmcblk0`或`/run/media/mmcblk0p1`，新启动脚本
会仅将该运行时路径迁移到
`/usr/local/helmet/radar_experiments`；TTC、距离、角度、滤波和BLE配置保持不变。

## 2. OTA 包内容

在工程目录执行：

```bash
cd /home/alientek/dvr_project/mier/lyr/camera_detect
make ota-package VERSION=1.0.8
```

该命令先编译 A35 主程序和 HUD、检查 Shell/Python，再生成：

```text
dist/helmet-a35-1.0.8.tar.gz
dist/helmet-a35-1.0.8.tar.gz.sha256
```

包内只包含运行文件：

- `VERSION`、`MANIFEST.sha256`
- `radar_fusion`、`hud`、`start_dvr.sh`
- NPU 模型、运行时动态库、声音和导航语音缓存
- Dashboard、运行维护脚本、service/config 示例

不会包含 C/C++ 源码、目标文件、日志/CSV/录像、现场 `radar_config`、M33
固件、内核、设备树或系统底层文件。包内部每个文件都有 SHA-256 清单；外部
`.sha256` 校验整个压缩包。

版本是否升级只比较包内 `VERSION`。因此即使业务内容完全相同，
`1.0.0 -> 1.0.1` 也会正常走完完整升级流程；相同 VERSION 不允许重复安装。

检查包内容：

```bash
tar -tzf dist/helmet-a35-1.0.8.tar.gz
(cd dist && sha256sum -c helmet-a35-1.0.8.tar.gz.sha256)
```

## 3. 首次部署 OTA 服务

开发板在线后，从主机执行：

```bash
cd /home/alientek/dvr_project/mier/lyr/camera_detect
make deploy-ota-service BOARD_IP=192.168.88.10
```

这会把 OTA 服务安装到 `/opt/helmet-ota`，创建
`/var/lib/helmet-ota/uploads`，安装并启用
`/etc/systemd/system/helmet-ota.service`。它不会覆盖当前业务目录。

板端检查：

```bash
systemctl status helmet-ota.service --no-pager
ss -lntp | grep ':8090'
curl http://127.0.0.1:8090/api/ota/version
```

首次从当前普通目录 `/xxl/camera_detect` 升级时，安装器会在停止
`dvr.service` 后把该目录原样移动到 `/xxl/releases/legacy-*`，随后建立
`/xxl/camera_detect` 软链接。原有 `radar_config` 会先复制到持久化目录。

## 4. 测试 1.0.0 -> 1.0.1

主机打两个包：

```bash
make ota-package VERSION=1.0.0
make ota-package VERSION=1.0.1
make ota-test
```

`make ota-test` 在临时目录启动真实 HTTP 服务和独立安装子进程，覆盖四个接口、
旧目录迁移、配置继承、两个不同 VERSION 的切换和手工回滚。测试模式不运行
ARM 二进制，也不调用主机 `systemd`。

板端联调时，按 [OTA API](OTA_API.md) 上传并安装 1.0.0，成功后再安装
1.0.1。每次完成后检查：

```bash
readlink -f /xxl/camera_detect
cat /xxl/camera_detect/VERSION
cat /xxl/camera_detect/radar_config
systemctl is-active dvr.service helmet-ota.service
pgrep -a radar_fusion
pgrep -a hud
curl http://127.0.0.1:8080/api/state
curl http://127.0.0.1:8090/api/ota/status
```

健康检查最多等待 45 秒，至少要求：

- `/xxl/camera_detect/VERSION` 是目标版本；
- `dvr.service` 为 active；
- `radar_fusion` 和 HUD 进程存在；
- Dashboard `/api/state` 可访问且响应结构正确。

Dashboard 的传感器数据允许暂时 `stale=true`，避免在没有雷达目标或实验环境
尚未建立时误回滚；服务不可访问或响应错误仍会判定失败。

## 5. 回滚与故障排查

安装失败会自动切回安装前的稳定目录。状态可能为：

- `success`：新版本健康检查通过；
- `rolled_back`：新版本失败，上一版本恢复并通过健康检查；
- `failed`：安装失败，且自动恢复也未能确认成功；
- `installing`：仍在校验、切换、启动或检查。

手工回滚到上一稳定版本：

```bash
/usr/bin/python3 /opt/helmet-ota/helmet_ota_installer.py --rollback
```

查看记录：

```bash
cat /var/lib/helmet-ota/status.json
cat /var/lib/helmet-ota/last_success.json
journalctl -u helmet-ota.service -n 100 --no-pager
tail -n 100 /var/lib/helmet-ota/install.log
```

`install.log` 达到 5 MiB 后轮转为 `install.log.1`。不要在安装状态中手工修改
`/xxl/camera_detect`、`/xxl/releases` 或状态 JSON。

## 6. M33 生命周期

M33 仍由独立的 `dvr-m33.service`/既有早期启动链管理，不在 OTA 包中。
`start_dvr.sh` 现在默认：

- A35 服务停止、失败重启或 OTA 切换时不停止已运行的 M33；
- 发现 M33 已运行但 RPMsg 未出现时不擅自重置 M33；
- 仅在 M33 根本未运行时保留原有 Linux remoteproc 回退启动能力。

现场诊断确需恢复旧行为时，可临时给 `dvr.service` 设置
`STOP_M33_ON_EXIT=1` 或 `ALLOW_M33_RESET_ON_START_FAILURE=1`，正常 OTA
不应启用。

## 7. 真实开发板验证记录（2026-07-27）

已在 `192.168.88.10` 的真实 STM32MP257 开发板完成第一轮验证。板端环境为
Linux 6.6.48、Python 3.12.4、约 1.7 GiB RAM，测试前
`/xxl/camera_detect` 是没有 `VERSION` 的普通目录。

已通过：

1. 部署 `/opt/helmet-ota`，`helmet-ota.service` 为 enabled/active，8090
   的 version/status 接口可从主机访问；
2. 通过 HTTP 上传并安装 1.0.0，旧普通目录迁移为
   `/xxl/releases/legacy-*`，当前目录变为软链接；
3. 相同业务内容仅修改 VERSION，完成 `1.0.0 -> 1.0.1` 升级；
4. 手工从 1.0.1 回滚到 1.0.0，再切回 1.0.1；
5. 通过 OTA 安装包含运行清理修复的 1.0.2；
6. 构造 HUD 必然退出的临时 1.0.3 负向包，健康检查在 45 秒后报告
   `HUD process is not running`，随后自动恢复 1.0.2，状态为
   `rolled_back`；
7. 每次停止、切换、回滚后 `dvr.service`、`radar_fusion`、release 内 HUD
   和 Dashboard 均恢复；Dashboard 数据为 `stale=false`；
8. M33 remoteproc 在全部升级、手工回滚、失败回滚和额外 DVR 重启前后始终为
   `running`；
9. `radar_config` SHA-256 始终为
   `bfad967d6c5567f35a7bb215f06d365e9c993572035878b7d31e238472cff326`，
   证明现场配置没有被包覆盖。

实测发现并修复一处清理问题：原脚本的 `pkill -f radar_fusion` 会误杀命令行
文本中包含该名称的 SSH 运维 shell。1.0.2 已改为
`pkill -KILL -x radar_fusion`，随后在同一 SSH 命令内重启 DVR，连接保持正常。

连续执行多个正向/负向包后，Python OTA 服务曾保留约 97 MB 堆内存、峰值约
140 MB；开发板仍有约 1.5 GiB 可用。安装完成后单独重启
`helmet-ota.service`，常驻占用回到约 14 MB，DVR 和 M33 不受影响。正式升级
频率很低，因此第一版可用，后续可把包校验隔离到短生命周期子进程进一步控制
常驻内存。

该轮板端结束状态（历史记录）：

```text
/xxl/camera_detect       -> /xxl/releases/1.0.2
/xxl/releases/.previous -> /xxl/releases/1.0.1
dvr.service              active/enabled
dvr-m33.service          active/enabled
helmet-ota.service       active/enabled
M33 remoteproc           running
```

负向 1.0.3 release 和上传文件已清除，不可恢复；保留的 `rolled_back` 状态是
该次自动回滚的测试证据，`last_success.json` 仍正确记录稳定版 1.0.2。

仍需后续验证：

1. Android 真机按 [OTA API](OTA_API.md) 完成上传、进度展示和断网续查；
2. 在安装校验、切换和健康检查阶段分别测试手机断开 WiFi；
3. 做一次有串口监控和可靠供电保护的可控掉电恢复测试；
4. 室外环境验证摄像头/NPU、真实雷达告警、IMU 摔倒上报和手机短信链。

## 8. 1.0.7基线与1.0.8演示包（2026-08-09）

当前真实板端保持：

```text
/xxl/camera_detect       -> /xxl/releases/1.0.7
/xxl/releases/.previous -> /xxl/releases/1.0.6
Dashboard/API版本        1.0.7
OTA状态                  success
```

已为手机和云端联合演示生成`1.0.8`包，但没有提前安装到板端。包的唯一SHA-256为：

```text
0110247cd9fe50e8e28fe702539dfe449781b6f1256e3f45636e7d7b98182301
```

安装成功后Dashboard根据`system_version=1.0.8`自动切换为钴蓝/安全橙主题并显示
`OTA DEMO`；1.0.7仍显示原主题。准确文件大小、云端元数据和演示检查见
[RELEASE_1.0.8.md](RELEASE_1.0.8.md)。

## 9. 第一版安全边界

第一版使用 HTTPS 之外的局域网 HTTP，并以 SHA-256 保证传输完整性；SHA-256
不能证明发布者身份。测试期间应只在开发板私有 WiFi 使用 8090，不要暴露到
公网。量产前需要增加包签名、公钥验签、身份认证、重放/降级策略和 HTTPS。
