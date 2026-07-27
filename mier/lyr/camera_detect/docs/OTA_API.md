# A35 OTA HTTP API（Android 对接）

服务地址：

```text
http://<开发板IP>:8090
```

OTA 服务独立于 Dashboard 8080 和现有手机 UDP 8888/8889。所有响应都是
UTF-8 JSON，响应头包含 `Cache-Control: no-store`。第一版 API 版本为 1。

## 推荐调用流程

```text
GET version
  -> 手机计算整个 tar.gz 的 SHA-256
  -> POST upload（原始文件体）
  -> 保存响应 package_id
  -> POST install
  -> 每 1 秒 GET status
  -> success 完成
     rolled_back 提示升级失败但旧版已恢复
     failed 提示需要人工检查
```

`install` 返回 HTTP 202 后，手机可以断开；板端安装进程会继续运行。不要因为
单次查询超时重复提交 install，先重新请求 status。

## 1. 查询当前版本

```http
GET /api/ota/version HTTP/1.1
Host: 192.168.88.10:8090
```

成功：HTTP 200

```json
{
  "ok": true,
  "api_version": 1,
  "current_version": "1.0.0",
  "current_target": "/xxl/releases/1.0.0",
  "previous_version": "legacy",
  "previous_target": "/xxl/releases/legacy-20260727-120000"
}
```

升级前的旧普通目录没有 `VERSION` 时，`current_version` 为 `legacy`；
业务目录不存在时为 `unknown`。`previous_*` 在首次成功升级前可能是 null。

## 2. 上传 OTA 包

```http
POST /api/ota/upload HTTP/1.1
Host: 192.168.88.10:8090
Content-Type: application/gzip
Content-Length: <文件字节数>
X-OTA-SHA256: <tar.gz 整体的 64 位小写 SHA-256>

<helmet-a35-1.0.1.tar.gz 原始字节>
```

不要使用 `multipart/form-data`，请求体就是 tar.gz 原始字节。必须提供准确的
`Content-Length`，不支持 chunked upload。默认最大包大小 256 MiB。

成功：HTTP 201

```json
{
  "ok": true,
  "package_id": "5bf03c4af67f1234-8a4d1e22",
  "package_path": "/var/lib/helmet-ota/uploads/5bf03c4af67f1234-8a4d1e22.tar.gz",
  "uploaded_at": "2026-07-27T04:00:00.000+00:00",
  "version": "1.0.1",
  "sha256": "5bf03c4af67f1234...",
  "compressed_bytes": 12345678,
  "uncompressed_bytes": 23456789,
  "file_count": 42
}
```

手机只需要保存 `package_id`、`version` 和 `sha256`，不要依赖
`package_path`。

常见失败：HTTP 400

```json
{"ok":false,"error":"uploaded package SHA-256 mismatch"}
```

可能原因包括整体 SHA 不匹配、非法 tar.gz、缺少文件、包内清单错误、路径越界、
包含软链接/设备文件、包含 `radar_config` 或不在运行时白名单中的文件。

## 3. 触发安装

```http
POST /api/ota/install HTTP/1.1
Host: 192.168.88.10:8090
Content-Type: application/json
Content-Length: ...

{"package_id":"5bf03c4af67f1234-8a4d1e22"}
```

已受理：HTTP 202

```json
{
  "ok": true,
  "accepted": true,
  "job_id": "3fb64dd21cb44f4b90cd1a7ce52eca21",
  "package_id": "5bf03c4af67f1234-8a4d1e22",
  "target_version": "1.0.1",
  "installer_pid": 1234
}
```

202 只代表后台任务已启动，不代表升级成功。应保存 `job_id`，随后轮询 status。
若另一任务正在安装，返回 HTTP 409：

```json
{"ok":false,"error":"another OTA installation is already running"}
```

包不存在或 `package_id` 非法返回 HTTP 400。相同 VERSION、目标 release 已存在、
启动失败等发生在后台流程中的错误通过 status 返回。

## 4. 查询安装状态

```http
GET /api/ota/status HTTP/1.1
Host: 192.168.88.10:8090
```

成功：HTTP 200。安装中示例：

```json
{
  "ok": true,
  "api_version": 1,
  "state": "installing",
  "phase": "health_check",
  "progress": 85,
  "job_id": "3fb64dd21cb44f4b90cd1a7ce52eca21",
  "package_id": "5bf03c4af67f1234-8a4d1e22",
  "from_version": "1.0.0",
  "target_version": "1.0.1",
  "current_version": "1.0.1",
  "rollback_performed": false,
  "error": null,
  "updated_at": "2026-07-27T04:00:10.000+00:00"
}
```

成功结束：

```json
{
  "ok": true,
  "state": "success",
  "phase": "complete",
  "progress": 100,
  "current_version": "1.0.1",
  "previous_version": "1.0.0",
  "health": "dvr, radar_fusion, HUD and Dashboard are healthy",
  "rollback_performed": false,
  "error": null
}
```

自动回滚结束：

```json
{
  "ok": true,
  "state": "rolled_back",
  "phase": "complete",
  "progress": 100,
  "current_version": "1.0.0",
  "rollback_performed": true,
  "error": "health check timed out: radar_fusion process is not running"
}
```

无法恢复：

```json
{
  "ok": true,
  "state": "failed",
  "phase": "complete",
  "progress": 100,
  "rollback_performed": false,
  "error": "具体错误信息"
}
```

Android 建议按 `state` 判断：

- `idle`、`uploaded`：没有安装任务运行；
- `installing`：继续每秒查询，页面禁用重复安装；
- `success`：显示升级后的 `current_version`；
- `rolled_back`：明确提示“升级失败，已恢复旧版本”；
- `failed`：提示保留开发板电源并导出日志。

## Android 实现注意事项

- 上传前对文件原始字节计算 SHA-256，输出 64 位小写十六进制。
- OkHttp 上传应使用能返回确定 `contentLength()` 的文件 RequestBody。
- 上传和安装分别设置较长超时；status 查询使用短超时并允许重试。
- `install` 收到 202 后，即使 WiFi 短暂断开也不要重新上传；恢复连接后先查 status。
- 不要根据 HTTP 连接断开判断安装失败。
- 第一版没有认证或数字签名，只允许在开发板私有 WiFi 内使用。

## curl 联调样例

```bash
BOARD=192.168.88.10
PKG=dist/helmet-a35-1.0.1.tar.gz
SHA=$(sha256sum "$PKG" | awk '{print $1}')

curl "http://${BOARD}:8090/api/ota/version"
curl -X POST \
  -H "Content-Type: application/gzip" \
  -H "X-OTA-SHA256: ${SHA}" \
  --data-binary "@${PKG}" \
  "http://${BOARD}:8090/api/ota/upload"

# 从上传响应复制 package_id
curl -X POST \
  -H "Content-Type: application/json" \
  -d '{"package_id":"<package_id>"}' \
  "http://${BOARD}:8090/api/ota/install"

watch -n 1 "curl -s http://${BOARD}:8090/api/ota/status"
```
