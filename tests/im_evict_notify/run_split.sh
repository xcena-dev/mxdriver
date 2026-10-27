#!/bin/bash
# With the driver's provider registered, a parked THP split by a partial
# munmap/mprotect/mremap must still be paged in; kernel log scan.
set -u
cd "$(dirname "$0")"
DAX=${DAX:-dax0.0}
DBG=/sys/kernel/debug
MXDEV=${MXDEV:-0}
IMCTL=/sys/class/mx_dma/mx_dma${MXDEV}_ioctl/im

uname -r
sudo grep -E "debug_locks" /proc/lockdep_stats 2>/dev/null || echo "lockdep: not enabled"
sudo test -e $DBG/im_soft_fault/quiesce || { echo "im_soft_fault test hooks missing"; exit 2; }

if ! daxctl list -d "$DAX" | grep -q '"mode":"system-ram"'; then
    # Where the kernel onlines hot-added memory itself (Ubuntu's default), daxctl needs --force
    # and the memory lands in the zone the kernel picks (ZONE_NORMAL) instead of ZONE_MOVABLE.
    force=""; [ "$(cat /sys/devices/system/memory/auto_online_blocks)" = offline ] || force=--force
    sudo daxctl reconfigure-device --mode=system-ram $force "$DAX" > /dev/null || exit 2
fi
node=$(daxctl list -d "$DAX" | sed -n 's/.*"target_node":\([0-9]*\).*/\1/p')
base=$(sudo cat /sys/bus/dax/devices/$DAX/resource)
size=$(sudo cat /sys/bus/dax/devices/$DAX/size)
echo "dax=$DAX node=$node base=$base size=$size"

echo 1 | sudo tee $IMCTL/enable > /dev/null || exit 2

bin=$(mktemp -d) || exit 2
trap 'rm -rf "$bin"' EXIT
gcc -O1 -pthread -Wall -o "$bin/im_split_test" im_split_test.c || exit 2

start=$(sudo dmesg | wc -l)
sudo MXDEV="$MXDEV" "$bin/im_split_test" "$node"
rc=$?
echo 0 | sudo tee $IMCTL/enable > /dev/null
sudo dmesg | tail -n +"$((start + 1))" > dmesg_split.log
bad=$(grep -c -E 'WARNING|BUG|lockdep|circular locking|sleeping function|Oops|RCU stall|hung_task' dmesg_split.log || true)
echo "kernel log lines during test: $(wc -l < dmesg_split.log), warnings: $bad"
sudo head -1 $DBG/im_soft_fault/status
[ "$bad" -eq 0 ] || rc=1
echo "EXIT=$rc"
exit "$rc"
