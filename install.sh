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
#
# sudo's env_reset drops all three variables, so hand them to sudo rather than to
# the shell that calls it: `sudo XCENA_TARGET_KVER=... ./install.sh`.
KVER="${XCENA_TARGET_KVER:-}"
KDIR="${XCENA_TARGET_KDIR:-}"
if [[ -n "$KDIR" ]]; then
    [[ -d "$KDIR" ]] || { echo "[ERROR] kernel build tree not found: ${KDIR}"; exit 1; }
    # Absolute from here on: dkms cds into its own build tree before running
    # make -C "$kernel_source_dir", where a relative path resolves elsewhere.
    KDIR="$(cd "$KDIR" && pwd)"
    # A header tree's kernelrelease target recomputes the version from the
    # Makefile and loses the distro suffix (6.8.0-117-generic reads back as
    # 6.8.12); include/config/kernel.release holds what the tree really builds.
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

# Whether the target has CXL, which selects WO_CXL. Told the same way as the
# kernel above:
#
#   XCENA_TARGET_HAS_CXL  true | false. Given by a caller installing elsewhere,
#                         which is the only side that knows the target's
#                         hardware. Without it the installer reads this
#                         machine's ACPI table, which is the answer whenever it
#                         runs on the target.
#
# Resolved once here and exported, because dkms.conf reads the same variable when
# it assembles the make arguments.
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
    # The table just read belongs to this machine. A caller who named a target
    # kernel is installing for another machine, so it is not the target's table --
    # and either answer above is then a guess about the wrong hardware.
    #
    # Ask whether a target was named rather than comparing $KVER against
    # $(uname -r): an image is assembled under chroot/nspawn, neither of which
    # changes what uname reports, so that comparison goes quiet whenever the two
    # kernel releases happen to coincide.
    if [[ -n "${XCENA_TARGET_KVER:-}${XCENA_TARGET_KDIR:-}" ]]; then
        echo "[WARN] that table is this machine's, and the target is elsewhere."
        echo "       set XCENA_TARGET_HAS_CXL to state the target's hardware."
    fi
fi
export XCENA_TARGET_HAS_CXL

# XCENA_DEVICE_ACCESS=open|group sets the device-node access the module applies at load;
# unset keeps the access conf an earlier run wrote, so a re-install never lifts that restriction.
# XCENA_DEVICE_GROUP names the group for 'group' (default xcena).
case "${XCENA_DEVICE_ACCESS:-}" in
    ""|open|group) ;;
    *) echo "[ERROR] XCENA_DEVICE_ACCESS must be 'open' or 'group', got '${XCENA_DEVICE_ACCESS}'"; exit 1 ;;
esac
DEVICE_GROUP="${XCENA_DEVICE_GROUP:-xcena}"
ACCESS_CONF=/etc/modprobe.d/mx_dma-access.conf
if [[ "${XCENA_DEVICE_ACCESS:-}" == group ]]; then
    getent group "$DEVICE_GROUP" >/dev/null 2>&1 || groupadd --system "$DEVICE_GROUP"
    ACCESS_GID=$(getent group "$DEVICE_GROUP" | cut -d: -f3) || true
    # gid 0 would hand the devices to the root group, and users joining the access group would join root.
    if [[ ! "$ACCESS_GID" =~ ^[1-9][0-9]*$ ]]; then
        echo "[ERROR] Group '${DEVICE_GROUP}' must resolve to a non-root gid, got '${ACCESS_GID}'"; exit 1
    fi
fi

# Before the DKMS install: its post-install hook rebuilds the initramfs, which may load mx_dma.
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
    make BUILDSYSTEM_DIR="$KDIR" clean 2>/dev/null || true
    ./scripts/stage-dkms-source.sh "${SRC_DIR}"

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
    if [[ "$XCENA_TARGET_HAS_CXL" == "false" ]]; then
        MAKEVAR="WO_CXL=1"
    fi

    # shellcheck disable=SC2086
    make $MAKEVAR BUILDSYSTEM_DIR="$KDIR" clean
    # shellcheck disable=SC2086
    make $MAKEVAR BUILDSYSTEM_DIR="$KDIR" -j"$(nproc)" install

    depmod -a "${KVER}"
    # The initramfs may load mx_dma, so rebuild it as the DKMS install does,
    # which also carries the access conf and this module into it.
    ./scripts/dkms-post-install.sh "${KVER}"
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
    udevadm control --reload-rules \
        || echo "[INFO] udevadm reload failed as above; rule change applies at boot."
    echo "[INFO] Removed obsolete xcena_set_devdax_perm helper."
fi

# Earlier builds created this group, and users were told to join it or pass its GID to containers;
# keep creating it so those setups still work on a new host.
getent group xcena >/dev/null 2>&1 || groupadd --system xcena

# Remove the device-group udev rule that earlier builds installed.
# Nodes it restricted keep that group and mode until the module reloads.
# Only an open install reopens them now.
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
    # modprobe applies every options line in order, so the last value is the one the module gets.
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

# CXL-only setup. WO_CXL=1 builds use pci_register_driver and do not depend on
# the PCI bus_notifier ordering, so none of the softdep / initramfs bits
# below apply there.
if [[ "$XCENA_TARGET_HAS_CXL" == "true" ]]; then
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
        # Only a hostonly image carries /etc/modprobe.d, so name the access conf for the rest.
        cat > /etc/dracut.conf.d/mx_dma.conf <<'EOF'
force_drivers+=" mx_dma "
install_optional_items+=" /etc/modprobe.d/mx_dma-access.conf "
EOF
        INITRAMFS_BACKEND="dracut"
    else
        echo "[INFO] No supported initramfs configuration path found, skipping regeneration."
    fi
fi

# Regenerate initramfs once at the end so it picks up softdep ordering and,
# where configured, the bundled mx_dma module.
#
# -u updates an image that already exists and fails without one, which a target
# kernel staged into an image has not got. dracut creates one either way, so
# only the initramfs-tools path needs the image to be there first.
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
