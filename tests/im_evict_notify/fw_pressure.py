#!/usr/bin/env python3
"""FW eviction pressure with no host folios: shrink the device DRAM cache, then PrefetchSync
through the upper part of the device. Usage (root, emulator guest): fw_pressure.py <GiB to move>
"""

import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from cli import gaia  # noqa: E402

import pyxif  # noqa: E402

MIB = 1 << 20
GIB = 1 << 30
CHUNK = 256 * MIB
SUB_SET_DRAM_CAPACITY = 1


def region_size():
    path = "/sys/bus/cxl/devices/region0/size"
    return int(open(path).read(), 16)


def main():
    total = int(sys.argv[1]) * GIB
    lo, hi = 16 * GIB, region_size() - 2 * GIB
    gaia.send(0, SUB_SET_DRAM_CAPACITY, 4 * GIB)
    time.sleep(2)
    cur, moved, t0 = lo, 0, time.time()
    try:
        while moved < total:
            if cur + CHUNK > hi:
                cur = lo
            pyxif.memory_prefetch_sync(0, cur, CHUNK)
            cur += CHUNK
            moved += CHUNK
    finally:
        gaia.send(0, SUB_SET_DRAM_CAPACITY, 0)
    print(f"moved {moved // MIB} MiB in {time.time() - t0:.0f}s", flush=True)


if __name__ == "__main__":
    main()
