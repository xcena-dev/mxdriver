#!/bin/bash
# Runs <dir>/<name>.session for each name, turning IM off and on after each one (root),
# so the parks an exited session left behind are gone before the next one loads.
# t3_*d: readers on 64-107, IM on 4 CPUs of its own; t3_*h: readers on 64-111, IM on 4 of them.
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
L=$1
shift
MXDEV=${MXDEV:-0}
IMCTL=/sys/class/mx_dma/mx_dma${MXDEV}_ioctl/im/enable
for name in "$@"; do
    case $name in
    t3_*d) r=64-107 i=126,127,254,255 ;;
    t3_*h) r=64-111 i=108,109,110,111 ;;
    *) r=64-125,192-253 i=126,127,254,255 ;;
    esac
    echo "$(date +%T) start $name readers=$r im=$i" >> "$L/chain.log"
    sync
    MXDEV=$MXDEV READER_CPUS=$r IM_CPUS=$i bash "$HERE/perf_run.sh" "$L/$name.session" "$L/$name.log"
    echo "$(date +%T) end $name $(tail -1 "$L/$name.log")" >> "$L/chain.log"
    t0=$(date +%s.%N)
    echo 0 > "$IMCTL"
    echo 1 > "$IMCTL"
    echo "$(date +%T) im cycled in $(echo "$(date +%s.%N) - $t0" | bc) s" >> "$L/chain.log"
    sync
done
