#!/usr/bin/env python3
"""Per-cell breakdown of an appbench kv-session log written through cellmark.py.
Usage: cellstat.py <log>"""

import json
import sys


def delta_avg(a, b, avg_key, count_key):
    """The average over the cell from two cumulative (average, count) samples."""
    ca, cb = int(a.get(count_key, 0)), int(b.get(count_key, 0))
    if cb <= ca:
        return 0.0
    return (float(b.get(avg_key, 0)) * cb - float(a.get(avg_key, 0)) * ca) / (cb - ca)


def main():
    marks, results, order = {}, {}, []
    pending = None
    for line in open(sys.argv[1]):
        if line.startswith("APPMARK "):
            m = json.loads(line.split(" ", 1)[1])
            marks.setdefault(m["label"], {})[m["phase"]] = m
            if m["phase"] == "pre":
                pending = m["label"]
                order.append(pending)
        elif line.startswith("APPRESULT ") and pending:
            results[pending] = json.loads(line.split(" ", 1)[1])
    hdr = (
        "cell",
        "GB/s",
        "p99ms",
        "rdrCPU",
        "IMcpu",
        "poller",
        "parks/s",
        "evt/s",
        "batch",
        "b_err",
        "settled",
        "wb_us",
        "fetch/s",
        "fetch_us",
        "Miss/s",
    )
    print(" ".join(f"{h:>9}" for h in hdr))
    for lab in order:
        m, r = marks.get(lab, {}), results.get(lab)
        if "pre" not in m or "post" not in m or r is None or lab == "load":
            continue
        a, b = m["pre"], m["post"]
        dt = r["wall_s"]
        xa, xb = a["mxim"], b["mxim"]

        def d(k):
            return int(xb.get(k, 0)) - int(xa.get(k, 0))

        im_cpu = (
            (b["im_cpu_busy_jiffies"] - a["im_cpu_busy_jiffies"]) / 100 / dt
            if a["im_cpu_busy_jiffies"] is not None
            else 0
        )
        poller = (b["poller_jiffies"] - a["poller_jiffies"]) / 100 / dt
        row = (
            lab,
            f"{r['gb_per_s']:.2f}",
            f"{r['lat_us'].get('p99', 0) / 1000:.0f}",
            f"{r['cpu_s'] / dt:.1f}",
            f"{im_cpu:.2f}",
            f"{poller:.2f}",
            f"{d('ack_parked') / dt:.0f}",
            f"{d('events') / dt:.0f}",
            str(d("reply_batch")),
            str(d("reply_batch_err")),
            str(d("reply_settled")),
            f"{delta_avg(xa, xb, 'wb_us_avg', 'ack_parked'):.0f}",
            f"{d('fetches') / dt:.0f}",
            f"{delta_avg(xa, xb, 'fetch_us_avg', 'fetches'):.0f}",
            f"{(b['rel']['Miss'] - a['rel']['Miss']) / dt:.0f}",
        )
        print(" ".join(f"{c:>9}" for c in row))


if __name__ == "__main__":
    main()
