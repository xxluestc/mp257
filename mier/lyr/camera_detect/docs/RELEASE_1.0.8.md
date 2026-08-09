# A35 OTA 1.0.8 发布与队友交付说明

> 当前状态：等待确认旧`1.0.8`是否已经交付。2026-08-09生成的旧包不包含后续
> “安全停止/安全启动项目”修复，已经失效，请勿上传云端或继续转发。

## 1. 发布定位

开发板当前稳定基线保持为`1.0.7`，本次只准备下一版`1.0.8` OTA包，不提前安装到
演示板。现场演示应从`1.0.7`开始，由手机上传并触发安装`1.0.8`，用版本号、面板
主题变化和板端健康状态共同证明OTA成功。

最终演示包应包含当前仓库已验证的全部A35功能修复。可见差异是：

- 面板版本从`1.0.7`变为`1.0.8`；
- Dashboard自动启用“Cobalt Signal”钴蓝/安全橙主题；
- 右上角版本区域增加`OTA DEMO`标识；
- 页面布局、雷达阈值、按钮位置和业务操作方式不变。

主题由`/api/state`返回的精确版本控制。只有`system_version=1.0.8`才启用新主题，
`1.0.7`及其他版本继续使用原主题。

## 2. 版本与交付文件待确认

先确认2026-08-09生成的旧`1.0.8`是否已经发给队友或上传云端：

- 尚未交付：删除旧构建产物，在最终代码上重新生成`1.0.8`，更新本页大小和SHA-256；
- 已经交付或上传：不得覆盖旧`1.0.8`，必须生成`1.0.9`并同步修改云端元数据。

在最终包重新构建并验证以前，旧文件大小`8,274,938`及旧SHA-256均不再是有效的
发布信息。仓库构建位置仍为：

```text
mier/lyr/camera_detect/dist/helmet-a35-1.0.8.tar.gz
mier/lyr/camera_detect/dist/helmet-a35-1.0.8.tar.gz.sha256
```

`dist/`是本机构建产物，不提交Git。给云端队友时必须把上述两个文件原样发送，不能
解压后重新压缩，也不能修改包内文件。

最终云端元数据模板：

```json
{
  "product": "helmet-a35",
  "version": "<确认后的版本>",
  "download_url": "https://<云端地址>/ota/helmet-a35-<版本>.tar.gz",
  "sha256": "<最终包SHA-256>",
  "size_bytes": "<最终包字节数>",
  "release_notes": "A35稳定版更新；OTA完成后Dashboard切换为钴蓝/安全橙演示主题。",
  "mandatory": false
}
```

## 3. 已完成的发布检查

- AArch64主程序与HUD编译通过；
- Shell语法、Python编译和6项Dashboard测试通过；
- 板端安全停止与安全启动往返验证通过，Dashboard、M33、网络和OTA保持在线；
- 旧OTA包曾连续构建两次且SHA-256一致，但因后续运行代码变化已失效；
- 最终包仍需在版本确认后重新构建并复验；
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

确认旧包尚未交付时，可重新构建`1.0.8`；否则把命令中的版本改为`1.0.9`。重建后
SHA-256和大小必然变化，必须同步修改云端元数据和本页数据：

```bash
cd /home/alientek/dvr_project/mier/lyr/camera_detect
make ota-package VERSION=1.0.8
(cd dist && sha256sum -c helmet-a35-1.0.8.tar.gz.sha256)
```

同一个`1.0.8`发布给队友后不要再用相同版本号生成不同内容。若交付后还需要修改
运行代码，应改用`1.0.9`，避免云端、手机和开发板出现同版本不同包。
