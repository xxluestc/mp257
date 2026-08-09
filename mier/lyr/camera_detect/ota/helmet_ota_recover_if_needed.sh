#!/bin/sh
# 常见启动路径只检查事务状态；仅有未完成安装时加载完整 Python 恢复器。

set -eu

STATUS_PATH="${HELMET_OTA_STATUS_PATH:-/var/lib/helmet-ota/status.json}"
INSTALLER="${HELMET_OTA_INSTALLER:-/opt/helmet-ota/helmet_ota_installer.py}"

if [ ! -f "$STATUS_PATH" ] ||
   ! grep -Eq '"state"[[:space:]]*:[[:space:]]*"installing"' "$STATUS_PATH"; then
    exit 0
fi

exec /usr/bin/python3 "$INSTALLER" --recover
