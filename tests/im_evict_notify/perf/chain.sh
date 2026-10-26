#!/bin/bash
# Runs session files one after another once no perf_run.sh is running (root).
# Usage: chain.sh <dir> <name>[:<reader cpus>]...  runs <dir>/<name>.session into <dir>/<name>.log
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
L=$1
shift
while pgrep -f "perf_run.sh" > /dev/null; do sleep 5; done
for n in "$@"; do
    name=${n%%:*}
    cpus=${n#*:}
    [ "$cpus" = "$n" ] && cpus=${READER_CPUS:-64-125,192-253}
    echo "$(date +%T) start $name on $cpus" >> "$L/chain.log"
    sync
    READER_CPUS=$cpus bash "$HERE/perf_run.sh" "$L/$name.session" "$L/$name.log"
    echo "$(date +%T) end $name $(tail -1 "$L/$name.log")" >> "$L/chain.log"
    sync
done
