#!/bin/bash
# ============================================================================
# DVR 系统一键启动脚本
# 功能：启动 M33 固件 + 雷达/摄像头/NPU 融合 + DVR 行车记录 + LED/音频告警
# 适用：室外实测，无需逐条手动执行命令
# ============================================================================

set -o pipefail

# -------------------------- 配置 --------------------------
CAMERA_DIR="/xxl/camera_detect"
FW_DIR="/home/root/project"
FW_SCRIPT="${FW_DIR}/fw_cortex_m33.sh"
RADAR_FUSION="${CAMERA_DIR}/radar_fusion"
RPMSG_DEV="/dev/ttyRPMSG0"
LOG_FILE="${CAMERA_DIR}/dvr_system.log"
M33_READY_TIMEOUT=15          # 等待 M33 RPMsg 设备就绪的最大秒数
FALL_DELAY=0                  # 默认不模拟摔倒；由 -t 参数覆盖
RADAR_PID=0
READER_PID=0                  # 过滤进程组的 leader PID

# -------------------------- 用法 --------------------------
usage() {
    echo "用法: $0 [选项]"
    echo ""
    echo "选项:"
    echo "  -t N    启动 N 秒后自动模拟一次 IMU 摔倒事件（用于测试）"
    echo "  -T F    雷达 TTC 阈值（秒），默认 2.5（后方电动车快速靠近场景）"
    echo "  -D M    雷达距离阈值（米），默认 3"
    echo "  -l      指定日志文件路径（默认: ${LOG_FILE}）"
    echo "  -h      显示此帮助"
    echo ""
    echo "示例:"
    echo "  $0                    # 正常启动，等待真实 IMU 摔倒或雷达告警"
    echo "  $0 -t 10              # 启动 10 秒后模拟摔倒，验证端到端流程"
    echo "  $0 -T 5.0 -D 2        # TTC<5s 或距离<=2m 即触发雷达告警"
    echo "  $0 -t 10 -l /tmp/dvr.log"
    exit 1
}

# 默认阈值：后方电动车快速靠近、即将追尾场景
TTC_THRESHOLD="2.5"
DIST_THRESHOLD="3"

while getopts "t:T:D:l:h" opt; do
    case "$opt" in
        t) FALL_DELAY="$OPTARG" ;;
        T) TTC_THRESHOLD="$OPTARG" ;;
        D) DIST_THRESHOLD="$OPTARG" ;;
        l) LOG_FILE="$OPTARG" ;;
        h|*) usage ;;
    esac
done

# -------------------------- 日志函数 --------------------------
log() {
    local msg="[$(date '+%Y-%m-%d %H:%M:%S')] $*"
    echo "$msg" | tee -a "$LOG_FILE"
}

# -------------------------- 退出清理 --------------------------
# 安全地等待进程结束，最多 wait_sec 秒
wait_or_kill() {
    local pid="$1"
    local name="$2"
    local wait_sec="${3:-3}"

    [ -z "$pid" ] || [ "$pid" -le 0 ] && return 0
    kill -0 "$pid" 2>/dev/null || return 0

    log "停止 ${name} (pid=${pid})..."
    kill -TERM "$pid" 2>/dev/null || true

    local i=0
    while [ "$i" -lt "$wait_sec" ]; do
        kill -0 "$pid" 2>/dev/null || return 0
        sleep 1
        i=$((i + 1))
    done

    if kill -0 "$pid" 2>/dev/null; then
        log "${name} 未响应，强制结束..."
        kill -KILL "$pid" 2>/dev/null || true
        sleep 1
    fi
}

cleanup() {
    # 防止信号重入导致清理逻辑嵌套
    trap '' INT TERM HUP QUIT

    log "收到退出信号，开始清理..."

    # 1. 停止 radar_fusion
    wait_or_kill "$RADAR_PID" "radar_fusion" 3
    RADAR_PID=0

    # 2. 停止过滤进程组（包括 tail/grep/awk）
    if [ "$READER_PID" -gt 0 ] 2>/dev/null; then
        log "停止日志过滤进程组 (pgid=${READER_PID})..."
        kill -TERM -"$READER_PID" 2>/dev/null || true
        wait_or_kill "$READER_PID" "日志过滤" 2
        READER_PID=0
    fi

    # 3. 确保没有遗留子进程占用摄像头/雷达
    pkill -9 -f "radar_fusion" 2>/dev/null || true

    # 4. 停止 M33 固件（最多等 10 秒）
    log "停止 M33 固件..."
    (
        cd "$FW_DIR" || exit 1
        if command -v timeout >/dev/null 2>&1; then
            timeout 10 ./fw_cortex_m33.sh stop >> "$LOG_FILE" 2>&1 || true
        else
            ./fw_cortex_m33.sh stop >> "$LOG_FILE" 2>&1 || true
        fi
    )

    log "DVR 系统已停止"
    exit 0
}
trap cleanup INT TERM HUP QUIT

# -------------------------- 检查文件/设备 --------------------------
log "========================================"
log "DVR 系统启动脚本开始"
log "日志文件: ${LOG_FILE}"
log "========================================"

if [ ! -x "$FW_SCRIPT" ]; then
    log "错误: M33 固件启动脚本不存在或无执行权限: $FW_SCRIPT"
    exit 1
fi

if [ ! -x "$RADAR_FUSION" ]; then
    log "错误: radar_fusion 不存在或无执行权限: $RADAR_FUSION"
    exit 1
fi

if [ ! -d "/run/media/mmcblk0p1/dvr" ]; then
    log "警告: DVR 目录 /run/media/mmcblk0p1/dvr 不存在，尝试创建..."
    mkdir -p /run/media/mmcblk0p1/dvr || {
        log "错误: 无法创建 DVR 目录，请检查 TF 卡是否挂载"
        exit 1
    }
fi

# -------------------------- 清理旧状态 --------------------------
log "清理可能占用摄像头的进程..."
fuser -k /dev/video7 2>/dev/null || true
fuser -k /dev/ttySTM1 2>/dev/null || true
sleep 0.5

# -------------------------- 启动 M33 固件 --------------------------
# fw_cortex_m33.sh 会根据当前目录名确定固件文件名
# 固件为 /lib/firmware/project_CM33_NonSecure.elf，所以必须在 /home/root/project 目录下执行
log "停止可能正在运行的 M33 固件..."
(
    cd "$FW_DIR" || exit 1
    ./fw_cortex_m33.sh stop >> "$LOG_FILE" 2>&1 || true
)
sleep 1

log "启动 M33 固件..."
if ! (
    cd "$FW_DIR" || exit 1
    ./fw_cortex_m33.sh start >> "$LOG_FILE" 2>&1
); then
    log "错误: M33 固件启动失败"
    exit 1
fi

# -------------------------- 等待 RPMsg 就绪 --------------------------
log "等待 RPMsg 设备 ${RPMSG_DEV} 就绪（最多 ${M33_READY_TIMEOUT}s）..."
ready=0
for i in $(seq 1 "$M33_READY_TIMEOUT"); do
    if [ -e "$RPMSG_DEV" ]; then
        ready=1
        break
    fi
    sleep 1
done

if [ "$ready" -ne 1 ]; then
    log "错误: ${RPMSG_DEV} 未在 ${M33_READY_TIMEOUT}s 内出现，M33 固件可能启动异常"
    (
        cd "$FW_DIR" || exit 1
        ./fw_cortex_m33.sh stop >> "$LOG_FILE" 2>&1 || true
    )
    exit 1
fi
log "RPMsg 设备已就绪: ${RPMSG_DEV}"

# -------------------------- 启动 radar_fusion --------------------------
ARGS=""
if [ "$FALL_DELAY" -gt 0 ] 2>/dev/null; then
    ARGS="${ARGS} -t ${FALL_DELAY}"
    log "测试模式: ${FALL_DELAY} 秒后将自动模拟 IMU 摔倒事件"
fi
if [ -n "$TTC_THRESHOLD" ]; then
    ARGS="${ARGS} -T ${TTC_THRESHOLD}"
    log "雷达 TTC 阈值: ${TTC_THRESHOLD} s"
fi
if [ -n "$DIST_THRESHOLD" ]; then
    ARGS="${ARGS} -D ${DIST_THRESHOLD}"
    log "雷达距离阈值: ${DIST_THRESHOLD} m"
fi

log "启动 radar_fusion..."
log "命令: LD_LIBRARY_PATH=/usr/lib:/vendor/lib:${CAMERA_DIR}/stai_mpu ${RADAR_FUSION} ${ARGS}"
log "完整日志: ${LOG_FILE}"
log "终端只显示关键事件，完整日志请查看上方文件"

# 关键日志过滤规则
KEY_PATTERN='ROAD USER DETECTED|NPU CONFIRMED|ALERT:|FALL DETECTED|Save triggered|Recording started|Post-trigger|Child: Saved|Encoder finished|TARGET ON|TARGET GONE|Playing .* alert|V2X_ALERT|RPMsg ready|DVR] TF card|DVR] ffmpeg'

# 后台实时过滤并打印关键日志（从当前日志末尾开始，不打印历史）
# 使用子shell，这样 READER_PID 就是进程组 leader，cleanup 可以一次性 kill 整个管道
(
    tail -n 0 -f "$LOG_FILE" 2>/dev/null | stdbuf -oL grep --line-buffered -E "$KEY_PATTERN" | \
    stdbuf -oL awk '
    BEGIN { yellow="\033[1;33m"; cyan="\033[1;36m"; reset="\033[0m" }
    /FALL DETECTED|ALERT:/ { print yellow "[关键] " $0 reset; next }
    /Child: Saved/         { print cyan "[保存] " $0 reset; next }
                            { print "[关键] " $0 }
    '
) &
READER_PID=$!

export LD_LIBRARY_PATH="/usr/lib:/vendor/lib:${CAMERA_DIR}/stai_mpu"
"$RADAR_FUSION" $ARGS >> "$LOG_FILE" 2>&1 &
RADAR_PID=$!
log "radar_fusion 已启动，pid=${RADAR_PID}，过滤进程 pid=${READER_PID}"

# 等待 radar_fusion 结束
wait "$RADAR_PID"
RADAR_PID=0

# 正常退出：关闭过滤进程组
if [ "$READER_PID" -gt 0 ] 2>/dev/null; then
    log "关闭日志过滤进程组..."
    kill -TERM -"$READER_PID" 2>/dev/null || true
    wait "$READER_PID" 2>/dev/null || true
fi
READER_PID=0

log "radar_fusion 已退出，执行清理..."
(
    cd "$FW_DIR" || exit 1
    ./fw_cortex_m33.sh stop >> "$LOG_FILE" 2>&1 || true
)
log "DVR 系统已停止"
