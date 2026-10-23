#!/bin/bash
# E2E run: IM dax as system-ram, kernel + mx_dma answer FW intents, kernel log scan.
set -u
cd "$(dirname "$0")"
DAX=${DAX:-dax0.0}

uname -r
sudo grep -E "debug_locks" /proc/lockdep_stats 2>/dev/null || echo "lockdep: not enabled"
if ! daxctl list -d "$DAX" | grep -q '"mode":"system-ram"'; then
    # Where the kernel onlines hot-added memory itself (Ubuntu's default), daxctl needs --force
    # and the memory lands in the zone the kernel picks (ZONE_NORMAL) instead of ZONE_MOVABLE.
    force=""; [ "$(cat /sys/devices/system/memory/auto_online_blocks)" = offline ] || force=--force
    sudo daxctl reconfigure-device --mode=system-ram $force "$DAX" > /dev/null || exit 2
fi

start=$(sudo dmesg | wc -l)
sudo python3 e2e_test.py "$@"
rc=$?
sudo dmesg | tail -n +"$((start + 1))" > dmesg_e2e.log
bad=$(grep -c -E 'WARNING|BUG|lockdep|circular locking|sleeping function|Oops|RCU stall|hung_task' dmesg_e2e.log || true)
echo "kernel log lines during test: $(wc -l < dmesg_e2e.log), warnings: $bad"
grep -E 'im_evict|im_soft_fault|mx_dma' dmesg_e2e.log | sort | uniq -c | sort -rn | head -8
sudo grep -E "debug_locks" /proc/lockdep_stats 2>/dev/null
sudo cat /sys/kernel/debug/mx_dma0_im/stats
sudo head -1 /sys/kernel/debug/im_soft_fault/status
[ "$bad" -eq 0 ] || rc=1
echo "EXIT=$rc"
exit "$rc"
