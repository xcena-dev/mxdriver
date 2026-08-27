#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0
set -euo pipefail

repo_dir=$(cd "$(dirname "$0")/.." && pwd)
stage_root=$(mktemp -d /tmp/mx-dkms-source-layout.XXXXXX)
trap 'rm -rf -- "$stage_root"' EXIT

stage_dir="$stage_root/source"
"$repo_dir/scripts/stage-dkms-source.sh" "$stage_dir"

test -f "$stage_dir/Makefile"
test -f "$stage_dir/dkms.conf"
test -f "$stage_dir/mx_dma.h"
test -f "$stage_dir/include/uapi/mx_dma_lease.h"
test -f "$stage_dir/scripts/dkms-post-install.sh"
cmp -s "$repo_dir/include/uapi/mx_dma_lease.h" \
    "$stage_dir/include/uapi/mx_dma_lease.h"

while IFS= read -r include_path; do
    if [ ! -f "$stage_dir/$include_path" ] &&
       [ ! -f "$stage_dir/include/uapi/$include_path" ]; then
        echo "staged DKMS source is missing quoted include: $include_path" >&2
        exit 1
    fi
done < <(
    rg -o --no-filename '#include "[^"]+"' "$stage_dir" \
        --glob '*.[ch]' |
        sed -E 's/^#include "([^"]+)"$/\1/' |
        sort -u
)

if find "$stage_dir" -type f \
    \( -name '*.o' -o -name '*.ko' -o -name '*.mod.c' \) | grep -q .; then
    echo "staged DKMS source contains build artifacts" >&2
    exit 1
fi

echo "DKMS source layout test: PASS"
