#!/bin/bash
# ============================================================================
# gen_nav_tts.sh - 导航文案在线 TTS 生成脚本
#
# 用法:
#   ./gen_nav_tts.sh "要说的导航文字" /path/to/output.wav
#
# 说明:
#   1. 优先使用 edge-tts 在线生成高质量中文语音
#   2. 输出格式: 48000Hz / 立体声 / 16bit PCM WAV (适配 MAX98357A)
#   3. 生成的文件会被 nav_tts.c 缓存，下次直接播放
#
# 依赖:
#   - edge-tts (pip install edge-tts)
#   - ffmpeg
# ============================================================================

set -e

TEXT="$1"
OUTPUT="$2"
VOICE="${3:-zh-CN-XiaoxiaoNeural}"

if [ -z "$TEXT" ] || [ -z "$OUTPUT" ]; then
    echo "用法: $0 <导航文字> <输出WAV路径> [发音人]"
    exit 1
fi

# 优先使用用户目录下的 edge-tts，否则依赖 PATH
EDGE_TTS="${HOME}/.local/bin/edge-tts"
if [ ! -x "$EDGE_TTS" ]; then
    EDGE_TTS="edge-tts"
fi

TMP_MP3="$(mktemp /tmp/nav_tts_XXXXXX.mp3)"

cleanup() {
    rm -f "$TMP_MP3"
}
trap cleanup EXIT

echo "[TTS] 生成: $TEXT -> $OUTPUT"
$EDGE_TTS --voice "$VOICE" --text "$TEXT" --write-media "$TMP_MP3" >/dev/null 2>&1
ffmpeg -y -i "$TMP_MP3" -ar 48000 -ac 2 -sample_fmt s16 "$OUTPUT" >/dev/null 2>&1

echo "[TTS] 完成: $OUTPUT"
