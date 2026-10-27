#!/usr/bin/env python3
"""FW eviction pressure with no host folios: shrink the device DRAM cache, then PrefetchSync
through the upper part of the device. Usage (root): fw_pressure.py <GiB to move> [--dev N] [--dax daxN.M]
"""

import argparse
import os
import re
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from cli import gaia  # noqa: E402

import pyxif  # noqa: E402

MIB = 1 << 20
GIB = 1 << 30
CHUNK = 256 * MIB
SUB_SET_DRAM_CAPACITY = 1


def region_size(dax):
    # The dax sits under its CXL region in sysfs: .../regionN/dax_regionN/daxN.M.
    path = os.path.realpath(f"/sys/bus/dax/devices/{dax}")
    regions = [p for p in path.split("/") if re.fullmatch(r"region\d+", p)]
    assert len(regions) == 1, f"expected one CXL region above {path}, found {regions}"
    return int(open(f"/sys/bus/cxl/devices/{regions[0]}/size").read(), 16)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("gib", type=int)
    ap.add_argument("--dev", type=int, default=0)
    ap.add_argument("--dax", default="dax0.0")
    args = ap.parse_args()
    dev = args.dev
    total = args.gib * GIB
    lo, hi = 16 * GIB, region_size(args.dax) - 2 * GIB
    gaia.send(dev, SUB_SET_DRAM_CAPACITY, 4 * GIB)
    time.sleep(2)
    cur, moved, t0 = lo, 0, time.time()
    try:
        while moved < total:
            if cur + CHUNK > hi:
                cur = lo
            pyxif.memory_prefetch_sync(dev, cur, CHUNK)
            cur += CHUNK
            moved += CHUNK
    finally:
        gaia.send(dev, SUB_SET_DRAM_CAPACITY, 0)
    print(f"moved {moved // MIB} MiB in {time.time() - t0:.0f}s", flush=True)


if __name__ == "__main__":
    main()
