#!/bin/bash
# Sets the anon THP size an IM arm runs on under MADV_HUGEPAGE (root):
# 2m for 2 MiB THPs, 512k for 512 KiB mTHPs.
set -eu
T=/sys/kernel/mm/transparent_hugepage
case ${1:-} in
2m) echo inherit > $T/hugepages-2048kB/enabled; echo never > $T/hugepages-512kB/enabled ;;
512k) echo never > $T/hugepages-2048kB/enabled; echo madvise > $T/hugepages-512kB/enabled ;;
*) echo "usage: $0 2m|512k" >&2; exit 2 ;;
esac
echo "APPNOTE thp 2048kB=$(grep -o '\[[a-z]*\]' $T/hugepages-2048kB/enabled) 512kB=$(grep -o '\[[a-z]*\]' $T/hugepages-512kB/enabled)"
