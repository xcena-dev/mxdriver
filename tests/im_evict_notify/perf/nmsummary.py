#!/usr/bin/env python3
"""Fig. 4-style summary of an nm-like kv-session log (cellmark.py marks): per pattern, reader count and arm,
the mean over blocks of throughput, p99, charged CPU (readers plus the IM CPUs) and CPU per GiB.
Usage: nmsummary.py <log>"""

import collections
import json
import re
import sys


def main():
    marks, results, pending = {}, {}, None
    for line in open(sys.argv[1]):
        if line.startswith("APPMARK "):
            m = json.loads(line.split(" ", 1)[1])
            marks.setdefault(m["label"], {})[m["phase"]] = m
            if m["phase"] == "pre":
                pending = m["label"]
        elif line.startswith("APPRESULT ") and pending:
            results[pending] = json.loads(line.split(" ", 1)[1])
    cells = collections.defaultdict(list)
    for lab, r in results.items():
        k = re.fullmatch(r"(s0|s)(u128|u512|z1024|u\d+|z\d+)t(\d+)b(\d+)", lab)
        if not k or "pre" not in marks[lab] or "post" not in marks[lab]:
            continue
        a, b = marks[lab]["pre"], marks[lab]["post"]
        dt = r["wall_s"]
        im = (
            (b["im_cpu_busy_jiffies"] - a["im_cpu_busy_jiffies"]) / 100
            if a["im_cpu_busy_jiffies"] is not None
            else 0
        )
        gib = r["bytes"] / 2**30
        cells[(k.group(2).upper(), int(k.group(3)), k.group(1).upper())].append(
            (
                r["gb_per_s"],
                r["lat_us"].get("p99", 0) / 1000,
                (r["cpu_s"] + im) / dt,
                (r["cpu_s"] + im) / gib,
            )
        )
    print(
        f"{'pattern':>8} {'thr':>4} {'arm':>3} {'n':>2} {'GB/s':>7} {'p99 ms':>7} {'CPU-s/s':>8} {'CPU-s/GiB':>10}"
    )
    for key in sorted(cells, key=lambda k: (k[0], k[1], k[2])):
        v = cells[key]
        mean = [sum(x[i] for x in v) / len(v) for i in range(4)]
        print(
            f"{key[0]:>8} {key[1]:>4} {key[2]:>3} {len(v):>2} {mean[0]:7.2f} {mean[1]:7.1f} {mean[2]:8.2f} {mean[3]:10.3f}"
        )


if __name__ == "__main__":
    main()
