#!/usr/bin/env python3
"""E2E check: FW eviction intent -> host park -> SSD -> fault -> page-in -> restore.
Run as root with the IM dax onlined as system-ram (see run_e2e.sh): in the emulator guest as is,
on real hardware with --hw.
"""

import argparse
import atexit
import json
import os
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from cli import gaia  # noqa: E402

import pyxif  # noqa: E402

MIB = 1 << 20
GIB = 1 << 30
CHUNK = 256 * MIB
SUB_SET_DRAM_CAPACITY = 1
SUB_GET_STAT = 5
SUB_GET_EVICT_EVENTS = 12
IMSF = "/sys/kernel/debug/im_soft_fault/"
MXDMA_TIMEOUT = "/sys/module/mx_dma/parameters/timeout_ms"
MXDMA_CACHE_BACKDOOR = "/sys/module/mx_dma/parameters/im_cache_backdoor"
ACK_TIMEOUT_MAX_MS = 3600 * 1000
HERE = os.path.dirname(os.path.abspath(__file__))

_ap = argparse.ArgumentParser()
_ap.add_argument("--dev", type=int, default=0, help="mx_dma and pyxif device index")
_ap.add_argument(
    "--dax", default="dax0.0", help="that device's dax, onlined as system-ram"
)
_ap.add_argument(
    "--hw", action="store_true", help="real hardware: no QEMU cache backdoor"
)
_ap.add_argument("--numa-balancing", action="store_true")
_ap.add_argument("--ack-timeout-ms", type=int)
ARGS = _ap.parse_args()
DEV = ARGS.dev
MXIM = f"/sys/kernel/debug/mx_dma{DEV}_im/"
IMCTL = f"/sys/class/mx_dma/mx_dma{DEV}_ioctl/im/"
DAX = f"/sys/bus/dax/devices/{ARGS.dax}/"
NUMA_BALANCING = ARGS.numa_balancing
# The emulator's cache model takes seconds to write a 2 MiB folio back; real CPUs take far less.
ACK_TIMEOUT_MS = ARGS.ack_timeout_ms or (5000 if ARGS.hw else 60000)
REL_FIELDS = (
    "Hit",
    "Miss",
    "SsdRead",
    "SsdWrite",
    "EvictionTrigger",
    "EvictionDone",
    "FreePage",
    "PendingPage",
)

results = []


def check(name, ok, detail=""):
    results.append((name, bool(ok)))
    print(
        f"[{'PASS' if ok else 'FAIL'}] {name}" + (f" — {detail}" if detail else ""),
        flush=True,
    )


def write(path, value):
    with open(path, "w") as f:
        f.write(str(value))


def try_write(path, value):
    try:
        write(path, value)
        return 0
    except OSError as e:
        return e.errno


def kv(path):
    out = {}
    for tok in open(path).read().split():
        if "=" in tok:
            k, v = tok.split("=", 1)
            out[k] = v
    return out


def imsf_status():
    # Only the first line is the global count; each parked folio adds a line reusing faults=.
    return dict(
        t.split("=", 1) for t in open(IMSF + "status").readline().split() if "=" in t
    )


def delta(after, before, key):
    return int(after[key]) - int(before[key])


def relocator_stats(layouts):
    payload, _ = gaia.recv_var(DEV, SUB_GET_STAT, 2 * MIB)
    version, _ = struct.unpack_from("<II", payload, 0)
    layout = layouts[version]
    ip = layout["ip_order"].index("relocator")
    _, per_slot, instances, data_off = struct.unpack_from("<IIII", payload, 8 + ip * 16)
    fields = {f["name"]: f for f in layout["ips"]["relocator"]}
    out = {}
    for name in REL_FIELDS:
        f = fields[name]
        out[name] = sum(
            int.from_bytes(
                payload[data_off + i * per_slot + f["offset"] :][: f["size"]], "little"
            )
            for i in range(instances)
        )
    return out


def pending_intents():
    payload, _ = gaia.recv_var(DEV, SUB_GET_EVICT_EVENTS, 16 * 1024)
    return struct.unpack_from("<IIII", payload, 0)[1]


def vmstat_numa():
    return {
        k: int(v)
        for k, v in (line.split() for line in open("/proc/vmstat"))
        if k.startswith("numa_")
    }


def cxl_region():
    # The dax sits under its CXL region in sysfs: .../regionN/dax_regionN/daxN.M.
    regions = [
        p for p in os.path.realpath(DAX).split("/") if re.fullmatch(r"region\d+", p)
    ]
    assert len(regions) == 1, f"expected one CXL region above {DAX}, found {regions}"
    region = os.path.join("/sys/bus/cxl/devices", regions[0])
    base = int(open(os.path.join(region, "resource")).read(), 16)
    size = int(open(os.path.join(region, "size")).read(), 16)
    return base, size


# Built into a private directory: the helper runs as root.
HELPER_DIR = tempfile.mkdtemp(prefix="im_e2e_")
atexit.register(shutil.rmtree, HELPER_DIR, ignore_errors=True)
HELPER = os.path.join(HELPER_DIR, "im_e2e_folio")


class Owner:
    """One process holding `count` THPs on the IM node."""

    def __init__(self, node, count):
        self.p = subprocess.Popen(
            [HELPER, str(node), str(count)] + (["numa"] if NUMA_BALANCING else []),
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            text=True,
            bufsize=1,
        )
        self.pfns = [int(self.p.stdout.readline().split()[2]) for _ in range(count)]

    def cmd(self, line):
        self.p.stdin.write(line + "\n")
        self.p.stdin.flush()
        return self.p.stdout.readline().strip()

    def fields(self, line):
        return dict(t.split("=") for t in self.cmd(line).split()[1:])

    def present(self, idx):
        return self.fields(f"STATE {idx}")["present"] == "1"

    def pfn(self, idx):
        return int(self.fields(f"STATE {idx}")["pfn"])

    def close(self):
        self.p.stdin.write("EXIT\n")
        self.p.stdin.flush()
        self.p.wait(timeout=30)


class Pressure:
    """FW-internal prefetch away from the folios, cycling through the upper part of the device."""

    def __init__(self, size):
        self.lo = 16 * GIB
        self.hi = size - 2 * GIB
        self.cur = self.lo

    def until(self, done, limit):
        moved, t0 = 0, time.time()
        while not done():
            if moved >= limit:
                return False, moved, time.time() - t0
            if self.cur + CHUNK > self.hi:
                self.cur = self.lo
            pyxif.memory_prefetch_sync(DEV, self.cur, CHUNK)
            self.cur += CHUNK
            moved += CHUNK
        return True, moved, time.time() - t0


def main():
    layouts = {
        int(k): v
        for k, v in json.load(open(os.path.join(HERE, "cli", "gaia_stat_layout.json")))[
            "versions"
        ].items()
    }
    base, size = cxl_region()
    node = int(
        subprocess.run(
            ["daxctl", "list", "-d", ARGS.dax], capture_output=True, text=True
        )
        .stdout.split('"target_node":')[1]
        .split(",")[0]
    )
    subprocess.run(
        [
            "gcc",
            "-O1",
            "-Wall",
            "-pthread",
            "-o",
            HELPER,
            os.path.join(HERE, "im_e2e_folio.c"),
        ],
        check=True,
    )

    # 1. Smallest DRAM cache, then THPs in two processes; a.0 gets a reader that never stops.
    gaia.send(DEV, SUB_SET_DRAM_CAPACITY, 4 * GIB)
    # A run that stops early must not leave a shared device with the smallest DRAM cache.
    atexit.register(gaia.send, DEV, SUB_SET_DRAM_CAPACITY, 0)
    time.sleep(2)
    numa0 = vmstat_numa()
    a, b = Owner(node, 3), Owner(node, 2)
    idle = [(a, 1), (a, 2), (b, 0), (b, 1)]
    every = [(a, 0)] + idle
    pfns = {(id(o), i): o.pfn(i) for o, i in every}
    on_node = all(
        p % 512 == 0 and base <= p << 12 < base + size and o.present(i)
        for (o, i), p in zip(every, pfns.values())
    )
    check(
        "five THPs on the IM node in two processes",
        on_node,
        " ".join(f"{p:#x}" for p in pfns.values()),
    )

    # 2. Enable finds the range on its own: the device's region, and the kmem dax inside it.
    err_zero = try_write(IMCTL + "ack_timeout_ms", 0)
    err_over = try_write(IMCTL + "ack_timeout_ms", ACK_TIMEOUT_MAX_MS + 1)
    write(IMCTL + "ack_timeout_ms", ACK_TIMEOUT_MS)
    check(
        "ack_timeout_ms refuses 0 and values over an hour",
        err_zero != 0
        and err_over != 0
        and open(IMCTL + "ack_timeout_ms").read().strip() == str(ACK_TIMEOUT_MS),
        f"errno {err_zero} / {err_over}",
    )
    # QEMU's CPU caches are not coherent with the device model without the backdoor;
    # on real hardware the backdoor register does not exist and clflush reaches the device.
    write(MXDMA_CACHE_BACKDOOR, "N" if ARGS.hw else "Y")
    write(IMCTL + "enable", 1)
    atexit.register(try_write, IMCTL + "enable", 0)
    st, imsf = kv(MXIM + "stats"), imsf_status()
    # kmem onlines only the whole memory blocks inside the dax, and IM covers just those.
    blk = int(open("/sys/devices/system/memory/block_size_bytes").read(), 16)
    dax_base = int(open(DAX + "resource").read(), 0)
    dax_end = dax_base + int(open(DAX + "size").read(), 0)
    ram_base = -(-dax_base // blk) * blk
    ram_size = dax_end // blk * blk - ram_base
    check(
        "enable takes the region and the kmem dax range without being told",
        st["enabled"] == "1"
        and open(IMCTL + "enable").read().strip() == "1"
        and int(st["region_base"], 16) == base
        and int(st["region_size"], 16) == size
        and int(st["ram_base"], 16) == ram_base
        and int(st["ram_size"], 16) == ram_size
        and imsf["range"].startswith(hex(ram_base >> 12)),
        f"region={st['region_base']}+{st['region_size']} dpa_base={st['dpa_base']} "
        f"ram={st['ram_base']}+{st['ram_size']} range={imsf['range']}",
    )
    check(
        "kernel poller enabled the FW notify with the configured ACK timeout",
        st["ack_timeout_ms"] == str(ACK_TIMEOUT_MS) and int(st["generation"]) > 0,
        f"generation={st['generation']} ack_timeout_ms={st['ack_timeout_ms']}",
    )
    # Counters last as long as the device stays attached, so errors are counted from here.
    st_enabled, imsf_enabled = st, imsf

    pressure = Pressure(size)
    a.cmd("HAMMER 0")

    # 3. Rounds: pressure parks every idle folio, then each is read back through a fault.
    for rnd in (1, 2):
        st0, imsf0, s0 = (
            kv(MXIM + "stats"),
            imsf_status(),
            relocator_stats(layouts),
        )
        ok, moved, secs = pressure.until(
            lambda: not any(o.present(i) for o, i in idle), 40 * GIB
        )
        st1, s1 = kv(MXIM + "stats"), relocator_stats(layouts)
        check(
            f"round {rnd}: every idle folio parked on a FW intent",
            ok,
            f"{moved // MIB} MiB of pressure in {secs:.0f}s, intents={delta(st1, st0, 'events')} "
            f"untracked={delta(st1, st0, 'ack_untracked')} parked={delta(st1, st0, 'ack_parked')} "
            f"already={delta(st1, st0, 'ack_already')} nack={delta(st1, st0, 'nack')}",
        )
        check(
            f"round {rnd}: FW evicted the parked groups",
            # A group re-read since its last eviction is clean, so it leaves without an SSD write.
            s1["EvictionDone"] > s0["EvictionDone"],
            f"EvictionDone Δ={s1['EvictionDone'] - s0['EvictionDone']} "
            f"SsdWrite Δ={s1['SsdWrite'] - s0['SsdWrite']}",
        )
        reads = [o.fields(f"READ {i}") for o, i in idle]
        s2, imsf2 = relocator_stats(layouts), imsf_status()
        check(
            f"round {rnd}: faulting reads resume with intact data",
            all(r["ok"] == "1" for r in reads),
            "first access ms " + " ".join(r["first_ms"] for r in reads),
        )
        moved = [
            f"{i}:present={int(o.present(i))},pfn={o.pfn(i):#x}"
            for o, i in idle
            if not o.present(i) or o.pfn(i) != pfns[(id(o), i)]
        ]
        check(
            f"round {rnd}: folios restored in place, no CPU miss on the device",
            not moved and s2["Miss"] == s1["Miss"],
            f"restored Δ={delta(imsf2, imsf0, 'restored')} faults Δ={delta(imsf2, imsf0, 'faults')} "
            f"SsdRead Δ={s2['SsdRead'] - s1['SsdRead']} Miss Δ={s2['Miss'] - s1['Miss']}"
            + (f" not in place: {' '.join(moved)}" if moved else ""),
        )

    hammer = a.fields("STOP")
    if NUMA_BALANCING:
        numa1 = vmstat_numa()
        d = {k: numa1[k] - numa0[k] for k in numa1}
        check(
            "NUMA balancing scanned the folios' mappings during the rounds",
            open("/proc/sys/kernel/numa_balancing").read().strip() != "0"
            and d["numa_huge_pte_updates"] > 0
            and d["numa_hint_faults"] > 0,
            f"huge_pte_updates Δ={d['numa_huge_pte_updates']} hint_faults Δ={d['numa_hint_faults']} "
            f"pages_migrated Δ={d['numa_pages_migrated']}",
        )
    st, imsf = kv(MXIM + "stats"), imsf_status()
    check(
        "a reader that keeps touching its folio always sees the right data",
        hammer["ok"] == "1" and int(hammer["passes"]) > 0,
        f"passes={hammer['passes']} slowest pass={hammer['max_ms']} ms, nack={st['nack']} "
        f"faults={imsf['faults']}",
    )
    check(
        "poller and page-ins saw no errors",
        all(
            delta(st, st_enabled, k) == 0
            for k in ("poll_err", "reply_err", "stale", "fetch_err")
        )
        and delta(imsf, imsf_enabled, "fetch_fail") == 0,
        f"polls={st['polls']} poll_us_avg={st['poll_us_avg']} poll_us_max={st['poll_us_max']} "
        f"fetches={st['fetches']} fetch_us_avg={st['fetch_us_avg']} fetch_us_max={st['fetch_us_max']} "
        f"replies the FW had already settled={st['reply_settled']} outside the kmem range={st['ack_outside']}",
    )

    # 4. Disable while folios are parked: the driver pages them in and restores them first.
    pressure.until(lambda: not all(o.present(i) for o, i in idle), 40 * GIB)
    parked_now = [(o, i) for o, i in idle if not o.present(i)]
    st0 = kv(MXIM + "stats")
    write(IMCTL + "enable", 0)
    st1, imsf = kv(MXIM + "stats"), imsf_status()
    reads = [o.fields(f"READ {i}") for o, i in parked_now]
    check(
        "disable pages in and restores every parked folio",
        len(parked_now) > 0
        and all(o.present(i) for o, i in idle)
        and imsf["parked"] == imsf["restored"]
        and delta(st1, st0, "fetches") >= len(parked_now)
        and all(r["ok"] == "1" for r in reads)
        and pending_intents() == 0,
        f"parked at disable={len(parked_now)} fetches Δ={delta(st1, st0, 'fetches')} "
        f"parked={imsf['parked']} restored={imsf['restored']}",
    )

    # 5. A page-in that fails still resumes the task, through the device's own miss path.
    write(IMCTL + "enable", 1)
    timeout_ms = open(MXDMA_TIMEOUT).read().strip()
    st0, imsf0 = kv(MXIM + "stats"), imsf_status()
    pressure.until(lambda: not all(o.present(i) for o, i in idle), 40 * GIB)
    victim = next(((o, i) for o, i in idle if not o.present(i)), None)
    write(MXDMA_TIMEOUT, 1)
    r = (
        victim[0].fields(f"READ {victim[1]}")
        if victim
        else {"ok": "0", "first_ms": "-"}
    )
    write(MXDMA_TIMEOUT, timeout_ms)
    st1, imsf1 = kv(MXIM + "stats"), imsf_status()
    check(
        "a failed page-in falls back to the device and returns intact data",
        victim is not None
        and r["ok"] == "1"
        and victim[0].present(victim[1])
        and delta(imsf1, imsf0, "fetch_fail") >= 1,
        f"first access {r['first_ms']} ms, fetch_fail Δ={delta(imsf1, imsf0, 'fetch_fail')} "
        f"fetch_err Δ={delta(st1, st0, 'fetch_err')} poll_err Δ={delta(st1, st0, 'poll_err')}",
    )
    # A pass in flight may still be writing a folio back, so give the next poll time to start.
    polls_before = int(st1["polls"])
    t_wait = time.time()
    while (
        int(kv(MXIM + "stats")["polls"]) <= polls_before and time.time() - t_wait < 180
    ):
        time.sleep(1)
    st2 = kv(MXIM + "stats")
    check(
        "the poller keeps working after the timeout window",
        int(st2["polls"]) > polls_before and delta(st2, st1, "poll_err") == 0,
        f"next poll after {time.time() - t_wait:.0f}s, polls Δ={int(st2['polls']) - polls_before}",
    )
    write(IMCTL + "enable", 0)

    # 6. One full-word check, then clean up.
    full = a.fields("FULL 1")
    check(
        "full 2 MiB check after all rounds", full["ok"] == "1", f"{full['total_ms']} ms"
    )
    a.close()
    b.close()

    failed = [n for n, ok in results if not ok]
    print(f"\nSUMMARY: {len(results) - len(failed)}/{len(results)} passed", flush=True)
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
