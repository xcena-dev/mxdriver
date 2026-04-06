#!/bin/bash

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "${SCRIPT_DIR}"

# Read version from dkms.conf (single source of truth)
PACKAGE_NAME=$(sed -n 's/^PACKAGE_NAME="\?\([^"]*\)"\?$/\1/p' dkms.conf)
PACKAGE_VERSION=$(sed -n 's/^PACKAGE_VERSION="\?\([^"]*\)"\?$/\1/p' dkms.conf)
[[ -n "${PACKAGE_NAME}" && -n "${PACKAGE_VERSION}" ]] || { echo "[ERROR] Failed to parse dkms.conf"; exit 1; }
SRC_DIR="/usr/src/${PACKAGE_NAME}-${PACKAGE_VERSION}"

HAS_CXL=false
if [[ -e /sys/firmware/acpi/tables/CEDT ]]; then
    HAS_CXL=true
    echo "[INFO] CEDT found – building **with** CXL support."
else
    echo "[INFO] CEDT not found – building **without** CXL (WO_CXL=1)."
fi

install_dkms() {
    echo "[INFO] Installing ${PACKAGE_NAME} ${PACKAGE_VERSION} via DKMS..."

    # Remove legacy-installed module to avoid DKMS diff warning
    for kdir in /lib/modules/*/updates; do
        rm -f "${kdir}"/mx_dma.ko* 2>/dev/null || true
    done

    # Remove previous DKMS registration if exists
    if dkms status "${PACKAGE_NAME}/${PACKAGE_VERSION}" 2>/dev/null | grep -q "${PACKAGE_NAME}"; then
        echo "[INFO] Removing previous DKMS registration..."
        dkms remove "${PACKAGE_NAME}/${PACKAGE_VERSION}" --all 2>/dev/null || true
    fi
    # Force-clean DKMS tree in case remove left stale entries
    rm -rf "/var/lib/dkms/${PACKAGE_NAME}" 2>/dev/null || true

    # Copy source to DKMS source tree (clean first to exclude build artifacts like mx_dma.mod.c)
    rm -rf "${SRC_DIR}"
    mkdir -p "${SRC_DIR}/scripts"
    make clean 2>/dev/null || true
    cp -a Makefile dkms.conf *.c *.h "${SRC_DIR}/"
    cp -a scripts/dkms-post-install.sh "${SRC_DIR}/scripts/"

    dkms add "${PACKAGE_NAME}/${PACKAGE_VERSION}"
    dkms build "${PACKAGE_NAME}/${PACKAGE_VERSION}"
    dkms install "${PACKAGE_NAME}/${PACKAGE_VERSION}" --force

    echo "[INFO] DKMS installation completed. Module will auto-rebuild on kernel upgrades."
}

install_legacy() {
    echo "[INFO] dkms not found – falling back to legacy install."

    MAKEVAR=""
    if [[ "$HAS_CXL" == "false" ]]; then
        MAKEVAR="WO_CXL=1"
    fi

    # shellcheck disable=SC2086
    make $MAKEVAR clean
    # shellcheck disable=SC2086
    make $MAKEVAR -j"$(nproc)" install

    depmod -a

    if command -v update-initramfs >/dev/null 2>&1; then
        echo "[INFO] update-initramfs found, updating initramfs..."
        update-initramfs -u -k "$(uname -r)"
    else
        echo "[INFO] update-initramfs not found, skipping initramfs update."
    fi
}

# Install module (prefer DKMS, fallback to legacy)
if command -v dkms >/dev/null 2>&1; then
    install_dkms
else
    install_legacy
fi

# Auto-load on boot
echo mx_dma | tee /etc/modules-load.d/mx_dma.conf

# CXL support: install udev rule and helper
if [[ "$HAS_CXL" == "true" ]]; then
    echo "[INFO] Installing xcena_set_devdax_perm for CXL support..."
    install -m 0755 config/xcena_set_devdax_perm /usr/local/sbin/xcena_set_devdax_perm
    install -m 0644 config/99-xcena_set_devdax_perm.rules /etc/udev/rules.d/99-xcena_set_devdax_perm.rules
    udevadm control --reload-rules
    echo "[INFO] xcena_set_devdax_perm installation completed."
fi
