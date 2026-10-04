#!/bin/sh
# 校验外置TF中的正式 DVR MP4。默认使用 ffprobe；--full 额外整段解码。

set -u

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
TF_CONTROL_SCRIPT="${SCRIPT_DIR}/tf_card_control.sh"
DVR_DIR=""
FULL_DECODE=0

case "${1:-}" in
    --full)
        FULL_DECODE=1
        shift
        ;;
esac
if [ "$#" -gt 0 ]; then
    DVR_DIR="$1"
else
    status_output=$("$TF_CONTROL_SCRIPT" status 2>/dev/null) || status_output=""
    state=""
    tf_mount=""
    for token in $status_output; do
        case "$token" in
            state=*) state=${token#state=} ;;
            mount=*) tf_mount=${token#mount=} ;;
        esac
    done
    if [ "$state" != "mounted" ] || [ -z "$tf_mount" ]; then
        echo "错误：TF卡未挂载，无法确定DVR目录" >&2
        exit 2
    fi
    DVR_DIR="${tf_mount%/}/dvr"
fi

if ! command -v ffprobe >/dev/null 2>&1; then
    echo "错误：缺少 ffprobe" >&2
    exit 2
fi
if [ "$FULL_DECODE" -eq 1 ] && ! command -v ffmpeg >/dev/null 2>&1; then
    echo "错误：--full 需要 ffmpeg" >&2
    exit 2
fi
if [ ! -d "$DVR_DIR" ]; then
    echo "错误：DVR目录不存在：$DVR_DIR" >&2
    exit 2
fi

checked=0
failed=0
list_file="/tmp/dvr_verify_list.$$"
find "$DVR_DIR" -maxdepth 1 -type f -name '*.mp4' -print >"$list_file"

while IFS= read -r video; do
    [ -n "$video" ] || continue
    checked=$((checked + 1))
    if ! ffprobe -v error -select_streams v:0 \
        -show_entries stream=codec_name,width,height,duration \
        -of csv=p=0 "$video" >/tmp/dvr_verify_probe.out 2>/tmp/dvr_verify_probe.err; then
        echo "失败：$(basename "$video")（ffprobe无法读取）"
        failed=$((failed + 1))
        continue
    fi
    if [ "$FULL_DECODE" -eq 1 ] &&
       ! ffmpeg -nostdin -v error -i "$video" -map 0:v:0 -f null - \
           >/tmp/dvr_verify_decode.out 2>/tmp/dvr_verify_decode.err; then
        echo "失败：$(basename "$video")（整段解码失败）"
        failed=$((failed + 1))
        continue
    fi
    probe_result=$(tr -d '\r\n' </tmp/dvr_verify_probe.out)
    echo "通过：$(basename "$video") [$probe_result]"
done <"$list_file"

rm -f "$list_file" /tmp/dvr_verify_probe.out /tmp/dvr_verify_probe.err \
    /tmp/dvr_verify_decode.out /tmp/dvr_verify_decode.err

pending=$(find "$DVR_DIR" -maxdepth 1 -type f -name '*.mp4.part' | wc -l)
failures=$(find "$DVR_DIR" -maxdepth 1 -type f -name '*.encoder.log' | wc -l)
echo "汇总：检查=$checked 失败=$failed 未提交part=$pending 编码诊断=$failures"
[ "$failed" -eq 0 ] && [ "$pending" -eq 0 ]
