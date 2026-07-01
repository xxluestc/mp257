#!/bin/bash
# ============================================================================
# 蓝牙串口终端 / DVR 远程控制脚本
# 功能：通过 /dev/ttySTM0 蓝牙串口接收手机 APP 命令，控制 DVR 启停
#       并反馈运行状态，实现手机端串口终端效果
# 适用：CH9141 蓝牙串口透传模块，默认 115200 8N1
# ============================================================================

# -------------------------- 配置 --------------------------
CAMERA_DIR="/xxl/camera_detect"
BT_UART="/dev/ttySTM0"
LOG_FILE="${CAMERA_DIR}/dvr_system.log"
BT_LOG_FILE="${CAMERA_DIR}/bt_shell.log"

# -------------------------- 蓝牙输出函数 --------------------------
bt_log() {
    local msg="[$(date '+%Y-%m-%d %H:%M:%S')] $*"
    echo "$msg" | tee -a "$BT_LOG_FILE" > "$BT_UART" 2>/dev/null
}

bt_print() {
    echo "$*" | tee -a "$BT_LOG_FILE" > "$BT_UART" 2>/dev/null
}

# -------------------------- 打印欢迎信息 --------------------------
print_welcome() {
    bt_print ""
    bt_print "========== DVR 蓝牙串口终端 =========="
    bt_print "可用命令："
    bt_print "  start            启动 DVR 系统"
    bt_print "  stop             停止 DVR 系统"
    bt_print "  restart          重启 DVR 系统"
    bt_print "  status           查询运行状态"
    bt_print "  log [N]          输出最近 N 行日志（默认 20）"
    bt_print "  exec <命令>      执行任意 shell 命令并返回结果"
    bt_print "  ls               列出已保存的紧急视频"
    bt_print "  clear            清空 DVR 日志文件"
    bt_print "  help             显示本帮助"
    bt_print "======================================"
}

# -------------------------- 命令处理 --------------------------
cmd_start() {
    if pgrep -x "radar_fusion" >/dev/null 2>&1; then
        bt_log "radar_fusion 已在运行，无需重复启动"
        return
    fi
    if pgrep -f "start_dvr.sh" >/dev/null 2>&1; then
        bt_log "start_dvr.sh 已在运行"
        return
    fi
    bt_log "正在启动 DVR 系统..."
    (
        cd "$CAMERA_DIR"
        ./start_dvr.sh >> "$BT_LOG_FILE" 2>&1
    ) &
    sleep 3
    cmd_status
}

cmd_stop() {
    bt_log "正在停止 DVR 系统..."
    pkill -TERM -f "start_dvr.sh" 2>/dev/null || true
    pkill -TERM -x "radar_fusion" 2>/dev/null || true
    sleep 2
    pkill -KILL -f "start_dvr.sh" 2>/dev/null || true
    pkill -KILL -x "radar_fusion" 2>/dev/null || true
    bt_log "DVR 系统已停止"
}

cmd_restart() {
    cmd_stop
    sleep 1
    cmd_start
}

cmd_status() {
    if pgrep -x "radar_fusion" >/dev/null 2>&1; then
        bt_log "状态: DVR 运行中"
    else
        bt_log "状态: DVR 未运行"
    fi
}

cmd_log() {
    local n="${1:-20}"
    if [ ! -f "$LOG_FILE" ]; then
        bt_log "日志文件不存在: $LOG_FILE"
        return
    fi
    bt_log "最近 ${n} 行日志："
    tail -n "$n" "$LOG_FILE" | while IFS= read -r line; do
        bt_print "$line"
    done
}

cmd_exec() {
    local shell_cmd="$*"
    if [ -z "$shell_cmd" ]; then
        bt_log "exec 命令为空"
        return
    fi
    bt_log "执行: $shell_cmd"
    local output
    output=$(eval "$shell_cmd" 2>&1) || true
    if [ -z "$output" ]; then
        bt_print "(无输出)"
    else
        echo "$output" | while IFS= read -r line; do
            bt_print "$line"
        done
    fi
}

cmd_ls() {
    local dvr_dir="/run/media/mmcblk0p1/dvr"
    bt_log "紧急视频列表："
    if [ -d "$dvr_dir" ]; then
        ls -lh "$dvr_dir"/*.mp4 2>/dev/null | while IFS= read -r line; do
            bt_print "$line"
        done
    else
        bt_print "目录不存在: $dvr_dir"
    fi
}

cmd_clear() {
    bt_log "清空日志文件..."
    : > "$LOG_FILE"
    : > "$BT_LOG_FILE"
    bt_log "日志已清空"
}

# -------------------------- 处理单条命令 --------------------------
handle_line() {
    local line="$1"
    # 去除回车换行和首尾空格
    line=$(echo "$line" | tr -d '\r\n' | sed 's/^[[:space:]]*//;s/[[:space:]]*$//')
    [ -z "$line" ] && return

    bt_log "收到命令: $line"

    # 解析命令和参数
    local cmd=""
    local args=""
    cmd=$(echo "$line" | awk '{print $1}')
    args=$(echo "$line" | cut -s -d' ' -f2-)

    case "$cmd" in
        start)   cmd_start ;;
        stop)    cmd_stop ;;
        restart) cmd_restart ;;
        status)  cmd_status ;;
        log)     cmd_log "$args" ;;
        exec)    cmd_exec "$args" ;;
        ls)      cmd_ls ;;
        clear)   cmd_clear ;;
        help|*)  print_welcome ;;
    esac
}

# -------------------------- 串口初始化 --------------------------
init_uart() {
    if [ ! -e "$BT_UART" ]; then
        echo "错误: 蓝牙串口设备不存在: $BT_UART" >&2
        exit 1
    fi
    # 设置 115200 8N1，启用规范模式按行读取，禁用本地回显
    stty -F "$BT_UART" 115200 cs8 -cstopb -parenb -echo icanon min 1 time 0 2>/dev/null || true
}

# -------------------------- 主循环 --------------------------
main() {
    init_uart

    # 将标准输入输出重定向到蓝牙串口，实现真正的串口终端
    exec < "$BT_UART"
    exec > "$BT_UART" 2>&1

    print_welcome
    cmd_status

    while IFS= read -r line; do
        handle_line "$line"
    done
}

main "$@"
