#!/bin/sh
# Safely inspect, mount, or eject the removable TF card.

set -eu

TF_DEVICE=${TF_DEVICE:-/dev/mmcblk0p1}
TF_MOUNT=${TF_MOUNT:-/run/media/mmcblk0p1}

is_mounted() {
    awk -v device="$TF_DEVICE" -v mountpoint="$TF_MOUNT" \
        '$1 == device || $2 == mountpoint { found = 1 } END { exit !found }' \
        /proc/mounts
}

case "${1:-status}" in
    status)
        if is_mounted; then
            echo "mounted device=$TF_DEVICE mount=$TF_MOUNT"
        elif [ -b "$TF_DEVICE" ]; then
            echo "inserted_not_mounted device=$TF_DEVICE mount=$TF_MOUNT"
        else
            echo "not_inserted device=$TF_DEVICE"
        fi
        ;;
    mount)
        if is_mounted; then
            echo "TF card is already mounted at $TF_MOUNT"
            exit 0
        fi
        if [ ! -b "$TF_DEVICE" ]; then
            echo "TF card partition $TF_DEVICE was not found" >&2
            exit 2
        fi
        mkdir -p "$TF_MOUNT"
        mount "$TF_DEVICE" "$TF_MOUNT"
        sync
        echo "TF card mounted at $TF_MOUNT"
        ;;
    eject)
        if ! is_mounted; then
            echo "TF card is already unmounted"
            exit 0
        fi
        sync
        umount "$TF_MOUNT"
        sync
        echo "TF card safely unmounted; it may now be removed"
        ;;
    *)
        echo "Usage: $0 {status|mount|eject}" >&2
        exit 2
        ;;
esac
