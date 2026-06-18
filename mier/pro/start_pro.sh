#!/bin/sh
# start_pro.sh - 雷达 + DVR 联调启动脚本
# 部署路径: /xxl/pro/start_pro.sh
#
# 架构:
#   radar_link (纯雷达) → 命名管道 → DVR (行车记录 + NPU融合)
#
# 用法:
#   ./start_pro.sh              # 启动DVR后台 + radar_link前台
#   ./start_pro.sh stop         # 停止所有相关进程

DVR_BIN="/xxl/dvr/dvr"
RADAR_BIN="/xxl/pro/radar_link"
DVR_STORAGE="/run/media/mmcblk0p1/dvr"
DVR_LOG="/tmp/dvr.log"

# NPU模型路径 (如果存在则启用NPU融合)
NPU_MODEL="/usr/local/share/npu/ssd_mobilenet_v2_fpnlite_10_256_int8_per_tensor.nb"
NPU_LABELS="/usr/local/share/npu/labels_coco_dataset_80.txt"

case "$1" in
    stop)
        echo "[PRO] Stopping DVR and radar..."
        pkill -f "dvr"
        pkill -f "radar_link"
        sleep 1
        echo "[PRO] Stopped."
        exit 0
        ;;
esac

echo "========================================"
echo "  Radar + DVR + NPU Fusion System"
echo "========================================"

# 确保 DVR 存储目录存在
mkdir -p "$DVR_STORAGE"

# 启动 DVR (后台常驻，等待管道触发)
if [ -x "$DVR_BIN" ]; then
    echo "[PRO] Starting DVR (background)..."
    DVR_CMD="$DVR_BIN -d /dev/video6 -s $DVR_STORAGE -W 1280 -H 720 -f 25"

    # 如果NPU模型存在，启用NPU融合验证
    if [ -f "$NPU_MODEL" ]; then
        echo "[PRO] NPU model found, enabling fusion verification"
        DVR_CMD="$DVR_CMD --npu-model $NPU_MODEL --npu-labels $NPU_LABELS"
    else
        echo "[PRO] NPU model not found at $NPU_MODEL"
        echo "[PRO] Running without NPU (trusting radar directly)"
    fi

    $DVR_CMD > $DVR_LOG 2>&1 &
    sleep 2
else
    echo "[PRO] WARNING: DVR binary not found at $DVR_BIN"
    echo "[PRO] Please build and deploy DVR first:"
    echo "  cd /home/alientek/dvr_project/mier/dvr && make deploy"
fi

# 启动雷达程序 (前台)
if [ -x "$RADAR_BIN" ]; then
    echo "[PRO] Starting radar_link (foreground)..."
    echo "[PRO] Press Ctrl+C to stop."
    echo "========================================"
    "$RADAR_BIN"
else
    echo "[PRO] ERROR: radar_link not found at $RADAR_BIN"
    exit 1
fi

echo "[PRO] Radar exited."
echo "[PRO] DVR is still running in background."
echo "[PRO] Run './start_pro.sh stop' to stop DVR."