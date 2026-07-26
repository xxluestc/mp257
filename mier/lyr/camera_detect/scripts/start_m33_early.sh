#!/bin/sh
# 在 Linux sysinit 阶段尽早启动项目 M33 固件。
# 若 remoteproc 不可用或启动失败，后续 start_dvr.sh 仍会执行同一回退流程。

set -u

FW_DIR="/home/root/project"
FW_SCRIPT="${FW_DIR}/fw_cortex_m33.sh"
RPROC_STATE="/sys/class/remoteproc/remoteproc0/state"

for i in $(seq 1 50); do
    [ -r "$RPROC_STATE" ] && break
    sleep 0.1
done

if [ ! -r "$RPROC_STATE" ]; then
    echo "[M33_EARLY] remoteproc0 unavailable; defer to dvr fallback" >&2
    exit 0
fi

state="$(tr -d '\0\r\n' < "$RPROC_STATE")"
if [ "$state" = "running" ]; then
    echo "[M33_EARLY] M33 already running"
    exit 0
fi

if [ ! -x "$FW_SCRIPT" ]; then
    echo "[M33_EARLY] firmware script unavailable; defer to dvr fallback" >&2
    exit 0
fi

echo "[M33_EARLY] starting project M33 firmware"
if ! (
    cd "$FW_DIR" || exit 1
    ./fw_cortex_m33.sh start
); then
    echo "[M33_EARLY] start failed; defer to dvr fallback" >&2
    exit 0
fi

state="$(tr -d '\0\r\n' < "$RPROC_STATE")"
if [ "$state" = "running" ]; then
    echo "[M33_EARLY] M33 running"
else
    echo "[M33_EARLY] unexpected state=${state}; defer to dvr fallback" >&2
fi
exit 0
