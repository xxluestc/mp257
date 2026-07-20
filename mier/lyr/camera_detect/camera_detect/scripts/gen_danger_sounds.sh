#!/bin/bash
# ============================================================================
# gen_danger_sounds.sh - 批量生成本地异常路况固定提示音
#
# 用法:
#   ./scripts/gen_danger_sounds.sh
#
# 说明:
#   1. 根据手机端当前实际上报的异常类型生成固定提示音
#   2. 输出格式: 48000Hz / 立体声 / 16bit PCM WAV (适配 MAX98357A)
#   3. 生成后自动放到 nav_tts_cache/，随程序一起部署到开发板
#   4. 当前手机端只会上报: 障碍物、施工
#
# 依赖:
#   - edge-tts (pip install edge-tts)
#   - ffmpeg
# ============================================================================

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
CACHE_DIR="${SCRIPT_DIR}/../nav_tts_cache"
VOICE="${1:-zh-CN-XiaoxiaoNeural}"

mkdir -p "$CACHE_DIR"

# 优先使用用户目录下的 edge-tts，否则依赖 PATH
EDGE_TTS="${HOME}/.local/bin/edge-tts"
if [ ! -x "$EDGE_TTS" ]; then
    EDGE_TTS="edge-tts"
fi

if ! command -v "$EDGE_TTS" >/dev/null 2>&1; then
    echo "错误: 未找到 edge-tts，请先安装: pip install edge-tts"
    exit 1
fi

if ! command -v ffmpeg >/dev/null 2>&1; then
    echo "错误: 未找到 ffmpeg，请先安装"
    exit 1
fi

# 定义提示音列表：文件名前缀|播报文本
# 当前只保留手机端实际上报的两种异常
declare -a SOUNDS=(
    "danger_障碍物|前方路面有障碍物，请注意避让"
    "danger_施工|前方施工路段，请减速慢行"
)

TMP_MP3="$(mktemp /tmp/danger_tts_XXXXXX.mp3)"
cleanup() {
    rm -f "$TMP_MP3"
}
trap cleanup EXIT

for item in "${SOUNDS[@]}"; do
    filename="${item%%|*}"
    text="${item#*|}"
    output="${CACHE_DIR}/${filename}.wav"

    if [ -f "$output" ]; then
        echo "[SKIP] 已存在: $output"
        continue
    fi

    echo "[TTS] 生成: $text -> $output"
    "$EDGE_TTS" --voice "$VOICE" --text "$text" --write-media "$TMP_MP3" >/dev/null 2>&1
    ffmpeg -y -i "$TMP_MP3" -ar 48000 -ac 2 -sample_fmt s16 "$output" >/dev/null 2>&1
    echo "[TTS] 完成: $output"
done

echo "全部生成完毕，共 ${#SOUNDS[@]} 个提示音"
