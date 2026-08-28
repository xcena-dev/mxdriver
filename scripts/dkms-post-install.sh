#!/bin/bash
# DKMS POST_INSTALL hook — update initramfs for the target kernel

kernelver="${1:-$(uname -r)}"

# -u fails without an image to update, which a target kernel staged into an
# image has not got. Split from a failed regeneration, which is a real failure
# and reports as one. install.sh gates the same call the same way.
if command -v update-initramfs >/dev/null 2>&1; then
    if [[ -e "/boot/initrd.img-${kernelver}" ]]; then
        update-initramfs -u -k "${kernelver}" \
            || echo "[WARN] initramfs update for ${kernelver} failed as above; the module is installed."
    else
        echo "[INFO] No initrd for ${kernelver} to update, skipping regeneration."
    fi
elif command -v dracut >/dev/null 2>&1; then
    dracut --force --kver "${kernelver}" \
        || echo "[WARN] initramfs update for ${kernelver} failed as above; the module is installed."
fi

# Never fail the DKMS install: this also runs from the kernel-upgrade
# autoinstall, where aborting would fail the package transaction.
exit 0
