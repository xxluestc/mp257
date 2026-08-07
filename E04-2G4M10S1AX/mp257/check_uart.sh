#!/bin/sh
set -eu

DEVICE="${1:-/dev/ttySTM0}"
TTY_NAME="${DEVICE##*/}"

echo "MP257 Bluetooth UART preflight"
echo "device: ${DEVICE}"

if [ -e "${DEVICE}" ]; then
    ls -l "${DEVICE}"
else
    echo "ERROR: ${DEVICE} does not exist" >&2
    exit 1
fi

echo
echo "kernel command line:"
cat /proc/cmdline

echo
echo "active kernel consoles:"
cat /sys/class/tty/console/active 2>/dev/null || echo "(unavailable)"

echo
echo "serial getty:"
if command -v systemctl >/dev/null 2>&1; then
    systemctl is-active "serial-getty@${TTY_NAME}.service" 2>/dev/null || true
    systemctl is-enabled "serial-getty@${TTY_NAME}.service" 2>/dev/null || true
else
    echo "(systemd unavailable)"
fi

console_busy=0
if grep -qw "${TTY_NAME}" /sys/class/tty/console/active 2>/dev/null; then
    console_busy=1
fi
if grep -Eq "(^| )console=${TTY_NAME}(,| |$)" /proc/cmdline; then
    console_busy=1
fi

if [ "${console_busy}" -eq 1 ]; then
    echo
    echo "RESULT: BUSY - ${TTY_NAME} is still a Linux console."
    echo "Do not run Bluetooth data on it yet; boot logs/login will corrupt the stream."
    exit 2
fi

echo
echo "RESULT: READY - no active kernel-console use was detected."
