#!/bin/sh

DEV="${1:-/dev/ttyRPMSG0}"

if [ ! -e "$DEV" ]; then
  echo "RPMsg device not found: $DEV" >&2
  echo "Start the M33 firmware first, then retry." >&2
  exit 1
fi

stty -onlcr -echo -F "$DEV"

echo "Reading V2X/IMU alerts from $DEV"
echo "Press Ctrl+C to stop."

exec 3< "$DEV"
sleep 0.1
if ! printf 'v2x_imu_alert_reader_ready\n' > "$DEV"; then
  echo "Failed to write ready message to $DEV" >&2
  exit 1
fi

while IFS= read -r line <&3; do
  case "$line" in
    V2X_ALERT*warning=*)
      warning_text=${line#*warning=}
      printf '%s %s\n' "$(date '+%Y-%m-%d %H:%M:%S')" "$warning_text"
      ;;
    V2X_ALERT*|IMU_ALERT*|FALL_ALERT*)
      printf '%s %s\n' "$(date '+%Y-%m-%d %H:%M:%S')" "$line"
      ;;
  esac
done
