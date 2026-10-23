"""Generic HIO protocol primitives (HioInnerCommand send/recv).

Protocol-agnostic — knows nothing about GaiaCommand, NvmeAdminPassthrough, or
any future HioProtocolOpcode. Callers compose the SQ-command body and pick
the opcode; this module just builds the 4 KB HioInnerCommand, ships it via
``xif::sendProtocol`` / ``xif::recvProtocol``, and reports back the echoed
header.

Layout mirrors:
  emulator/backend/sdk/metisx/cosmos/cosmos/core_fw/config/mx1p/hio_config.hpp
"""

import itertools
import mmap
import struct
import threading

import pyxif


CMD_PAGE_SIZE = 4096
SQ_COMMAND_SIZE = 64
HIO_INNER_HEADER_SIZE = 32

# Soft cap on the recv payload. The firmware-side responses currently top out
# at 4 KB (GaiaSmartLog), so 16 MiB is well above any expected use while still
# bounding the mmap allocation a misbehaving caller could request.
MAX_RECV_PAYLOAD_SIZE = 16 * 1024 * 1024


class HioProtocolOpcode:
    PROTOCOL_NOP = 0
    NVME_ADMIN_PASSTHROUGH = 1
    GAIA_COMMAND = 2
    UART_CONTROL = 4


# HioProtocolStatus (hio_config.hpp). Echoed back in the inner header.
class HioProtocolStatus:
    SUCCESS = 0
    NOT_IMPLEMENTED = 1
    INVALID_OPCODE = 2
    INVALID_VALUE = 3
    ERROR = 4
    VERSION_MISMATCH = 5
    RESOURCE_EXHAUST = 6


_STATUS_NAMES = {
    HioProtocolStatus.SUCCESS: "Success",
    HioProtocolStatus.NOT_IMPLEMENTED: "NotImplemented",
    HioProtocolStatus.INVALID_OPCODE: "InvalidOpcode",
    HioProtocolStatus.INVALID_VALUE: "InvalidValue",
    HioProtocolStatus.ERROR: "Error",
    HioProtocolStatus.VERSION_MISMATCH: "VersionMismatch",
    HioProtocolStatus.RESOURCE_EXHAUST: "ResourceExhaust",
}


def status_name(status):
    return _STATUS_NAMES.get(status, f"Unknown(0x{status:x})")


# Monotonic tag generator. Using PID alone makes every request from a single
# process carry the same tag, which would make any future request/response
# correlation logic trivially "match". A process-local counter is unique per
# call without the entropy of urandom and is cheap.
_tag_counter = itertools.count(1)
_tag_lock = threading.Lock()


def _new_tag():
    with _tag_lock:
        return next(_tag_counter) & 0xFFFFFFFFFFFFFFFF


def _build_inner_command(protocol_opcode, sq_command, tag, data_size):
    """Compose a 4 KB HioInnerCommand. ``sq_command`` is up to 64 B; shorter
    bodies are zero-padded."""
    if len(sq_command) > SQ_COMMAND_SIZE:
        raise ValueError(
            f"sq_command too large: {len(sq_command)} > {SQ_COMMAND_SIZE}"
        )
    buf = bytearray(CMD_PAGE_SIZE)
    # HioInnerHeader (32 B): protocolOpcode, flags, dataSize, status,
    # reserved0, tag, reserved1.
    struct.pack_into(
        "<HHIIIQQ", buf, 0,
        protocol_opcode, 0, data_size, 0, 0, tag, 0,
    )
    buf[HIO_INNER_HEADER_SIZE:HIO_INNER_HEADER_SIZE + len(sq_command)] = sq_command
    return buf


def _read_header(buf):
    """Return (protocol_opcode, data_size, status, tag) from HioInnerHeader."""
    protocol_opcode, _flags, data_size, status, _r0, tag, _r1 = struct.unpack_from(
        "<HHIIIQQ", buf, 0,
    )
    return protocol_opcode, data_size, status, tag


def _raise_if_error(status, protocol_opcode):
    if status == HioProtocolStatus.SUCCESS:
        return
    # The device rejected the command body outright: it and this cli disagree on the
    # payload layout, so say that instead of blaming the command.
    if status == HioProtocolStatus.VERSION_MISMATCH:
        raise RuntimeError(
            "device rejected the command payload as the wrong version — this cli and the "
            "device firmware are out of sync; update whichever is older "
            f"(status={status_name(status)} 0x{status:x}, "
            f"protocol_opcode=0x{protocol_opcode:x})"
        )
    raise RuntimeError(
        f"device returned error status {status_name(status)} "
        f"(0x{status:x}) for protocol_opcode=0x{protocol_opcode:x}"
    )


def send_inner(device_id, protocol_opcode, sq_command=b""):
    """Send an arbitrary HioInnerCommand (no return payload).

    Returns:
        dict with keys 'tag', 'recv_tag', 'status', 'data_size'.

    Raises:
        RuntimeError on ioctl failure or non-Success device status.
    """
    buf = mmap.mmap(-1, CMD_PAGE_SIZE)
    try:
        tag = _new_tag()
        buf[:CMD_PAGE_SIZE] = bytes(
            _build_inner_command(protocol_opcode, sq_command, tag, 0)
        )
        if not pyxif.send_protocol(device_id, buf, CMD_PAGE_SIZE):
            raise RuntimeError("pyxif.send_protocol returned False")
        _opc, data_size, status, recv_tag = _read_header(buf)
        _raise_if_error(status, protocol_opcode)
        if recv_tag != tag:
            raise RuntimeError(
                f"tag mismatch: sent 0x{tag:x}, received 0x{recv_tag:x} "
                f"— possible stale or misrouted response"
            )
        return {"tag": tag, "recv_tag": recv_tag,
                "status": status, "data_size": data_size}
    finally:
        buf.close()


def recv_inner_var(device_id, protocol_opcode, sq_command, max_payload_size):
    """Recv a variable-length HIO response.

    Like :func:`recv_inner`, but for responses whose exact size the host cannot
    know in advance: post a ``max_payload_size`` buffer and return exactly the
    ``data_size`` bytes the device reports in the echoed header (the firmware
    writes only what it has and stamps the real size). Use when the firmware
    computes the payload size, e.g. the Gaia get-stat response.

    Returns:
        tuple (payload: bytes, info: dict). ``info`` keys: 'tag', 'recv_tag',
        'status', 'data_size'.

    Raises:
        ValueError if ``max_payload_size`` is out of range.
        RuntimeError on ioctl failure, non-Success status, tag mismatch, or a
        device-reported size outside (0, max_payload_size].
    """
    if max_payload_size <= 0 or max_payload_size > MAX_RECV_PAYLOAD_SIZE:
        raise ValueError(
            f"max_payload_size {max_payload_size} out of range "
            f"(1 .. {MAX_RECV_PAYLOAD_SIZE})"
        )
    total_size = CMD_PAGE_SIZE + max_payload_size
    buf = mmap.mmap(-1, total_size)
    try:
        tag = _new_tag()
        buf[:CMD_PAGE_SIZE] = bytes(
            _build_inner_command(protocol_opcode, sq_command, tag, max_payload_size)
        )
        if not pyxif.recv_protocol(device_id, buf, total_size):
            raise RuntimeError("pyxif.recv_protocol returned False")
        _opc, data_size, status, recv_tag = _read_header(buf)
        _raise_if_error(status, protocol_opcode)
        if recv_tag != tag:
            raise RuntimeError(
                f"tag mismatch: sent 0x{tag:x}, received 0x{recv_tag:x} "
                f"— possible stale or misrouted response"
            )
        if not 0 < data_size <= max_payload_size:
            raise RuntimeError(
                f"device reported data_size {data_size} B outside "
                f"(0, {max_payload_size}] — cannot trust the response"
            )
        payload = bytes(buf[CMD_PAGE_SIZE:CMD_PAGE_SIZE + data_size])
        info = {"tag": tag, "recv_tag": recv_tag,
                "status": status, "data_size": data_size}
        return payload, info
    finally:
        buf.close()


def recv_inner(device_id, protocol_opcode, sq_command, payload_size):
    """Recv an arbitrary HioInnerCommand whose response carries a
    ``payload_size``-byte payload immediately after the 4 KB command page.

    Returns:
        tuple (payload: bytes, info: dict). ``info`` keys: 'tag', 'recv_tag',
        'status', 'data_size'.

    Raises:
        ValueError if ``payload_size`` is out of range.
        RuntimeError on ioctl failure or non-Success device status.
    """
    if payload_size <= 0 or payload_size > MAX_RECV_PAYLOAD_SIZE:
        raise ValueError(
            f"payload_size {payload_size} out of range "
            f"(1 .. {MAX_RECV_PAYLOAD_SIZE})"
        )
    total_size = CMD_PAGE_SIZE + payload_size
    buf = mmap.mmap(-1, total_size)
    try:
        tag = _new_tag()
        buf[:CMD_PAGE_SIZE] = bytes(
            _build_inner_command(protocol_opcode, sq_command, tag, payload_size)
        )
        if not pyxif.recv_protocol(device_id, buf, total_size):
            raise RuntimeError("pyxif.recv_protocol returned False")
        _opc, data_size, status, recv_tag = _read_header(buf)
        _raise_if_error(status, protocol_opcode)
        if recv_tag != tag:
            raise RuntimeError(
                f"tag mismatch: sent 0x{tag:x}, received 0x{recv_tag:x} "
                f"— possible stale or misrouted response"
            )
        if data_size < payload_size:
            raise RuntimeError(
                f"short response: device returned {data_size} B, "
                f"expected {payload_size} B (zero-padded mmap would otherwise "
                f"look like valid payload)"
            )
        payload = bytes(buf[CMD_PAGE_SIZE:CMD_PAGE_SIZE + payload_size])
        info = {"tag": tag, "recv_tag": recv_tag,
                "status": status, "data_size": data_size}
        return payload, info
    finally:
        buf.close()
