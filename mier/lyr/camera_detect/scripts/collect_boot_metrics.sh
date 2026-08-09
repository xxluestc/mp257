#!/bin/sh
# 只读采集一次 STM32MP257 启动时间和核心功能状态。
# 输出使用内核单调时钟，避免 RTC/NTP 校时造成墙钟时间跳变。

set -u

echo "=== boot_time ==="
systemd-analyze time

echo "=== critical_chain ==="
systemd-analyze critical-chain --no-pager

echo "=== slow_units ==="
systemd-analyze blame --no-pager | head -30

echo "=== unit_monotonic_timestamps ==="
for unit in \
    dvr-m33.service \
    dvr.service \
    systemd-udev-settle.service \
    systemd-fsck@dev-mmcblk0p1.service \
    systemd-networkd-wait-online.service \
    hostapd.service \
    dnsmasq.service \
    helmet-ota.service; do
    systemctl show "$unit" \
        -p Id \
        -p ActiveState \
        -p SubState \
        -p ExecMainStartTimestampMonotonic \
        -p ExecMainExitTimestampMonotonic \
        -p ActiveEnterTimestampMonotonic
done

echo "=== application_milestones ==="
journalctl -b -u dvr-m33.service -u dvr.service \
    -o short-monotonic --no-pager |
    grep -E 'M33_EARLY|\[启动\]|Initialization complete|RPMsg ready message|OLED 初始化成功' || true

echo "=== device_initialization_usec ==="
for device in /dev/ttyRPMSG0 /dev/ttySTM0 /dev/ttySTM1 /dev/video7; do
    if [ -e "$device" ]; then
        usec="$(udevadm info -q property -n "$device" 2>/dev/null |
            sed -n 's/^USEC_INITIALIZED=//p')"
        printf '%s\t%s\n' "$device" "${usec:-unknown}"
    else
        printf '%s\tmissing\n' "$device"
    fi
done

echo "=== health ==="
systemctl is-active \
    dvr-m33.service dvr.service hostapd.service dnsmasq.service helmet-ota.service
printf 'remoteproc0='; cat /sys/class/remoteproc/remoteproc0/state 2>/dev/null || true
mountpoint /run/media/mmcblk0p1
printf 'radar_fusion='; pgrep -x radar_fusion >/dev/null && echo running || echo missing
printf 'hud='; pgrep -x hud >/dev/null && echo running || echo missing
printf 'dashboard='; \
    pgrep -f '^python3 -u /xxl/camera_detect/dashboard/radar_dashboard.py' \
        >/dev/null && echo running || echo missing
curl -fsS --max-time 3 http://127.0.0.1:8080/api/state || true
echo
curl -fsS --max-time 3 http://127.0.0.1:8090/api/ota/version || true
echo
ls -l \
    /run/media/mmcblk0p1/dvr/radar_experiments/radar_state.json \
    /run/media/mmcblk0p1/dvr/radar_experiments/sensor_events.csv 2>/dev/null || true

echo "=== failed_units ==="
systemctl --failed --no-pager
