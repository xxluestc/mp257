#!/bin/sh
# 只读审计 bootloader 到内核入口之间的配置和可测量项。
# 本脚本不会调用 fw_setenv，也不会修改 bootfs、FIP、DTB 或 extlinux.conf。

set -u

BOOT_DIR="${BOOT_DIR:-/boot}"

env_value() {
    key="$1"
    fw_printenv "$key" 2>/dev/null | sed -n "s/^${key}=//p"
}

echo "=== measurement_boundary ==="
echo "systemd/dmesg start at the Linux kernel; pre-kernel time requires UART or U-Boot bootstage"

echo "=== uboot_environment_readonly ==="
if command -v fw_printenv >/dev/null 2>&1; then
    for key in bootdelay bootcmd boot_targets boot_prefixes boot_syslinux_conf devplist; do
        fw_printenv "$key" 2>/dev/null || true
    done
else
    echo "fw_printenv=missing"
fi

echo "=== active_extlinux ==="
active_conf=""
if command -v fw_printenv >/dev/null 2>&1; then
    boot_prefixes="$(env_value boot_prefixes)"
    boot_syslinux_conf="$(env_value boot_syslinux_conf)"
    if [ -n "$boot_prefixes" ] && [ -n "$boot_syslinux_conf" ]; then
        active_conf="${BOOT_DIR}${boot_prefixes}${boot_syslinux_conf}"
    fi
fi
if [ -n "$active_conf" ] && [ -f "$active_conf" ]; then
    echo "path=${active_conf}"
    grep -Ei '^[[:space:]]*(DEFAULT|TIMEOUT|PROMPT|KERNEL|LINUX|INITRD|FDT|FDTDIR|APPEND|MENU BACKGROUND)' \
        "$active_conf" || true
    timeout="$(awk 'toupper($1) == "TIMEOUT" { print $2; exit }' "$active_conf")"
    case "$timeout" in
        ''|*[!0-9]*) ;;
        *) awk -v value="$timeout" \
            'BEGIN { printf "menu_timeout_seconds=%.1f\n", value / 10.0 }' ;;
    esac
else
    echo "path=not-resolved"
fi

echo "=== boot_artifacts ==="
for path in \
    "${BOOT_DIR}/Image.gz" \
    "${BOOT_DIR}/st-image-resize-initrd" \
    "${BOOT_DIR}/myb-stm32mp257x-2GB.dtb" \
    "${BOOT_DIR}/splash_landscape.bmp" \
    "${BOOT_DIR}/boot.scr.uimg"; do
    if [ -f "$path" ]; then
        bytes="$(wc -c < "$path")"
        printf '%s\t%s bytes\n' "$path" "$bytes"
    fi
done

echo "=== initramfs_evidence ==="
if [ -e /etc/.resized ]; then
    echo "rootfs_resize_marker=present"
else
    echo "rootfs_resize_marker=missing"
fi
dmesg --color=never 2>/dev/null |
    grep -Ei 'Unpacking initramfs|Freeing initrd memory|VFS: Mounted root' || true

echo "=== kernel_boot_profiler_support ==="
if [ -r /proc/config.gz ]; then
    zcat /proc/config.gz 2>/dev/null |
        grep -E '^CONFIG_(PRINTK_TIME|IKCONFIG|FTRACE|FUNCTION_TRACER|FUNCTION_GRAPH_TRACER|BOOT_CONFIG|BLK_DEV_INITRD|EXT4_FS|MMC)=' |
        sort || true
fi
for tool in systemd-analyze perf trace-cmd bootchartd; do
    if command -v "$tool" >/dev/null 2>&1; then
        printf '%s=%s\n' "$tool" "$(command -v "$tool")"
    else
        printf '%s=missing\n' "$tool"
    fi
done
