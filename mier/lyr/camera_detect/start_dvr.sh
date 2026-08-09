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
HUD_BIN="${CAMERA_DIR}/hud"
LEGACY_HUD_BIN="/home/root/hud"
HUD_PID_FILE="/tmp/hud.pid"
DASHBOARD_DIR="${CAMERA_DIR}/dashboard"
DASHBOARD_SCRIPT="${DASHBOARD_DIR}/radar_dashboard.py"
DASHBOARD_PID_FILE="/tmp/radar_dashboard.pid"
LOG_MAINT_SCRIPT="${CAMERA_DIR}/scripts/log_maintenance.sh"
RPMSG_DEV="/dev/ttyRPMSG0"
RPROC_STATE="/sys/class/remoteproc/remoteproc0/state"
LOG_FILE="${CAMERA_DIR}/dvr_system.log"
RADAR_LOG_DIR="/usr/local/helmet/radar_experiments"
DVR_STORAGE_DIR="/usr/local/helmet/dvr"
BLE_LED_ENABLED="1"
BLE_LED_UART="/dev/ttySTM0"
DASHBOARD_PORT="8080"
M33_READY_TIMEOUT=15          # 等待 M33 RPMsg 设备就绪的最大秒数
# M33 由 dvr-m33.service 独立管理。A35 应用停止/OTA 切换时默认不停止 M33。
# 只有现场显式设置为 1，才允许本脚本重置或随退出停止 M33。
STOP_M33_ON_EXIT="${STOP_M33_ON_EXIT:-0}"
ALLOW_M33_RESET_ON_START_FAILURE="${ALLOW_M33_RESET_ON_START_FAILURE:-0}"
FALL_DELAY=0                  # 默认不模拟摔倒；由 -t 参数覆盖
V2X_DELAY=0                   # 默认不模拟 V2X；由 -V 参数覆盖
V2X_DIRECTION="left_front"    # 默认 V2X 方向
RADAR_PID=0
HUD_PID=0
DASHBOARD_PID=0
READER_PID=0                  # 关键日志过滤子 shell
LOG_MAINT_PID=0
NAV_TTS_CACHE="${CAMERA_DIR}/nav_tts_cache"  # 导航 TTS 缓存目录

# -------------------------- 用法 --------------------------
usage() {
    echo "用法: $0 [选项]"
    echo ""
    echo "选项:"
    echo "  -t N    N 秒后模拟 A35 本地摔倒动作（只测 LED/音频/DVR，不经过 M33/HUD/短信）"
    echo "  -V N    启动 N 秒后自动模拟一次 V2X 告警（用于测试）"
    echo "  -x DIR  模拟 V2X 方向：nearby|left_front|right_front|left|right（默认 left_front）"
    echo "  -T F    雷达 TTC 阈值（秒），默认 2.5，可被 ${CAMERA_DIR}/radar_config 覆盖"
    echo "  -D M    雷达距离阈值（米），启动脚本默认 1.2，可被 ${CAMERA_DIR}/radar_config 覆盖"
    echo "  -p N    Dashboard 端口（默认 8080）"
    echo "  -l      指定日志文件路径（默认: ${LOG_FILE}）"
    echo "  -h      显示此帮助"
    echo ""
    echo "示例:"
    echo "  $0                    # 正常启动，等待真实 IMU 摔倒或雷达告警"
    echo "  $0 -t 10              # 启动 10 秒后模拟 A35 本地摔倒动作"
    echo "  $0 -V 5 -x right_front # 启动 5 秒后模拟右前方 V2X 告警"
    echo "  $0 -T 5.0 -D 2        # TTC<5s 或距离<=2m 即触发雷达告警"
    echo "  $0 -t 10 -l /tmp/dvr.log"
    exit 1
}

# 默认阈值：后方电动车快速靠近、即将追尾场景
TTC_THRESHOLD="2.5"
DIST_THRESHOLD="1.2"
LEFT_ANGLE="-10"
RIGHT_ANGLE="10"
ANGLE_SIGN="-1"
ANGLE_ALPHA="0.35"
DIRECTION_SAMPLES="3"

# 如果存在持久化配置文件，用配置文件覆盖默认值（命令行参数仍优先）
load_radar_config() {
    local config="${CAMERA_DIR}/radar_config"
    if [ -f "$config" ]; then
        local key
        local value
        while IFS='=' read -r key value; do
            [ -n "$value" ] || continue
            case "$key" in
                TTC) TTC_THRESHOLD="$value" ;;
                DIST) DIST_THRESHOLD="$value" ;;
                LEFT_ANGLE) LEFT_ANGLE="$value" ;;
                RIGHT_ANGLE) RIGHT_ANGLE="$value" ;;
                ANGLE_SIGN) ANGLE_SIGN="$value" ;;
                ANGLE_ALPHA) ANGLE_ALPHA="$value" ;;
                DIRECTION_SAMPLES) DIRECTION_SAMPLES="$value" ;;
                DASHBOARD_PORT) DASHBOARD_PORT="$value" ;;
                RADAR_LOG_DIR) RADAR_LOG_DIR="$value" ;;
                BLE_LED_ENABLED) BLE_LED_ENABLED="$value" ;;
                BLE_LED_UART) BLE_LED_UART="$value" ;;
            esac
        done < "$config"
    fi
}
load_radar_config

# 1.0.6以前的现场配置可能仍把CSV写到可移除TF。保留所有标定阈值和BLE参数，
# 但拒绝继续使用已退出业务链的旧TF路径，避免卡未挂载时写入根文件系统隐藏目录。
case "$RADAR_LOG_DIR" in
    /run/media/mmcblk0p1|/run/media/mmcblk0p1/*)
        printf '%s\n' "警告: 旧RADAR_LOG_DIR=${RADAR_LOG_DIR}已迁移到板载ext4" >&2
        RADAR_LOG_DIR="/usr/local/helmet/radar_experiments"
        ;;
esac

while getopts "t:V:x:T:D:p:l:h" opt; do
    case "$opt" in
        t) FALL_DELAY="$OPTARG" ;;
        V) V2X_DELAY="$OPTARG" ;;
        x) V2X_DIRECTION="$OPTARG" ;;
        T) TTC_THRESHOLD="$OPTARG" ;;
        D) DIST_THRESHOLD="$OPTARG" ;;
        p) DASHBOARD_PORT="$OPTARG" ;;
        l) LOG_FILE="$OPTARG" ;;
        h|*) usage ;;
    esac
done

# -------------------------- 日志函数 --------------------------
log() {
    local timestamp
    local msg
    printf -v timestamp '%(%Y-%m-%d %H:%M:%S)T' -1
    msg="[${timestamp}] $*"
    printf '%s\n' "$msg"
    printf '%s\n' "$msg" >> "$LOG_FILE"
}

# 等待单个业务设备。required=1 的设备缺失时返回失败；可选设备超时后由
# radar_fusion 沿用既有降级逻辑。使用精确设备等待替代全局 udev settle。
wait_for_device() {
    local path="$1"
    local description="$2"
    local timeout_tenths="$3"
    local required="$4"
    local attempt=0

    while [ "$attempt" -lt "$timeout_tenths" ]; do
        [ ! -e "$path" ] || return 0
        sleep 0.1
        attempt=$((attempt + 1))
    done

    if [ "$required" = "1" ]; then
        log "错误: ${description} 未在 $((timeout_tenths / 10))s 内就绪: ${path}"
        return 1
    fi
    log "警告: ${description} 未在 $((timeout_tenths / 10))s 内就绪: ${path}，按现有降级路径继续"
    return 0
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

stop_log_reader() {
    [ "$READER_PID" -gt 0 ] 2>/dev/null || return 0
    if kill -0 "$READER_PID" 2>/dev/null; then
        # Bash 后台子 shell 不一定是进程组 leader，不能依赖 kill -PID。
        # 先结束 tail/grep/awk 子进程，再结束并回收子 shell。
        pkill -TERM -P "$READER_PID" 2>/dev/null || true
        wait_or_kill "$READER_PID" "日志过滤" 2
        pkill -KILL -P "$READER_PID" 2>/dev/null || true
    fi
    wait "$READER_PID" 2>/dev/null || true
    READER_PID=0
}

start_storage_worker() {
    if [ ! -x "$LOG_MAINT_SCRIPT" ]; then
        log "警告: 日志维护脚本不可用: ${LOG_MAINT_SCRIPT}"
        return 1
    fi

    # 比赛录像与实验 CSV 固定使用板载 userfs/ext4。外置 TF 仅供人工导入导出，
    # 不在业务启动链自动挂载，避免故障卡影响风险检测和录像。
    (
        if ! mkdir -p "$DVR_STORAGE_DIR" "$RADAR_LOG_DIR"; then
            log "错误: 无法创建板载业务目录；录像和 CSV 暂不可用"
            exit 0
        fi

        "$LOG_MAINT_SCRIPT" --once || true
        log "板载 ext4 存储与日志容量限制已就绪"
        exec "$LOG_MAINT_SCRIPT" --watch
    ) &
    LOG_MAINT_PID=$!
    log "板载存储维护工作器已启动，pid=${LOG_MAINT_PID}"
}

stop_log_maintenance() {
    wait_or_kill "$LOG_MAINT_PID" "日志维护" 2
    LOG_MAINT_PID=0
}

stop_m33_if_requested() {
    if [ "$STOP_M33_ON_EXIT" != "1" ]; then
        log "保留独立运行的 M33（STOP_M33_ON_EXIT=${STOP_M33_ON_EXIT}）"
        return 0
    fi

    log "按配置停止 M33 固件..."
    (
        cd "$FW_DIR" || exit 1
        if command -v timeout >/dev/null 2>&1; then
            timeout 10 ./fw_cortex_m33.sh stop >> "$LOG_FILE" 2>&1 || true
        else
            ./fw_cortex_m33.sh stop >> "$LOG_FILE" 2>&1 || true
        fi
    )
}

# 启动 HUD（IMU 异常 UDP 转发到 App）
start_hud() {
    # 如果 HUD 已在运行，复用现有进程
    if [ -r "$HUD_PID_FILE" ]; then
        local existing_pid
        existing_pid=$(cat "$HUD_PID_FILE" 2>/dev/null)
        if [ -n "$existing_pid" ] && kill -0 "$existing_pid" 2>/dev/null; then
            log "HUD 已在运行 (pid=${existing_pid})，无需重新启动"
            HUD_PID=$existing_pid
            return 0
        fi
    fi

    if [ ! -x "$HUD_BIN" ] && [ -x "$LEGACY_HUD_BIN" ]; then
        HUD_BIN="$LEGACY_HUD_BIN"
        log "当前 release 未包含 HUD，兼容使用旧路径: ${HUD_BIN}"
    fi

    if [ ! -x "$HUD_BIN" ]; then
        log "警告: HUD 程序不存在或无执行权限: ${HUD_BIN}，App 将无法接收 IMU 通知"
        return 1
    fi

    log "启动 HUD: ${HUD_BIN}"
    rm -f "$HUD_PID_FILE"
    nohup "$HUD_BIN" >> /tmp/hud.log 2>&1 &
    HUD_PID=$!

    if kill -0 "$HUD_PID" 2>/dev/null; then
        log "HUD 启动成功，pid=${HUD_PID}"
        return 0
    else
        log "警告: HUD 启动失败，请查看 /tmp/hud.log"
        HUD_PID=0
        return 1
    fi
}

# 启动本地雷达 Dashboard（仅依赖 Python 3 标准库）
start_dashboard() {
    # 新版由独立 systemd unit 托管，确保暂停 dvr.service 时控制面板仍在线。
    # 未安装该 unit 的旧系统继续使用下方兼容启动路径。
    if [ "$(systemctl show radar-dashboard.service --property=LoadState --value 2>/dev/null)" = "loaded" ]; then
        log "Dashboard 由独立 radar-dashboard.service 托管（允许稍后按正常 systemd 顺序启动）"
        DASHBOARD_PID=0
        return 0
    fi

    if [ ! -f "$DASHBOARD_SCRIPT" ]; then
        log "警告: Dashboard 脚本不存在: ${DASHBOARD_SCRIPT}"
        return 1
    fi
    if ! command -v python3 >/dev/null 2>&1; then
        log "警告: 未安装 python3，Dashboard 无法启动"
        return 1
    fi

    if [ -r "$DASHBOARD_PID_FILE" ]; then
        local existing_pid
        existing_pid=$(cat "$DASHBOARD_PID_FILE" 2>/dev/null)
        if [ -n "$existing_pid" ] && kill -0 "$existing_pid" 2>/dev/null; then
            log "Dashboard 已在运行 (pid=${existing_pid})"
            DASHBOARD_PID=$existing_pid
            return 0
        fi
    fi

    mkdir -p "$RADAR_LOG_DIR"
    log "启动 Radar Dashboard: 0.0.0.0:${DASHBOARD_PORT}"
    nohup python3 -u "$DASHBOARD_SCRIPT" \
        --host 0.0.0.0 \
        --port "$DASHBOARD_PORT" \
        --data-dir "$RADAR_LOG_DIR" \
        >> "${CAMERA_DIR}/radar_dashboard.log" 2>&1 &
    DASHBOARD_PID=$!
    echo "$DASHBOARD_PID" > "$DASHBOARD_PID_FILE"

    if kill -0 "$DASHBOARD_PID" 2>/dev/null; then
        log "Dashboard 已启动，浏览器访问 http://<开发板IP>:${DASHBOARD_PORT}"
        return 0
    fi

    log "警告: Dashboard 启动失败，请查看 ${CAMERA_DIR}/radar_dashboard.log"
    DASHBOARD_PID=0
    rm -f "$DASHBOARD_PID_FILE"
    return 1
}

cleanup() {
    # 防止信号重入导致清理逻辑嵌套
    trap '' INT TERM HUP QUIT

    log "收到退出信号，开始清理..."

    # 1. 停止 radar_fusion
    wait_or_kill "$RADAR_PID" "radar_fusion" 3
    RADAR_PID=0

    # 2. 停止 HUD
    wait_or_kill "$HUD_PID" "HUD" 2
    HUD_PID=0
    rm -f "$HUD_PID_FILE"

    # 3. 只清理旧系统的兼容 Dashboard；独立 systemd 服务不属于本业务。
    if [ "$DASHBOARD_PID" -gt 0 ] 2>/dev/null; then
        wait_or_kill "$DASHBOARD_PID" "Radar Dashboard" 2
        DASHBOARD_PID=0
        rm -f "$DASHBOARD_PID_FILE"
    fi

    # 4. 停止过滤管道和日志容量维护
    stop_log_reader
    stop_log_maintenance

    # 5. 确保没有遗留子进程占用摄像头/雷达
    # 只按进程名清理遗留实例。不能使用 -f，否则会误杀命令行中仅仅包含
    # “radar_fusion”文本的 SSH/运维 shell。
    pkill -KILL -x radar_fusion 2>/dev/null || true

    # 6. M33 生命周期独立于 A35；OTA/服务重启默认保留 M33。
    stop_m33_if_requested

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

# 雷达是主程序的硬前提；摄像头由 radar_fusion 在运行中动态接入，不再阻塞
# 风险处理链启动。方向灯和本机告警 GPIO 保持原有可降级语义。
wait_for_device /dev/ttySTM1 "雷达串口" 100 1 || exit 1
if [ "$BLE_LED_ENABLED" = "1" ]; then
    wait_for_device "$BLE_LED_UART" "蓝牙方向灯串口" 100 0
fi
wait_for_device /dev/gpiochip3 "告警 GPIO" 100 0

start_storage_worker

# 创建导航 TTS 缓存目录
if [ ! -d "$NAV_TTS_CACHE" ]; then
    log "创建导航 TTS 缓存目录: $NAV_TTS_CACHE"
    mkdir -p "$NAV_TTS_CACHE" || {
        log "警告: 无法创建导航 TTS 缓存目录"
    }
fi

# 使能 MAX98357A 功放 (PB11)
# 某些系统已以名称 PB11 导出；否则尝试数字 gpio539
log "使能 MAX98357A 功放 (PB11)"
if [ -d /sys/class/gpio/PB11 ]; then
    echo out > /sys/class/gpio/PB11/direction 2>/dev/null || log "警告: 无法设置 PB11 方向"
    echo 1 > /sys/class/gpio/PB11/value 2>/dev/null || log "警告: 无法设置 PB11 高电平"
elif [ ! -d /sys/class/gpio/gpio539 ]; then
    echo 539 > /sys/class/gpio/export 2>/dev/null || log "警告: 无法 export PB11"
    if [ -d /sys/class/gpio/gpio539 ]; then
        echo out > /sys/class/gpio/gpio539/direction 2>/dev/null || log "警告: 无法设置 PB11 方向"
        echo 1 > /sys/class/gpio/gpio539/value 2>/dev/null || log "警告: 无法设置 PB11 高电平"
    fi
fi

# -------------------------- 启动 M33 固件 --------------------------
# fw_cortex_m33.sh 会根据当前目录名确定固件文件名
# 固件为 /lib/firmware/project_CM33_NonSecure.elf，所以必须在 /home/root/project 目录下执行
#
# 优先复用 dvr-m33.service 的早期启动实例。若早期服务未启动成功或
# RPMsg 没有出现，则自动回退到原有 Linux remoteproc 启动路径。
m33_running=0
if [ -r "$RPROC_STATE" ] &&
   [ "$(tr -d '\0\r\n' < "$RPROC_STATE")" = "running" ]; then
    log "检测到 M33 已运行，复用早期启动实例"
    for i in $(seq 1 20); do
        if [ -e "$RPMSG_DEV" ]; then
            m33_running=1
            break
        fi
        sleep 0.1
    done
fi

if [ "$m33_running" -ne 1 ]; then
    if [ -r "$RPROC_STATE" ] &&
       [ "$(tr -d '\0\r\n' < "$RPROC_STATE")" = "running" ]; then
        if [ "$ALLOW_M33_RESET_ON_START_FAILURE" = "1" ]; then
            log "M33 已运行但 RPMsg 未就绪，按配置执行 Linux remoteproc 重启"
            (
                cd "$FW_DIR" || exit 1
                ./fw_cortex_m33.sh stop >> "$LOG_FILE" 2>&1 || true
            )
        else
            log "错误: M33 已运行但 RPMsg 未就绪；为保护独立 M33 生命周期，本次不自动重置"
            log "如需现场恢复，请重启 dvr-m33.service；仅诊断时可设置 ALLOW_M33_RESET_ON_START_FAILURE=1"
            exit 1
        fi
    else
        log "早期服务未启动 M33，使用 Linux remoteproc 回退路径"
    fi

    log "启动 M33 固件..."
    if ! (
        cd "$FW_DIR" || exit 1
        ./fw_cortex_m33.sh start >> "$LOG_FILE" 2>&1
    ); then
        log "错误: M33 固件启动失败"
        exit 1
    fi
fi

# -------------------------- 等待 RPMsg 就绪 --------------------------
log "等待 RPMsg 设备 ${RPMSG_DEV} 就绪（最多 ${M33_READY_TIMEOUT}s）..."
ready=0
for i in $(seq 1 $((M33_READY_TIMEOUT * 5))); do
    if [ -e "$RPMSG_DEV" ]; then
        ready=1
        break
    fi
    sleep 0.2
done

if [ "$ready" -ne 1 ]; then
    log "错误: ${RPMSG_DEV} 未在 ${M33_READY_TIMEOUT}s 内出现，M33 固件可能启动异常"
    stop_m33_if_requested
    exit 1
fi
log "RPMsg 设备已就绪: ${RPMSG_DEV}"

# -------------------------- 启动 HUD --------------------------
# HUD 负责把 radar_fusion 转发到 127.0.0.1:8890 的 IMU 异常广播给手机 App
start_hud

# -------------------------- 确认 Radar Dashboard --------------------------
start_dashboard

# -------------------------- 启动 radar_fusion --------------------------
ARGS=""
if [ "$FALL_DELAY" -gt 0 ] 2>/dev/null; then
    ARGS="${ARGS} -t ${FALL_DELAY}"
    log "测试模式: ${FALL_DELAY} 秒后将自动模拟 IMU 摔倒事件"
fi
if [ "$V2X_DELAY" -gt 0 ] 2>/dev/null; then
    ARGS="${ARGS} -V ${V2X_DELAY} -x ${V2X_DIRECTION}"
    log "测试模式: ${V2X_DELAY} 秒后将自动模拟 V2X 告警 (direction=${V2X_DIRECTION})"
fi
if [ -n "$TTC_THRESHOLD" ]; then
    ARGS="${ARGS} -T ${TTC_THRESHOLD}"
    log "雷达 TTC 阈值: ${TTC_THRESHOLD} s"
fi
if [ -n "$DIST_THRESHOLD" ]; then
    ARGS="${ARGS} -D ${DIST_THRESHOLD}"
    log "雷达距离阈值: ${DIST_THRESHOLD} m"
fi
ARGS="${ARGS} --left-angle ${LEFT_ANGLE} --right-angle ${RIGHT_ANGLE}"
ARGS="${ARGS} --angle-sign ${ANGLE_SIGN}"
ARGS="${ARGS} --angle-alpha ${ANGLE_ALPHA} --direction-samples ${DIRECTION_SAMPLES}"
ARGS="${ARGS} --radar-log-dir ${RADAR_LOG_DIR}"
if [ "$BLE_LED_ENABLED" = "1" ]; then
    ARGS="${ARGS} --ble-led-uart ${BLE_LED_UART}"
    log "蓝牙碰撞方向灯: ${BLE_LED_UART} @ 115200"
else
    ARGS="${ARGS} --no-ble-led"
    log "蓝牙碰撞方向灯: 已禁用"
fi
log "雷达方向: 骑行者角度=传感器角度×${ANGLE_SIGN}; LEFT<=${LEFT_ANGLE}°, RIGHT>=${RIGHT_ANGLE}°"
log "方向滤波: alpha=${ANGLE_ALPHA}, stable_samples=${DIRECTION_SAMPLES}"
log "雷达实验数据: ${RADAR_LOG_DIR}"

log "启动 radar_fusion..."
log "命令: LD_LIBRARY_PATH=/usr/lib:/vendor/lib:${CAMERA_DIR}/stai_mpu ${RADAR_FUSION} ${ARGS}"
log "完整日志: ${LOG_FILE}"
log "终端只显示关键事件，完整日志请查看上方文件"

# 关键日志过滤规则
# 源日志已经按类型打上 [目标]/[告警]/[保存]/[系统] 前缀，这里直接按前缀过滤
KEY_PATTERN='^\[(启动|目标|告警|保存|系统|NAV)\]'

# 后台实时过滤并打印关键日志（从当前日志末尾开始，不打印历史）
# 使用子shell，这样 READER_PID 就是进程组 leader，cleanup 可以一次性 kill 整个管道
(
    tail -n 0 -f "$LOG_FILE" 2>/dev/null | stdbuf -oL grep --line-buffered -E "$KEY_PATTERN" | \
    stdbuf -oL awk '
    BEGIN { yellow="\033[1;33m"; red="\033[1;31m"; cyan="\033[1;36m"; gray="\033[0;90m"; reset="\033[0m" }
    /^\[目标\]/ { print $0; next }
    /^\[告警\]/ { print yellow $0 reset; next }
    /^\[保存\]/ { print cyan $0 reset; next }
    /^\[系统\]/ { print gray $0 reset; next }
                 { print $0 }
    '
) &
READER_PID=$!

# 必须切换到 CAMERA_DIR，radar_fusion 使用相对路径加载 models/ 等资源
export LD_LIBRARY_PATH="/usr/lib:/vendor/lib:${CAMERA_DIR}/stai_mpu"
cd "$CAMERA_DIR" || { log "错误: 无法进入 ${CAMERA_DIR}"; exit 1; }
"$RADAR_FUSION" $ARGS >> "$LOG_FILE" 2>&1 &
RADAR_PID=$!
log "radar_fusion 已启动，pid=${RADAR_PID}，过滤进程 pid=${READER_PID}"

# 等待 radar_fusion 结束
wait "$RADAR_PID"
RADAR_EXIT_STATUS=$?
RADAR_PID=0

# radar_fusion 无论以何种状态自行退出，都属于主业务异常。先关闭不会自行退出的
# tail/grep/awk，再完整清理；脚本最后返回非零以触发 Restart=on-failure。
log "radar_fusion 意外退出，status=${RADAR_EXIT_STATUS}"
stop_log_reader

log "radar_fusion 已退出，执行清理并请求 systemd 重启..."

# 停止 HUD
wait_or_kill "$HUD_PID" "HUD" 2
HUD_PID=0
rm -f "$HUD_PID_FILE"

# 只停止旧系统由本脚本启动的兼容 Dashboard。
if [ "$DASHBOARD_PID" -gt 0 ] 2>/dev/null; then
    wait_or_kill "$DASHBOARD_PID" "Radar Dashboard" 2
    DASHBOARD_PID=0
    rm -f "$DASHBOARD_PID_FILE"
fi

stop_log_maintenance

stop_m33_if_requested
log "DVR 系统异常退出，交由 systemd 重启"
exit 1
