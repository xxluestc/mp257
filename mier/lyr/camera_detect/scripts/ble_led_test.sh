#!/bin/sh
# Manual MP257 -> CH9140 -> WBA dual-LED test (Python standard library only).

set -eu

DEVICE="${BLE_LED_UART:-/dev/ttySTM0}"
ACTION="${1:-}"

case "$ACTION" in
    left)    COMMAND="RISK LEFT" ;;
    right)   COMMAND="RISK RIGHT" ;;
    center)  COMMAND="RISK CENTER" ;;
    clear)   COMMAND="RISK CLEAR" ;;
    left-on) COMMAND="LEFT ON" ;;
    left-off) COMMAND="LEFT OFF" ;;
    right-on) COMMAND="RIGHT ON" ;;
    right-off) COMMAND="RIGHT OFF" ;;
    ping)    COMMAND="PING" ;;
    *)
        echo "用法: $0 {left|right|center|clear|left-on|left-off|right-on|right-off|ping}" >&2
        exit 2
        ;;
esac

if pgrep -x radar_fusion >/dev/null 2>&1; then
    echo "radar_fusion 正在使用 ${DEVICE}，请先执行: systemctl stop dvr.service" >&2
    exit 3
fi

python3 - "$DEVICE" "$COMMAND" <<'PY'
import os
import select
import sys
import termios
import time

device, command = sys.argv[1], sys.argv[2]
fd = os.open(device, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
try:
    attrs = termios.tcgetattr(fd)
    attrs[0] = 0
    attrs[1] = 0
    attrs[2] = termios.CLOCAL | termios.CREAD | termios.CS8
    attrs[3] = 0
    attrs[4] = termios.B115200
    attrs[5] = termios.B115200
    attrs[6][termios.VMIN] = 0
    attrs[6][termios.VTIME] = 0
    termios.tcsetattr(fd, termios.TCSANOW, attrs)
    termios.tcflush(fd, termios.TCIOFLUSH)

    payload = (command + "\n").encode("ascii")
    os.write(fd, payload)
    print(f"TX: {command}")

    deadline = time.monotonic() + 3.0
    reply = bytearray()
    while time.monotonic() < deadline:
        ready, _, _ = select.select([fd], [], [], 0.2)
        if ready:
            data = os.read(fd, 256)
            if data:
                reply.extend(data)
                if b"\n" in reply:
                    break
    if reply:
        print("RX: " + reply.decode("utf-8", errors="replace").strip())
    else:
        print("RX: timeout（检查 WBA 是否已连接 CH9140）", file=sys.stderr)
        raise SystemExit(1)
finally:
    os.close(fd)
PY
