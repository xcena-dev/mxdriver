#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0
set -euo pipefail

if [ "$#" -ne 1 ] || [ -z "$1" ] || [ "$1" = "/" ]; then
    echo "usage: $0 <destination>" >&2
    exit 2
fi

repo_dir=$(cd "$(dirname "$0")/.." && pwd)
destination=$1

mkdir -p "$destination/scripts" "$destination/include/uapi"
source_files=("$repo_dir/Makefile" "$repo_dir/dkms.conf")
shopt -s nullglob
for source_file in "$repo_dir"/*.c "$repo_dir"/*.h; do
    [[ "$source_file" == *.mod.c ]] && continue
    source_files+=("$source_file")
done
cp -a "${source_files[@]}" "$destination/"
cp -a "$repo_dir"/include/uapi/. "$destination/include/uapi/"
cp -a "$repo_dir"/scripts/dkms-post-install.sh "$destination/scripts/"
