# A35 OTA 1.0.8 发布与队友交付说明

> **当前状态**：本页对应提交`c0c57d3`之后重新生成的最终1.0.8包，已包含TF录像
> 主存储、录像故障与告警链隔离、Dashboard彩色录像日志和短时安全退出修复。包和
> `.sha256`已纳入Git跟踪；另一台电脑拉取`master`后可直接取得同一份发布文件。

## 1. 发布定位

开发板当前稳定基线保持为`1.0.7`，本次只准备下一版`1.0.8` OTA包，不提前安装到
演示板。现场演示应从`1.0.7`开始，由手机上传并触发安装`1.0.8`，用版本号、面板
主题变化和板端健康状态共同证明OTA成功。

`1.0.8`相对演示板`1.0.7`的可见差异是：

- 面板版本从`1.0.7`变为`1.0.8`；
- Dashboard自动启用“Cobalt Signal”钴蓝/安全橙主题；
- 右上角版本区域增加`OTA DEMO`标识；
- 关键事件区域显示录像触发、编码、保存和失败的分类彩色日志；
- TF异常只禁用录像，不阻断雷达判断和PD11/蓝牙LED告警；
- 页面布局、雷达阈值、按钮位置和业务操作方式不变。

主题由`/api/state`返回的精确版本控制。只有`system_version=1.0.8`才启用新主题，
`1.0.7`及其他版本继续使用原主题。

## 2. 唯一交付文件

```text
文件：helmet-a35-1.0.8.tar.gz
大小：8,277,588 bytes
SHA-256：6c43b03a6d7974cc40f733be1324a106d5100a8ae4b9c99fe64f475936d231a8

校验文件：helmet-a35-1.0.8.tar.gz.sha256
```

旧`1.0.8`确认没有发给队友或上传云端，已由本包替换。仓库构建位置：

```text
apps/a35/dist/helmet-a35-1.0.8.tar.gz
apps/a35/dist/helmet-a35-1.0.8.tar.gz.sha256
```

`dist/`中的临时测试包默认不提交Git；上述最终`1.0.8`包及其校验文件是明确例外，
已经提交。给云端队友时必须原样发送，不能解压后重新压缩，也不能修改包内文件。

建议云端元数据：

```json
{
  "product": "helmet-a35",
  "version": "1.0.8",
  "download_url": "https://<云端地址>/ota/helmet-a35-1.0.8.tar.gz",
  "sha256": "6c43b03a6d7974cc40f733be1324a106d5100a8ae4b9c99fe64f475936d231a8",
  "size_bytes": 8277588,
  "release_notes": "A35稳定版更新；增加TF录像故障隔离和彩色录像日志，OTA后Dashboard切换演示主题。",
  "mandatory": false
}
```

## 3. 已完成的发布检查

- AArch64主程序与HUD编译通过；
- Shell语法、Python编译和9项Dashboard测试通过；
- 板端安全停止与安全启动往返验证通过，Dashboard、M33、网络和OTA保持在线；
- 最终OTA包连续构建两次SHA-256完全一致；
- 包内`VERSION=1.0.8`，共60个运行文件；
- 包内存在1.0.8版本主题CSS和切换逻辑；
- 整包SHA-256校验通过；
- 包内清单校验通过；
- 包内没有源码、日志、录像、现场`radar_config`或M33固件；
- 主机OTA集成测试覆盖上传、安装、状态查询、配置继承、手工回滚和自动回滚；
- 真实板端`1.0.7`的DVR、Dashboard、M33、OTA、WiFi服务及设备节点正常；
- 新生成的15.05秒H.264录像在板载和TF重挂后均整段解码通过。

为了保留`1.0.7 → 1.0.8`真实演示条件，本轮没有把`1.0.8`安装到开发板。真实板端
安装、主题切换和安装后健康检查应在手机/云端包准备完成后联合验证。

## 4. 队友下发流程

1. 云端原样保存tar.gz，并记录本页的version、size和SHA-256；
2. Android从云端下载完整文件，在手机端计算SHA-256并比较；
3. 手机连接开发板WiFi，GET `http://192.168.88.10:8090/api/ota/version`，确认
   当前为`1.0.7`；
4. 把tar.gz原始字节POST到`/api/ota/upload`，请求头携带`X-OTA-SHA256`；
5. 保存`package_id`，POST `/api/ota/install`；
6. 每秒GET `/api/ota/status`，直到`success`、`rolled_back`或`failed`；
7. 成功后确认板端版本、Dashboard主题、DVR和M33状态。

完整请求字段和错误处理见[OTA_API.md](OTA_API.md)，三方分工见
[OTA_HANDOFF.md](OTA_HANDOFF.md)。

## 5. 演示验收

升级前：

```text
GET /api/ota/version -> current_version=1.0.7
Dashboard            -> 原青绿/深蓝主题，右上角系统版本1.0.7
```

升级后：

```text
GET /api/ota/status  -> state=success, current_version=1.0.8
Dashboard            -> 钴蓝/安全橙主题，右上角系统版本1.0.8 + OTA DEMO
dvr.service          -> active
radar_fusion / HUD   -> 进程存在
M33 remoteproc       -> running
```

若状态为`rolled_back`，手机必须明确显示“升级失败，已恢复1.0.7”，不能显示升级成功。

## 6. 重新构建

只有代码再次发生变化且新包尚未交付时才可重新构建`1.0.8`；交付后如有修改必须改用
新版本号。重建后SHA-256和大小必然变化，必须同步修改云端元数据和本页数据：

```bash
cd /home/alientek/dvr_project/apps/a35
make ota-package VERSION=1.0.8
(cd dist && sha256sum -c helmet-a35-1.0.8.tar.gz.sha256)
```

同一个`1.0.8`发布给队友后不要再用相同版本号生成不同内容。若交付后还需要修改
运行代码，应改用`1.0.9`，避免云端、手机和开发板出现同版本不同包。
