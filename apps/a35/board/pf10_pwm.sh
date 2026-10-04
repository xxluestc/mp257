#!/bin/sh
#
# Control LED brightness through PF10 / TIM2_CH3 hardware PWM.
# Usage: pf10_pwm.sh <brightness:0-100> [frequency_hz] [normal|inversed]

set -eu

PWM_CHANNEL=2
BRIGHTNESS=${1:-}
FREQUENCY=${2:-1000}
POLARITY=${3:-normal}

usage()
{
    echo "Usage: $0 <brightness:0-100> [frequency_hz:100-20000] [normal|inversed]" >&2
    echo "Example: $0 20           # 20% brightness at 1 kHz" >&2
    echo "         $0 0            # output off" >&2
}

case "$BRIGHTNESS" in
    ''|*[!0-9]*)
        usage
        exit 2
        ;;
esac

case "$FREQUENCY" in
    ''|*[!0-9]*)
        usage
        exit 2
        ;;
esac

if [ "$BRIGHTNESS" -gt 100 ] ||
   [ "$FREQUENCY" -lt 100 ] ||
   [ "$FREQUENCY" -gt 20000 ]; then
    usage
    exit 2
fi

if [ "$POLARITY" != "normal" ] && [ "$POLARITY" != "inversed" ]; then
    usage
    exit 2
fi

PWM_CHIP=
for chip in /sys/class/pwm/pwmchip*; do
    [ -e "$chip" ] || continue
    node=$(readlink -f "$chip/device/of_node" 2>/dev/null || true)
    case "$node" in
        */timer@40000000/pwm)
            PWM_CHIP=$chip
            break
            ;;
    esac
done

if [ -z "$PWM_CHIP" ]; then
    echo "Error: TIM2 PWM device was not found; verify the active DTB." >&2
    exit 1
fi

PWM=$PWM_CHIP/pwm$PWM_CHANNEL
if [ ! -d "$PWM" ]; then
    echo "$PWM_CHANNEL" > "$PWM_CHIP/export"
    count=0
    while [ ! -d "$PWM" ] && [ "$count" -lt 100 ]; do
        sleep 0.01
        count=$((count + 1))
    done
fi

if [ ! -d "$PWM" ]; then
    echo "Error: PWM channel $PWM_CHANNEL export timed out." >&2
    exit 1
fi

period_ns=$((1000000000 / FREQUENCY))
duty_ns=$((period_ns * BRIGHTNESS / 100))
old_period=$(cat "$PWM/period")

# Every sysfs write applies the complete PWM state. A freshly exported channel
# has period=0, so do not write duty_cycle until a valid period is installed.
if [ "$old_period" -gt 0 ]; then
    echo 0 > "$PWM/duty_cycle"
fi
if [ "$(cat "$PWM/enable")" = "1" ]; then
    echo 0 > "$PWM/enable"
fi

echo "$period_ns" > "$PWM/period"
echo "$POLARITY" > "$PWM/polarity"
echo "$duty_ns" > "$PWM/duty_cycle"

if [ "$BRIGHTNESS" -gt 0 ]; then
    echo 1 > "$PWM/enable"
fi

echo "PF10 PWM: brightness=${BRIGHTNESS}% frequency=${FREQUENCY}Hz polarity=${POLARITY} enabled=$(cat "$PWM/enable")"
