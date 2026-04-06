#!/bin/bash

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "${SCRIPT_DIR}"

# Read version from dkms.conf (single source of truth)
PACKAGE_NAME=$(sed -n 's/^PACKAGE_NAME="\?\([^"]*\)"\?$/\1/p' dkms.conf)
PACKAGE_VERSION=$(sed -n 's/^PACKAGE_VERSION="\?\([^"]*\)"\?$/\1/p' dkms.conf)
[[ -n "${PACKAGE_NAME}" && -n "${PACKAGE_VERSION}" ]] || { echo "[ERROR] Failed to parse dkms.conf"; exit 1; }
SRC_DIR="/usr/src/${PACKAGE_NAME}-${PACKAGE_VERSION}"

# Remove via DKMS if registered
if command -v dkms >/dev/null 2>&1 && \
   dkms status "${PACKAGE_NAME}/${PACKAGE_VERSION}" 2>/dev/null | grep -q "${PACKAGE_NAME}"; then
    echo "[INFO] Removing ${PACKAGE_NAME} ${PACKAGE_VERSION} from DKMS..."
    dkms remove "${PACKAGE_NAME}/${PACKAGE_VERSION}" --all
    rm -rf "${SRC_DIR}"
fi

# Always clean up legacy-installed module (may coexist with DKMS)
for kdir in /lib/modules/*/updates; do
    rm -f "${kdir}"/mx_dma.ko* 2>/dev/null || true
done
depmod -a

# Remove auto-load config
rm -f /etc/modules-load.d/mx_dma.conf

# Remove CXL udev helpers
if [[ -f /usr/local/sbin/xcena_set_devdax_perm ]]; then
    echo "[INFO] Removing xcena_set_devdax_perm..."
    rm -f /usr/local/sbin/xcena_set_devdax_perm
    rm -f /etc/udev/rules.d/99-xcena_set_devdax_perm.rules
    udevadm control --reload-rules
    echo "[INFO] xcena_set_devdax_perm removal completed."
fi

# Update initramfs
if command -v update-initramfs >/dev/null 2>&1; then
    update-initramfs -u -k "$(uname -r)"
elif command -v dracut >/dev/null 2>&1; then
    dracut --force --kver "$(uname -r)"
fi
