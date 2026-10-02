#!/usr/bin/env python3
"""Host recorder for the ESP32 LD2410C MVP (spec R7).

Sub-commands:
  record HOST [--port 5410] [-o FILE]   record frames from the ESP32 to a .ldrec file
  export-csv FILE [-o CSV]              decode a recording to CSV
  info FILE                             counts, sequence range, duration, gaps, GPS/IMU/time stats

Recording protocol v3: the device sends typed records (0 LD2410C frame, 1 gps_fix,
2 imu, 3 time_sync) sharing one sequence. Byte layouts (all little-endian) are fixed in
docs/superpowers/plans/2026-10-02-esp32-gps-imu.md and mirrored by esp32/components/core
(C). The decoders are checked against the shared vectors in esp32/tests/vectors. The
recorder writes .ldrec v3 and reads v2 and v3 files. Stdlib only. The pure
encode/decode functions below do no I/O.
"""

from __future__ import annotations

import argparse
import contextlib
import csv
import logging
import signal
import socket
import struct
import sys
import threading
import time
from bisect import bisect_right
from dataclasses import dataclass
from datetime import datetime, timedelta, UTC
from pathlib import Path
from typing import IO, NamedTuple


log = logging.getLogger("ld2410_rec")

SEQ_MASK = 0xFFFFFFFF
DEFAULT_PORT = 5410
CONNECT_S = 1.0  # connect timeout; a stop request waits at most this long while connecting
POLL_S = 0.2  # blocking I/O is cut into slices of this length so `stop` is seen promptly

REQ_MAGIC = b"LDRQ"
BATCH_MAGIC = b"LDRB"
REC_VERSION = 3
REQ_LEN = 12
BATCH_HDR_LEN = 20
REC_HDR_LEN = 15  # seq u32, esp_time_us u64, type u8, len u16
FLAG_GAP = 0x01

# Record types on the wire (batch records)
WIRE_FRAME = 0
WIRE_GPS_FIX = 1
WIRE_IMU = 2
WIRE_TIME_SYNC = 3
GPS_FIX_LEN = 32
IMU_LEN = 18
TIME_SYNC_LEN = 9
GPS_FLAG_TIME = 0x01
GPS_FLAG_DATE = 0x02
GPS_FLAG_POS = 0x04
GPS_FLAG_ALT = 0x08
IMU_STATUS_VALID = 0x01
SOURCE_NAMES = {1: "gps", 2: "sntp"}

FILE_MAGIC = b"LDREC1\x00\x00"
FILE_VERSION = 3
FILE_VERSIONS_READ = (2, 3)
FILE_HDR_LEN = 20
REC_FRAME = 0
REC_GAP = 1
REC_REBOOT = 2
REC_GPS = 3
REC_IMU = 4
REC_TIME_SYNC = 5
REC_UNKNOWN = 6  # wire type this version does not know, kept as raw bytes
_V2_TYPES = (REC_FRAME, REC_GAP, REC_REBOOT)
_V3_TYPES = (*_V2_TYPES, REC_GPS, REC_IMU, REC_TIME_SYNC, REC_UNKNOWN)
_WIRE_TO_FILE = {
    WIRE_FRAME: REC_FRAME,
    WIRE_GPS_FIX: REC_GPS,
    WIRE_IMU: REC_IMU,
    WIRE_TIME_SYNC: REC_TIME_SYNC,
}

GATES = 9
DATA_HEADER = b"\xf4\xf3\xf2\xf1"
DATA_FOOTER = b"\xf8\xf7\xf6\xf5"
CMD_HEADER = b"\xfd\xfc\xfb\xfa"
CMD_FOOTER = b"\x04\x03\x02\x01"
ACK_FLAG = 0x0100
_BASIC_LEN = 11
_ENG_FIXED_LEN = _BASIC_LEN + 2 + 2 * GATES
_TAIL = b"\x55\x00"

class ProtocolError(ValueError):
    """Malformed bytes on the recording protocol or in a .ldrec file."""

class FrameError(ValueError):
    """A raw LD2410C frame that cannot be decoded."""

class PayloadError(ValueError):
    """A gps_fix / imu / time_sync payload of the wrong size or with nonzero reserved bytes."""

# --------------------------------------------------------------------------
# LD2410C frame decoder (mirrors esp32/components/core/ld2410_frame.c)
# --------------------------------------------------------------------------

@dataclass(frozen=True)
class LdData:
    data_type: int
    engineering: int
    target_state: int
    moving_dist_cm: int
    moving_energy: int
    still_dist_cm: int
    still_energy: int
    detect_dist_cm: int
    max_moving_gate: int
    max_still_gate: int
    moving_gate_energy: tuple[int, ...]
    still_gate_energy: tuple[int, ...]

@dataclass(frozen=True)
class LdAck:
    cmd: int  # command word with the ACK flag removed
    status: int
    ok: bool

def decode_payload(p: bytes) -> LdData:
    """Decode a data-frame payload (type byte .. 55 00 tail), like ld_frame_decode()."""
    n = len(p)
    if n < _BASIC_LEN + 2:
        raise FrameError(f"payload too short ({n} bytes)")
    dtype = p[0]
    if dtype not in (1, 2):
        raise FrameError(f"bad data type 0x{dtype:02x}")
    if dtype == 1 and n < _ENG_FIXED_LEN + 2:
        raise FrameError(f"engineering payload too short ({n} bytes)")
    if p[1] != 0xAA:
        raise FrameError("bad marker (expected 0xAA)")
    if p[n - 2 : n] != _TAIL:
        raise FrameError("bad tail (expected 55 00)")
    if p[2] > 3:
        raise FrameError(f"bad target state {p[2]}")
    mv, mv_e, st, st_e, det = struct.unpack_from("<HBHBH", p, 3)
    eng = dtype == 1
    return LdData(
        data_type=dtype,
        engineering=int(eng),
        target_state=p[2],
        moving_dist_cm=mv,
        moving_energy=mv_e,
        still_dist_cm=st,
        still_energy=st_e,
        detect_dist_cm=det,
        max_moving_gate=p[11] if eng else 0,
        max_still_gate=p[12] if eng else 0,
        moving_gate_energy=tuple(p[13 : 13 + GATES]) if eng else (0,) * GATES,
        still_gate_energy=tuple(p[13 + GATES : 13 + 2 * GATES]) if eng else (0,) * GATES,
    )

def decode_ack_payload(p: bytes) -> LdAck:
    if len(p) < 4:
        raise FrameError(f"ACK payload too short ({len(p)} bytes)")
    word, status = struct.unpack_from("<HH", p, 0)
    if not word & ACK_FLAG:
        raise FrameError("ACK flag not set")
    return LdAck(cmd=word & ~ACK_FLAG & 0xFFFF, status=status, ok=status == 0)

def split_frame(raw: bytes) -> tuple[str, bytes]:
    """Check header/length/footer of one whole frame; return (kind, payload)."""
    if len(raw) < 10:
        raise FrameError(f"frame too short ({len(raw)} bytes)")
    head, foot = raw[:4], raw[-4:]
    if head == DATA_HEADER and foot == DATA_FOOTER:
        kind = "data"
    elif head == CMD_HEADER and foot == CMD_FOOTER:
        kind = "cmd"
    else:
        raise FrameError("bad frame header/footer")
    (plen,) = struct.unpack_from("<H", raw, 4)
    if plen != len(raw) - 10:
        raise FrameError(
            f"length field {plen} does not match frame ({len(raw) - 10} payload bytes)"
        )
    return kind, raw[6:-4]

def decode_frame(raw: bytes) -> LdData | LdAck:
    kind, payload = split_frame(raw)
    return decode_payload(payload) if kind == "data" else decode_ack_payload(payload)

def decode_data_frame(raw: bytes) -> LdData:
    """Decode a stored raw frame that must be a data frame."""
    kind, payload = split_frame(raw)
    if kind != "data":
        raise FrameError("not a data frame")
    return decode_payload(payload)

# --------------------------------------------------------------------------
# Sensor payloads (mirrors esp32/components/core/rec_payload.c)
# --------------------------------------------------------------------------

@dataclass(frozen=True)
class GpsFix:
    utc_unix_ms: int
    lat_e7: int
    lon_e7: int
    alt_cm: int
    speed_cmps: int
    course_cdeg: int
    hdop_x100: int
    sats: int
    fix_quality: int
    flags: int

    @property
    def time_valid(self) -> bool:
        return bool(self.flags & GPS_FLAG_TIME)

    @property
    def date_valid(self) -> bool:
        return bool(self.flags & GPS_FLAG_DATE)

    @property
    def pos_valid(self) -> bool:
        return bool(self.flags & GPS_FLAG_POS)

    @property
    def alt_valid(self) -> bool:
        return bool(self.flags & GPS_FLAG_ALT)

    @property
    def has_fix(self) -> bool:
        return self.pos_valid and self.fix_quality > 0

@dataclass(frozen=True)
class ImuSample:
    acc_mg: tuple[int, int, int]
    gyr_ddps: tuple[int, int, int]
    pitch_cdeg: int
    roll_cdeg: int
    n_samples: int
    status: int

    @property
    def valid(self) -> bool:
        return bool(self.status & IMU_STATUS_VALID)

@dataclass(frozen=True)
class TimeSync:
    utc_unix_us: int
    source: int

_GPS_FMT = "<qiiiHHHBBB3x"
_IMU_FMT = "<6hhhBB"
_SYNC_FMT = "<qB"
assert struct.calcsize(_GPS_FMT) == GPS_FIX_LEN
assert struct.calcsize(_IMU_FMT) == IMU_LEN
assert struct.calcsize(_SYNC_FMT) == TIME_SYNC_LEN

def decode_gps_fix(p: bytes) -> GpsFix:
    if len(p) != GPS_FIX_LEN:
        raise PayloadError(f"gps_fix payload is {len(p)} bytes, expected {GPS_FIX_LEN}")
    if p[29] or p[30] or p[31]:
        raise PayloadError("gps_fix: nonzero reserved bytes")
    return GpsFix(*struct.unpack(_GPS_FMT, p))

def decode_imu(p: bytes) -> ImuSample:
    if len(p) != IMU_LEN:
        raise PayloadError(f"imu payload is {len(p)} bytes, expected {IMU_LEN}")
    v = struct.unpack(_IMU_FMT, p)
    return ImuSample(v[0:3], v[3:6], v[6], v[7], v[8], v[9])

def decode_time_sync(p: bytes) -> TimeSync:
    if len(p) != TIME_SYNC_LEN:
        raise PayloadError(f"time_sync payload is {len(p)} bytes, expected {TIME_SYNC_LEN}")
    return TimeSync(*struct.unpack(_SYNC_FMT, p))

def encode_gps_fix(g: GpsFix) -> bytes:
    """Encode like rec_gps_fix_encode(): speed_cmps saturates at 65535."""
    return struct.pack(
        _GPS_FMT, g.utc_unix_ms, g.lat_e7, g.lon_e7, g.alt_cm, min(max(g.speed_cmps, 0), 0xFFFF),
        g.course_cdeg, g.hdop_x100, g.sats, g.fix_quality, g.flags,
    )

def encode_imu(m: ImuSample) -> bytes:
    return struct.pack(_IMU_FMT, *m.acc_mg, *m.gyr_ddps, m.pitch_cdeg, m.roll_cdeg,
                       m.n_samples, m.status)

def encode_time_sync(t: TimeSync) -> bytes:
    return struct.pack(_SYNC_FMT, t.utc_unix_us, t.source)

# --------------------------------------------------------------------------
# Recording protocol
# --------------------------------------------------------------------------

def encode_request(from_seq: int) -> bytes:
    return struct.pack("<4sB3xI", REQ_MAGIC, REC_VERSION, from_seq & SEQ_MASK)

class BatchHeader(NamedTuple):
    version: int
    flags: int
    first_seq: int
    count: int
    boot_id: int

    @property
    def gap(self) -> bool:
        return bool(self.flags & FLAG_GAP)

class Record(NamedTuple):
    seq: int
    esp_time_us: int
    rtype: int  # wire type: 0 frame, 1 gps_fix, 2 imu, 3 time_sync, others kept raw
    raw: bytes  # payload

def parse_batch_header(buf: bytes) -> BatchHeader:
    if len(buf) < BATCH_HDR_LEN:
        raise ProtocolError(f"short batch header ({len(buf)} bytes)")
    magic, version, flags, res1, first_seq, count, res2, boot_id = struct.unpack_from(
        "<4sBBHIHHI", buf, 0
    )
    if magic != BATCH_MAGIC:
        raise ProtocolError(f"bad batch magic {magic!r}")
    if res1 or res2:
        raise ProtocolError("nonzero reserved field in batch header")
    if version != REC_VERSION:
        raise ProtocolError(f"unsupported batch version {version}")
    if boot_id == 0:
        raise ProtocolError("batch header has boot_id 0")
    return BatchHeader(version, flags, first_seq, count, boot_id)

def parse_records(buf: bytes, count: int) -> tuple[list[Record], int]:
    """Parse `count` records from buf; return (records, bytes consumed)."""
    out: list[Record] = []
    off = 0
    for i in range(count):
        if len(buf) - off < REC_HDR_LEN:
            raise ProtocolError(f"truncated record header (record {i} of {count})")
        seq, esp, rtype, ln = struct.unpack_from("<IQBH", buf, off)
        off += REC_HDR_LEN
        if len(buf) - off < ln:
            raise ProtocolError(f"record {i} length {ln} exceeds data ({len(buf) - off} left)")
        out.append(Record(seq, esp, rtype, bytes(buf[off : off + ln])))
        off += ln
    return out, off

def parse_batch(buf: bytes) -> tuple[BatchHeader, list[Record]]:
    """Parse one whole batch (header + records); trailing bytes are an error."""
    hdr = parse_batch_header(buf)
    recs, used = parse_records(buf[BATCH_HDR_LEN:], hdr.count)
    if BATCH_HDR_LEN + used != len(buf):
        raise ProtocolError("trailing bytes after batch")
    return hdr, recs

def seq_delta(seq: int, expected: int) -> int:
    """Wrap-safe signed distance seq - expected on the 32-bit sequence circle."""
    d = (seq - expected) & SEQ_MASK
    return d - (1 << 32) if d >= 0x80000000 else d

# --------------------------------------------------------------------------
# .ldrec file
# --------------------------------------------------------------------------

@dataclass(frozen=True)
class FrameRec:
    seq: int
    esp_time_us: int
    pc_time_ns: int
    raw: bytes

@dataclass(frozen=True)
class GapRec:
    from_seq: int
    to_seq: int  # missing range is [from_seq, to_seq)
    pc_time_ns: int

    @property
    def missing(self) -> int:
        return (self.to_seq - self.from_seq) & SEQ_MASK

@dataclass(frozen=True)
class RebootRec:
    old_boot_id: int
    new_boot_id: int
    pc_time_ns: int

@dataclass(frozen=True)
class _SensorRec:
    """Common part of the typed sensor records (payload kept as raw bytes)."""

    seq: int
    esp_time_us: int
    pc_time_ns: int
    payload: bytes

@dataclass(frozen=True)
class GpsRec(_SensorRec):
    pass

@dataclass(frozen=True)
class ImuRec(_SensorRec):
    pass

@dataclass(frozen=True)
class TimeSyncRec(_SensorRec):
    pass

@dataclass(frozen=True)
class UnknownRec:
    """A record of a wire type this version does not know: kept, never decoded."""

    seq: int
    esp_time_us: int
    pc_time_ns: int
    wire_type: int
    payload: bytes

FileRec = FrameRec | GapRec | RebootRec | GpsRec | ImuRec | TimeSyncRec | UnknownRec

_SENSOR_CLASSES = {REC_GPS: GpsRec, REC_IMU: ImuRec, REC_TIME_SYNC: TimeSyncRec}

def encode_file_header(created_unix_ns: int) -> bytes:
    return struct.pack("<8sHHQ", FILE_MAGIC, FILE_VERSION, 0, created_unix_ns)

def encode_frame_rec(seq: int, esp_time_us: int, pc_time_ns: int, raw: bytes) -> bytes:
    return struct.pack("<BIQQH", REC_FRAME, seq, esp_time_us, pc_time_ns, len(raw)) + raw

def encode_sensor_rec(
    file_type: int, seq: int, esp_time_us: int, pc_time_ns: int, payload: bytes
) -> bytes:
    """File record of type 3 gps_fix, 4 imu or 5 time_sync (same layout as a frame record)."""
    if file_type not in _SENSOR_CLASSES:
        raise ValueError(f"not a sensor record type: {file_type}")
    return struct.pack("<BIQQH", file_type, seq, esp_time_us, pc_time_ns, len(payload)) + payload

def encode_unknown_rec(
    wire_type: int, seq: int, esp_time_us: int, pc_time_ns: int, payload: bytes
) -> bytes:
    return (
        struct.pack("<BIQQBH", REC_UNKNOWN, seq, esp_time_us, pc_time_ns, wire_type, len(payload))
        + payload
    )

def encode_gap_rec(from_seq: int, to_seq: int, pc_time_ns: int) -> bytes:
    return struct.pack("<BIIQ", REC_GAP, from_seq, to_seq, pc_time_ns)

def encode_reboot_rec(old_boot_id: int, new_boot_id: int, pc_time_ns: int) -> bytes:
    return struct.pack("<BIIQ", REC_REBOOT, old_boot_id, new_boot_id, pc_time_ns)

def encode_wire_record(r: Record, pc_time_ns: int) -> bytes:
    """File bytes for one record received from the device."""
    ftype = _WIRE_TO_FILE.get(r.rtype)
    if ftype is None:
        return encode_unknown_rec(r.rtype, r.seq, r.esp_time_us, pc_time_ns, r.raw)
    if ftype == REC_FRAME:
        return encode_frame_rec(r.seq, r.esp_time_us, pc_time_ns, r.raw)
    return encode_sensor_rec(ftype, r.seq, r.esp_time_us, pc_time_ns, r.raw)

def parse_file(data: bytes, *, strict: bool = True) -> tuple[int, list[FileRec]]:
    """Parse a .ldrec image (version 2 or 3). Returns (created_unix_ns, records).

    A v2 file holds record types 0..2 only (the v2 layouts are unchanged in v3).
    strict=False tolerates a truncated last record (e.g. a recording killed
    mid-write) and returns what was complete.
    """
    if len(data) < FILE_HDR_LEN:
        raise ProtocolError(f"short file header ({len(data)} bytes)")
    magic, version, _res, created = struct.unpack_from("<8sHHQ", data, 0)
    if magic != FILE_MAGIC:
        raise ProtocolError("not a .ldrec file (bad magic)")
    if version not in FILE_VERSIONS_READ:
        raise ProtocolError(f"unsupported .ldrec version {version}")
    known = _V3_TYPES if version == 3 else _V2_TYPES
    recs: list[FileRec] = []
    off = FILE_HDR_LEN
    while off < len(data):
        rtype = data[off]
        try:
            if rtype in (REC_FRAME, *_SENSOR_CLASSES) and rtype in known:
                if len(data) - off < 1 + 22:
                    raise ProtocolError(f"truncated record (type {rtype}) at offset {off}")
                _, seq, esp, pc, ln = struct.unpack_from("<BIQQH", data, off)
                start = off + 23
                if len(data) - start < ln:
                    raise ProtocolError(f"record at offset {off}: length {ln} exceeds data")
                body = bytes(data[start : start + ln])
                if rtype == REC_FRAME:
                    recs.append(FrameRec(seq, esp, pc, body))
                else:
                    recs.append(_SENSOR_CLASSES[rtype](seq, esp, pc, body))
                off = start + ln
            elif rtype == REC_UNKNOWN and rtype in known:
                if len(data) - off < 1 + 23:
                    raise ProtocolError(f"truncated unknown-type record at offset {off}")
                _, seq, esp, pc, wt, ln = struct.unpack_from("<BIQQBH", data, off)
                start = off + 24
                if len(data) - start < ln:
                    raise ProtocolError(f"record at offset {off}: length {ln} exceeds data")
                recs.append(UnknownRec(seq, esp, pc, wt, bytes(data[start : start + ln])))
                off = start + ln
            elif rtype == REC_GAP and rtype in known:
                if len(data) - off < 17:
                    raise ProtocolError(f"truncated gap record at offset {off}")
                _, frm, to, pc = struct.unpack_from("<BIIQ", data, off)
                recs.append(GapRec(frm, to, pc))
                off += 17
            elif rtype == REC_REBOOT and rtype in known:
                if len(data) - off < 17:
                    raise ProtocolError(f"truncated reboot record at offset {off}")
                _, old, new, pc = struct.unpack_from("<BIIQ", data, off)
                recs.append(RebootRec(old, new, pc))
                off += 17
            else:
                raise ProtocolError(f"unknown record type {rtype} at offset {off}")
        except ProtocolError:
            if strict or rtype not in known:
                raise
            break
    return created, recs

# --------------------------------------------------------------------------
# UTC conversion
# --------------------------------------------------------------------------

_EPOCH = datetime(1970, 1, 1, tzinfo=UTC)

def format_utc_us(utc_unix_us: int) -> str:
    """ISO 8601 UTC with microseconds ('...Z'); '' when outside the datetime range."""
    try:
        dt = _EPOCH + timedelta(microseconds=utc_unix_us)
    except OverflowError:
        return ""
    return dt.isoformat(timespec="microseconds").replace("+00:00", "Z")

def boot_indices(records: list[FileRec]) -> list[int]:
    """Boot number of each record: 0 for the first boot, +1 at every reboot record.

    esp_time_us restarts at 0 on every ESP32 boot, so time syncs and 'latest sensor
    value' lookups never cross a boot boundary.
    """
    out: list[int] = []
    boot = 0
    for r in records:
        if isinstance(r, RebootRec):
            boot += 1
        out.append(boot)
    return out

class TimeIndex:
    """esp_time_us -> UTC, using the time_sync records of the same boot.

    The nearest sync at or before the instant is used; if none precedes it, the
    earliest following sync of that boot. UTC = sync.utc + (esp_time - sync.esp_time).
    Syncs with an undecodable payload are ignored. No sync in the boot -> None.
    """

    def __init__(self, records: list[FileRec], boots: list[int] | None = None) -> None:
        boots = boots if boots is not None else boot_indices(records)
        per_boot: dict[int, list[tuple[int, int, int]]] = {}
        for r, b in zip(records, boots, strict=True):
            if isinstance(r, TimeSyncRec):
                try:
                    t = decode_time_sync(r.payload)
                except PayloadError:
                    continue
                per_boot.setdefault(b, []).append((r.esp_time_us, t.utc_unix_us, t.source))
        self._esp: dict[int, list[int]] = {}
        self._syncs: dict[int, list[tuple[int, int, int]]] = {}
        for b, lst in per_boot.items():
            lst.sort(key=lambda x: x[0])  # stable: file order kept among equal esp times
            self._syncs[b] = lst
            self._esp[b] = [x[0] for x in lst]

    def utc_us(self, boot: int, esp_time_us: int) -> tuple[int, int] | None:
        """(utc_unix_us, source) for an ESP time of the given boot, or None."""
        lst = self._syncs.get(boot)
        if not lst:
            return None
        i = bisect_right(self._esp[boot], esp_time_us) - 1
        if i < 0:
            i = 0  # fallback: the following sync
        esp0, utc0, src = lst[i]
        return utc0 + (esp_time_us - esp0), src

    def utc_text(self, boot: int, esp_time_us: int) -> tuple[str, str]:
        """(ISO UTC text, source name) or ('', '')."""
        r = self.utc_us(boot, esp_time_us)
        if r is None:
            return "", ""
        text = format_utc_us(r[0])
        return text, (SOURCE_NAMES.get(r[1], f"src{r[1]}") if text else "")

# --------------------------------------------------------------------------
# CSV export
# --------------------------------------------------------------------------

CSV_COLUMNS = (
    ["seq", "esp_time_us", "pc_time_ns", "data_type", "target_state", "moving_dist_cm"]
    + ["moving_energy", "still_dist_cm", "still_energy", "detect_dist_cm"]
    + ["max_moving_gate", "max_still_gate"]
    + [f"move_g{i}" for i in range(GATES)]
    + [f"still_g{i}" for i in range(GATES)]
    + ["record", "gap_to_seq", "error", "old_boot_id", "new_boot_id"]
    + ["frame_utc", "time_source"]
    + ["gps_utc", "gps_lat", "gps_lon", "gps_alt_m", "gps_sats", "gps_hdop"]
    + ["gps_fix_quality", "gps_flags", "pitch_deg", "roll_deg"]
)
GPS_CSV_COLUMNS = [
    "seq", "esp_time_us", "pc_time_ns", "esp_utc", "time_source", "fix_utc", "flags",
    "lat", "lon", "alt_m", "speed_mps", "course_deg", "hdop", "sats", "fix_quality", "error",
]  # fmt: skip
IMU_CSV_COLUMNS = [
    "seq", "esp_time_us", "pc_time_ns", "esp_utc", "time_source",
    "acc_x_mg", "acc_y_mg", "acc_z_mg", "gyr_x_dps", "gyr_y_dps", "gyr_z_dps",
    "pitch_deg", "roll_deg", "n_samples", "status", "error",
]  # fmt: skip

def fixed(v: int, decimals: int) -> str:
    """Integer v / 10**decimals as an exact decimal string (no float rounding)."""
    sign = "-" if v < 0 else ""
    q, r = divmod(abs(v), 10**decimals)
    return f"{sign}{q}.{r:0{decimals}d}" if decimals else f"{sign}{q}"

def _gps_values(g: GpsFix) -> dict[str, str]:
    """Display values of a fix; invalid parts are empty strings."""
    utc = ""
    if g.time_valid and g.date_valid and g.utc_unix_ms:
        utc = format_utc_us(g.utc_unix_ms * 1000)
    return {
        "utc": utc,
        "lat": fixed(g.lat_e7, 7) if g.pos_valid else "",
        "lon": fixed(g.lon_e7, 7) if g.pos_valid else "",
        "alt_m": fixed(g.alt_cm, 2) if g.alt_valid else "",
        "speed_mps": fixed(g.speed_cmps, 2),
        "course_deg": fixed(g.course_cdeg, 2),
        "hdop": fixed(g.hdop_x100, 2),
        "sats": str(g.sats),
        "fix_quality": str(g.fix_quality),
        "flags": str(g.flags),
    }

def csv_row(
    rec: FileRec,
    *,
    when: tuple[str, str] = ("", ""),
    gps: GpsFix | None = None,
    imu: ImuSample | None = None,
) -> dict[str, object]:
    """One CSV row (dict keyed by CSV_COLUMNS) for a gap/reboot/frame record; never raises.

    `when` is (frame_utc, time_source); `gps` / `imu` the latest sensor values at the frame.
    """
    if isinstance(rec, RebootRec):
        return {
            "pc_time_ns": rec.pc_time_ns,
            "record": "reboot",
            "old_boot_id": rec.old_boot_id,
            "new_boot_id": rec.new_boot_id,
        }
    if isinstance(rec, GapRec):
        return {
            "seq": rec.from_seq,
            "pc_time_ns": rec.pc_time_ns,
            "record": "gap",
            "gap_to_seq": rec.to_seq,
        }
    if not isinstance(rec, FrameRec):
        raise TypeError(f"no main-CSV row for {type(rec).__name__}")
    row: dict[str, object] = {
        "seq": rec.seq,
        "esp_time_us": rec.esp_time_us,
        "pc_time_ns": rec.pc_time_ns,
        "record": "frame",
        "frame_utc": when[0],
        "time_source": when[1],
    }
    if gps is not None:
        v = _gps_values(gps)
        row.update(
            gps_utc=v["utc"], gps_lat=v["lat"], gps_lon=v["lon"], gps_alt_m=v["alt_m"],
            gps_sats=v["sats"], gps_hdop=v["hdop"], gps_fix_quality=v["fix_quality"],
            gps_flags=v["flags"],
        )  # fmt: skip
    if imu is not None and imu.valid:
        row["pitch_deg"] = fixed(imu.pitch_cdeg, 2)
        row["roll_deg"] = fixed(imu.roll_cdeg, 2)
    try:
        d = decode_data_frame(rec.raw)
    except FrameError as e:
        row["error"] = str(e)
        return row
    row.update(
        data_type=d.data_type,
        target_state=d.target_state,
        moving_dist_cm=d.moving_dist_cm,
        moving_energy=d.moving_energy,
        still_dist_cm=d.still_dist_cm,
        still_energy=d.still_energy,
        detect_dist_cm=d.detect_dist_cm,
    )
    if d.engineering:
        row["max_moving_gate"] = d.max_moving_gate
        row["max_still_gate"] = d.max_still_gate
        for i in range(GATES):
            row[f"move_g{i}"] = d.moving_gate_energy[i]
            row[f"still_g{i}"] = d.still_gate_energy[i]
    return row

def csv_rows(records: list[FileRec]) -> list[dict[str, object]]:
    """Main CSV rows: radar frames (with UTC, latest GPS fix and tilt), gaps, reboots.

    gps/imu/time_sync/unknown records have no row here (see write_gps_csv / write_imu_csv).
    The 'latest' sensor values are those most recently recorded before the frame in the
    same boot; a reboot forgets them.
    """
    boots = boot_indices(records)
    tix = TimeIndex(records, boots)
    rows: list[dict[str, object]] = []
    gps: GpsFix | None = None
    imu: ImuSample | None = None
    cur_boot = 0
    for r, b in zip(records, boots, strict=True):
        if b != cur_boot:
            cur_boot, gps, imu = b, None, None
        if isinstance(r, GpsRec):
            with contextlib.suppress(PayloadError):  # damaged: keep the previous fix
                gps = decode_gps_fix(r.payload)
        elif isinstance(r, ImuRec):
            with contextlib.suppress(PayloadError):
                imu = decode_imu(r.payload)
        elif isinstance(r, FrameRec):
            rows.append(csv_row(r, when=tix.utc_text(b, r.esp_time_us), gps=gps, imu=imu))
        elif isinstance(r, (GapRec, RebootRec)):
            rows.append(csv_row(r))
    return rows

def write_csv(records: list[FileRec], out: IO[str]) -> int:
    w = csv.DictWriter(out, fieldnames=CSV_COLUMNS, restval="", lineterminator="\n")
    w.writeheader()
    rows = csv_rows(records)
    for row in rows:
        w.writerow(row)
    return len(rows)

def write_gps_csv(records: list[FileRec], out: IO[str]) -> int:
    """One row per gps_fix record (a damaged payload gives a row with `error` set)."""
    boots = boot_indices(records)
    tix = TimeIndex(records, boots)
    w = csv.DictWriter(out, fieldnames=GPS_CSV_COLUMNS, restval="", lineterminator="\n")
    w.writeheader()
    n = 0
    for r, b in zip(records, boots, strict=True):
        if not isinstance(r, GpsRec):
            continue
        esp_utc, src = tix.utc_text(b, r.esp_time_us)
        row: dict[str, object] = {
            "seq": r.seq, "esp_time_us": r.esp_time_us, "pc_time_ns": r.pc_time_ns,
            "esp_utc": esp_utc, "time_source": src,
        }  # fmt: skip
        try:
            v = _gps_values(decode_gps_fix(r.payload))
            row.update(
                fix_utc=v["utc"], flags=v["flags"], lat=v["lat"], lon=v["lon"],
                alt_m=v["alt_m"], speed_mps=v["speed_mps"], course_deg=v["course_deg"],
                hdop=v["hdop"], sats=v["sats"], fix_quality=v["fix_quality"],
            )  # fmt: skip
        except PayloadError as e:
            row["error"] = str(e)
        w.writerow(row)
        n += 1
    return n

def write_imu_csv(records: list[FileRec], out: IO[str]) -> int:
    """One row per imu record; pitch/roll are empty when the status says invalid."""
    boots = boot_indices(records)
    tix = TimeIndex(records, boots)
    w = csv.DictWriter(out, fieldnames=IMU_CSV_COLUMNS, restval="", lineterminator="\n")
    w.writeheader()
    n = 0
    for r, b in zip(records, boots, strict=True):
        if not isinstance(r, ImuRec):
            continue
        esp_utc, src = tix.utc_text(b, r.esp_time_us)
        row: dict[str, object] = {
            "seq": r.seq, "esp_time_us": r.esp_time_us, "pc_time_ns": r.pc_time_ns,
            "esp_utc": esp_utc, "time_source": src,
        }  # fmt: skip
        try:
            m = decode_imu(r.payload)
            row.update(
                acc_x_mg=m.acc_mg[0], acc_y_mg=m.acc_mg[1], acc_z_mg=m.acc_mg[2],
                gyr_x_dps=fixed(m.gyr_ddps[0], 1), gyr_y_dps=fixed(m.gyr_ddps[1], 1),
                gyr_z_dps=fixed(m.gyr_ddps[2], 1), n_samples=m.n_samples, status=m.status,
            )  # fmt: skip
            if m.valid:
                row["pitch_deg"] = fixed(m.pitch_cdeg, 2)
                row["roll_deg"] = fixed(m.roll_cdeg, 2)
        except PayloadError as e:
            row["error"] = str(e)
        w.writerow(row)
        n += 1
    return n

# --------------------------------------------------------------------------
# info
# --------------------------------------------------------------------------

def summarize(created_unix_ns: int, records: list[FileRec]) -> dict[str, object]:
    frames = [r for r in records if isinstance(r, FrameRec)]
    gaps = [r for r in records if isinstance(r, GapRec)]
    reboots = [r for r in records if isinstance(r, RebootRec)]
    gps = [r for r in records if isinstance(r, GpsRec)]
    imus = [r for r in records if isinstance(r, ImuRec)]
    syncs = [r for r in records if isinstance(r, TimeSyncRec)]
    fixes = 0
    for g in gps:
        with contextlib.suppress(PayloadError):
            fixes += decode_gps_fix(g.payload).has_fix
    sources: dict[str, int] = {}
    for t in syncs:
        try:
            src = decode_time_sync(t.payload).source
        except PayloadError:
            continue
        name = SOURCE_NAMES.get(src, f"src{src}")
        sources[name] = sources.get(name, 0) + 1
    info: dict[str, object] = {
        "created_unix_ns": created_unix_ns,
        "frames": len(frames),
        "gaps": [(g.from_seq, g.to_seq, g.missing) for g in gaps],
        "missing_frames": sum(g.missing for g in gaps),
        "reboots": [(r.old_boot_id, r.new_boot_id, r.pc_time_ns) for r in reboots],
        "first_seq": frames[0].seq if frames else None,
        "last_seq": frames[-1].seq if frames else None,
        "duration_s": None,
        "esp_duration_s": None,
        "gps_records": len(gps),
        "gps_fixes": fixes,
        "fix_ratio": fixes / len(gps) if gps else None,
        "imu_records": len(imus),
        "time_syncs": len(syncs),
        "time_sources": sources,
        "unknown_records": sum(isinstance(r, UnknownRec) for r in records),
    }
    if len(frames) >= 2:
        info["duration_s"] = (frames[-1].pc_time_ns - frames[0].pc_time_ns) / 1e9
        info["esp_duration_s"] = (frames[-1].esp_time_us - frames[0].esp_time_us) / 1e6
    return info

# --------------------------------------------------------------------------
# Network recorder
# --------------------------------------------------------------------------

class _Writer:
    """Appends records to an open binary file and tracks the next expected seq.

    The device sequence restarts at 0 after every ESP32 reboot; boot_id tells
    boots apart. Sequence checks (duplicates, gaps) apply within one boot only.
    All record types (frame, gps_fix, imu, time_sync, unknown) share that one sequence.
    """

    def __init__(self, fh: IO[bytes], next_seq: int | None = None) -> None:
        self.fh = fh
        self.next_seq = next_seq
        self.boot_id: int | None = None
        self.frames = 0  # LD2410C frames only (what max_frames counts)
        self.sensor_records = 0  # gps_fix + imu + time_sync
        self.unknown_records = 0
        self.gaps = 0
        self.reboots = 0

    def add_batch(
        self, hdr: BatchHeader, recs: list[Record], pc_time_ns: int, requested_seq: int = 0
    ) -> bool:
        """Write a batch. Returns False if it was discarded because the device
        rebooted: the caller must reconnect and request from seq 0 of the new boot."""
        if hdr.boot_id != self.boot_id:
            if self.boot_id is not None:
                log.warning("device rebooted: boot_id %08x -> %08x", self.boot_id, hdr.boot_id)
                self.fh.write(encode_reboot_rec(self.boot_id, hdr.boot_id, pc_time_ns))
                self.reboots += 1
                self.boot_id = hdr.boot_id
                self.next_seq = None  # new sequence baseline
                if requested_seq != 0:
                    # We asked from the old boot's seq, so frames the new boot
                    # produced before it are missing from this batch; ask from 0.
                    return False
            else:
                self.boot_id = hdr.boot_id
        for r in recs:
            if self.next_seq is not None:
                d = seq_delta(r.seq, self.next_seq)
                if d < 0:
                    log.warning("dropping duplicate/old seq %d (expected %d)", r.seq, self.next_seq)
                    continue
                if d > 0:
                    log.warning(
                        "gap: seq %d..%d missing%s",
                        self.next_seq,
                        (r.seq - 1) & SEQ_MASK,
                        " (device reported GAP)" if hdr.gap else "",
                    )
                    self.fh.write(encode_gap_rec(self.next_seq, r.seq, pc_time_ns))
                    self.gaps += 1
            self.fh.write(encode_wire_record(r, pc_time_ns))
            if r.rtype == WIRE_FRAME:
                self.frames += 1
            elif r.rtype in _WIRE_TO_FILE:
                self.sensor_records += 1
            else:
                self.unknown_records += 1
                log.warning("record seq %d has unknown type %d, kept as raw", r.seq, r.rtype)
            self.next_seq = (r.seq + 1) & SEQ_MASK
        self.fh.flush()
        return True

class Stopped(Exception):
    """The caller's stop event was set while waiting for the network."""

def _recv_exact(
    sock: socket.socket,
    n: int,
    stop: threading.Event | None = None,
    idle_timeout: float | None = None,
) -> bytes:
    """Read exactly n bytes. With `stop`, the wait is sliced (the socket timeout is
    then the slice, POLL_S) so a stop request is honoured within about one slice;
    `idle_timeout` bounds the time without any received byte (TimeoutError)."""
    buf = bytearray()
    last = time.monotonic()
    while len(buf) < n:
        if stop is not None and stop.is_set():
            raise Stopped
        try:
            chunk = sock.recv(n - len(buf))
        except TimeoutError:
            if stop is None:
                raise
            if idle_timeout is not None and time.monotonic() - last >= idle_timeout:
                raise TimeoutError("timed out waiting for data") from None
            continue
        if not chunk:
            raise ConnectionError("connection closed by peer")
        buf += chunk
        last = time.monotonic()
    return bytes(buf)

def read_batch(
    sock: socket.socket,
    stop: threading.Event | None = None,
    idle_timeout: float | None = None,
) -> tuple[BatchHeader, list[Record]]:
    """Read one batch from a socket (blocking, honours the socket timeout, or
    `stop`/`idle_timeout` when given; see _recv_exact)."""
    hdr = parse_batch_header(_recv_exact(sock, BATCH_HDR_LEN, stop, idle_timeout))
    recs: list[Record] = []
    for i in range(hdr.count):
        head = _recv_exact(sock, REC_HDR_LEN, stop, idle_timeout)
        seq, esp, rtype, ln = struct.unpack("<IQBH", head)
        try:
            raw = _recv_exact(sock, ln, stop, idle_timeout)
        except ConnectionError as e:
            raise ProtocolError(f"record {i} cut off after header: {e}") from e
        recs.append(Record(seq, esp, rtype, raw))
    return hdr, recs

def _record_loop(
    host: str,
    port: int,
    w: _Writer,
    stop: threading.Event,
    max_frames: int | None,
    timeout: float,
    backoff_initial: float,
    backoff_max: float,
) -> None:
    backoff = backoff_initial
    while not stop.is_set() and (max_frames is None or w.frames < max_frames):
        from_seq = w.next_seq if w.next_seq is not None else 0
        try:
            with socket.create_connection((host, port), timeout=CONNECT_S) as sock:
                if stop.is_set():
                    break
                sock.settimeout(POLL_S)
                sock.sendall(encode_request(from_seq))
                log.info("connected to %s:%d, from_seq=%d", host, port, from_seq)
                while not stop.is_set() and (max_frames is None or w.frames < max_frames):
                    hdr, recs = read_batch(sock, stop, timeout)
                    backoff = backoff_initial
                    if not w.add_batch(hdr, recs, time.time_ns(), from_seq):
                        break  # device rebooted: reconnect from seq 0 of the new boot
        except (OSError, ProtocolError) as e:
            log.warning("connection lost: %s; retry in %.1fs", e, backoff)
            if stop.wait(backoff):
                break
            backoff = min(backoff * 2, backoff_max)

def record(
    host: str,
    port: int,
    out_path: str | Path,
    stop: threading.Event | None = None,
    *,
    max_frames: int | None = None,
    timeout: float = 5.0,
    backoff_initial: float = 0.5,
    backoff_max: float = 10.0,
) -> _Writer:
    """Record until `stop` is set (or max_frames frames written). Reconnects forever.

    The first connection asks from seq 0 and accepts whatever the device has (no gap
    record: there is no baseline). Later connections resume from last_seq + 1 and
    write a gap record when the device skips ahead. If the batch header's boot_id
    changes (ESP32 rebooted, its sequence restarted), a reboot record is written, the
    sequence baseline is reset and the recorder reconnects asking from seq 0.
    """
    stop = stop or threading.Event()
    with open(out_path, "wb") as fh:
        fh.write(encode_file_header(time.time_ns()))
        fh.flush()
        w = _Writer(fh)
        with contextlib.suppress(Stopped):
            _record_loop(host, port, w, stop, max_frames, timeout, backoff_initial, backoff_max)
    return w

# --------------------------------------------------------------------------
# CLI
# --------------------------------------------------------------------------

def _out(text: str) -> None:
    sys.stdout.write(text + "\n")

def _load(path: str) -> tuple[int, list[FileRec]]:
    data = Path(path).read_bytes()
    try:
        return parse_file(data)
    except ProtocolError as e:
        log.warning("%s; using the complete records only", e)
        return parse_file(data, strict=False)

def cmd_record(args: argparse.Namespace) -> int:
    """Exit code 0 on Ctrl+C (a deliberate stop is not an error).

    First SIGINT/SIGTERM only sets the stop event: all waits are sliced (POLL_S) so the
    recorder returns within ~0.2 s and the file is closed cleanly. A further Ctrl+C
    raises KeyboardInterrupt at once; `info`/`export-csv` tolerate a truncated last record.
    """
    out = args.output or time.strftime("ld2410_%Y%m%d_%H%M%S.ldrec")
    stop = threading.Event()
    presses = 0

    def on_sigint(_signum: int, _frame: object) -> None:
        nonlocal presses
        presses += 1
        stop.set()
        if presses >= 2:
            raise KeyboardInterrupt

    old = signal.signal(signal.SIGINT, on_sigint)
    try:
        w = record(args.host, args.port, out, stop)
    except KeyboardInterrupt:
        stop.set()
        log.info("interrupted")
        return 0
    finally:
        signal.signal(signal.SIGINT, old)
    if presses:
        log.info("interrupted")
    _out(f"{out}: {w.frames} frames, {w.sensor_records} sensor records, {w.gaps} gaps")
    return 0

def cmd_export_csv(args: argparse.Namespace) -> int:
    _, recs = _load(args.file)
    for path, writer in ((args.gps, write_gps_csv), (args.imu, write_imu_csv)):
        if path:
            with open(path, "w", newline="", encoding="utf-8") as f:
                writer(recs, f)
    if args.output:
        with open(args.output, "w", newline="", encoding="utf-8") as f:
            write_csv(recs, f)
    else:
        write_csv(recs, sys.stdout)
    return 0

def cmd_info(args: argparse.Namespace) -> int:
    created, recs = _load(args.file)
    s = summarize(created, recs)
    _out(f"frames:        {s['frames']}")
    _out(f"gps records:   {s['gps_records']}")
    ratio = s["fix_ratio"]
    _out(f"gps fix ratio: {'n/a' if ratio is None else f'{ratio:.3f}'} ({s['gps_fixes']} with fix)")
    _out(f"imu records:   {s['imu_records']}")
    srcs = s["time_sources"]
    srctxt = ", ".join(f"{k} {v}" for k, v in sorted(srcs.items())) or "none"  # type: ignore[attr-defined]
    _out(f"time syncs:    {s['time_syncs']} ({srctxt})")
    if s["unknown_records"]:
        _out(f"unknown type:  {s['unknown_records']} records kept raw")
    _out(f"seq range:     {s['first_seq']} .. {s['last_seq']}")
    _out(f"duration (pc): {s['duration_s']} s")
    _out(f"duration (esp): {s['esp_duration_s']} s")
    gaps = s["gaps"]
    _out(f"reboots:       {len(s['reboots'])}")  # type: ignore[arg-type]
    for old, new, _pc in s["reboots"]:  # type: ignore[attr-defined]
        _out(f"  boot_id {old:08x} -> {new:08x}")
    _out(f"gaps:          {len(gaps)} ({s['missing_frames']} frames missing)")  # type: ignore[arg-type]
    for frm, to, n in gaps:  # type: ignore[attr-defined]
        _out(f"  [{frm}, {to}) = {n} frames")
    return 0

def build_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(description="LD2410C ESP32 recorder")
    sub = p.add_subparsers(dest="command", required=True)
    r = sub.add_parser("record", help="record from the ESP32")
    r.add_argument("host")
    r.add_argument("--port", type=int, default=DEFAULT_PORT)
    r.add_argument("-o", "--output")
    r.set_defaults(func=cmd_record)
    e = sub.add_parser("export-csv", help="decode a recording to CSV")
    e.add_argument("file")
    e.add_argument("-o", "--output")
    e.add_argument("--gps", metavar="FILE", help="also write the GPS fixes to this CSV")
    e.add_argument("--imu", metavar="FILE", help="also write the IMU samples to this CSV")
    e.set_defaults(func=cmd_export_csv)
    i = sub.add_parser("info", help="summarize a recording")
    i.add_argument("file")
    i.set_defaults(func=cmd_info)
    return p

def main(argv: list[str] | None = None) -> int:
    logging.basicConfig(level=logging.INFO, format="%(asctime)s %(levelname)s %(message)s")
    args = build_parser().parse_args(argv)
    try:
        return args.func(args)
    except (OSError, ProtocolError) as e:
        sys.stderr.write(f"error: {e}\n")
        return 1

if __name__ == "__main__":
    with contextlib.suppress(BrokenPipeError):
        sys.exit(main())

