"""GaiaCommand wrappers — IM-specific layer on top of the local ``hio`` module.

Knows the GaiaCommandPayload_t wire format (gaia_config.hpp), including the magic
that guards it against firmware skew, and the GaiaSubOpcode / SystemSubOpcode /
PrefetchMode enums. The actual ioctl plumbing
lives in ``commands.infiniteMemory.hio``.
"""

import struct

from . import hio


# GaiaSubOpcode (hio_config.hpp): payload word 0, the opcode the Gaia router
# dispatches on. 0..12 are cache ops over a device byte range; SystemCommand (63)
# is the one value that puts a SystemSubOpcode in arg1 instead.
class GaiaSubOpcode:
    PREFETCH = 0
    PIN = 1
    UNPIN = 2
    UNPIN_ALL = 3
    PRELOAD = 4
    UNLOAD = 5
    UNLOAD_ALL = 6
    UNMAP = 7
    PREFETCH_SYNC = 8
    COMMIT = 9
    COMMIT_SYNC = 10
    FLUSH = 11
    DISCARD = 12
    SYSTEM_COMMAND = 63


# SystemSubOpcode (gaia_config.hpp). Dispatched by HioProtocolManager when
# protocolOpcode == GaiaCommand and gaiaOpcode == SystemCommand.
class SystemSubOpcode:
    SET_PREFETCH_MODE = 0   # send-only; value = PrefetchMode
    SET_DRAM_CAPACITY = 1   # send-only; value = capacity (bytes)
    SET_SSD_CAPACITY = 2    # send-only; value = capacity (bytes)
    GET_SMART = 3           # recv; payload = GaiaSmartLog (4 KB)
    SET_DIRTY_WRITEBACK_THRESHOLD = 4  # send-only; value = percent in bits [7:0] (0..100)
    GET_STAT = 5            # recv; payload = GaiaStatDumpHeader + per-IP slot blobs
    SET_STAT_ENABLE = 6     # send-only; value = StatEnableValue (see encode_stat_enable)
    SET_AUTO_PREFETCH_GROUP_COUNT = 7  # send-only; value = group count in bits [7:0] (1..MaxAutoPrefetchGroupCountPerFault)
    SET_DETECTOR_THRESHOLD = 8  # send-only; value = detector SEQ-decision threshold (per-mille, 0..1000)
    SET_PIN_THRESHOLD = 9   # send-only; value = pin ceiling (bytes, device-wide, multiple of RelocatorIntervalBytes = 16 MiB), or u64 max = ceiling / u64 max - 1 = default
    SET_PRELOAD_CAPACITY = 10   # send-only; vendor; value = size bytes (sentinels below)
    SET_COMPUTABLE = 11     # send-only; vendor; value = 0/1


# Request sentinels shared by the value-setting sub-opcodes that accept them
# (SetPinThreshold, SetPreloadCapacity — gaia_config.hpp). The device resolves both
# against its current geometry instead of reading them as a byte count, so
# neither can be spelled as a plain number.
REQUEST_MAX = 0xFFFFFFFFFFFFFFFF       # UINT64_MAX
REQUEST_DEFAULT = 0xFFFFFFFFFFFFFFFE   # UINT64_MAX - 1
REQUEST_MAX_BYTES = REQUEST_DEFAULT - 1  # largest value spellable as a number


# GaiaStatIp (gaia_config.hpp): SetStatEnable target index.
GAIA_STAT_IP_BY_NAME = {
    "relocator": 0,
    "storage": 1,
    "arbitrator": 2,
    "router": 3,
    "detector": 4,
    "scheduler": 5,
}


def encode_stat_enable(ip_index, group_mask, enable):
    """Pack a StatEnableValue (gaia_config.hpp): groupMask[31:0], ipIndex[39:32],
    enable[40]."""
    return ((group_mask & 0xFFFFFFFF)
            | ((ip_index & 0xFF) << 32)
            | ((1 if enable else 0) << 40))


# PrefetchMode (gaia_config.hpp).
class PrefetchMode:
    NEVER = 0
    ALWAYS = 1
    AUTO = 2


PREFETCH_MODE_BY_NAME = {
    "never": PrefetchMode.NEVER,
    "always": PrefetchMode.ALWAYS,
    "auto": PrefetchMode.AUTO,
}


# gaia_config.hpp. Firmware that does not see both answers VersionMismatch instead of reading
# the payload as an opcode; bump GAIA_COMMAND_VERSION whenever the layout or opcode values change.
GAIA_COMMAND_MAGIC = 0x494D    # InfiniteMemory; dumps as "MI"
GAIA_COMMAND_VERSION = 2


def _payload(gaia_opcode, arg1, arg2):
    """Encode a GaiaCommandPayload_t: magic (u16) + version (u16) + gaiaOpcode (u32)
    + arg1 (u64) + arg2 (u64). Padding up to 64 B is added by the hio layer."""
    return struct.pack("<HHIQQ", GAIA_COMMAND_MAGIC, GAIA_COMMAND_VERSION,
                       gaia_opcode, arg1, arg2)


def _sq_command(sub_opcode, value):
    """SystemCommand payload: the SystemSubOpcode lands in arg1, its value in arg2."""
    return _payload(GaiaSubOpcode.SYSTEM_COMMAND, sub_opcode, value)


def send(device_id, sub_opcode, value=0):
    """Send a GaiaCommand sub-opcode (no return payload)."""
    return hio.send_inner(
        device_id, hio.HioProtocolOpcode.GAIA_COMMAND, _sq_command(sub_opcode, value)
    )


def recv(device_id, sub_opcode, payload_size, value=0):
    """Recv a GaiaCommand sub-opcode returning ``payload_size`` bytes."""
    return hio.recv_inner(
        device_id, hio.HioProtocolOpcode.GAIA_COMMAND,
        _sq_command(sub_opcode, value), payload_size,
    )


def recv_var(device_id, sub_opcode, max_payload_size, value=0):
    """Recv a GaiaCommand sub-opcode whose response size the firmware decides
    (host posts a max-size buffer; the device reports the actual size)."""
    return hio.recv_inner_var(
        device_id, hio.HioProtocolOpcode.GAIA_COMMAND,
        _sq_command(sub_opcode, value), max_payload_size,
    )


def send_cache(device_id, gaia_opcode, addr=0, size=0):
    """Send a Gaia cache op (GaiaSubOpcode 0..12) over a device byte range.

    The device-wide ops (UnpinAll / UnloadAll) take no range and leave both
    args 0. Fire-and-forget ops (Prefetch / Commit) are acked at enqueue, so a
    Success here means "queued", not "finished" — use the *Sync variants to
    wait for completion.
    """
    return hio.send_inner(
        device_id, hio.HioProtocolOpcode.GAIA_COMMAND,
        _payload(gaia_opcode, addr, size),
    )
