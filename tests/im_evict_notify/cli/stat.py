"""Gaia get-stat (GetStat sub-opcode) fetch + decode for InfiniteMemory.

The cosmos firmware's stepGetStat mirrors every IP's stat layout into the
response: a self-describing header followed by one raw blob per MU instance.

  payload = [GaiaStatDumpHeader][raw slot blobs, in IP order]

GaiaStatDumpHeader (gaia_config.hpp):
  u32 version       # GaiaStatVersion — selects the field layout below
  u32 ipCount
  GaiaStatDumpIpEntry ips[ipCount]:
    u32 groupMask     # core-managed enabled-group bitmask for this IP
    u32 bytesPerSlot  # layout TotalSize (meaningful bytes per instance)
    u32 numInstances
    u32 dataOffset    # byte offset from payload base to this IP's first slot

The header locates each IP's blobs and gives the version; field positions
inside a blob come from gaia_stat_layout.json (generated from gaia_stat_layout.hpp
by cosmos/mu_fw/gaia/tools/gen_gaia_stat_layout.py) keyed by that version, so the
parser follows the firmware layout without hand-editing. Disabled groups read
back as zero.
"""

import csv
import json
import os
import struct
import time
from pathlib import Path

import click

from . import gaia
from . import hio


# Upper bound for the recv buffer: the whole GAIA stat region (gaia_local_map.hpp
# GaiaStatRegionSize). The firmware writes only what it has and stamps the real
# size in the header, which is all we read back.
_GET_STAT_PAYLOAD_CAP = 2 * 1024 * 1024

_HEADER_FIXED_FMT = "<II"  # version, ipCount
_HEADER_FIXED_SIZE = struct.calcsize(_HEADER_FIXED_FMT)
_IP_ENTRY_FMT = "<IIII"  # groupMask, bytesPerSlot, numInstances, dataOffset
_IP_ENTRY_SIZE = struct.calcsize(_IP_ENTRY_FMT)

# Per-version field layout, generated from gaia_stat_layout.hpp. Each version maps
# to {ip_order: [...], ips: {ip: [{name, group, type, size, offset}, ...]}}.
_LAYOUT_PATH = Path(__file__).resolve().parent / "gaia_stat_layout.json"


def _load_layouts():
    try:
        data = json.loads(_LAYOUT_PATH.read_text())
    except (OSError, ValueError):
        return {}
    return {int(v): entry for v, entry in data.get("versions", {}).items()}


# GaiaStatVersion -> layout entry. Empty if the manifest is missing/unreadable.
_GAIA_STAT_LAYOUTS = _load_layouts()


def fetch_get_stat(device_id):
    """Issue GetStat and return (payload, info)."""
    return gaia.recv_var(
        device_id, gaia.SystemSubOpcode.GET_STAT, _GET_STAT_PAYLOAD_CAP
    )


def parse_header(payload):
    """Return (version, [ip entry dicts]) from the dump header."""
    version, ip_count = struct.unpack_from(_HEADER_FIXED_FMT, payload, 0)
    ips = []
    for k in range(ip_count):
        off = _HEADER_FIXED_SIZE + k * _IP_ENTRY_SIZE
        mask, bytes_per_slot, num_instances, data_offset = struct.unpack_from(
            _IP_ENTRY_FMT, payload, off
        )
        ips.append({
            "group_mask": mask,
            "bytes_per_slot": bytes_per_slot,
            "num_instances": num_instances,
            "data_offset": data_offset,
        })
    return version, ips


def _enabled_groups(mask):
    groups = [b for b in range(32) if mask & (1 << b)]
    return ",".join(str(g) for g in groups) if groups else "-"


def _read_field(payload, base, stride, count, offset, size, reduce_="sum"):
    """Combine one field across ``count`` instances (LE, arbitrary precision).

    Counters are summed. A field the manifest marks ``reduce: "max"`` is a per-MU high-water mark
    or a value replicated on every MU, and must be reduced by max instead -- summing 16-32
    instances would report a number no single MU ever held. Manifests predating the key carry no
    ``reduce``, so the default keeps their behaviour.
    """
    values = (
        int.from_bytes(payload[base + inst * stride + offset:][:size], "little")
        for inst in range(count)
    )
    return max(values, default=0) if reduce_ == "max" else sum(values)


def print_get_stat(payload, info, show_all=False, prev=None):
    """Print the dump: per-IP header summary then the combined counters.

    The header's version selects the field layout. An unknown version means the
    firmware layout moved past what this build knows, so decoding would lie.

    By default only enabled groups (per each IP's group_mask) are listed; pass
    show_all=True to include disabled groups (they read back as zero).
    """
    version, ips = parse_header(payload)
    layout = _GAIA_STAT_LAYOUTS.get(version)
    if layout is None:
        raise RuntimeError(
            f"unsupported get-stat version {version} "
            f"(known: {sorted(_GAIA_STAT_LAYOUTS)})"
        )
    ip_order = layout["ip_order"]
    ip_fields = layout["ips"]
    for k, ip in enumerate(ips):
        ip["name"] = ip_order[k] if k < len(ip_order) else f"ip{k}"

    status = info["status"]
    click.echo(
        f"[ OK ] im get-stat recv (status={hio.status_name(status)} "
        f"0x{status:x}, {info['data_size']} B)"
    )
    for ip in ips:
        click.echo(
            f"  {ip['name']:<11}: groups={{{_enabled_groups(ip['group_mask'])}}} "
            f"(mask=0x{ip['group_mask']:x}) instances={ip['num_instances']} "
            f"bytes/slot={ip['bytes_per_slot']}"
        )

    cur = {}
    for ip in ips:
        fields = ip_fields.get(ip["name"])
        if not fields:
            continue
        mask = ip["group_mask"]
        click.echo("")
        click.echo(f"{ip['name']} (over {ip['num_instances']} instances; "
                   f"counters summed, max fields reduced by max):")
        # Group for display: by group, then by in-blob offset.
        for f in sorted(fields, key=lambda x: (x["group"], x["offset"])):
            if not show_all and not (mask >> f["group"]) & 1:
                continue
            value = _read_field(
                payload, ip["data_offset"], ip["bytes_per_slot"],
                ip["num_instances"], f["offset"], f["size"], f.get("reduce", "sum"),
            )
            key = f"{ip['name']}.{f['name']}"
            cur[key] = value
            shown = str(value)
            old = prev.get(key) if prev is not None else None
            if old is not None and old != value:
                if value > old:
                    shown = click.style(f"{value} ▲{value - old}", fg="bright_green", bold=True)
                else:
                    shown = click.style(f"{value} ▼{old - value}", fg="bright_red", bold=True)
            click.echo(f"  [g{f['group']}] {f['name']:<24}: {shown}")
    return cur


def follow_get_stat(fetch, show_all=False, interval=1.0):
    """Watch-style follow: re-fetch and re-render the whole dump every
    ``interval`` seconds (full refresh). ``fetch`` returns (payload, info).
    Ctrl+C to stop."""
    prev = None
    try:
        while True:
            try:
                payload, info = fetch()
            except RuntimeError as e:
                click.echo(f"(poll failed: {e})")
                time.sleep(interval)
                continue
            click.clear()
            prev = print_get_stat(payload, info, show_all=show_all, prev=prev)
            click.echo("")
            click.echo(f"(refreshing every {interval}s — changed values highlighted — Ctrl+C to stop)")
            time.sleep(interval)
    except KeyboardInterrupt:
        click.echo("")


def _value_row(payload):
    """Return (ordered column keys, {key: value}) for ALL fields (key 'ip.field').

    All fields are emitted regardless of enabled state so the CSV columns stay
    fixed across a run (disabled groups read back as zero)."""
    version, ips = parse_header(payload)
    layout = _GAIA_STAT_LAYOUTS.get(version)
    if layout is None:
        raise RuntimeError(
            f"unsupported get-stat version {version} "
            f"(known: {sorted(_GAIA_STAT_LAYOUTS)})"
        )
    ip_order = layout["ip_order"]
    ip_fields = layout["ips"]
    keys, vals = [], {}
    for k, ip in enumerate(ips):
        name = ip_order[k] if k < len(ip_order) else f"ip{k}"
        fields = ip_fields.get(name)
        if not fields:
            continue
        for f in sorted(fields, key=lambda x: (x["group"], x["offset"])):
            key = f"{name}.{f['name']}"
            keys.append(key)
            vals[key] = _read_field(
                payload, ip["data_offset"], ip["bytes_per_slot"],
                ip["num_instances"], f["offset"], f["size"], f.get("reduce", "sum"),
            )
    return keys, vals


def dump_get_stat(fetch, path, interval=1.0):
    """Append one CSV row per poll (timestamp + every stat) to ``path`` every
    ``interval`` seconds, for later graphing. ``fetch`` returns (payload, info).
    A header is written only when starting a fresh/empty file. Ctrl+C to stop."""
    need_header = (not os.path.exists(path)) or os.path.getsize(path) == 0
    click.echo(f"Dumping stats to {path} every {interval}s (Ctrl+C to stop)...")
    rows = 0
    try:
        with open(path, "a", newline="") as fh:
            writer = csv.writer(fh)
            while True:
                try:
                    payload, _ = fetch()
                except RuntimeError as e:
                    click.echo(f"(poll failed: {e})")
                    time.sleep(interval)
                    continue
                keys, vals = _value_row(payload)
                if need_header:
                    writer.writerow(["timestamp"] + keys)
                    need_header = False
                writer.writerow([f"{time.time():.3f}"] + [vals[k] for k in keys])
                fh.flush()
                rows += 1
                click.echo(f"\r  {rows} rows written", nl=False)
                time.sleep(interval)
    except KeyboardInterrupt:
        click.echo("")
