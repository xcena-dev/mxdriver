#!/usr/bin/env python3
"""IM eviction-notify handshake over HIO (SetEvictNotify, GetEvictEvents, EvictReply), judged by
relocator GetStat counters. Run as root with the IM dax still in devdax mode: in the emulator
guest as is, on real hardware with --hw.
"""

import argparse
import array
import atexit
import fcntl
import json
import mmap
import os
import shutil
import statistics
import struct
import subprocess
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from cli import gaia  # noqa: E402

import pyxif  # noqa: E402

KIB = 1024
MIB = 1024 * KIB
GIB = 1024 * MIB
SPN_BYTES = 64 * KIB
GROUP_BYTES = 512 * KIB

SUB_SET_DRAM_CAPACITY = 1
SUB_GET_STAT = 5
SUB_GET_EVICT_EVENTS = 12
SUB_SET_EVICT_NOTIFY = 13
SUB_EVICT_REPLY = 14

# Population free-page watermark: 80 groups per relocator, scaled so the total is fixed.
MIN_FREE_TOTAL_GROUPS = 80 * 32

# Long enough that no victim expires while a step inspects it.
LONG_TIMEOUT_MS = 30 * 60 * 1000

# A relocator out of free pages with its victims parked makes a prefetch wait for the host,
# and with LONG_TIMEOUT_MS that holds up every later command. Stop above this many free groups.
FREE_MARGIN_GROUPS = 24

MX_CACHE_FLUSH = 0x40184301  # _IOW('C', 1, struct mx_cache_op)
MX_CACHE_INVALIDATE = 0x40184302  # _IOW('C', 2, struct mx_cache_op)

EVENT_HEADER = struct.Struct("<IIII")
EVENT = struct.Struct("<BBBBIII")

REL_FIELDS = (
    "Hit",
    "Miss",
    "SsdRead",
    "SsdWrite",
    "EvictionTrigger",
    "EvictionHit",
    "EvictionDone",
    "FreePage",
    "ValidPage",
    "PendingPage",
    "TotalPage",
)

results = []


def check(name, ok, detail=""):
    results.append((name, bool(ok), detail))
    print(
        f"[{'PASS' if ok else 'FAIL'}] {name}" + (f" — {detail}" if detail else ""),
        flush=True,
    )


class Stats:
    def __init__(self, dev):
        self.dev = dev
        layout_path = os.path.join(
            os.path.dirname(os.path.abspath(__file__)), "cli", "gaia_stat_layout.json"
        )
        self.layouts = {
            int(k): v for k, v in json.load(open(layout_path))["versions"].items()
        }

    def relocator(self):
        payload, _ = gaia.recv_var(self.dev, SUB_GET_STAT, 2 * MIB)
        version, ip_count = struct.unpack_from("<II", payload, 0)
        layout = self.layouts[version]
        ip = layout["ip_order"].index("relocator")
        mask, per_slot, instances, data_off = struct.unpack_from(
            "<IIII", payload, 8 + ip * 16
        )
        fields = {f["name"]: f for f in layout["ips"]["relocator"]}

        def value(inst, name):
            f = fields[name]
            base = data_off + inst * per_slot + f["offset"]
            return int.from_bytes(payload[base : base + f["size"]], "little")

        out = {
            name: sum(value(i, name) for i in range(instances)) for name in REL_FIELDS
        }
        active = [i for i in range(instances) if value(i, "TotalPage") > 0]
        out["active"] = len(active)
        out["MinFreePage"] = min((value(i, "FreePage") for i in active), default=0)
        return out

    def settle(
        self,
        keys=("EvictionTrigger", "EvictionDone", "PendingPage", "FreePage"),
        tries=20,
        gap=0.5,
    ):
        prev = self.relocator()
        for _ in range(tries):
            time.sleep(gap)
            cur = self.relocator()
            if all(cur[k] == prev[k] for k in keys):
                return cur
            prev = cur
        return prev


def get_events(dev):
    t0 = time.perf_counter()
    payload, _ = gaia.recv_var(dev, SUB_GET_EVICT_EVENTS, 16 * KIB)
    dt = time.perf_counter() - t0
    version, count, rel_count, slots = EVENT_HEADER.unpack_from(payload, 0)
    events = []
    for i in range(count):
        rid, slot, gen, _r, seq, spn, dpn = EVENT.unpack_from(
            payload, EVENT_HEADER.size + i * EVENT.size
        )
        events.append(
            {
                "rid": rid,
                "slot": slot,
                "gen": gen,
                "seq": seq,
                "spn": spn,
                "dpn": dpn,
                "offset": spn * SPN_BYTES,
            }
        )
    return {
        "version": version,
        "rel_count": rel_count,
        "slots": slots,
        "events": events,
        "secs": dt,
    }


def set_notify(dev, enable, generation, timeout_ms=0):
    t0 = time.perf_counter()
    gaia.send(
        dev,
        SUB_SET_EVICT_NOTIFY,
        (1 if enable else 0)
        | ((generation & 0xFF) << 8)
        | ((timeout_ms & 0xFFFFFFFF) << 16),
    )
    return time.perf_counter() - t0


def prefetch_sync_async(dev, offset, size):
    # pyxif holds the GIL for the whole call, so pressure runs in its own process.
    code = f"import pyxif; print(pyxif.memory_prefetch_sync({dev}, {offset}, {size}))"
    return subprocess.Popen(
        [sys.executable, "-c", code], stdout=subprocess.PIPE, text=True
    )


def wait_proc(proc, secs):
    try:
        proc.wait(secs)
    except subprocess.TimeoutExpired:
        pass
    return proc.poll() is not None


def key(ev):
    return (ev["rid"], ev["slot"], ev["seq"])


def listed(dev, ev):
    return key(ev) in {key(e) for e in get_events(dev)["events"]}


def evict_reply(dev, ev, ack):
    value = (
        ev["rid"] | (ev["slot"] << 8) | ((1 if ack else 0) << 16) | (ev["seq"] << 32)
    )
    t0 = time.perf_counter()
    try:
        gaia.send(dev, SUB_EVICT_REPLY, value)
        return True, time.perf_counter() - t0, ""
    except RuntimeError as e:
        return False, time.perf_counter() - t0, str(e)


# Set by main() with --hw: real CPU caches are flushed through a mapping of the dax instead.
CLFLUSH = {"helper": None, "dax": None, "hpa_base": 0}


def cache_op(hpa, size, op):
    if CLFLUSH["helper"] is not None:
        # CLFLUSH writes a line back and invalidates it, so it serves both ops.
        subprocess.run(
            [
                CLFLUSH["helper"],
                CLFLUSH["dax"],
                str(hpa - CLFLUSH["hpa_base"]),
                str(size),
            ],
            check=True,
        )
        return
    fd = os.open("/dev/xcena_cache", os.O_RDWR)
    try:
        fcntl.ioctl(fd, op, struct.pack("<QQII", hpa, size, 0, 0))
    finally:
        os.close(fd)


def pattern(offset, size):
    words = array.array(
        "Q",
        (
            (((offset + i * 8) >> 3) ^ 0xA5A5_0000_5A5A_0000) & 0xFFFFFFFFFFFFFFFF
            for i in range(size // 8)
        ),
    )
    return words.tobytes()


def us(secs):
    return f"{secs * 1e6:.0f} µs"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--dev", type=int, default=0)
    ap.add_argument("--dax", default="/dev/dax0.0")
    ap.add_argument("--data-mib", type=int, default=8)
    ap.add_argument("--prefetch-base-mib", type=int, default=64)
    # Each step takes a few groups from every relocator, so one step cannot cross FREE_MARGIN_GROUPS;
    # 256 MiB steps drained a relocator on hardware.
    ap.add_argument("--chunk-mib", type=int, default=96)
    ap.add_argument("--dram-capacity-gib", type=int, default=4)
    ap.add_argument(
        "--hw", action="store_true", help="real hardware: CLFLUSH, no /dev/xcena_cache"
    )
    args = ap.parse_args()

    dev = args.dev
    stats = Stats(dev)
    hpa_base = int(
        open(f"/sys/bus/dax/devices/{os.path.basename(args.dax)}/resource").read(), 16
    )
    if args.hw:
        helper_dir = tempfile.mkdtemp(prefix="im_clflush_")
        atexit.register(shutil.rmtree, helper_dir, ignore_errors=True)
        helper = os.path.join(helper_dir, "im_clflush")
        here = os.path.dirname(os.path.abspath(__file__))
        subprocess.run(
            ["gcc", "-O1", "-Wall", "-o", helper, os.path.join(here, "im_clflush.c")],
            check=True,
        )
        CLFLUSH.update(helper=helper, dax=args.dax, hpa_base=hpa_base)
    dax_size = int(
        open(f"/sys/bus/dax/devices/{os.path.basename(args.dax)}/size").read()
    )
    data_bytes = args.data_mib * MIB
    print(
        f"dax={args.dax} hpa=0x{hpa_base:x} size={dax_size / GIB:.1f} GiB", flush=True
    )

    # 0. Notify off by default; an empty poll still answers with a header.
    ev0 = get_events(dev)
    check(
        "GetEvictEvents answers while disabled",
        ev0["version"] == 1 and len(ev0["events"]) == 0,
        f"relocators={ev0['rel_count']} slots/rel={ev0['slots']} rtt={us(ev0['secs'])}",
    )

    # Pages an earlier run left in the cache are older than the data and would be the first victims.
    # Unmap all the test touches, up to the end of its last prefetch.
    span = -(-(args.prefetch_base_mib * MIB + 13 * GIB) // GIB) * GIB
    gaia.send_cache(dev, gaia.GaiaSubOpcode.UNMAP, 0, span)

    # 1. Smallest DRAM cache so the eviction watermark is reachable with a moderate fill.
    gaia.send(dev, SUB_SET_DRAM_CAPACITY, args.dram_capacity_gib * GIB)
    s = stats.settle()
    print(
        f"after SetDramCapacity: active relocators={s['active']} total={s['TotalPage']} free={s['FreePage']} "
        f"valid={s['ValidPage']} pending={s['PendingPage']}",
        flush=True,
    )

    # 2. Dirty, known data at the front of the window: the oldest pages of their relocators.
    fd = os.open(args.dax, os.O_RDWR)
    mm = mmap.mmap(fd, 2 * GIB, mmap.MAP_SHARED, mmap.PROT_READ | mmap.PROT_WRITE)
    t0 = time.perf_counter()
    for off in range(0, data_bytes, GROUP_BYTES):
        mm[off : off + GROUP_BYTES] = pattern(off, GROUP_BYTES)
    cache_op(hpa_base, data_bytes, MX_CACHE_FLUSH)
    print(
        f"wrote+flushed {args.data_mib} MiB in {time.perf_counter() - t0:.1f}s",
        flush=True,
    )
    data_groups = {off // SPN_BYTES for off in range(0, data_bytes, GROUP_BYTES)}

    s0 = stats.settle()

    # 3. Enable notify, then build pressure with FW-internal prefetch until intents appear.
    # The ACK timeout is long here so victims stay parked while each step inspects them.
    set_notify(dev, True, 1, LONG_TIMEOUT_MS)
    target_free = MIN_FREE_TOTAL_GROUPS * 6 // 10
    base = args.prefetch_base_mib * MIB
    chunk = args.chunk_mib * MIB
    prefetched = 0
    ev = get_events(dev)
    while True:
        s = stats.relocator()
        covered = data_groups <= {e["spn"] for e in ev["events"]}
        if (
            covered
            or s["MinFreePage"] < FREE_MARGIN_GROUPS
            or s["FreePage"] <= target_free
        ):
            break
        if base + prefetched + chunk > dax_size:
            break
        t0 = time.perf_counter()
        st = pyxif.memory_prefetch_sync(dev, base + prefetched, chunk)
        prefetched += chunk
        ev = get_events(dev)
        print(
            f"prefetch {prefetched // MIB} MiB ({st}) in {time.perf_counter() - t0:.1f}s: free={s['FreePage']} "
            f"(min {s['MinFreePage']}) intents={len(ev['events'])}",
            flush=True,
        )
        if st != pyxif.MemoryStatus.Success:
            check(
                "pressure steps stay under the FW eviction cap",
                False,
                f"{st} at {prefetched // MIB} MiB",
            )
            # Notify off hands the parked victims back, so the waiting prefetch can finish.
            set_notify(dev, False, 1)
            gaia.send(dev, SUB_SET_DRAM_CAPACITY, 0)
            return 1

    time.sleep(1.0)
    ev = get_events(dev)
    s1 = stats.settle()
    events = ev["events"]
    print(
        f"intents={len(events)} free={s1['FreePage']} pending={s1['PendingPage']} "
        f"trigger Δ={s1['EvictionTrigger'] - s0['EvictionTrigger']}",
        flush=True,
    )

    check(
        "intents are posted under pressure", len(events) > 0, f"{len(events)} intents"
    )
    check(
        "no eviction starts before an ACK",
        s1["EvictionTrigger"] == s0["EvictionTrigger"]
        and s1["EvictionDone"] == s0["EvictionDone"],
        f"EvictionTrigger Δ={s1['EvictionTrigger'] - s0['EvictionTrigger']}, "
        f"EvictionDone Δ={s1['EvictionDone'] - s0['EvictionDone']}",
    )
    check(
        "parked victims are counted as Pending",
        s1["PendingPage"] >= len(events),
        f"PendingPage={s1['PendingPage']} intents={len(events)}",
    )
    check(
        "intent generation matches SetEvictNotify", all(e["gen"] == 1 for e in events)
    )
    in_data = [e for e in events if e["spn"] in data_groups]
    check(
        "oldest (written) groups are the first victims",
        len(in_data) > 0,
        f"{len(in_data)}/{len(data_groups)} written groups parked",
    )
    if not in_data:
        return finish(dev, mm, fd, args)

    # 4. Parked data stays resident: reads hit and return the written bytes.
    probe = in_data[0]
    sa = stats.settle()
    cache_op(hpa_base + probe["offset"], GROUP_BYTES, MX_CACHE_INVALIDATE)
    ok = mm[probe["offset"] : probe["offset"] + GROUP_BYTES] == pattern(
        probe["offset"], GROUP_BYTES
    )
    sb = stats.settle()
    check("parked group reads back intact", ok, f"offset=0x{probe['offset']:x}")
    check(
        "parked group read is served without an SSD read",
        sb["Miss"] == sa["Miss"] and sb["SsdRead"] == sa["SsdRead"],
        f"Hit Δ={sb['Hit'] - sa['Hit']} Miss Δ={sb['Miss'] - sa['Miss']} SsdRead Δ={sb['SsdRead'] - sa['SsdRead']}",
    )

    # 4b. A host command on a parked group withdraws its intent before the command runs.
    fillers = [e for e in events if e["spn"] not in data_groups]
    commands = [
        ("PrefetchSync", lambda off: pyxif.memory_prefetch_sync(dev, off, GROUP_BYTES)),
        ("Flush", lambda off: pyxif.memory_flush(dev, off, GROUP_BYTES)),
        ("Discard", lambda off: gaia.send_cache(dev, gaia.GaiaSubOpcode.DISCARD, off, GROUP_BYTES)),
        (
            "Pin",
            lambda off: (
                pyxif.memory_pin(dev, off, GROUP_BYTES),
                pyxif.memory_unpin(dev, off, GROUP_BYTES),
            ),
        ),
    ]
    targets = fillers[: len(commands)]
    fillers = fillers[len(commands) :]
    for (name, run), target in zip(commands, targets):
        s_c0 = stats.relocator()
        run(target["offset"])
        s_c1 = stats.settle()
        check(
            f"{name} on a parked group withdraws its intent",
            not listed(dev, target)
            and s_c1["EvictionTrigger"] == s_c0["EvictionTrigger"],
            f"EvictionTrigger Δ={s_c1['EvictionTrigger'] - s_c0['EvictionTrigger']}",
        )
    sb = stats.settle()

    # 5. Stale or malformed replies are refused and change nothing.
    ok_stale, _, err = evict_reply(dev, dict(probe, seq=probe["seq"] + 100000), True)
    s_stale = stats.settle()
    check(
        "reply with a wrong seq is rejected",
        not ok_stale and s_stale["EvictionTrigger"] == sb["EvictionTrigger"],
        err,
    )

    # 6. NACK returns the victim to the Valid pool without evicting it.
    nack_target = next((e for e in fillers if listed(dev, e)), None) or in_data[-1]
    ok_nack, t_nack, err = evict_reply(dev, nack_target, False)
    s_n = stats.settle()
    after = get_events(dev)["events"]
    still = any(
        (e["rid"], e["slot"], e["seq"])
        == (nack_target["rid"], nack_target["slot"], nack_target["seq"])
        for e in after
    )
    check(
        "NACK accepted and the intent is withdrawn",
        ok_nack and not still,
        f"rtt={us(t_nack)} {err}",
    )
    check(
        "NACK does not start an eviction",
        s_n["EvictionTrigger"] == s_stale["EvictionTrigger"],
        f"EvictionTrigger Δ={s_n['EvictionTrigger'] - s_stale['EvictionTrigger']}",
    )

    # 7. ACK lets exactly that group go to SSD; a later read misses and returns the same bytes.
    ok_ack, t_ack, err = evict_reply(dev, probe, True)
    s_a = stats.settle()
    check("ACK accepted", ok_ack, f"rtt={us(t_ack)} {err}")
    check(
        "ACK starts and completes one eviction",
        s_a["EvictionTrigger"] - s_n["EvictionTrigger"] == 1
        and s_a["EvictionDone"] - s_n["EvictionDone"] == 1,
        f"EvictionTrigger Δ={s_a['EvictionTrigger'] - s_n['EvictionTrigger']} "
        f"EvictionDone Δ={s_a['EvictionDone'] - s_n['EvictionDone']} SsdWrite Δ={s_a['SsdWrite'] - s_n['SsdWrite']}",
    )
    cache_op(hpa_base + probe["offset"], GROUP_BYTES, MX_CACHE_INVALIDATE)
    t0 = time.perf_counter()
    ok = mm[probe["offset"] : probe["offset"] + GROUP_BYTES] == pattern(
        probe["offset"], GROUP_BYTES
    )
    t_read = time.perf_counter() - t0
    s_r = stats.settle()
    check(
        "evicted group reads back intact",
        ok,
        f"read {GROUP_BYTES // KIB} KiB in {t_read * 1e3:.1f} ms",
    )
    check(
        "evicted group read goes to SSD",
        s_r["Miss"] > s_a["Miss"] and s_r["SsdRead"] > s_a["SsdRead"],
        f"Miss Δ={s_r['Miss'] - s_a['Miss']} SsdRead Δ={s_r['SsdRead'] - s_a['SsdRead']}",
    )

    # 7b. A PrefetchSync that meets a read already in flight waits for it to finish.
    second = next((e for e in in_data[1:] if listed(dev, e)), None)
    if second is not None:
        s_e0 = stats.relocator()
        evict_reply(dev, second, True)
        for _ in range(240):
            if stats.relocator()["EvictionDone"] > s_e0["EvictionDone"]:
                break
            time.sleep(0.5)
        s_p0 = stats.settle()
        off = second["offset"]
        pyxif.memory_prefetch(dev, off, GROUP_BYTES)
        t0 = time.perf_counter()
        pyxif.memory_prefetch_sync(dev, off, GROUP_BYTES)
        t_sync = time.perf_counter() - t0
        s_p = stats.settle()
        cache_op(hpa_base + off, GROUP_BYTES, MX_CACHE_INVALIDATE)
        ok = mm[off : off + GROUP_BYTES] == pattern(off, GROUP_BYTES)
        s_q = stats.settle()
        check(
            "PrefetchSync joined the in-flight read: the group is resident when it returns",
            ok
            and s_p0["EvictionDone"] > s_e0["EvictionDone"]
            and s_p["SsdRead"] - s_p0["SsdRead"] == 1
            and s_q["Miss"] == s_p["Miss"],
            f"PrefetchSync {t_sync * 1e3:.1f} ms, SsdRead Δ={s_p['SsdRead'] - s_p0['SsdRead']}, "
            f"Miss Δ on the CPU read={s_q['Miss'] - s_p['Miss']}",
        )

    # 8. Poll cost with intents pending.
    rtts = [get_events(dev)["secs"] for _ in range(30)]
    print(
        f"GetEvictEvents rtt with {len(get_events(dev)['events'])} intents: "
        f"median={us(statistics.median(rtts))} p90={us(sorted(rtts)[26])} max={us(max(rtts))}",
        flush=True,
    )

    return finish(dev, mm, fd, args)


def finish(dev, mm, fd, args):
    stats = Stats(dev)

    # 8b. Unmap takes whole GiBs: clear the first GiB, which holds the data and the oldest victims.
    in_first = [e for e in get_events(dev)["events"] if e["offset"] < GIB]
    if in_first:
        s_u0 = stats.relocator()
        gaia.send_cache(dev, gaia.GaiaSubOpcode.UNMAP, 0, GIB)
        s_u1 = stats.settle()
        still = [e for e in in_first if listed(dev, e)]
        check(
            "Unmap on parked groups withdraws their intents",
            not still and s_u1["EvictionTrigger"] == s_u0["EvictionTrigger"],
            f"parked in range={len(in_first)} still listed={len(still)} "
            f"EvictionTrigger Δ={s_u1['EvictionTrigger'] - s_u0['EvictionTrigger']}",
        )
    s_before = stats.settle()

    # 9. Disable while a PrefetchSync waits for free pages that only parked victims can give.
    big = (args.prefetch_base_mib + 8 * 1024) * MIB
    # A fresh session returns the earlier victims, so new ones park while the prefetch runs.
    set_notify(dev, True, 3, LONG_TIMEOUT_MS)
    proc = prefetch_sync_async(dev, big, 4 * GIB)
    finished_early = wait_proc(proc, 15.0)
    parked = len(get_events(dev)["events"])
    t_off = time.perf_counter()
    rtt_off = set_notify(dev, False, 3)
    finished = wait_proc(proc, 300.0)
    t_done = time.perf_counter() - t_off
    out = proc.stdout.read().strip() if finished else "still running"
    check(
        "disable answers while a PrefetchSync waits on parked victims",
        not finished_early
        and parked > 0
        and rtt_off < 1.0
        and finished
        and "Success" in out,
        f"parked={parked} disable rtt={us(rtt_off)} prefetch {out} {t_done:.1f}s after disable",
    )
    s_d = stats.settle()
    left = get_events(dev)["events"]
    check(
        "disable withdraws every intent",
        len(left) == 0,
        f"remaining={len(left)} pending={s_d['PendingPage']}",
    )
    below_watermark = s_before["FreePage"] < MIN_FREE_TOTAL_GROUPS
    check(
        "FW evicts on its own again after disable",
        (not below_watermark) or s_d["EvictionTrigger"] > s_before["EvictionTrigger"],
        f"free {s_before['FreePage']}→{s_d['FreePage']}, "
        f"EvictionTrigger Δ={s_d['EvictionTrigger'] - s_before['EvictionTrigger']}, "
        f"EvictionDone Δ={s_d['EvictionDone'] - s_before['EvictionDone']}",
    )
    rtts = [get_events(dev)["secs"] for _ in range(30)]
    print(
        f"GetEvictEvents rtt (empty): median={us(statistics.median(rtts))} max={us(max(rtts))}",
        flush=True,
    )

    # 10. Unanswered intents are evicted once the ACK timeout passes.
    timeout_ms = 300
    s_t0 = stats.settle()
    set_notify(dev, True, 2, timeout_ms)
    proc = prefetch_sync_async(dev, big + 4 * GIB, GIB)
    first_seen, gone_after = {}, {}
    t_end = time.perf_counter() + 20.0
    while time.perf_counter() < t_end and (len(gone_after) < 8 or proc.poll() is None):
        now = time.perf_counter()
        current = {key(e): e for e in get_events(dev)["events"]}
        for k in current:
            first_seen.setdefault(k, now)
        for k, t in first_seen.items():
            if k not in current and k not in gone_after:
                gone_after[k] = now - t
    finished = wait_proc(proc, 120.0)
    s_t1 = stats.settle()
    lifetimes = sorted(gone_after.values())
    check(
        "unanswered intents are evicted after the ACK timeout",
        len(lifetimes) > 0
        and s_t1["EvictionTrigger"] > s_t0["EvictionTrigger"]
        and finished,
        f"timeout={timeout_ms} ms, intents seen={len(first_seen)} expired={len(lifetimes)} "
        + (
            f"lifetime median={statistics.median(lifetimes) * 1e3:.0f} ms "
            f"max={lifetimes[-1] * 1e3:.0f} ms, "
            if lifetimes
            else ""
        )
        + f"EvictionTrigger Δ={s_t1['EvictionTrigger'] - s_t0['EvictionTrigger']}",
    )
    if gone_after:
        rid, slot, seq = next(iter(gone_after))
        ok_late, _, err = evict_reply(dev, {"rid": rid, "slot": slot, "seq": seq}, True)
        check("a late ACK for an expired intent is refused", not ok_late, err)
    set_notify(dev, False, 2)

    gaia.send(dev, SUB_SET_DRAM_CAPACITY, 0)
    mm.close()
    os.close(fd)

    failed = [r for r in results if not r[1]]
    print(f"\nSUMMARY: {len(results) - len(failed)}/{len(results)} passed", flush=True)
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
