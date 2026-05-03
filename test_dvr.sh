#!/bin/sh
# ============================================================
#  DVR 系统一键测试脚本
#  用途: 验证行车记录仪核心功能（摄像头、LCD显示、触发录制）
#
#  使用方法:
#    test_dvr.sh          # 自动执行完整测试流程
#
#  测试流程:
#    [1] 清理环境 - 停止残留DVR/Qt/监控进程，清理旧文件
#    [2] 启动DVR - 后台启动dvr程序，等待初始化完成
#    [3] 检查M-core - 检查M核(RPMSG)是否运行（可选）
#    [4] 发送触发命令:
#        - TARGET_ON: 模拟目标检测信号，开始循环缓冲
#        - WARNING:   模拟预警信号，触发30秒紧急保存
#    [5] 输出测试结果 - 显示状态转换、保存记录、文件列表
#
#  预期结果:
#    - STATE: IDLE -> BUFFERING (目标检测触发)
#    - EMERGENCY triggered: WARNING (预警保存)
#    - Saved N frames (视频文件生成)
#    - emergency_*.mp4 文件出现在SD卡上
#
#  注意事项:
#    - 需要root权限运行
#    - SD卡必须已挂载在 /run/media/mmcblk0p1
#    - 总耗时约50秒（含等待ffmpeg编码完成）
# ============================================================

LOG="/tmp/dvr.log"           # DVR运行日志路径
PIPE="/tmp/dvr_trigger_pipe" # 触发命令命名管道
SD="/run/media/mmcblk0p1"    # SD卡挂载点

echo "========================================="
echo "  DVR System Test Script"
echo "========================================="

# ---- [1/6] 清理环境 ----
# 停止可能残留的进程，释放内存和LCD资源
echo "[1/6] Cleanup..."
pkill -9 -x dvr 2>/dev/null       # 精确匹配停止DVR（避免误杀test_dvr.sh自身）
pkill -9 weston 2>/dev/null       # 停止Weston显示服务（释放framebuffer）
pkill -9 seatd 2>/dev/null        # 停止seatd（Weston依赖）
systemctl stop netdata 2>/dev/null # 停止netdata监控（节省~52MB内存）
sleep 1                            # 等待进程完全退出
rm -f "$SD"/emergency_*.mp4 "$SD"/dvr_buffer.bin 2>/dev/null  # 清理旧的测试文件
> "$LOG"                          # 清空日志

# ---- [2/6] 启动DVR ----
# 后台启动DVR程序，重定向输出到日志文件
echo "[2/6] Starting DVR..."
/usr/local/bin/dvr > "$LOG" 2>&1 &
DVR_PID=$!                       # 记录DVR进程PID
sleep 5                          # 等待摄像头初始化+ISP配置+LCD显示就绪

# 检查DVR是否成功启动（通过PID存活判断）
if ! kill -0 $DVR_PID 2>/dev/null; then
    echo "ERROR: DVR failed to start!"
    tail -15 "$LOG"               # 打印最后15行日志帮助排查
    exit 1
fi
echo "      OK (pid=$DVR_PID)"

# ---- [3/6] 检查M核状态 ----
# M核通过RPMSG与A核通信，用于发送TARGET_ON/WARNING等命令
# 如果M核未运行，脚本仍可通过命名管道手动触发测试
echo "[3/6] M-core: $(cat /sys/class/remoteproc/remoteproc0.state 2>/dev/null || echo 'N/A')"

# ---- [4/6] 发送触发命令 ----
# 模拟实际使用场景：
#   TARGET_ON -> 目标检测到物体，开始缓冲录制
#   WARNING   -> 收到预警信号，触发紧急保存（前后共30秒）
echo "[4/6] Triggers (buffer 10s, then WARNING)..."
echo TARGET_ON > "$PIPE" 2>/dev/null   # 发送目标出现信号
sleep 10                               # 缓冲10秒（模拟目标持续存在）
echo WARNING > "$PIPE" 2>/dev/null     # 发送预警信号

# 计算需要的等待时间：
#   - 前10秒已缓冲，后边需要补足到30秒 = 20秒
#   - 再加15秒给ffmpeg编码时间
BUF_BEFORE=10
AFTER_NEEDED=$(( 30 - BUF_BEFORE ))    # 补足后段: 30-10=20秒
if [ $AFTER_NEEDED -lt 15 ]; then AFTER_NEEDED=15; fi  # 至少后录15秒
ENCODE_EXTRA=15                        # ffmpeg编码额外时间
TOTAL_WAIT=$(( AFTER_NEEDED + ENCODE_EXTRA ))
echo "      Buffered ${BUF_BEFORE}s before trigger, waiting ${TOTAL_WAIT}s for save..."
sleep $TOTAL_WAIT

# ---- [5/6] 输出测试结果 ----
echo ""
echo "========== RESULTS =========="
grep "STATE:" "$LOG" 2>/dev/null || echo "(no state changes)"      # 状态机转换记录
grep -E "Saving|Saved|EMERGENCY|No frames" "$LOG" 2>/dev/null || echo "(no saves)"  # 保存记录
ls -lh "$SD"/emergency_*.mp4 2>/dev/null || echo "(no files on SD)" # 生成的视频文件
grep -iE "error|fail" "$LOG" 2>/dev/null | grep -v RPMSG | head -3 || echo "(no errors)"  # 错误信息
echo "============================="
