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

log_message() {
    message=$1
    printf '%s\n' "$message"
    if command -v logger >/dev/null 2>&1; then
        logger -t helmet-safe-stop -- "$message" 2>/dev/null || true
    fi
}

log_message "action=$ACTION stopping dvr.service"
if ! systemctl stop dvr.service; then
    log_message "action=$ACTION failed: could not stop dvr.service"
    exit 1
fi
sync

if [ "$ACTION" = "stop" ]; then
    # The Dashboard is the always-on control plane. Keep it available so the
    # operator can restart the business task or request a later safe poweroff.
    log_message "action=stop complete: DVR stopped and storage synchronized; Dashboard/M33/network/OTA kept running"
    exit 0
fi

log_message "action=poweroff storage synchronized; requesting orderly system power-off"
exec systemctl poweroff
