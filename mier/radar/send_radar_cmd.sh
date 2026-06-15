#!/bin/bash
# 雷达串口命令发送工具
# 用法: ./send_radar_cmd.sh <hex_cmd_byte> [hex_param1 hex_param2 ...]
# 示例: ./send_radar_cmd.sh D1 01      (打开雷达感应)
# 示例: ./send_radar_cmd.sh 02 00      (设置感应等级为0)
# 示例: ./send_radar_cmd.sh D2 88 13   (设置最远距离5000cm)
# 示例: ./send_radar_cmd.sh 30         (查询感应信息)

DEVICE="/dev/ttySTM1"
BAUD="921600"

if [ $# -lt 1 ]; then
    echo "用法: $0 <hex_cmd_byte> [hex_param1 hex_param2 ...]"
    echo "示例:"
    echo "  $0 D1 01        # 打开雷达感应"
    echo "  $0 02 00        # 设置感应等级0(最灵敏)"
    echo "  $0 D2 88 13     # 设置最远距离5000cm"
    echo "  $0 30           # 查询感应信息"
    echo "  $0 1E           # 查询版本"
    echo "  $0 D0           # 查询雷达状态"
    exit 1
fi

CMD="$1"
shift

# 构建参数部分
PARAMS=""
PARAM_LEN=0
for p in "$@"; do
    PARAMS="$PARAMS $p"
    PARAM_LEN=$((PARAM_LEN + 1))
done

# 构建帧 (0x58 + cmd + len + params)
FRAME_HEX="58 $CMD $(printf '%02X' $PARAM_LEN) $PARAMS"
# 移除多余空格
FRAME_HEX=$(echo "$FRAME_HEX" | tr -s ' ')

# 计算校验和 (低16位)
CSUM=0
for byte in $FRAME_HEX; do
    CSUM=$((CSUM + 0x$byte))
done
CSUM=$((CSUM & 0xFFFF))
CS_LOW=$(printf '%02X' $((CSUM & 0xFF)))
CS_HIGH=$(printf '%02X' $(((CSUM >> 8) & 0xFF)))

FRAME_HEX="$FRAME_HEX $CS_LOW $CS_HIGH"

echo "发送帧: $FRAME_HEX"

# 配置串口并发送
stty -F "$DEVICE" "$BAUD" raw cs8 -cstopb -parenb -echo 2>/dev/null

# 将十六进制转换为二进制发送
python3 -c "
import sys
data = bytes.fromhex('$FRAME_HEX'.replace(' ', ''))
sys.stdout.buffer.write(data)
" > "$DEVICE"

# 读取回复 (最多等待1秒)
timeout 1 xxd -l 64 "$DEVICE" 2>/dev/null
