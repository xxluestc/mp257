# A35 OTA 手机与云端对接交接说明

本文面向 Android 和云端队友，说明当前已经完成的开发板能力、三方职责和第一版
联调流程。板端接口的完整字段与错误响应以 [OTA_API.md](OTA_API.md) 为准，
板端部署、打包和回滚以 [OTA.md](OTA.md) 为准。

## 1. 第一版整体链路

```text
发布人员/CI
  │ make ota-package VERSION=x.y.z
  ▼
云端对象存储/发布接口
  │ 提供 tar.gz、SHA-256、版本和发布说明
  ▼
Android 手机
  │ 先下载并校验完整包
  │ 再连接开发板 WiFi
  │ HTTP 8090 上传、触发安装、查询状态
  ▼
STM32MP257 开发板
  │ 校验 → 解压 → 停 DVR → 切换版本 → 启动
  │ → 健康检查 → 成功提交或失败自动回滚
  ▼
Android 展示最终 success / rolled_back / failed
```

第一版由手机充当云端与开发板之间的传输桥梁。开发板不会直接请求云端，也没有
“给一个下载 URL 后由开发板拉包”的接口。这样可以在开发板没有互联网、只提供
本地 WiFi 的情况下工作。

## 2. 已完成的开发板能力

板端已经完成并在真实 STM32MP257 上验证：

- 独立 `helmet-ota.service`，监听 TCP 8090，不依赖 Dashboard 8080，也不复用
  UDP 8888/8889；
- `GET /api/ota/version`；
- `POST /api/ota/upload`；
- `POST /api/ota/install`；
- `GET /api/ota/status`；
- OTA 包、外部 SHA-256、包内清单和安全解压校验；
- `/xxl/releases/<version>/` 与 `/xxl/camera_detect` 软链接切换；
- 现场 `radar_config` 持久化继承；
- 停止、启动、健康检查和失败自动回滚；
- 手机断开后，已经受理的安装继续由板端后台完成；
- A35 升级过程中不升级、也不主动停止独立运行的 M33。

真实开发板已完成多轮正向升级、手工回滚和故障包自动回滚。当前板端稳定基线为
`1.0.7`，已准备的唯一下一版联调包为`1.0.8`。为了保留真实OTA演示起点，
`1.0.8`尚未安装到开发板。该包的精确信息与验收步骤见
[RELEASE_1.0.8.md](RELEASE_1.0.8.md)。

## 3. 三方职责

### 3.1 A35/开发板负责人

- 修改并验证 `camera_detect` 运行代码；
- 每次发布使用唯一且递增的 VERSION 打包；
- 把 `.tar.gz`、`.tar.gz.sha256` 和发布说明交给云端；
- 维护 8090 API 的向后兼容；
- 根据 `/var/lib/helmet-ota/` 日志定位板端安装和回滚问题。

### 3.2 云端负责人

- 原样保存 OTA tar.gz，不能二次解压、重打包或修改字节；
- 保存并返回整个 tar.gz 的 SHA-256、字节数、版本和下载地址；
- 保证下载支持完整文件传输；第一版手机应整包下载完成后再上传到开发板；
- 不把开发板 8090 暴露到公网，也不直接控制板端安装；
- 维护版本发布、灰度范围、撤回和发布说明等云端业务规则。

建议云端给 Android 返回以下最小元数据，具体 URL 可由手机和云端队友自行约定：

```json
{
  "product": "helmet-a35",
  "version": "1.0.8",
  "download_url": "https://example.invalid/ota/helmet-a35-1.0.8.tar.gz",
  "sha256": "6c43b03a6d7974cc40f733be1324a106d5100a8ae4b9c99fe64f475936d231a8",
  "size_bytes": 8277588,
  "release_notes": "A35稳定版更新；增加TF录像故障隔离和彩色录像日志，升级后Dashboard切换OTA演示主题。",
  "mandatory": false
}
```

云端返回的 `sha256` 必须与随包生成的 `.sha256` 文件一致。

### 3.3 Android 负责人

- 向云端查询版本和下载 OTA 包；
- 下载完成后，先在手机本地计算整个 tar.gz 的 SHA-256 并比较；
- 连接开发板 WiFi 后访问 `http://192.168.88.10:8090`；
- 查询板端当前版本，避免重复升级；
- 将 tar.gz **原始字节**上传到 `/api/ota/upload`；
- 保存返回的 `package_id`，再请求 `/api/ota/install`；
- 保存 `job_id`，每秒查询 `/api/ota/status`；
- 正确区分成功、已回滚和无法恢复三种最终结果；
- WiFi/HTTP 暂时断开时先恢复连接并查询 status，不自动重复 install。

Android 必须注意：

- 上传不是 `multipart/form-data`，请求体只能是 tar.gz 原始字节；
- 必须设置确定的 `Content-Length`；
- 必须设置 `X-OTA-SHA256`；
- 上传超时建议至少 120 秒；
- `install` 返回 HTTP 202 只表示板端后台任务已经受理；
- 安装阶段连接断开不等于安装失败；
- Native App 需要允许访问板端局域网 HTTP，并确保请求实际走开发板 WiFi。

## 4. 推荐的 Android 状态机

```text
IDLE
  └─ 查询云端版本
       └─ 下载到手机
            └─ 本地 SHA-256 校验
                 └─ 等待/连接开发板 WiFi
                      └─ GET board version
                           └─ POST upload
                                └─ UPLOADED(package_id)
                                     └─ POST install
                                          └─ INSTALLING(job_id)
                                               ├─ success      → SUCCESS
                                               ├─ rolled_back  → ROLLED_BACK
                                               └─ failed       → FAILED
```

以下状态必须持久化到手机本地，避免 Activity 重建或网络切换后丢失流程：

- 云端目标版本；
- 本地包路径、大小和 SHA-256；
- 板端 `package_id`；
- 板端 `job_id`；
- 最近一次 status 响应和查询时间。

App 恢复后，如果已经拿到 `job_id` 或曾收到 install 202，应先 GET status，
不要先重新上传或重新触发安装。

## 5. Android 与板端最小联调步骤

开始联调前，电脑或手机连接开发板 WiFi，并确认：

```bash
curl http://192.168.88.10:8090/api/ota/version
curl http://192.168.88.10:8090/api/ota/status
```

Android 第一轮只需要完成：

1. 展示板端 `current_version`；
2. 从手机本地选择一个新的 OTA tar.gz；
3. 计算 SHA-256；
4. 原始字节上传并保存 `package_id`；
5. 触发 install 并保存 `job_id`；
6. 每秒刷新 `state/phase/progress`；
7. 对 `success`、`rolled_back`、`failed` 给出不同结果提示。

完整请求、成功响应和错误响应请直接照
[OTA_API.md](OTA_API.md) 实现，不要根据本页的流程图猜测字段。

## 6. 云端与手机最小联调步骤

第一版可以先不实现复杂发布系统，只需提供：

1. 一个返回最新版本元数据的 HTTPS 接口；
2. 一个可下载 tar.gz 原始文件的 HTTPS 地址；
3. 元数据中准确的 `sha256` 和 `size_bytes`。

Android 应先在有互联网的网络下完整下载包，再连接开发板 WiFi。如果手机连接
开发板 WiFi 后仍保留蜂窝网络，App 需要明确区分“访问云端的网络”和“访问
192.168.88.10 的开发板 WiFi 网络”，避免 8090 请求错误地走蜂窝网络。

云端联调的第一项验收是：Android 下载到的文件执行 SHA-256 后，与发布端生成的
`.sha256` 完全一致。只有这一项通过，才继续进行板端 upload。

## 7. 发布包生成与交付

在工程目录生成新版本：

```bash
cd mier/lyr/camera_detect
make ota-package VERSION=1.0.8
```

交给云端的文件：

```text
dist/helmet-a35-1.0.8.tar.gz
dist/helmet-a35-1.0.8.tar.gz.sha256
```

发布前必须检查：

```bash
(cd dist && sha256sum -c helmet-a35-1.0.8.tar.gz.sha256)
tar -tzf dist/helmet-a35-1.0.8.tar.gz
```

`dist/`中的临时测试版本默认不提交Git；最终`1.0.8`包和`.sha256`是发布例外，已经
随`master`提交。另一台电脑拉取后可直接把这两个文件交给云端，禁止重新压缩或改动。

## 8. 三方联合验收用例

第一轮至少完成：

1. 手机查询板端当前版本；
2. 正常上传新 VERSION 并安装成功；
3. 相同 VERSION 重复安装时正确提示失败；
4. 上传 SHA-256 错误的包时停止在 upload 阶段；
5. install 返回 202 后关闭手机页面，再打开仍能恢复进度；
6. install 后手机主动断开 WiFi，板端仍完成安装；
7. 恢复 WiFi 后手机能查到最终状态；
8. 板端返回 `rolled_back` 时，手机明确显示“升级失败，旧版已恢复”；
9. 云端文件、手机下载文件和板端 upload 响应中的 SHA-256 三者一致；
10. OTA 完成后 DVR、雷达、HUD、Dashboard 和 M33 仍正常。

受控掉电恢复测试必须在可靠供电、串口监控和原版本可回退的条件下单独进行，
不要在首次手机联调时直接断开发板电源。

## 9. 当前安全边界

第一版 8090 使用开发板私有 WiFi 内的 HTTP，没有用户认证、HTTPS 和数字签名。
SHA-256 只能验证文件完整性，不能证明发布者身份。

因此：

- 8090 只能用于开发板私有局域网，不能做端口映射或暴露公网；
- 云端必须使用 HTTPS；
- 手机必须先校验云端元数据中的 SHA-256；
- 量产前必须补充包签名、公钥验签、认证授权、防降级和防重放策略。

## 10. 问题定位时需要提供的信息

Android/云端队友反馈问题时，请同时提供：

- 云端版本、URL、size 和 SHA-256；
- 手机实际计算的 SHA-256；
- upload/install 的 HTTP 状态码和完整 JSON；
- 最近一次 `/api/ota/status` JSON；
- `package_id`、`job_id` 和发生时间；
- 是否发生 WiFi/蜂窝网络切换。

板端负责人随后检查：

```bash
systemctl status helmet-ota.service dvr.service --no-pager
cat /var/lib/helmet-ota/status.json
tail -n 100 /var/lib/helmet-ota/install.log
journalctl -u helmet-ota.service -n 100 --no-pager
```

不要通过聊天只反馈“升级失败”；缺少版本、SHA、HTTP 响应和 status 时，很难判断
问题发生在云端下载、手机网络、板端校验、安装切换还是健康检查阶段。
