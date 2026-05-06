#!/bin/bash
# TTS语音生成脚本 - 使用espeak-ng生成中文语音WAV文件
# 用法: ./gen_tts.sh "要说的文字" 输出文件名

if [ $# -lt 2 ]; then
    echo "用法: $0 <中文文本> <输出文件名(不含扩展名)>"
    echo "示例: $0 '你好世界' hello"
    exit 1
fi

TEXT="$1"
NAME="$2"
OUTDIR="$(dirname "$0")/../assets"
mkdir -p "$OUTDIR"

MONO_WAV="${OUTDIR}/${NAME}_mono.wav"
STEREO_WAV="${OUTDIR}/${NAME}.wav"

espeak-ng -v zh -s 160 -p 50 -a 100 "$TEXT" -w "$MONO_WAV"

python3 -c "
import wave, struct

with wave.open('$MONO_WAV', 'r') as wav:
    params = wav.getparams()
    frames = wav.readframes(wav.getnframes())

with wave.open('$STEREO_WAV', 'w') as wout:
    wout.setnchannels(2)
    wout.setsampwidth(params.sampwidth)
    wout.setframerate(params.framerate)
    for i in range(0, len(frames), params.sampwidth):
        sample = frames[i:i+params.sampwidth]
        wout.writeframes(sample + sample)
"

rm -f "$MONO_WAV"
echo "生成: $STEREO_WAV"
