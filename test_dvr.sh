#!/bin/sh
LOG="/tmp/dvr.log"
PIPE="/tmp/dvr_trigger_pipe"
SD="/run/media/mmcblk0p1"

echo "========================================="
echo "  DVR System Test Script"
echo "========================================="

echo "[1/6] Cleanup..."
pkill -9 -x dvr 2>/dev/null
pkill -9 weston 2>/dev/null
pkill -9 seatd 2>/dev/null
systemctl stop netdata 2>/dev/null
sleep 1
rm -f "$SD"/emergency_*.mp4 "$SD"/dvr_buffer.bin 2>/dev/null
> "$LOG"

echo "[2/6] Starting DVR..."
/usr/local/bin/dvr > "$LOG" 2>&1 &
DVR_PID=$!
sleep 5

if ! kill -0 $DVR_PID 2>/dev/null; then
    echo "ERROR: DVR failed to start!"
    tail -15 "$LOG"
    exit 1
fi
echo "      OK (pid=$DVR_PID)"

echo "[3/6] M-core: $(cat /sys/class/remoteproc/remoteproc0.state 2>/dev/null || echo 'N/A')"

echo "[4/6] Triggers (buffer 10s, then WARNING)..."
echo TARGET_ON > "$PIPE" 2>/dev/null
sleep 10
echo WARNING > "$PIPE" 2>/dev/null

BUF_BEFORE=10
AFTER_NEEDED=$(( 30 - BUF_BEFORE ))
if [ $AFTER_NEEDED -lt 15 ]; then AFTER_NEEDED=15; fi
ENCODE_EXTRA=15
TOTAL_WAIT=$(( AFTER_NEEDED + ENCODE_EXTRA ))
echo "      Buffered ${BUF_BEFORE}s before trigger, waiting ${TOTAL_WAIT}s for save..."

sleep $TOTAL_WAIT

echo ""
echo "========== RESULTS =========="
grep "STATE:" "$LOG" 2>/dev/null || echo "(no state changes)"
grep -E "Saving|Saved|EMERGENCY|No frames" "$LOG" 2>/dev/null || echo "(no saves)"
ls -lh "$SD"/emergency_*.mp4 2>/dev/null || echo "(no files on SD)"
grep -iE "error|fail" "$LOG" 2>/dev/null | grep -v RPMSG | head -3 || echo "(no errors)"
echo "============================="
