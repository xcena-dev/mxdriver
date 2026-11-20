#!/bin/bash

kernelver="${1:-$(uname -r)}"

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

exit 0
