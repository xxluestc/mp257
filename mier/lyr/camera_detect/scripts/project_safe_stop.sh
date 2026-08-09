#!/bin/sh
# Stop helmet A35 application services and flush storage before power removal.

set -eu

ACTION=${1:-stop}
DELAY_SEC=${SAFE_STOP_DELAY_SEC:-0}

case "$ACTION" in
    stop|poweroff) ;;
    *)
        echo "Usage: $0 {stop|poweroff}" >&2
        exit 2
        ;;
esac

case "$DELAY_SEC" in
    ''|*[!0-9]*)
        echo "SAFE_STOP_DELAY_SEC must be a non-negative integer" >&2
        exit 2
        ;;
esac

if [ "$DELAY_SEC" -gt 0 ]; then
    sleep "$DELAY_SEC"
fi

# A35 OTA does not own the independently started M33 firmware.  Keep M33,
# network and OTA alive when only stopping the application task.
systemctl stop dvr.service 2>/dev/null || true
sync

if [ "$ACTION" = "stop" ]; then
    systemctl stop radar-dashboard.service 2>/dev/null || true
    sync
    echo "A35 DVR and Dashboard services stopped safely; M33/network/OTA kept running"
    exit 0
fi

echo "Storage synchronized; requesting orderly system power-off"
systemctl poweroff
