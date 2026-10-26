#!/usr/bin/env python3
"""Use-case cells from appbench kv-session logs written through cellmark.py: per cell throughput, read and
update p99, machine-wide busy cores, page-ins and SSD traffic.
Usage: uccompare.py [--cpus N] <log>...  (N: CPUs of the measured host, default 256)"""

import argparse
import json


def cells(path, ncpu):
    marks, results, pending = {}, {}, None
    for line in open(path):
        if line.startswith("APPMARK "):
            m = json.loads(line.split(" ", 1)[1])
            marks.setdefault(m["label"], {})[m["phase"]] = m
            if m["phase"] == "pre":
                pending = m["label"]
        elif line.startswith("APPRESULT ") and pending:
            results[pending] = json.loads(line.split(" ", 1)[1])
    for lab, r in results.items():
        a, b = marks[lab].get("pre"), marks[lab].get("post")
        if not a or not b:
            continue
        dt = r["wall_s"]

        def rel(k):
            return (b["rel"].get(k, 0) - a["rel"].get(k, 0)) / dt

        def im(k):
            return int(b["mxim"].get(k, 0)) - int(a["mxim"].get(k, 0))

        busy = r.get("host_busy_pct", 0) * ncpu / 100
        upd = r.get("update_lat_us") or {}
        yield (
            lab,
            {
                "GB/s": r["gb_per_s"],
                "kops": r["ops_per_s"] / 1000,
                "rd_p99": r["lat_us"].get("p99", 0) / 1000,
                "up_p99": (upd.get("p99") or 0) / 1000,
                "max_ms": max(r["lat_us"].get("max") or 0, upd.get("max") or 0) / 1000,
                "busy": busy,
                "core/GBs": busy / r["gb_per_s"] if r["gb_per_s"] else 0,
                "pagein/s": im("fetches") / dt,
                "SsdR GiB": rel("SsdRead") / 2048,
                "SsdW GiB": rel("SsdWrite") / 2048,
                "errors": r.get("errors", 0) + r.get("fetch_errors", 0),
            },
        )


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--cpus", type=int, default=256)
    ap.add_argument("logs", nargs="+")
    a = ap.parse_args()
    rows = [(lab, v) for path in a.logs for lab, v in cells(path, a.cpus)]
    if not rows:
        return
    hdr = list(rows[0][1])
    print(f"{'cell':>14} " + " ".join(f"{h:>9}" for h in hdr))
    for lab, v in rows:
        print(
            f"{lab:>14} "
            + " ".join(
                f"{v[h]:9.2f}" if isinstance(v[h], float) else f"{v[h]:>9}" for h in hdr
            )
        )


if __name__ == "__main__":
    main()
