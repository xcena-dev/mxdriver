# IM performance tools (real hardware)

`appbench` key-value workloads over private anonymous THPs on the IM NUMA node,
compared with IM on (S, soft page fault) and off (S0, direct device access).

## Build appbench

`appbench` comes from `xcena-dev/imdaxfs` at `ef7952c`; the patch adds
`--region anon:<node>` and the `kv-session` command.

```
git -C imdaxfs checkout ef7952c
git -C imdaxfs apply "$PWD/appbench-anon-session.patch"
g++ -std=c++20 -O3 -march=native -pthread -Wall -o appbench imdaxfs/bench/apps/appbench.cpp
```

## Run (root)

```
python3 nm_session.py --node 5 --gib 1024 --threads 16,64,120 \
    --mark "python3 $PWD/cellmark.py" > nm.session
MXDEV=0 ./perf_run.sh nm.session nm.log
./chain.sh <dir> a b:64-111      # <dir>/a.session, then <dir>/b.session on CPUs 64-111
```

- A session runs in one `appbench` process, so every cell reads what its `kv-load`
  wrote. A line starting with `!` runs in the shell and must succeed.
- `cellmark.py pre <label> <S|S0>` and `cellmark.py post <label>` bracket each cell.
- `perf_run.sh` keeps the IM poller and the unbound workqueues on `IM_CPUS`
  (default `126,127,254,255`) and the session on `READER_CPUS`.
- Turn IM off and on between sessions. A parked folio whose process has exited
  stays allocated until IM is disabled.

## Read the results

| Script | Shows |
|---|---|
| `nmsummary.py` | per pattern, thread count and arm: throughput, p99, charged CPU |
| `t1compare.py` | a baseline grid against separate S and S0 sessions |
| `cellstat.py` | per cell: parks, verdict batches, writeback, page-ins, device misses |
| `uccompare.py` | per cell with updates: read and update p99, machine-wide busy cores, SSD traffic |
| `fwrate.py` | firmware counters per second; `FWRATE_ENABLE_ALL=1` enables every stat group |
| `devstat.py` | relocator and node memory counters, fsynced so they survive a host reset |
