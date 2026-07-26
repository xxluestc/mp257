#!/bin/sh
# 对仍被进程打开的文本日志执行 copy-truncate 轮转。
# CSV 实验数据由各写入进程自行关闭、改名、重开，避免丢失表头或破坏 CSV。

set -u

CAMERA_DIR="${CAMERA_DIR:-/xxl/camera_detect}"
HUD_LOG_PATH="${HUD_LOG_PATH:-/tmp/hud.log}"
INTERVAL_SEC="${LOG_CHECK_INTERVAL_SEC:-60}"

DVR_LOG_MAX_BYTES="${DVR_LOG_MAX_BYTES:-10485760}"          # 10 MiB
DVR_LOG_BACKUPS="${DVR_LOG_BACKUPS:-3}"
DASHBOARD_LOG_MAX_BYTES="${DASHBOARD_LOG_MAX_BYTES:-5242880}" # 5 MiB
DASHBOARD_LOG_BACKUPS="${DASHBOARD_LOG_BACKUPS:-3}"
HUD_LOG_MAX_BYTES="${HUD_LOG_MAX_BYTES:-5242880}"            # 5 MiB
HUD_LOG_BACKUPS="${HUD_LOG_BACKUPS:-2}"

ACTIVE_LOCK_DIR=""
SLEEP_PID=0

cleanup() {
    [ "$SLEEP_PID" -le 0 ] || kill "$SLEEP_PID" 2>/dev/null || true
    [ -z "$ACTIVE_LOCK_DIR" ] || rmdir "$ACTIVE_LOCK_DIR" 2>/dev/null || true
    exit 0
}
trap cleanup INT TERM HUP

is_uint() {
    case "$1" in
        ''|*[!0-9]*) return 1 ;;
        *) return 0 ;;
    esac
}

rotate_copytruncate() {
    path="$1"
    max_bytes="$2"
    backups="$3"

    [ -f "$path" ] || return 0
    is_uint "$max_bytes" || return 1
    is_uint "$backups" || return 1
    [ "$max_bytes" -gt 0 ] || return 0
    [ "$backups" -gt 0 ] || return 0

    size="$(wc -c < "$path" 2>/dev/null)" || return 0
    [ "$size" -ge "$max_bytes" ] || return 0

    lock_dir="${path}.rotate.lock"
    mkdir "$lock_dir" 2>/dev/null || return 0
    ACTIVE_LOCK_DIR="$lock_dir"

    oldest="${path}.${backups}"
    rm -f "$oldest"
    index="$backups"
    while [ "$index" -gt 1 ]; do
        previous=$((index - 1))
        [ ! -f "${path}.${previous}" ] ||
            mv -f "${path}.${previous}" "${path}.${index}"
        index="$previous"
    done

    temporary="${path}.1.tmp"
    if cp -p "$path" "$temporary" 2>/dev/null; then
        : > "$path"
        mv -f "$temporary" "${path}.1"
    else
        rm -f "$temporary"
    fi

    rmdir "$lock_dir" 2>/dev/null || true
    ACTIVE_LOCK_DIR=""
}

run_once() {
    rotate_copytruncate "${CAMERA_DIR}/dvr_system.log" \
        "$DVR_LOG_MAX_BYTES" "$DVR_LOG_BACKUPS"
    rotate_copytruncate "${CAMERA_DIR}/radar_dashboard.log" \
        "$DASHBOARD_LOG_MAX_BYTES" "$DASHBOARD_LOG_BACKUPS"
    rotate_copytruncate "$HUD_LOG_PATH" \
        "$HUD_LOG_MAX_BYTES" "$HUD_LOG_BACKUPS"
}

case "${1:---once}" in
    --once)
        run_once
        ;;
    --watch)
        while :; do
            run_once
            sleep "$INTERVAL_SEC" &
            SLEEP_PID=$!
            wait "$SLEEP_PID"
            SLEEP_PID=0
        done
        ;;
    *)
        echo "Usage: $0 [--once|--watch]" >&2
        exit 2
        ;;
esac
