#!/bin/bash
# Runs an appbench kv-session over IM (root): the IM kthreads (poller, page-in workqueue) on IM_CPUS,
# the readers on READER_CPUS. Usage: perf_run.sh <session file> <output log>
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
export MXDEV=${MXDEV:-0}
export IM_CPUS=${IM_CPUS:-126,127,254,255}
READER_CPUS=${READER_CPUS:-64-125,192-253}
WQ=/sys/devices/virtual/workqueue/cpumask

old=$(cat $WQ)
mask=$(python3 -c "
import sys
cpus = set()
for p in sys.argv[1].split(','):
    lo, _, hi = p.partition('-')
    cpus.update(range(int(lo), int(hi or lo) + 1))
v = sum(1 << c for c in cpus)
words = []
while v or not words:
    words.append('%08x' % (v & 0xffffffff))
    v >>= 32
print(','.join(reversed(words)))
" "$IM_CPUS")
echo "$mask" > $WQ
trap 'echo "$old" > $WQ' EXIT
echo "unbound workqueues on $(cat $WQ), readers on $READER_CPUS" | tee "$2"
cat /sys/class/mx_dma/mx_dma${MXDEV}_ioctl/im/ack_timeout_ms /sys/module/mx_dma/parameters/timeout_ms \
    /sys/module/mx_dma/parameters/im_cache_backdoor | tr '\n' ' ' | sed 's/^/ack_timeout_ms timeout_ms backdoor: /' >> "$2"
echo >> "$2"
rc=0
taskset -c "$READER_CPUS" "$HERE/appbench" kv-session < "$1" >> "$2" 2>&1 || rc=$?
echo "EXIT=$rc" >> "$2"
