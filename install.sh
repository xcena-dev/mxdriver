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
}

# Install module (prefer DKMS, fallback to legacy)
if command -v dkms >/dev/null 2>&1; then
    install_dkms
else
    install_legacy
fi

# Auto-load on boot
echo mx_dma | tee /etc/modules-load.d/mx_dma.conf

INITRAMFS_BACKEND=""

# CXL-only setup. WO_CXL=1 builds use pci_register_driver and do not depend on
# the PCI bus_notifier ordering, so none of the softdep / initramfs / udev
# bits below apply there.
if [[ "$HAS_CXL" == "true" ]]; then
    # Reverse softdep: cxl_pci modalias path must pull mx_dma in first so the
    # PCI bus notifier is registered before cxl_pci binds XCENA devices.
    # Complements MODULE_SOFTDEP("post: cxl_pci") in the driver, which only
    # covers the "modprobe mx_dma first" entry path.
    cat > /etc/modprobe.d/mx_dma-order.conf <<'EOF'
softdep cxl_pci pre: mx_dma
EOF

    # Bundle mx_dma into initramfs when cxl_pci may also live there
    # (CXL-on-boot configurations); otherwise cxl_pci can bind inside
    # initramfs before mx_dma ever loads, and the notifier registered later
    # misses BOUND_DRIVER.
    if command -v update-initramfs >/dev/null 2>&1 && [[ -d /etc/initramfs-tools ]]; then
        grep -qx 'mx_dma' /etc/initramfs-tools/modules 2>/dev/null \
            || echo mx_dma >> /etc/initramfs-tools/modules
        INITRAMFS_BACKEND="initramfs-tools"
    elif command -v dracut >/dev/null 2>&1 && [[ -d /etc/dracut.conf.d ]]; then
        cat > /etc/dracut.conf.d/mx_dma.conf <<'EOF'
force_drivers+=" mx_dma "
EOF
        INITRAMFS_BACKEND="dracut"
    else
        echo "[INFO] No supported initramfs configuration path found, skipping regeneration."
    fi

    echo "[INFO] Installing xcena_set_devdax_perm for CXL support..."
    install -m 0755 config/xcena_set_devdax_perm /usr/local/sbin/xcena_set_devdax_perm
    install -m 0644 config/99-xcena_set_devdax_perm.rules /etc/udev/rules.d/99-xcena_set_devdax_perm.rules
    udevadm control --reload-rules
    echo "[INFO] xcena_set_devdax_perm installation completed."
fi

# Regenerate initramfs once at the end so it picks up softdep ordering and,
# where configured, the bundled mx_dma module.
if [[ "$INITRAMFS_BACKEND" == "initramfs-tools" ]]; then
    echo "[INFO] Updating initramfs..."
    update-initramfs -u -k "$(uname -r)"
elif [[ "$INITRAMFS_BACKEND" == "dracut" ]]; then
    echo "[INFO] Updating initramfs via dracut..."
    dracut --force --kver "$(uname -r)"
fi
