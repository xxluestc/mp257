#!/bin/bash
# 生成 V2X 方向提示音频
# 输出到 camera_detect/sounds/，格式 48000Hz 立体声 16bit PCM WAV

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
OUTDIR="${SCRIPT_DIR}/../sounds"
mkdir -p "$OUTDIR"

EDGE_TTS="$HOME/.local/bin/edge-tts"
VOICE="zh-CN-XiaoxiaoNeural"

generate() {
    local text="$1"
    local name="$2"
    local tmp_mp3="/tmp/v2x_${name}_$$.mp3"
    local out_wav="${OUTDIR}/${name}.wav"

    echo "[TTS] $name: $text"
    "$EDGE_TTS" --voice "$VOICE" --text "$text" --write-media "$tmp_mp3" >/dev/null 2>&1
    ffmpeg -y -i "$tmp_mp3" -ar 48000 -ac 2 -sample_fmt s16 "$out_wav" >/dev/null 2>&1
    rm -f "$tmp_mp3"

    local duration
    duration=$(ffprobe -hide_banner -v quiet -show_entries format=duration -of csv=p=0 "$out_wav" 2>/dev/null)
    local size
    size=$(ls -lh "$out_wav" | awk '{print $5}')
    echo "[TTS] 完成: $out_wav (${duration}s, $size)"
}

generate "附近有来车，请注意观察"   v2x_nearby
generate "左前方有来车，请注意"     v2x_left_front
generate "右前方有来车，请注意"     v2x_right_front
generate "左侧有来车，请注意"       v2x_left
generate "右侧有来车，请注意"       v2x_right

echo "[TTS] 所有 V2X 音频已生成到 $OUTDIR"
