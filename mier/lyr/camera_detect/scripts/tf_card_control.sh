#!/bin/sh
# Safely inspect, mount, or eject the removable TF card.

set -eu

TF_BASE=/dev/mmcblk0
TF_DEVICE_OVERRIDE=${TF_DEVICE:-}
TF_MOUNT_OVERRIDE=${TF_MOUNT:-}

detect_device() {
    if [ -n "$TF_DEVICE_OVERRIDE" ]; then
        printf '%s\n' "$TF_DEVICE_OVERRIDE"
    elif [ -b "${TF_BASE}p1" ]; then
        printf '%s\n' "${TF_BASE}p1"
    elif [ -b "$TF_BASE" ]; then
        # Some cards are formatted as a whole-disk FAT filesystem and have no p1.
        printf '%s\n' "$TF_BASE"
    else
        # Keep status output stable even when the card is absent.
        printf '%s\n' "${TF_BASE}p1"
    fi
}

TF_DEVICE=$(detect_device)
if [ -n "$TF_MOUNT_OVERRIDE" ]; then
    TF_MOUNT=$TF_MOUNT_OVERRIDE
elif [ "$TF_DEVICE" = "$TF_BASE" ]; then
    TF_MOUNT=/run/media/mmcblk0
else
    TF_MOUNT=/run/media/mmcblk0p1
fi

mounted_at() {
    awk -v device="$TF_DEVICE" '$1 == device { print $2; exit }' /proc/mounts
}

is_mounted() {
    [ -n "$(mounted_at)" ]
}

case "${1:-status}" in
    status)
        if is_mounted; then
            TF_ACTUAL_MOUNT=$(mounted_at)
            echo "state=mounted device=$TF_DEVICE mount=$TF_ACTUAL_MOUNT"
        elif [ -b "$TF_BASE" ] || [ -b "$TF_DEVICE" ]; then
            echo "state=inserted_not_mounted device=$TF_DEVICE mount=$TF_MOUNT"
        else
            echo "state=not_inserted device=$TF_DEVICE mount=$TF_MOUNT"
        fi
        ;;
    mount)
        if is_mounted; then
            TF_ACTUAL_MOUNT=$(mounted_at)
            echo "TF card is already mounted at $TF_ACTUAL_MOUNT"
            exit 0
        fi
        if [ ! -b "$TF_DEVICE" ]; then
            echo "TF card device $TF_DEVICE was not found" >&2
            exit 2
        fi
        if awk -v mountpoint="$TF_MOUNT" '$2 == mountpoint { found = 1 } END { exit !found }' /proc/mounts; then
            echo "Mount point $TF_MOUNT is occupied by another device" >&2
            exit 3
        fi
        mkdir -p "$TF_MOUNT"
        mount "$TF_DEVICE" "$TF_MOUNT"
        sync
        echo "TF card mounted at $TF_MOUNT"
        ;;
    eject)
        if ! is_mounted; then
            if [ -b "$TF_BASE" ] || [ -b "$TF_DEVICE" ]; then
                echo "TF card is not mounted; no eject action was performed" >&2
            else
                echo "TF card is not inserted; no eject action was performed" >&2
            fi
            exit 3
        fi
        TF_ACTUAL_MOUNT=$(mounted_at)
        sync
        umount "$TF_ACTUAL_MOUNT"
        sync
        echo "TF card safely unmounted; it may now be removed"
        ;;
    *)
        echo "Usage: $0 {status|mount|eject}" >&2
        exit 2
        ;;
esac
