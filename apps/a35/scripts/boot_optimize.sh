#!/bin/sh
# STM32MP257 骑行辅助项目：可回滚的第一阶段启动优化。
#
# apply:
#   1. 安装 Linux sysinit 阶段启动 M33 的 dvr-m33.service；
#   2. 安装提前启动的 dvr.service；
#   3. 禁用与本项目无关、且已确认不参与关键链路的服务。
#
# apply-optee-rng / rollback-optee-rng:
#   只应用或回退 optee_rng 预加载，供冷启动 A/B 使用，不触碰其他优化项。
#
# rollback:
#   恢复服务启用状态、dvr.service 和原有 U-Boot M33 固件文件。

set -eu

ACTION="${1:-status}"
STATE_DIR="/etc/dvr-boot-optimization"
STATE_FILE="${STATE_DIR}/service-states.tsv"
UNIT_BACKUP="${STATE_DIR}/dvr.service.before"
M33_UNIT_BACKUP="${STATE_DIR}/dvr-m33.service.before"
M33_UNIT_MARKER="${STATE_DIR}/dvr-m33-unit-was-absent"
DNSMASQ_OVERRIDE_BACKUP="${STATE_DIR}/dnsmasq-override.before"
DNSMASQ_OVERRIDE_MARKER="${STATE_DIR}/dnsmasq-override-was-absent"
OPTEE_RNG_BACKUP="${STATE_DIR}/optee-rng.modules-load.before"
OPTEE_RNG_MARKER="${STATE_DIR}/optee-rng.modules-load-was-absent"
PROJECT_DIR="/xxl/camera_detect"
UNIT_SOURCE="${PROJECT_DIR}/dvr.service.example"
UNIT_TARGET="/etc/systemd/system/dvr.service"
M33_UNIT_SOURCE="${PROJECT_DIR}/dvr-m33.service.example"
M33_UNIT_TARGET="/etc/systemd/system/dvr-m33.service"
DNSMASQ_OVERRIDE_SOURCE="${PROJECT_DIR}/dnsmasq.service.override.example"
DNSMASQ_OVERRIDE_TARGET="/etc/systemd/system/dnsmasq.service.d/override.conf"
OPTEE_RNG_SOURCE="${PROJECT_DIR}/optee-rng.modules-load.example"
OPTEE_RNG_TARGET="/etc/modules-load.d/90-dvr-optee-rng.conf"

# 保留 WiFi AP、DNS/DHCP、网络、音频、TF 卡、OP-TEE、M33、日志和本项目服务。
# 下列均为当前骑行辅助产品不使用的桌面、监控、SNMP、蓝牙、TSN 或调试服务。
OPTIONAL_SERVICES="
snmpd.service
snmptrapd.service
autorun.service
weston-graphical-session.service
weston-checkgpu.service
seatd-weston.service
psplash-drm-start.service
bluetooth.service
bt.service
bluetooth-brcmfmac-sleep.service
avahi-daemon.service
netdata.service
sysstat.service
tcf-agent.service
st-tsn.service
mstpd.service
kdump.service
rpcbind.service
st-m33firmware-load.service
iiod.service
"

require_root() {
    if [ "$(id -u)" -ne 0 ]; then
        echo "This script must run as root" >&2
        exit 1
    fi
}

show_status() {
    echo "M33 boot mode: Linux sysinit (U-Boot handover rejected on this DT)"
    echo "M33 runtime:"
    for item in state firmware; do
        path="/sys/class/remoteproc/remoteproc0/${item}"
        [ -r "$path" ] && echo "  ${item}=$(cat "$path")"
    done
    echo "DVR service:"
    systemctl is-enabled dvr.service 2>/dev/null || true
    systemctl is-active dvr.service 2>/dev/null || true
    echo "OP-TEE RNG preload:"
    if [ -f "$OPTEE_RNG_TARGET" ]; then
        printf "  config="
        tr '\n' ' ' < "$OPTEE_RNG_TARGET"
        echo
    else
        echo "  config=absent"
    fi
    printf "  module="
    if grep -q '^optee_rng ' /proc/modules 2>/dev/null; then
        echo "loaded"
    else
        echo "not-loaded"
    fi
    if [ -r /sys/class/misc/hw_random/rng_current ]; then
        printf "  rng_current="
        cat /sys/class/misc/hw_random/rng_current
    fi
    echo "Optional services:"
    for unit in $OPTIONAL_SERVICES; do
        printf "  %-38s enabled=%-10s active=%s\n" "$unit" \
            "$(systemctl is-enabled "$unit" 2>/dev/null || true)" \
            "$(systemctl is-active "$unit" 2>/dev/null || true)"
    done
}

save_optee_rng_state_once() {
    mkdir -p "$STATE_DIR"
    if [ ! -e "$OPTEE_RNG_BACKUP" ] && [ ! -e "$OPTEE_RNG_MARKER" ]; then
        if [ -f "$OPTEE_RNG_TARGET" ]; then
            cp -a "$OPTEE_RNG_TARGET" "$OPTEE_RNG_BACKUP"
        else
            : > "$OPTEE_RNG_MARKER"
        fi
    fi
}

install_optee_rng_preload() {
    [ -f "$OPTEE_RNG_SOURCE" ] || {
        echo "OP-TEE RNG module-load template not found: $OPTEE_RNG_SOURCE" >&2
        exit 1
    }
    modinfo optee_rng >/dev/null 2>&1 || {
        echo "Kernel module optee_rng is unavailable" >&2
        exit 1
    }
    save_optee_rng_state_once
    install -m 0644 "$OPTEE_RNG_SOURCE" "$OPTEE_RNG_TARGET"
}

restore_optee_rng_preload() {
    if [ -f "$OPTEE_RNG_BACKUP" ]; then
        cp -a "$OPTEE_RNG_BACKUP" "$OPTEE_RNG_TARGET"
    elif [ -f "$OPTEE_RNG_MARKER" ]; then
        rm -f "$OPTEE_RNG_TARGET"
    else
        echo "No saved OP-TEE RNG preload state at $STATE_DIR" >&2
        return 1
    fi
}

apply_optee_rng_only() {
    require_root
    install_optee_rng_preload
    sync
    echo "OP-TEE RNG preload applied; reboot is required for cold-start A/B."
    echo "Rollback: $0 rollback-optee-rng"
}

rollback_optee_rng_only() {
    require_root
    restore_optee_rng_preload
    sync
    echo "OP-TEE RNG preload rolled back; reboot is required to validate."
}

save_state_once() {
    mkdir -p "$STATE_DIR"
    [ -f "$STATE_FILE" ] || : > "$STATE_FILE"
    for unit in $OPTIONAL_SERVICES; do
        if [ "$unit" = "iiod.service" ]; then
            # 厂商单元错误地声明 Alias=iiod.service（与自身同名），因此
            # systemctl is-enabled/disable 会报告 bad/refuse。直接按唯一的
            # multi-user wants 链接记录真实状态。
            iiod_state="disabled"
            [ ! -L /etc/systemd/system/multi-user.target.wants/iiod.service ] ||
                iiod_state="enabled"
            if grep -q '^iiod.service[[:space:]]not-found$' "$STATE_FILE"; then
                sed -i "s/^iiod.service[[:space:]]not-found$/iiod.service\t${iiod_state}/" \
                    "$STATE_FILE"
            elif ! grep -q '^iiod.service[[:space:]]' "$STATE_FILE"; then
                printf "%s\t%s\n" "$unit" "$iiod_state" >> "$STATE_FILE"
            fi
            continue
        fi
        if ! grep -q "^${unit}[[:space:]]" "$STATE_FILE"; then
            state="$(systemctl is-enabled "$unit" 2>/dev/null || true)"
            [ -n "$state" ] || state="not-found"
            printf "%s\t%s\n" "$unit" "$state" >> "$STATE_FILE"
        fi
    done
    if [ ! -e "$UNIT_BACKUP" ] && [ -f "$UNIT_TARGET" ]; then
        cp -a "$UNIT_TARGET" "$UNIT_BACKUP"
    fi
    if [ ! -e "$M33_UNIT_BACKUP" ] && [ ! -e "$M33_UNIT_MARKER" ]; then
        if [ -f "$M33_UNIT_TARGET" ]; then
            cp -a "$M33_UNIT_TARGET" "$M33_UNIT_BACKUP"
        else
            : > "$M33_UNIT_MARKER"
        fi
    fi
    if [ ! -e "$DNSMASQ_OVERRIDE_BACKUP" ] &&
       [ ! -e "$DNSMASQ_OVERRIDE_MARKER" ]; then
        if [ -f "$DNSMASQ_OVERRIDE_TARGET" ]; then
            cp -a "$DNSMASQ_OVERRIDE_TARGET" "$DNSMASQ_OVERRIDE_BACKUP"
        else
            : > "$DNSMASQ_OVERRIDE_MARKER"
        fi
    fi
}

apply_optimization() {
    require_root
    [ -f "$UNIT_SOURCE" ] || {
        echo "DVR unit template not found: $UNIT_SOURCE" >&2
        exit 1
    }
    [ -f "$M33_UNIT_SOURCE" ] || {
        echo "M33 unit template not found: $M33_UNIT_SOURCE" >&2
        exit 1
    }
    [ -f "$DNSMASQ_OVERRIDE_SOURCE" ] || {
        echo "dnsmasq override template not found: $DNSMASQ_OVERRIDE_SOURCE" >&2
        exit 1
    }
    save_state_once
    install -m 0644 "$UNIT_SOURCE" "$UNIT_TARGET"
    install -m 0644 "$M33_UNIT_SOURCE" "$M33_UNIT_TARGET"
    mkdir -p "$(dirname "$DNSMASQ_OVERRIDE_TARGET")"
    install -m 0644 "$DNSMASQ_OVERRIDE_SOURCE" "$DNSMASQ_OVERRIDE_TARGET"

    for unit in $OPTIONAL_SERVICES; do
        if [ "$unit" = "iiod.service" ]; then
            systemctl stop "$unit" >/dev/null 2>&1 || true
            if [ -L /etc/systemd/system/multi-user.target.wants/iiod.service ]; then
                rm -f /etc/systemd/system/multi-user.target.wants/iiod.service
            fi
        elif [ "$unit" = "st-m33firmware-load.service" ]; then
            # 不在运行中的系统上调用厂商单元 ExecStop，避免它误停项目 M33；
            # 只移除下次启动的 symlink。
            systemctl disable "$unit" >/dev/null 2>&1 || true
        else
            systemctl disable --now "$unit" >/dev/null 2>&1 || true
        fi
    done
    systemctl daemon-reload
    systemctl enable dvr.service >/dev/null
    systemctl enable dvr-m33.service >/dev/null
    sync

    echo "Boot optimization applied."
    echo "Reboot is required to validate U-Boot M33 start and boot timing."
    echo "Rollback: $0 rollback"
}

rollback_optimization() {
    require_root
    [ -d "$STATE_DIR" ] || {
        echo "No saved optimization state at $STATE_DIR" >&2
        exit 1
    }

    if [ -f "$UNIT_BACKUP" ]; then
        cp -a "$UNIT_BACKUP" "$UNIT_TARGET"
    fi
    if [ -f "$M33_UNIT_BACKUP" ]; then
        cp -a "$M33_UNIT_BACKUP" "$M33_UNIT_TARGET"
    elif [ -f "$M33_UNIT_MARKER" ]; then
        rm -f "$M33_UNIT_TARGET"
    fi
    if [ -f "$DNSMASQ_OVERRIDE_BACKUP" ]; then
        cp -a "$DNSMASQ_OVERRIDE_BACKUP" "$DNSMASQ_OVERRIDE_TARGET"
    elif [ -f "$DNSMASQ_OVERRIDE_MARKER" ]; then
        rm -f "$DNSMASQ_OVERRIDE_TARGET"
    fi
    restore_optee_rng_preload || true
    systemctl disable dvr-m33.service >/dev/null 2>&1 || true

    if [ -f "$STATE_FILE" ]; then
        while IFS="$(printf '\t')" read -r unit state; do
            if [ "$unit" = "iiod.service" ]; then
                case "$state" in
                    enabled|enabled-runtime|linked|linked-runtime|alias)
                        ln -sf /usr/lib/systemd/system/iiod.service \
                            /etc/systemd/system/multi-user.target.wants/iiod.service
                        ;;
                    disabled|masked|masked-runtime)
                        rm -f /etc/systemd/system/multi-user.target.wants/iiod.service
                        ;;
                esac
                continue
            fi
            case "$state" in
                enabled|enabled-runtime|linked|linked-runtime|alias)
                    systemctl enable "$unit" >/dev/null 2>&1 || true
                    ;;
                disabled|masked|masked-runtime)
                    systemctl disable "$unit" >/dev/null 2>&1 || true
                    ;;
            esac
        done < "$STATE_FILE"
    fi
    systemctl daemon-reload
    sync
    echo "Boot optimization configuration rolled back; reboot to validate."
}

case "$ACTION" in
    status) show_status ;;
    apply) apply_optimization ;;
    rollback) rollback_optimization ;;
    apply-optee-rng) apply_optee_rng_only ;;
    rollback-optee-rng) rollback_optee_rng_only ;;
    *)
        echo "Usage: $0 {status|apply|rollback|apply-optee-rng|rollback-optee-rng}" >&2
        exit 2
        ;;
esac
