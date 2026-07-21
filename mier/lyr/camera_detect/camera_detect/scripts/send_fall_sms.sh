#!/bin/bash
# ============================================================================
# 摔倒短信通知脚本
# 功能：被 radar_fusion 检测到摔倒后异步调用，负责发送短信通知
# 用法：./send_fall_sms.sh "IMU_ALERT type=fall ..."
#
# 注意：本脚本只提供通用框架和日志记录，实际的短信发送接口需要
#       根据你所使用的短信平台（阿里云、腾讯云、Twilio 等）自行填写。
# ============================================================================

set -u

EVENT_LINE="${1:-}"
LOG_FILE="/xxl/camera_detect/fall_sms.log"
TIMESTAMP=$(date '+%Y-%m-%d %H:%M:%S')

# 简单的键值解析函数：从 EVENT_LINE 中提取 key=value
get_kv() {
    local key="$1"
    echo "$EVENT_LINE" | grep -oE "${key}=[^[:space:]]+" | cut -d'=' -f2 | head -n1
}

# 解析 GPS 等字段
REASON=$(get_kv "reason"); [ -z "$REASON" ] && REASON="unknown"
SEQ=$(get_kv "seq");       [ -z "$SEQ" ] && SEQ="0"
GPS_VALID=$(get_kv "gps_valid"); [ -z "$GPS_VALID" ] && GPS_VALID="0"
LAT_1E7=$(get_kv "lat_1e7");     [ -z "$LAT_1E7" ] && LAT_1E7="0"
LON_1E7=$(get_kv "lon_1e7");     [ -z "$LON_1E7" ] && LON_1E7="0"
SPEED_CMS=$(get_kv "speed_cms"); [ -z "$SPEED_CMS" ] && SPEED_CMS="0"
HEADING_CDEC=$(get_kv "heading_cdeg"); [ -z "$HEADING_CDEC" ] && HEADING_CDEC="0"

# 构建短信内容
SMS_MSG="[摔倒告警] 时间:${TIMESTAMP}"
if [ "$GPS_VALID" = "1" ] && [ "$LAT_1E7" != "0" ] && [ "$LON_1E7" != "0" ]; then
    SMS_MSG="${SMS_MSG} 位置:lat=${LAT_1E7},lon=${LON_1E7}"
else
    SMS_MSG="${SMS_MSG} 未获取到有效GPS位置"
fi
SMS_MSG="${SMS_MSG} 速度:${SPEED_CMS}cm/s 航向:${HEADING_CDEC}"

# 记录到本地日志（即使短信接口未配置，也能看到触发记录）
echo "[${TIMESTAMP}] EVENT=${EVENT_LINE}" >> "$LOG_FILE"
echo "[${TIMESTAMP}] SMS_MSG=${SMS_MSG}" >> "$LOG_FILE"

# ============================================================================
# TODO: 在此处替换为实际短信平台接口
# 示例（阿里云短信）：
# curl -s -X POST "https://dysmsapi.aliyuncs.com/" \
#      -d "PhoneNumbers=13800138000" \
#      -d "SignName=你的签名" \
#      -d "TemplateCode=你的模板CODE" \
#      -d "TemplateParam={\"msg\":\"${SMS_MSG}\"}"
#
# 示例（Twilio）：
# curl -s -X POST "https://api.twilio.com/2010-04-01/Accounts/ACxxx/Messages.json" \
#      --user "ACxxx:your_auth_token" \
#      --data-urlencode "To=+8613800138000" \
#      --data-urlencode "From=+1234567890" \
#      --data-urlencode "Body=${SMS_MSG}"
# ============================================================================

# 如果配置了 SMS_API_URL 环境变量，则尝试通过 HTTP POST 发送
if [ -n "${SMS_API_URL:-}" ]; then
    RESPONSE=$(curl -s -X POST "$SMS_API_URL" \
        -d "phone=${SMS_PHONE:-}" \
        -d "message=${SMS_MSG}" 2>&1) || true
    echo "[${TIMESTAMP}] SMS_API_RESPONSE=${RESPONSE}" >> "$LOG_FILE"
fi

exit 0
