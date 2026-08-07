#!/bin/bash

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "${SCRIPT_DIR}"

# Read version from dkms.conf (single source of truth)
PACKAGE_NAME=$(sed -n 's/^PACKAGE_NAME="\?\([^"]*\)"\?$/\1/p' dkms.conf)
PACKAGE_VERSION=$(sed -n 's/^PACKAGE_VERSION="\?\([^"]*\)"\?$/\1/p' dkms.conf)
[[ -n "${PACKAGE_NAME}" && -n "${PACKAGE_VERSION}" ]] || { echo "[ERROR] Failed to parse dkms.conf"; exit 1; }
SRC_DIR="/usr/src/${PACKAGE_NAME}-${PACKAGE_VERSION}"

# Kernel to build for. Installing onto this machine needs neither input below; a
# caller installing for a different kernel (image build, or a locally built tree)
# gives one of them:
#
#   XCENA_TARGET_KDIR  kernel build tree. Default /lib/modules/<version>/build,
#                      the path DKMS and the distro header packages use.
#   XCENA_TARGET_KVER  kernel version. Names the DKMS registration, the install
#                      path, depmod and initramfs. Read from the tree when only
#                      XCENA_TARGET_KDIR is given.
#
# Giving both is allowed, but they have to agree: the module is built from the
# tree and registered under the version, so a mismatch installs a module that
# cannot load (DKMS), or installs it into a directory depmod never indexes
# (legacy path) -- both silently.
KVER="${XCENA_TARGET_KVER:-}"
KDIR="${XCENA_TARGET_KDIR:-}"
if [[ -n "$KDIR" ]]; then
    TREE_KVER="$(make -s -C "$KDIR" kernelrelease 2>/dev/null || true)"
    if [[ -z "$KVER" ]]; then
        KVER="$TREE_KVER"
        [[ -n "$KVER" ]] || { echo "[ERROR] cannot read kernelrelease from '${KDIR}'"; exit 1; }
    elif [[ -n "$TREE_KVER" && "$TREE_KVER" != "$KVER" ]]; then
        echo "[ERROR] XCENA_TARGET_KVER='${KVER}' disagrees with the tree at '${KDIR}'"
        echo "        (that tree builds '${TREE_KVER}'). Give one of them, or matching values."
        exit 1
    fi
fi
[[ -n "$KVER" ]] || KVER="$(uname -r)"
[[ -n "$KDIR" ]] || KDIR="/lib/modules/${KVER}/build"
if [[ ! -e "$KDIR" ]]; then
    echo "[ERROR] kernel build tree not found: ${KDIR}"
    echo "        install linux-headers-${KVER}, or set XCENA_TARGET_KDIR to the tree."
    exit 1
fi
echo "[INFO] target kernel: ${KVER} (build tree: ${KDIR})"

# CXL support is the build default; WO_CXL=1 is the opt-out for a machine that
# has no CXL. The evidence for opting out is this machine's ACPI table, so it
# only speaks for the machine running the installer. Installing for another
# kernel means the target is elsewhere and its tables are not visible here, so
# the default stands -- reading the builder's table there would put a driver in
# the image chosen by whichever machine happened to build it.
HAS_CXL=true
if [[ "$KVER" != "$(uname -r)" ]]; then
    echo "[INFO] installing for another kernel – building **with** CXL support (this machine's ACPI tables do not describe the target)."
elif [[ -e /sys/firmware/acpi/tables/CEDT ]]; then
    echo "[INFO] CEDT found – building **with** CXL support."
else
    HAS_CXL=false
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

    # Copy source to DKMS source tree (clean first to exclude build artifacts like
    # mx_dma.mod.c). The clean needs the target kernel's build tree too: with the
    # default it runs against the running kernel, which during an image build is a
    # kernel absent from this root, so the clean silently does nothing and the
    # artifacts get copied.
    rm -rf "${SRC_DIR}"
    mkdir -p "${SRC_DIR}/scripts"
    make BUILDSYSTEM_DIR="$KDIR" clean 2>/dev/null || true
    cp -a Makefile dkms.conf *.c *.h "${SRC_DIR}/"
    cp -a scripts/dkms-post-install.sh "${SRC_DIR}/scripts/"

    # DKMS resolves the tree as /lib/modules/<kver>/build on its own, so only a
    # tree outside that path has to be spelled out.
    local dkms_src=()
    [[ "$KDIR" != "/lib/modules/${KVER}/build" ]] && dkms_src=(--kernelsourcedir "$KDIR")

    dkms add "${PACKAGE_NAME}/${PACKAGE_VERSION}"
    dkms build "${PACKAGE_NAME}/${PACKAGE_VERSION}" -k "${KVER}" ${dkms_src[@]+"${dkms_src[@]}"}
    dkms install "${PACKAGE_NAME}/${PACKAGE_VERSION}" -k "${KVER}" ${dkms_src[@]+"${dkms_src[@]}"} --force

    echo "[INFO] DKMS installation completed. Module will auto-rebuild on kernel upgrades."
}

install_legacy() {
    echo "[INFO] dkms not found – falling back to legacy install."

    MAKEVAR=""
    if [[ "$HAS_CXL" == "false" ]]; then
        MAKEVAR="WO_CXL=1"
    fi

    # shellcheck disable=SC2086
    make $MAKEVAR BUILDSYSTEM_DIR="$KDIR" clean
    # shellcheck disable=SC2086
    make $MAKEVAR BUILDSYSTEM_DIR="$KDIR" -j"$(nproc)" install

    depmod -a "${KVER}"
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

# Remove the obsolete devdax permission helper left by older installs. The usual
# upgrade path re-runs install.sh (not uninstall.sh), so without this the stale
# udev rule survives and keeps re-applying 0666 to /dev/dax*.
if [[ -f /etc/udev/rules.d/99-xcena_set_devdax_perm.rules \
      || -f /usr/local/sbin/xcena_set_devdax_perm ]]; then
    rm -f /etc/udev/rules.d/99-xcena_set_devdax_perm.rules
    rm -f /usr/local/sbin/xcena_set_devdax_perm
    # Best-effort: udevd is not running during an image build, and what a booted
    # system reads is the rule file, which is already gone by this point.
    udevadm control --reload-rules 2>/dev/null \
        || echo "[INFO] udevd not running; rule change applies at boot."
    echo "[INFO] Removed obsolete xcena_set_devdax_perm helper."
fi

# CXL-only setup. WO_CXL=1 builds use pci_register_driver and do not depend on
# the PCI bus_notifier ordering, so none of the softdep / initramfs bits
# below apply there.
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
fi

# Regenerate initramfs once at the end so it picks up softdep ordering and,
# where configured, the bundled mx_dma module.
if [[ "$INITRAMFS_BACKEND" == "initramfs-tools" ]]; then
    echo "[INFO] Updating initramfs..."
    update-initramfs -u -k "${KVER}"
elif [[ "$INITRAMFS_BACKEND" == "dracut" ]]; then
    echo "[INFO] Updating initramfs via dracut..."
    dracut --force --kver "${KVER}"
fi
