#!/bin/bash
# TTS语音生成脚本 - 使用微软edge-tts生成高质量中文语音WAV文件
# 输出格式: 48000Hz / 立体声(2ch) / 16bit PCM WAV (适配MAX98357A)
#
# 用法:
#   ./gen_tts_wav.sh "要说的文字" [输出文件名] [发音人]
#
# 示例:
#   ./gen_tts_wav.sh "录制开始" rec_start
#   ./gen_tts_wav.sh "注意安全，请佩戴好头盔" safety_alert
#   ./gen_tts_wav.sh "系统启动中" booting zh-CN-YunxiNeural
#
# 可用中文发音人:
#   zh-CN-XiaoxiaoNeural  晓晓(女声, 默认)
#   zh-CN-YunxiNeural     云希(男声)
#   zh-CN-XiaoyiNeural    晓伊(女声)
#   zh-CN-YunjianNeural   云健(男声)

set -e

TEXT="$1"
NAME="${2:-$(echo "$TEXT" | md5sum | cut -c1-8)}"
VOICE="${3:-zh-CN-XiaoxiaoNeural}"

if [ -z "$TEXT" ]; then
    echo "用法: $0 <中文文本> [输出文件名] [发音人]"
    echo ""
    echo "示例:"
    echo "  $0 '录制开始' rec_start"
    echo "  $0 '注意安全，请佩戴好头盔' safety_alert"
    echo "  $0 '系统启动中' booting zh-CN-YunxiNeural"
    exit 1
fi

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
OUTDIR="${SCRIPT_DIR}/../assets"
mkdir -p "$OUTDIR"

EDGE_TTS="$HOME/.local/bin/edge-tts"
TMP_MP3="/tmp/tts_gen_$$.mp3"
OUT_WAV="${OUTDIR}/${NAME}.wav"

echo "[TTS] 文本: $TEXT"
echo "[TTS] 发音人: $VOICE"
echo "[TTS] 输出: $OUT_WAV"

$EDGE_TTS --voice "$VOICE" --text "$TEXT" --write-media "$TMP_MP3" > /dev/null 2>&1
ffmpeg -y -i "$TMP_MP3" -ar 48000 -ac 2 -sample_fmt s16 "$OUT_WAV" > /dev/null 2>&1
rm -f "$TMP_MP3"

DURATION=$(ffprobe -hide_banner -v quiet -show_entries format=duration -of csv=p=0 "$OUT_WAV" 2>/dev/null)
SIZE=$(ls -lh "$OUT_WAV" | awk '{print $5}')
echo "[TTS] 完成! 时长: ${DURATION}s, 大小: $SIZE"
echo "[TTS] 文件: $OUT_WAV"
