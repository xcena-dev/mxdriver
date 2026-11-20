#!/bin/bash

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "${SCRIPT_DIR}"

PACKAGE_NAME=$(sed -n 's/^PACKAGE_NAME="\?\([^"]*\)"\?$/\1/p' dkms.conf)
PACKAGE_VERSION=$(sed -n 's/^PACKAGE_VERSION="\?\([^"]*\)"\?$/\1/p' dkms.conf)
[[ -n "${PACKAGE_NAME}" && -n "${PACKAGE_VERSION}" ]] || { echo "[ERROR] Failed to parse dkms.conf"; exit 1; }
SRC_DIR="/usr/src/${PACKAGE_NAME}-${PACKAGE_VERSION}"

if command -v dkms >/dev/null 2>&1 && \
   dkms status "${PACKAGE_NAME}/${PACKAGE_VERSION}" 2>/dev/null | grep -q "${PACKAGE_NAME}"; then
    echo "[INFO] Removing ${PACKAGE_NAME} ${PACKAGE_VERSION} from DKMS..."
    dkms remove "${PACKAGE_NAME}/${PACKAGE_VERSION}" --all
    rm -rf "${SRC_DIR}"
fi

for kdir in /lib/modules/*/updates; do
    rm -f "${kdir}"/mx_dma.ko* 2>/dev/null || true
done
depmod -a

rm -f /etc/modules-load.d/mx_dma.conf

rm -f /etc/modprobe.d/mx_dma-order.conf
rm -f /etc/dracut.conf.d/mx_dma.conf
if [[ -f /etc/initramfs-tools/modules ]]; then
    sed -i '/^mx_dma$/d' /etc/initramfs-tools/modules
fi

if [[ -f /etc/udev/rules.d/60-xcena-mx_dma.rules ]]; then
    rm -f /etc/udev/rules.d/60-xcena-mx_dma.rules
    udevadm control --reload-rules \
        || echo "[INFO] udevadm reload failed as above; applies at boot."
fi

if [[ -f /usr/local/sbin/xcena_set_devdax_perm ]]; then
    echo "[INFO] Removing xcena_set_devdax_perm..."
    rm -f /usr/local/sbin/xcena_set_devdax_perm
    rm -f /etc/udev/rules.d/99-xcena_set_devdax_perm.rules
    udevadm control --reload-rules \
        || echo "[INFO] udevadm reload failed as above; rule change applies at boot."
    echo "[INFO] xcena_set_devdax_perm removal completed."
fi

if command -v update-initramfs >/dev/null 2>&1; then
    if [[ -e "/boot/initrd.img-$(uname -r)" ]]; then
        update-initramfs -u -k "$(uname -r)"
    else
        echo "[INFO] No initrd for $(uname -r) to update, skipping regeneration."
    fi
elif command -v dracut >/dev/null 2>&1; then
    dracut --force --kver "$(uname -r)"
fi
