#!/bin/sh
# 清除本项目运行日志，保留正式 MP4、人工 labels.csv 和系统 journal。

set -eu

CAMERA_DIR="${CAMERA_DIR:-/xxl/camera_detect}"
RADAR_LOG_DIR="${RADAR_LOG_DIR:-/usr/local/helmet/radar_experiments}"
DVR_SERVICE="${DVR_SERVICE:-dvr.service}"
SERVICE_WAS_ACTIVE=0
SERVICE_RESTARTED=0

if [ "$(id -u)" -ne 0 ]; then
    echo "请以 root 运行: $0" >&2
    exit 1
fi

restart_if_needed() {
    if [ "$SERVICE_WAS_ACTIVE" -eq 1 ] &&
       [ "$SERVICE_RESTARTED" -eq 0 ]; then
        systemctl start "$DVR_SERVICE" || true
    fi
}
trap restart_if_needed EXIT INT TERM HUP

remove_numbered_family() {
    path="$1"
    backups="$2"
    rm -f "$path"
    index=1
    while [ "$index" -le "$backups" ]; do
        rm -f "${path}.${index}"
        index=$((index + 1))
    done
    rm -f "${path}.1.tmp"
    rmdir "${path}.rotate.lock" 2>/dev/null || true
}

if systemctl is-active --quiet "$DVR_SERVICE"; then
    SERVICE_WAS_ACTIVE=1
    systemctl stop "$DVR_SERVICE"
fi

remove_numbered_family "${CAMERA_DIR}/dvr_system.log" 3
remove_numbered_family "${CAMERA_DIR}/radar_dashboard.log" 3
remove_numbered_family "/tmp/hud.log" 2
rm -f /tmp/nav_aplay_err.log
rm -f "${CAMERA_DIR}/danger_tts_samples.log"

if [ -d "$RADAR_LOG_DIR" ]; then
    remove_numbered_family "${RADAR_LOG_DIR}/radar_data.csv" 4
    remove_numbered_family "${RADAR_LOG_DIR}/sensor_events.csv" 4
    remove_numbered_family "${RADAR_LOG_DIR}/imu_delivery.csv" 4
    rm -f "${RADAR_LOG_DIR}/radar_state.json"
fi

sync

if [ "$SERVICE_WAS_ACTIVE" -eq 1 ]; then
    systemctl start "$DVR_SERVICE"
    SERVICE_RESTARTED=1
fi

trap - EXIT INT TERM HUP
echo "项目历史日志已清除；MP4、labels.csv 和 systemd journal 均保留。"
