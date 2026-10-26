#!/usr/bin/env python3
"""Every few seconds, append the relocator counters and the IM node's memory use to a log and fsync it,
so the state right before a host stall survives a reset. Usage (root): devstat.py <log> [node] [period]"""

import json
import os
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, ".."))
import evict_notify_test as ent  # noqa: E402

DEV = int(os.environ.get("MXDEV", "0"))
KEYS = (
    "FreePage",
    "MinFreePage",
    "PendingPage",
    "EvictionTrigger",
    "EvictionDone",
    "SsdWrite",
    "SsdRead",
    "Miss",
)


def node_kb(node, field):
    for line in open(f"/sys/devices/system/node/node{node}/meminfo"):
        if f" {field}:" in line:
            return int(line.split()[-2])
    return None


def main():
    path, node = sys.argv[1], int(sys.argv[2]) if len(sys.argv) > 2 else 5
    period = float(sys.argv[3]) if len(sys.argv) > 3 else 5.0
    stats = ent.Stats(DEV)
    fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_APPEND, 0o644)
    while True:
        rel = stats.relocator()
        row = {
            "t": round(time.time(), 1),
            **{k: rel[k] for k in KEYS},
            "anon_thp_gib": round(node_kb(node, "AnonHugePages") / 2**20, 1),
            "free_gib": round(node_kb(node, "MemFree") / 2**20, 1),
        }
        os.write(fd, (json.dumps(row) + "\n").encode())
        os.fsync(fd)
        time.sleep(period)


if __name__ == "__main__":
    main()
