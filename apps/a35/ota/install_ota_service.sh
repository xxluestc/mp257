#!/bin/sh
# Install/update the stable board-side OTA service outside /xxl/camera_detect.

set -eu

SOURCE_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
INSTALL_DIR=${INSTALL_DIR:-/opt/helmet-ota}
STATE_DIR=${STATE_DIR:-/var/lib/helmet-ota}
UNIT_DIR=${UNIT_DIR:-/etc/systemd/system}

if [ "$(id -u)" -ne 0 ]; then
    echo "install_ota_service.sh must run as root" >&2
    exit 1
fi

for name in helmet_ota_common.py helmet_ota_installer.py helmet_ota_server.py \
    helmet_ota_recover_if_needed.sh helmet-ota.service.example; do
    if [ ! -f "${SOURCE_DIR}/${name}" ]; then
        echo "missing OTA service file: ${SOURCE_DIR}/${name}" >&2
        exit 1
    fi
done

install -d -m 0755 "$INSTALL_DIR"
install -d -m 0750 "$STATE_DIR" "${STATE_DIR}/uploads"
install -m 0644 "${SOURCE_DIR}/helmet_ota_common.py" "${INSTALL_DIR}/helmet_ota_common.py"
install -m 0755 "${SOURCE_DIR}/helmet_ota_installer.py" "${INSTALL_DIR}/helmet_ota_installer.py"
install -m 0755 "${SOURCE_DIR}/helmet_ota_server.py" "${INSTALL_DIR}/helmet_ota_server.py"
install -m 0755 "${SOURCE_DIR}/helmet_ota_recover_if_needed.sh" \
    "${INSTALL_DIR}/helmet_ota_recover_if_needed.sh"
install -m 0644 "${SOURCE_DIR}/helmet-ota.service.example" "${UNIT_DIR}/helmet-ota.service"

cat > "${INSTALL_DIR}/README" <<'EOF'
Helmet A35 OTA stable service.
Runtime state: /var/lib/helmet-ota
HTTP API:      http://<board-ip>:8090/api/ota/
Project docs:  apps/a35/docs/OTA.md and OTA_API.md
EOF

systemctl daemon-reload
systemctl enable helmet-ota.service
systemctl restart helmet-ota.service
systemctl --no-pager --full status helmet-ota.service
