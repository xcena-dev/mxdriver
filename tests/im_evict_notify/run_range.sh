#!/bin/bash
# While the IM dax is still devdax: attach turned the FW notify off,
# and enable refuses a device whose memory is not onlined through kmem; kernel log scan.
set -u
cd "$(dirname "$0")"
DAX=${DAX:-dax0.0}
MXDEV=${MXDEV:-0}
IMCTL=/sys/class/mx_dma/mx_dma${MXDEV}_ioctl/im
passed=0
failed=0

check() {
    if [ "$2" -eq 0 ]; then
        echo "[PASS] $1${3:+ — $3}"
        passed=$((passed + 1))
    else
        echo "[FAIL] $1${3:+ — $3}"
        failed=$((failed + 1))
    fi
}

uname -r
sudo grep -E "debug_locks" /proc/lockdep_stats 2>/dev/null || echo "lockdep: not enabled"
test -e $IMCTL/enable || { echo "$IMCTL missing"; exit 2; }
daxctl list -d "$DAX" | grep -q '"mode":"devdax"' || { echo "$DAX must be devdax for this run"; exit 2; }

reset=$(sudo dmesg | grep -E "im_evict: mx_dma${MXDEV} firmware eviction notify reset" | tail -1)
[[ "$reset" == *"reset (0)" ]]
check "attach turned the FW eviction notify off" $? "${reset:-no log line}"

start=$(sudo dmesg | wc -l)
err=$(echo 1 | sudo tee $IMCTL/enable 2>&1 > /dev/null)
refused=$?
[ "$refused" -ne 0 ] && [ "$(cat $IMCTL/enable)" = 0 ] \
    && sudo grep -q '^enabled=0 ' /sys/kernel/debug/mx_dma${MXDEV}_im/stats
check "enable refuses a device with no kmem dax and stays off" $? "${err:-no error}"

sudo dmesg | tail -n +"$((start + 1))" > dmesg_range.log
grep -q "mx_dma${MXDEV} has no usable IM range (-19)" dmesg_range.log
check "the refusal names the missing range" $? "$(grep -m1 'im_evict' dmesg_range.log)"
bad=$(grep -c -E 'WARNING|BUG|lockdep|circular locking|sleeping function|Oops|RCU stall|hung_task' dmesg_range.log || true)
[ "$bad" -eq 0 ]
check "no kernel warnings from the in-kernel sysfs walk" $? "warnings=$bad"

echo
echo "SUMMARY: $passed/$((passed + failed)) passed"
[ "$failed" -eq 0 ]
rc=$?
echo "EXIT=$rc"
exit "$rc"
