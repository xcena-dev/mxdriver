#!/usr/bin/env python3
"""Rates and gauges of the firmware's relocator, storage and router counters over a window (root).
Usage: fwrate.py [dev] [seconds]  -- counters print per second, gauges (Page/Ost/Size/Outstanding) as the
current sum, and the storage NVMe read/write latency mean and buckets over the window."""

import json
import os
import struct
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, ".."))
from cli import gaia  # noqa: E402

SUB_GET_STAT = 5
GAUGE = {
    "FreePage",
    "DirtyPage",
    "ValidPage",
    "PendingPage",
    "PinnedPage",
    "PreloadedPage",
    "TotalPage",
    "ReservedPage",
    "EvictionOst",
    "WriteOst",
    "ReadOst",
    "DevicePrefetchOst",
    "MainQueueSize",
    "BackgroundQueueSize",
    "NvmeOutstanding",
    "TrackerOutstanding",
}


def snap(dev, layouts):
    payload, _ = gaia.recv_var(dev, SUB_GET_STAT, 2 << 20)
    ver, _ = struct.unpack_from("<II", payload, 0)
    lay = layouts[ver]
    out = {}
    for ip in ("relocator", "storage", "router"):
        if ip not in lay["ips"]:
            continue
        idx = lay["ip_order"].index(ip)
        _, per, inst, off = struct.unpack_from("<IIII", payload, 8 + idx * 16)
        for f in lay["ips"][ip]:
            v = 0
            for i in range(inst):
                b = off + i * per + f["offset"]
                v += int.from_bytes(payload[b : b + f["size"]], "little")
            out[f"{ip}.{f['name']}"] = v
    return out


def enable_all_groups(dev):
    # SetStatEnable (System sub-opcode 6): value = groupMask | ipIndex << 32 | enable << 40.
    for ip in (0, 1, 3):
        gaia.send(dev, 6, 0xFFFFFFFF | ip << 32 | 1 << 40)


def main():
    dev = int(sys.argv[1]) if len(sys.argv) > 1 else 0
    if os.environ.get("FWRATE_ENABLE_ALL") == "1":
        enable_all_groups(dev)
    secs = float(sys.argv[2]) if len(sys.argv) > 2 else 5
    layouts = {
        int(k): v
        for k, v in json.load(
            open(os.path.join(HERE, "..", "cli", "gaia_stat_layout.json"))
        )["versions"].items()
    }
    a = snap(dev, layouts)
    time.sleep(secs)
    b = snap(dev, layouts)
    for k in sorted(b):
        name = k.split(".", 1)[1]
        if "LatencyBucket" in name:
            continue
        gauge = name in GAUGE or name.endswith(
            (
                "Max",
                "Peak",
                "Enable",
                "Threshold",
                "ThresholdByte",
                "Capacity",
                "GroupCount",
                "ThresholdPercent",
                "MaxTicks",
            )
        )
        if gauge:
            print(f"{k:45s} now {b[k]}")
        elif b[k] != a[k]:
            print(f"{k:45s} {(b[k] - a[k]) / secs:14.1f} /s")
    for kind in ("Read", "Write"):
        n = b.get(f"storage.Nvme{kind}Commands", 0) - a.get(
            f"storage.Nvme{kind}Commands", 0
        )
        ticks = b.get(f"storage.Nvme{kind}LatencySumTicks", 0) - a.get(
            f"storage.Nvme{kind}LatencySumTicks", 0
        )
        buckets = [
            b.get(f"storage.Nvme{kind}LatencyBucket{i}", 0)
            - a.get(f"storage.Nvme{kind}LatencyBucket{i}", 0)
            for i in range(16)
        ]
        print(
            f"NVMe {kind}: {n / secs:.0f} cmd/s, mean {ticks / n if n else 0:.0f} ticks, buckets {buckets}"
        )


if __name__ == "__main__":
    main()
