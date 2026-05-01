#!/bin/bash

PIPE=/tmp/dvr_trigger_pipe

if [ ! -p "$PIPE" ]; then
    echo "Pipe $PIPE not found. Is dvr running?"
    exit 1
fi

send_cmd() {
    echo "$1" > "$PIPE"
    echo "Sent: $1"
}

echo "========================================"
echo "  DVR Trigger Test Tool"
echo "========================================"
echo "  Commands:"
echo "    1 - TARGET_ON  (start buffering)"
echo "    2 - TARGET_OFF (stop buffering)"
echo "    3 - WARNING    (emergency save)"
echo "    4 - FALL       (emergency save)"
echo "    5 - COLLISION  (emergency save)"
echo "    q - quit"
echo "========================================"

while true; do
    read -p "> " cmd
    case "$cmd" in
        1) send_cmd "TARGET_ON" ;;
        2) send_cmd "TARGET_OFF" ;;
        3) send_cmd "WARNING" ;;
        4) send_cmd "FALL" ;;
        5) send_cmd "COLLISION" ;;
        q) exit 0 ;;
        *) echo "Unknown: $cmd" ;;
    esac
done
