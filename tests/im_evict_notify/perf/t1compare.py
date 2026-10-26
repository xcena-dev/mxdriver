#!/usr/bin/env python3
"""Compares a baseline nm-grid log (S and S0 interleaved) with separate S and S0 session logs: per pattern
and reader count, block means of throughput, charged CPU (readers plus IM CPUs) and p99, their S/S0 ratios
and the S-cell park and page-in counters. Usage: t1compare.py <baseline log> <S log> <S0 log>"""

import collections
import json
import re
import sys

PAT = r"(s0|s)(u128|u512|z1024)t(\d+)b(\d+)"


def cells(path):
    marks, results, pending = {}, {}, None
    for line in open(path):
        if line.startswith("APPMARK "):
            m = json.loads(line.split(" ", 1)[1])
            marks.setdefault(m["label"], {})[m["phase"]] = m
            if m["phase"] == "pre":
                pending = m["label"]
        elif line.startswith("APPRESULT ") and pending:
            results[pending] = json.loads(line.split(" ", 1)[1])
    out = collections.defaultdict(list)
    for lab, r in results.items():
        k = re.fullmatch(PAT, lab)
        if not k or "post" not in marks.get(lab, {}):
            continue
        a, b = marks[lab]["pre"], marks[lab]["post"]
        dt = r["wall_s"]
        im = (b["im_cpu_busy_jiffies"] - a["im_cpu_busy_jiffies"]) / 100
        xa, xb = a["mxim"], b["mxim"]

        def d(key):
            return int(xb.get(key, 0)) - int(xa.get(key, 0))

        fetches = d("fetches")
        fetch_us = 0.0
        if fetches > 0:
            fetch_us = (
                float(xb.get("fetch_us_avg", 0)) * int(xb.get("fetches", 0))
                - float(xa.get("fetch_us_avg", 0)) * int(xa.get("fetches", 0))
            ) / fetches
        out[(k.group(2).upper(), int(k.group(3)), k.group(1).upper())].append(
            {
                "gbs": r["gb_per_s"],
                "p99": r["lat_us"].get("p99", 0) / 1000,
                "cpu": (r["cpu_s"] + im) / dt,
                "cpu_gib": (r["cpu_s"] + im) / (r["bytes"] / 2**30),
                "parks": d("ack_parked") / dt,
                "fetch": fetches / dt,
                "fetch_us": fetch_us,
                "settled": d("reply_settled"),
                "miss": (b["rel"]["Miss"] - a["rel"]["Miss"]) / dt,
            }
        )
    return {k: {f: sum(x[f] for x in v) / len(v) for f in v[0]} for k, v in out.items()}


def main():
    base, s_new, s0_new = cells(sys.argv[1]), cells(sys.argv[2]), cells(sys.argv[3])
    keys = sorted(
        {(p, t) for p, t, _ in list(s_new) + list(s0_new)},
        key=lambda k: (["U128", "U512", "Z1024"].index(k[0]), k[1]),
    )
    print(
        "Table 1  pattern thr | GB/s S / S0 (ratio) | charged CPU-s/s S / S0 (ratio) | CPU-s/GiB S / S0 (ratio) | p99 ms S / S0"
    )
    for p, t in keys:
        s, s0 = s_new.get((p, t, "S")), s0_new.get((p, t, "S0"))
        if not s or not s0:
            continue
        print(
            f"  {p:>6} {t:>4} | {s['gbs']:6.2f} / {s0['gbs']:6.2f} ({s['gbs'] / s0['gbs']:.2f}) | "
            f"{s['cpu']:6.1f} / {s0['cpu']:6.1f} ({s['cpu'] / s0['cpu']:.2f}) | "
            f"{s['cpu_gib']:.3f} / {s0['cpu_gib']:.3f} ({s['cpu_gib'] / s0['cpu_gib']:.2f}) | {s['p99']:6.1f} / {s0['p99']:5.1f}"
        )
    print(
        "Table 2  pattern thr | baseline S GB/s, p99 | new S GB/s, p99 | factor | baseline S0 GB/s -> new S0"
    )
    for p, t in keys:
        b, s, b0, s0 = (
            base.get((p, t, "S")),
            s_new.get((p, t, "S")),
            base.get((p, t, "S0")),
            s0_new.get((p, t, "S0")),
        )
        if not b or not s:
            continue
        print(
            f"  {p:>6} {t:>4} | {b['gbs']:6.2f}, {b['p99']:6.1f} | {s['gbs']:6.2f}, {s['p99']:6.1f} | {s['gbs'] / b['gbs']:5.2f}x | "
            f"{b0['gbs'] if b0 else 0:6.2f} -> {s0['gbs'] if s0 else 0:6.2f}"
        )
    print(
        "Table 4  pattern thr | parks/s | page-ins/s | page-in us | FW early evictions | CPU device misses/s"
    )
    for p, t in keys:
        s = s_new.get((p, t, "S"))
        if s:
            print(
                f"  {p:>6} {t:>4} | {s['parks']:7.0f} | {s['fetch']:7.0f} | {s['fetch_us']:7.0f} | {s['settled']:8.0f} | {s['miss']:8.0f}"
            )


if __name__ == "__main__":
    main()
