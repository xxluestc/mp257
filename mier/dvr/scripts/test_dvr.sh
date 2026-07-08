#!/bin/sh
# ============================================================
#  DVR 行车记录测试脚本 (USB摄像头)
#
#  使用方法:
#    开发板上 (/xxl/dvr/):
#      ./test_dvr.sh                        # 自动模式测试
#      ./test_dvr.sh -m manual              # 手动管道模式测试
#      ./test_dvr.sh -m auto -t 3 -c 25     # 自动模式, 自定义延迟
#
#  流程 (自动模式):
#    [1] 启动DVR (--auto --target-delay N --collision-delay N)
#    [2] DVR自动: TARGET_ON → 缓冲 → COLLISION → 保存 → 退出
#    [3] 输出结果
#
#  流程 (手动模式):
#    [1] 启动DVR (后台)
#    [2] 发送 TARGET_ON
#    [3] 等待缓冲区积累
#    [4] 发送 WARNING (触发保存 15s前 + 15s后)
#    [5] 等待编码完成
#    [6] 输出结果
# ============================================================

PIPE="/tmp/dvr_trigger_pipe"
OUTPUT_DIR="/run/media/mmcblk0p1/dvr"
MODE="auto"                  # auto | manual
TARGET_DELAY=5               # TARGET_ON延迟(仅auto)
COLLISION_DELAY=25           # COLLISION延迟(仅auto, 需>=15确保完整15s前)
MANUAL_WAIT=15               # 手动模式等待时间

usage() {
    echo "Usage: $0 [-m auto|manual] [-t target_delay] [-c collision_delay] [-w manual_wait] [-h]"
    echo ""
    echo "Options:"
    echo "  -m MODE     Test mode: auto (default) or manual"
    echo "  -t SEC      Auto mode: delay before TARGET_ON (default: 5)"
    echo "  -c SEC      Auto mode: delay after TARGET_ON before COLLISION (default: 25)"
    echo "  -w SEC      Manual mode: wait time after TARGET_ON (default: 15)"
    echo "  -h          Show this help"
    exit 0
}

while getopts "m:t:c:w:h" opt; do
    case $opt in
        m) MODE="$OPTARG" ;;
        t) TARGET_DELAY="$OPTARG" ;;
        c) COLLISION_DELAY="$OPTARG" ;;
        w) MANUAL_WAIT="$OPTARG" ;;
        h) usage ;;
    esac
done

echo "========================================="
echo "  DVR Dashcam Test (USB Camera)"
echo "  Mode: $MODE"
echo "========================================="

# ---- [1] 清理环境 ----
echo "[1] Cleanup..."
killall -9 dvr 2>/dev/null
sleep 1
mkdir -p "$OUTPUT_DIR"
rm -f "$OUTPUT_DIR"/emergency_*.mp4 "$OUTPUT_DIR"/dvr_buffer.bin "$OUTPUT_DIR"/dvr_tmp.mjpg "$OUTPUT_DIR"/dvr.log 2>/dev/null

# ---- [2] 启动DVR ----
echo "[2] Starting DVR..."
DIR=$(dirname "$0")

if [ "$MODE" = "auto" ]; then
    # 自动模式: DVR自行管理整个流程, 完成后自动退出
    echo "      Mode: AUTO (target=${TARGET_DELAY}s, collision=${COLLISION_DELAY}s)"

    # 计算预估总时间: target_delay + collision_delay + save_after(15s) + encode(~10s) + margin
    ESTIMATED=$(( TARGET_DELAY + COLLISION_DELAY + 15 + 15 ))
    echo "      Estimated total time: ~${ESTIMATED}s"

    "$DIR/dvr" --auto \
        --target-delay "$TARGET_DELAY" \
        --collision-delay "$COLLISION_DELAY" \
        --auto-event collision \
        -d /dev/video7 -s "$OUTPUT_DIR" \
        -W 1280 -H 720 -f 25 \
        > "$OUTPUT_DIR/dvr.log" 2>&1

    DVR_EXIT_CODE=$?
    echo "      DVR exited with code=$DVR_EXIT_CODE"

else
    # 手动模式: 使用命名管道触发
    echo "      Mode: MANUAL (pipe trigger)"

    "$DIR/dvr" -d /dev/video7 -s "$OUTPUT_DIR" -W 1280 -H 720 -f 25 \
        > "$OUTPUT_DIR/dvr.log" 2>&1 &
    DVR_PID=$!
    sleep 3

    # 检查DVR是否成功启动
    if ! kill -0 $DVR_PID 2>/dev/null; then
        echo "ERROR: DVR failed to start!"
        tail -20 "$OUTPUT_DIR/dvr.log"
        exit 1
    fi
    echo "      OK (pid=$DVR_PID)"

    # 等待管道创建
    sleep 1
    if [ ! -p "$PIPE" ]; then
        echo "ERROR: Trigger pipe not created: $PIPE"
        kill $DVR_PID 2>/dev/null
        exit 1
    fi

    # ---- [3] TARGET_ON ----
    echo "[3] Sending TARGET_ON..."
    echo "TARGET_ON" > "$PIPE"
    echo "      Waiting ${MANUAL_WAIT}s to build buffer..."

    # ---- [4] 等待然后触发WARNING ----
    sleep "$MANUAL_WAIT"

    echo "[4] Sending WARNING (save 15s before + 15s after)..."
    echo "WARNING" > "$PIPE"

    # 等待: save_after_seconds + 编码时间
    TOTAL_WAIT=35
    echo "      Waiting ${TOTAL_WAIT}s for save+encode..."
    sleep $TOTAL_WAIT

    # 停止DVR
    kill $DVR_PID 2>/dev/null
    sleep 1
fi

# ---- 输出结果 ----
echo ""
echo "========== RESULTS =========="

echo "--- State changes ---"
grep "STATE:" "$OUTPUT_DIR/dvr.log" 2>/dev/null || echo "(none)"

echo ""
echo "--- Auto trigger ---"
grep "\[AUTO\]" "$OUTPUT_DIR/dvr.log" 2>/dev/null || echo "(none)"

echo ""
echo "--- Save events ---"
grep -E "Saving|Saved|EMERGENCY|No frames|Async encoding" "$OUTPUT_DIR/dvr.log" 2>/dev/null || echo "(none)"

echo ""
echo "--- Frame info ---"
grep -E "Frames=|window=|actual=|fps=" "$OUTPUT_DIR/dvr.log" 2>/dev/null || echo "(none)"

echo ""
echo "--- Output files ---"
ls -lh "$OUTPUT_DIR"/emergency_*.mp4 2>/dev/null || echo "(no mp4 files)"

echo ""
echo "--- Full log: $OUTPUT_DIR/dvr.log ---"
echo "--- Errors ---"
grep -iE "error|fail" "$OUTPUT_DIR/dvr.log" 2>/dev/null | head -10 || echo "(no errors)"

echo ""
echo "--- ffmpeg log ---"
if [ -f /tmp/dvr_ffmpeg.log ]; then
    head -20 /tmp/dvr_ffmpeg.log
else
    echo "(no ffmpeg log)"
fi

echo "============================="

# ---- SCP提示 ----
echo ""
echo "=== To copy video to PC ==="
for f in "$OUTPUT_DIR"/emergency_*.mp4; do
    if [ -f "$f" ]; then
        echo "  scp root@192.168.88.10:$f /home/alientek/dvr_project/mier/dvr/"
    fi
done
echo ""
echo "Done."