#!/bin/sh
LOG="/tmp/dvr.log"
PIPE="/tmp/dvr_trigger_pipe"
SD="/run/media/mmcblk0p1"

echo "========================================="
echo "  DVR System Test Script"
echo "========================================="

echo "[1/6] Cleaning up old state..."
pkill -9 dvr 2>/dev/null
pkill -9 weston 2>/dev/null
pkill -9 seatd 2>/dev/null
systemctl stop netdata 2>/dev/null
sleep 1
echo stop > /sys/class/remoteproc/remoteproc0/state 2>/dev/null
sleep 2
rm -f "$SD"/emergency_*.mp4 2>/dev/null
> "$LOG"

echo "[2/6] Starting DVR..."
/usr/local/bin/dvr > "$LOG" 2>&1 &
DVR_PID=$!
sleep 5

if ! kill -0 $DVR_PID 2>/dev/null; then
    echo "ERROR: DVR failed to start!"
    cat "$LOG"
    exit 1
fi
echo "      DVR started (pid=$DVR_PID)"

echo "[3/6] Restarting M-core (will send 4 commands)..."
echo start > /sys/class/remoteproc/remoteproc0.state 2>/dev/null
sleep 2

if [ "$(cat /sys/class/remoteproc/remoteproc0.state 2>/dev/null)" = "running" ]; then
    echo "      M-core running"
else
    echo "      NOTE: M-core not available, use pipe commands manually"
fi

echo "[4/6] Waiting for test (45s)..."
sleep 45

echo ""
echo "========== TEST RESULTS =========="
echo ""

echo "--- Memory ---"
free -h | grep Mem
echo ""

echo "--- M-core Commands Received ---"
CNT=$(grep -c "Received from M-core" "$LOG" 2>/dev/null)
echo "${CNT:-0} commands received"
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
grep -i "error\|fail" "$LOG" 2>/dev/null | grep -v "WARNING.*SD card\|WARN.*target" | head -5 || echo "(none)"
echo ""

echo "========================================="
echo "  Manual Pipe Test (optional)"
echo "========================================="
echo "Run these commands:"
echo "  echo 'TARGET_ON' > $PIPE"
echo "  echo 'WARNING'   > $PIPE"
echo "  echo 'FALL'      > $PIPE"
echo "  echo 'TARGET_OFF'> $PIPE"
echo ""
echo "Watch log:  tail -f $LOG"
echo "========================================="
