#!/usr/bin/env python3
"""Marks a cell of an appbench kv-session over IM (root): `pre <label> <S|S0>` sets IM for the arm and
pins the poller to IM_CPUS, `post <label>` closes the cell. Each APPMARK line carries the IM CPUs' jiffies
and the driver, kernel and relocator counters, so a cell's cost is post minus pre. MXDEV picks the device."""

import json
import os
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, ".."))
import evict_notify_test as ent  # noqa: E402

DEV = int(os.environ.get("MXDEV", "0"))
IM_CPUS = os.environ.get("IM_CPUS", "")
IMCTL = f"/sys/class/mx_dma/mx_dma{DEV}_ioctl/im/enable"
MXIM = f"/sys/kernel/debug/mx_dma{DEV}_im/stats"
IMSF = "/sys/kernel/debug/im_soft_fault/status"
REL_KEYS = (
    "Hit",
    "Miss",
    "SsdRead",
    "SsdWrite",
    "EvictionTrigger",
    "EvictionDone",
    "PendingPage",
    "FreePage",
)


def cpus(spec):
    out = []
    for part in filter(None, spec.split(",")):
        lo, _, hi = part.partition("-")
        out += range(int(lo), int(hi or lo) + 1)
    return out


def busy_jiffies(cpu_ids):
    want = {f"cpu{c}" for c in cpu_ids}
    total = 0
    for line in open("/proc/stat"):
        f = line.split()
        if f[0] in want:
            v = list(map(int, f[1:8]))
            total += sum(v) - v[3] - v[4]
    return total


def poller_pid():
    r = subprocess.run(
        ["pgrep", "-x", f"mx_im_poll{DEV}"], capture_output=True, text=True
    )
    return int(r.stdout.split()[0]) if r.stdout.split() else None


def poller_jiffies(pid):
    if pid is None:
        return 0
    f = open(f"/proc/{pid}/stat").read().rsplit(")", 1)[1].split()
    return int(f[11]) + int(f[12])


def kv_line(path, whole=False):
    try:
        with open(path) as f:
            text = f.read() if whole else f.readline()
    except OSError:
        return {}
    return dict(t.split("=", 1) for t in text.split() if "=" in t)


def mark(phase, label, arm=None):
    pid = poller_pid()
    rel = ent.Stats(DEV).relocator()
    m = {
        "phase": phase,
        "label": label,
        "arm": arm,
        "t": time.time(),
        "im_enabled": open(IMCTL).read().strip(),
        "im_cpu_busy_jiffies": busy_jiffies(cpus(IM_CPUS)) if IM_CPUS else None,
        "poller_jiffies": poller_jiffies(pid),
        "mxim": kv_line(MXIM, whole=True),
        "imsf": kv_line(IMSF),
        "rel": {k: rel[k] for k in REL_KEYS},
    }
    print("APPMARK " + json.dumps(m), flush=True)
    # A platform reset takes unsynced results with it; the log is written through the page cache.
    os.sync()


def main():
    phase, label = sys.argv[1], sys.argv[2]
    if phase == "pre":
        arm = sys.argv[3]
        want = {"S": "1", "S0": "0"}[arm]
        if open(IMCTL).read().strip() != want:
            with open(IMCTL, "w") as f:
                f.write(want)
        pid = poller_pid()
        if want == "1" and IM_CPUS:
            if pid is None:
                sys.exit(f"cellmark: IM is on but mx_im_poll{DEV} is not running")
            subprocess.run(
                ["taskset", "-pc", IM_CPUS, str(pid)], check=True, capture_output=True
            )
            allowed = [
                line.split()[1]
                for line in open(f"/proc/{pid}/status")
                if line.startswith("Cpus_allowed_list")
            ]
            print(f"APPNOTE poller {pid} Cpus_allowed_list {allowed}", flush=True)
        mark("pre", label, arm)
    elif phase == "post":
        mark("post", label)
    else:
        sys.exit("usage: cellmark.py pre <label> <S|S0> | post <label>")


if __name__ == "__main__":
    main()
