#!/bin/bash
# IM dax as system-ram, THP park/restore checks through the test hooks, kernel log scan.
set -u
cd "$(dirname "$0")"
DAX=${DAX:-dax0.0}

uname -r
sudo grep -E "debug_locks|lock-classes" /proc/lockdep_stats 2>/dev/null || echo "lockdep: not enabled"
test -d /sys/kernel/debug/im_soft_fault || sudo mount -t debugfs none /sys/kernel/debug 2>/dev/null
sudo test -e /sys/kernel/debug/im_soft_fault/status || { echo "im_soft_fault debugfs missing"; exit 2; }

if ! daxctl list -d "$DAX" | grep -q '"mode":"system-ram"'; then
    # Where the kernel onlines hot-added memory itself (Ubuntu's default), daxctl needs --force
    # and the memory lands in the zone the kernel picks (ZONE_NORMAL) instead of ZONE_MOVABLE.
    force=""; [ "$(cat /sys/devices/system/memory/auto_online_blocks)" = offline ] || force=--force
    sudo daxctl reconfigure-device --mode=system-ram $force "$DAX" || exit 2
fi
node=$(daxctl list -d "$DAX" | sed -n 's/.*"target_node":\([0-9]*\).*/\1/p')
mode=$(daxctl list -d "$DAX" | sed -n 's/.*"mode":"\([^"]*\)".*/\1/p')

echo "dax=$DAX mode=$mode node=$node"
cat /sys/kernel/mm/transparent_hugepage/enabled

bin=$(mktemp -d) || exit 2
trap 'rm -rf "$bin"' EXIT
gcc -O1 -pthread -Wall -o "$bin/im_mm_test" im_mm_test.c || exit 2

start=$(sudo dmesg | wc -l)
sudo "$bin/im_mm_test" "$node"
rc=$?
sudo dmesg | tail -n +"$((start + 1))" > dmesg_stage2.log
bad=$(grep -c -E 'WARNING|BUG|lockdep|circular locking|sleeping function|Oops|RCU stall|hung_task' dmesg_stage2.log || true)
echo "kernel log lines during test: $(wc -l < dmesg_stage2.log), warnings: $bad"
sudo grep -E "debug_locks" /proc/lockdep_stats 2>/dev/null
sudo cat /sys/kernel/debug/im_soft_fault/status
[ "$bad" -eq 0 ] || rc=1
echo "EXIT=$rc"
exit "$rc"
