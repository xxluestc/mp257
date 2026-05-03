#!/bin/sh
LOG="/tmp/dvr.log"
PIPE="/tmp/dvr_trigger_pipe"
SD="/run/media/mmcblk0p1"

echo "========================================="
echo "  DVR System Test Script"
echo "========================================="

echo "[1/6] Cleaning up old state..."
pkill -9 dvr 2>/dev/null
echo stop > /sys/class/remoteproc/remoteproc0/state 2>/dev/null
sleep 2
rm -f "$SD"/emergency_*.mp4 2>/dev/null
> "$LOG"

echo "[2/6] Starting DVR..."
/usr/local/bin/dvr > "$LOG" 2>&1 &
sleep 5

if ! pgrep -x dvr > /dev/null; then
    echo "ERROR: DVR failed to start!"
    cat "$LOG"
    exit 1
fi
echo "      DVR started (pid=$(pgrep -x dvr))"

echo "[3/6] Restarting M-core (will send 4 commands)..."
echo start > /sys/class/remoteproc/remoteproc0/state 2>/dev/null
sleep 2

if [ "$(cat /sys/class/remoteproc/remoteproc0/state)" != "running" ]; then
    echo "WARNING: M-core not running, testing via pipe only..."
else
    echo "      M-core running"
fi

echo "[4/6] Waiting for M-core 4-cycle test (40s)..."
sleep 40

echo ""
echo "========== TEST RESULTS =========="
echo ""

echo "--- M-core Commands Received ---"
grep -c "Received from M-core" "$LOG" 2>/dev/null && echo "commands received" || echo "0 commands (M-core may not be running)"
echo ""

echo "--- State Transitions ---"
grep "STATE:" "$LOG" 2>/dev/null || echo "(none)"
echo ""

echo "--- Emergency Saves ---"
grep -E "EMERGENCY|Saving clip" "$LOG" 2>/dev/null || echo "(none)"
echo ""

echo "--- Clip Manager ---"
grep -E "CLIP|Saved normal|protected" "$LOG" 2>/dev/null || echo "(none)"
echo ""

echo "--- SD Card Files ---"
ls -lh "$SD"/emergency_*.mp4 2>/dev/null || echo "(no emergency files)"
echo ""

echo "--- Errors (if any) ---"
grep -i "error\|fail\|warn" "$LOG" 2>/dev/null | grep -v "WARNING.*SD card\|WARN.*target" | head -5 || echo "(none)"
echo ""

echo "========================================="
echo "  Manual Pipe Test (optional)"
echo "========================================="
echo "Run these commands in another terminal:"
echo "  echo 'TARGET_ON' > $PIPE"
echo "  echo 'WARNING'   > $PIPE"
echo "  echo 'FALL'      > $PIPE"
echo "  echo 'TARGET_OFF'> $PIPE"
echo ""
echo "Watch log:  tail -f $LOG"
echo "========================================="
