#!/bin/bash

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "${SCRIPT_DIR}"

PACKAGE_NAME=$(sed -n 's/^PACKAGE_NAME="\?\([^"]*\)"\?$/\1/p' dkms.conf)
PACKAGE_VERSION=$(sed -n 's/^PACKAGE_VERSION="\?\([^"]*\)"\?$/\1/p' dkms.conf)
[[ -n "${PACKAGE_NAME}" && -n "${PACKAGE_VERSION}" ]] || { echo "[ERROR] Failed to parse dkms.conf"; exit 1; }
SRC_DIR="/usr/src/${PACKAGE_NAME}-${PACKAGE_VERSION}"

KVER="${XCENA_TARGET_KVER:-}"
KDIR="${XCENA_TARGET_KDIR:-}"
if [[ -n "$KDIR" ]]; then
    [[ -d "$KDIR" ]] || { echo "[ERROR] kernel build tree not found: ${KDIR}"; exit 1; }
    KDIR="$(cd "$KDIR" && pwd)"
    TREE_KVER="$(cat "$KDIR/include/config/kernel.release" 2>/dev/null || true)"
    [[ -n "$TREE_KVER" ]] || TREE_KVER="$(make -s -C "$KDIR" kernelrelease 2>/dev/null || true)"
    if [[ -z "$KVER" ]]; then
        KVER="$TREE_KVER"
        [[ -n "$KVER" ]] || { echo "[ERROR] cannot read the kernel release from '${KDIR}'"; exit 1; }
    elif [[ -z "$TREE_KVER" ]]; then
        echo "[WARN] cannot read the kernel release from '${KDIR}';"
        echo "       XCENA_TARGET_KVER='${KVER}' goes unchecked against that tree."
    elif [[ "$TREE_KVER" != "$KVER" ]]; then
        echo "[ERROR] XCENA_TARGET_KVER='${KVER}' disagrees with the tree at '${KDIR}'"
        echo "        (that tree builds '${TREE_KVER}'). Give one of them, or matching values."
        exit 1
    fi
fi
[[ -n "$KVER" ]] || KVER="$(uname -r)"
[[ -n "$KDIR" ]] || KDIR="/lib/modules/${KVER}/build"
if [[ ! -d "$KDIR" ]]; then
    echo "[ERROR] kernel build tree not found: ${KDIR}"
    echo "        install linux-headers-${KVER}, or set XCENA_TARGET_KDIR to the tree."
    exit 1
fi
echo "[INFO] target kernel: ${KVER} (build tree: ${KDIR})"

if [[ -n "${XCENA_TARGET_HAS_CXL:-}" ]]; then
    case "$XCENA_TARGET_HAS_CXL" in
        true|false) ;;
        *) echo "[ERROR] XCENA_TARGET_HAS_CXL must be 'true' or 'false', got '${XCENA_TARGET_HAS_CXL}'"; exit 1 ;;
    esac
    echo "[INFO] target CXL: ${XCENA_TARGET_HAS_CXL} (given)"
else
    if [[ -e /sys/firmware/acpi/tables/CEDT ]]; then
        XCENA_TARGET_HAS_CXL=true
        echo "[INFO] CEDT found – building **with** CXL support."
    else
        XCENA_TARGET_HAS_CXL=false
        echo "[INFO] CEDT not found – building **without** CXL (WO_CXL=1)."
    fi
    if [[ -n "${XCENA_TARGET_KVER:-}${XCENA_TARGET_KDIR:-}" ]]; then
        echo "[WARN] that table is this machine's, and the target is elsewhere."
        echo "       set XCENA_TARGET_HAS_CXL to state the target's hardware."
    fi
fi
export XCENA_TARGET_HAS_CXL

case "${XCENA_DEVICE_ACCESS:-}" in
    ""|open|group) ;;
    *) echo "[ERROR] XCENA_DEVICE_ACCESS must be 'open' or 'group', got '${XCENA_DEVICE_ACCESS}'"; exit 1 ;;
esac
DEVICE_GROUP="${XCENA_DEVICE_GROUP:-xcena}"
ACCESS_CONF=/etc/modprobe.d/mx_dma-access.conf
if [[ "${XCENA_DEVICE_ACCESS:-}" == group ]]; then
    getent group "$DEVICE_GROUP" >/dev/null 2>&1 || groupadd --system "$DEVICE_GROUP"
    ACCESS_GID=$(getent group "$DEVICE_GROUP" | cut -d: -f3) || true
    if [[ ! "$ACCESS_GID" =~ ^[1-9][0-9]*$ ]]; then
        echo "[ERROR] Group '${DEVICE_GROUP}' must resolve to a non-root gid, got '${ACCESS_GID}'"; exit 1
    fi
fi

case "${XCENA_DEVICE_ACCESS:-}" in
    group)
        echo "options mx_dma dev_gid=${ACCESS_GID} dev_mode=0660" > "$ACCESS_CONF"
        ;;
    open)
        rm -f "$ACCESS_CONF"
        ;;
esac

install_dkms() {
    echo "[INFO] Installing ${PACKAGE_NAME} ${PACKAGE_VERSION} via DKMS..."

    for kdir in /lib/modules/*/updates; do
        rm -f "${kdir}"/mx_dma.ko* 2>/dev/null || true
    done

    if dkms status "${PACKAGE_NAME}/${PACKAGE_VERSION}" 2>/dev/null | grep -q "${PACKAGE_NAME}"; then
        echo "[INFO] Removing previous DKMS registration..."
        dkms remove "${PACKAGE_NAME}/${PACKAGE_VERSION}" --all 2>/dev/null || true
    fi
    rm -rf "/var/lib/dkms/${PACKAGE_NAME}" 2>/dev/null || true

    rm -rf "${SRC_DIR}"
    make BUILDSYSTEM_DIR="$KDIR" clean 2>/dev/null || true
    ./scripts/stage-dkms-source.sh "${SRC_DIR}"

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
    if [[ "$XCENA_TARGET_HAS_CXL" == "false" ]]; then
        MAKEVAR="WO_CXL=1"
    fi

    # shellcheck disable=SC2086
    make $MAKEVAR BUILDSYSTEM_DIR="$KDIR" clean
    # shellcheck disable=SC2086
    make $MAKEVAR BUILDSYSTEM_DIR="$KDIR" -j"$(nproc)" install

    depmod -a "${KVER}"
    ./scripts/dkms-post-install.sh "${KVER}"
}

if command -v dkms >/dev/null 2>&1; then
    install_dkms
else
    install_legacy
fi

echo mx_dma | tee /etc/modules-load.d/mx_dma.conf

INITRAMFS_BACKEND=""

if [[ -f /etc/udev/rules.d/99-xcena_set_devdax_perm.rules \
      || -f /usr/local/sbin/xcena_set_devdax_perm ]]; then
    rm -f /etc/udev/rules.d/99-xcena_set_devdax_perm.rules
    rm -f /usr/local/sbin/xcena_set_devdax_perm
    udevadm control --reload-rules \
        || echo "[INFO] udevadm reload failed as above; rule change applies at boot."
    echo "[INFO] Removed obsolete xcena_set_devdax_perm helper."
fi

getent group xcena >/dev/null 2>&1 || groupadd --system xcena

if [[ -f /etc/udev/rules.d/60-xcena-mx_dma.rules ]]; then
    rm -f /etc/udev/rules.d/60-xcena-mx_dma.rules
    udevadm control --reload-rules \
        || echo "[INFO] udevadm reload failed as above; rule change applies at boot."
    if [[ -f "$ACCESS_CONF" ]]; then
        echo "[INFO] Removed the device-group udev rule; device nodes keep their current access until mx_dma reloads."
    else
        for node in /dev/mx_dma/mx_dma*; do
            [[ -c "$node" ]] || continue
            chown root:root "$node" && chmod 0666 "$node" \
                || echo "[WARN] Could not reopen ${node}; it reopens when the module reloads."
        done
        echo "[INFO] Removed the device-group udev rule; device nodes are open to all users."
    fi
fi

if [[ -f "$ACCESS_CONF" ]]; then
    access_gid=$(sed -n 's/.*dev_gid=\([0-9]\+\).*/\1/p' "$ACCESS_CONF" | tail -n 1)
    access_mode=$(sed -n 's/.*dev_mode=\([0-9]\+\).*/\1/p' "$ACCESS_CONF" | tail -n 1)
    access_group=$(getent group "$access_gid" | cut -d: -f1) || true
    echo "[INFO] Device access: restricted to gid ${access_gid}${access_group:+ (group '${access_group}')}, mode ${access_mode}."
else
    echo "[INFO] Device access: open to all users (0666)."
fi
if [[ -n "${XCENA_DEVICE_ACCESS:-}" ]]; then
    echo "[INFO] The access change applies when mx_dma next loads: reboot to apply it."
fi

if [[ "$XCENA_TARGET_HAS_CXL" == "true" ]]; then
    cat > /etc/modprobe.d/mx_dma-order.conf <<'EOF'
softdep cxl_pci pre: mx_dma
EOF

    if command -v update-initramfs >/dev/null 2>&1 && [[ -d /etc/initramfs-tools ]]; then
        grep -qx 'mx_dma' /etc/initramfs-tools/modules 2>/dev/null \
            || echo mx_dma >> /etc/initramfs-tools/modules
        INITRAMFS_BACKEND="initramfs-tools"
    elif command -v dracut >/dev/null 2>&1 && [[ -d /etc/dracut.conf.d ]]; then
        cat > /etc/dracut.conf.d/mx_dma.conf <<'EOF'
force_drivers+=" mx_dma "
install_optional_items+=" /etc/modprobe.d/mx_dma-access.conf "
EOF
        INITRAMFS_BACKEND="dracut"
    else
        echo "[INFO] No supported initramfs configuration path found, skipping regeneration."
    fi
fi

if [[ "$INITRAMFS_BACKEND" == "initramfs-tools" ]]; then
    if [[ -e "/boot/initrd.img-${KVER}" ]]; then
        echo "[INFO] Updating initramfs..."
        update-initramfs -u -k "${KVER}"
    else
        echo "[INFO] No initrd for ${KVER} to update, skipping regeneration."
    fi
elif [[ "$INITRAMFS_BACKEND" == "dracut" ]]; then
    echo "[INFO] Updating initramfs via dracut..."
    dracut --force --kver "${KVER}"
fi
