#!/bin/bash
# DKMS POST_INSTALL hook — update initramfs for the target kernel

kernelver="${1:-$(uname -r)}"

if command -v update-initramfs >/dev/null 2>&1; then
    update-initramfs -u -k "${kernelver}" || echo "[WARN] initramfs update failed (non-fatal)"
elif command -v dracut >/dev/null 2>&1; then
    dracut --force --kver "${kernelver}" || echo "[WARN] initramfs update failed (non-fatal)"
fi

exit 0
