#!/usr/bin/env python3
"""Host recorder for the ESP32 LD2410C MVP (spec R7).

Sub-commands:
  record HOST [--port 5410] [-o FILE]   record frames from the ESP32 to a .ldrec file
  export-csv FILE [-o CSV]              decode a recording to CSV
  info FILE                             frame count, sequence range, duration, gaps

Byte layouts (all little-endian) are fixed in the plan
docs/superpowers/plans/2026-10-02-esp32-ld2410-mvp.md and mirrored by
esp32/components/core (C). The decoder is checked against the shared vectors in
esp32/tests/vectors. Stdlib only. The pure encode/decode functions below do no I/O.
"""

from __future__ import annotations

import argparse
import contextlib
import csv
import logging
import socket
import struct
import sys
import threading
import time
from dataclasses import dataclass
from pathlib import Path
from typing import IO, NamedTuple


log = logging.getLogger("ld2410_rec")

SEQ_MASK = 0xFFFFFFFF
DEFAULT_PORT = 5410

REQ_MAGIC = b"LDRQ"
BATCH_MAGIC = b"LDRB"
REC_VERSION = 2
REQ_LEN = 12
BATCH_HDR_LEN = 20
REC_HDR_LEN = 14  # seq u32, esp_time_us u64, len u16
FLAG_GAP = 0x01

FILE_MAGIC = b"LDREC1\x00\x00"
FILE_VERSION = 2
FILE_HDR_LEN = 20
REC_FRAME = 0
REC_GAP = 1
REC_REBOOT = 2

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
    raw: bytes

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
        seq, esp, ln = struct.unpack_from("<IQH", buf, off)
        off += REC_HDR_LEN
        if len(buf) - off < ln:
            raise ProtocolError(f"record {i} length {ln} exceeds data ({len(buf) - off} left)")
        out.append(Record(seq, esp, bytes(buf[off : off + ln])))
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

FileRec = FrameRec | GapRec | RebootRec

def encode_file_header(created_unix_ns: int) -> bytes:
    return struct.pack("<8sHHQ", FILE_MAGIC, FILE_VERSION, 0, created_unix_ns)

def encode_frame_rec(seq: int, esp_time_us: int, pc_time_ns: int, raw: bytes) -> bytes:
    return struct.pack("<BIQQH", REC_FRAME, seq, esp_time_us, pc_time_ns, len(raw)) + raw

def encode_gap_rec(from_seq: int, to_seq: int, pc_time_ns: int) -> bytes:
    return struct.pack("<BIIQ", REC_GAP, from_seq, to_seq, pc_time_ns)

def encode_reboot_rec(old_boot_id: int, new_boot_id: int, pc_time_ns: int) -> bytes:
    return struct.pack("<BIIQ", REC_REBOOT, old_boot_id, new_boot_id, pc_time_ns)

def parse_file(data: bytes, *, strict: bool = True) -> tuple[int, list[FileRec]]:
    """Parse a .ldrec image. Returns (created_unix_ns, records).

    strict=False tolerates a truncated last record (e.g. a recording killed
    mid-write) and returns what was complete.
    """
    if len(data) < FILE_HDR_LEN:
        raise ProtocolError(f"short file header ({len(data)} bytes)")
    magic, version, _res, created = struct.unpack_from("<8sHHQ", data, 0)
    if magic != FILE_MAGIC:
        raise ProtocolError("not a .ldrec file (bad magic)")
    if version != FILE_VERSION:
        raise ProtocolError(f"unsupported .ldrec version {version}")
    recs: list[FileRec] = []
    off = FILE_HDR_LEN
    while off < len(data):
        rtype = data[off]
        try:
            if rtype == REC_FRAME:
                if len(data) - off < 1 + 22:
                    raise ProtocolError(f"truncated frame record at offset {off}")
                _, seq, esp, pc, ln = struct.unpack_from("<BIQQH", data, off)
                start = off + 23
                if len(data) - start < ln:
                    raise ProtocolError(f"frame record at offset {off}: length {ln} exceeds data")
                recs.append(FrameRec(seq, esp, pc, bytes(data[start : start + ln])))
                off = start + ln
            elif rtype == REC_GAP:
                if len(data) - off < 17:
                    raise ProtocolError(f"truncated gap record at offset {off}")
                _, frm, to, pc = struct.unpack_from("<BIIQ", data, off)
                recs.append(GapRec(frm, to, pc))
                off += 17
            elif rtype == REC_REBOOT:
                if len(data) - off < 17:
                    raise ProtocolError(f"truncated reboot record at offset {off}")
                _, old, new, pc = struct.unpack_from("<BIIQ", data, off)
                recs.append(RebootRec(old, new, pc))
                off += 17
            else:
                raise ProtocolError(f"unknown record type {rtype} at offset {off}")
        except ProtocolError:
            if strict or rtype not in (REC_FRAME, REC_GAP, REC_REBOOT):
                raise
            break
    return created, recs

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
)

def csv_row(rec: FileRec) -> dict[str, object]:
    """One CSV row (dict keyed by CSV_COLUMNS) for a file record; never raises."""
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
    row: dict[str, object] = {
        "seq": rec.seq,
        "esp_time_us": rec.esp_time_us,
        "pc_time_ns": rec.pc_time_ns,
        "record": "frame",
    }
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

def write_csv(records: list[FileRec], out: IO[str]) -> int:
    w = csv.DictWriter(out, fieldnames=CSV_COLUMNS, restval="", lineterminator="\n")
    w.writeheader()
    for r in records:
        w.writerow(csv_row(r))
    return len(records)

# --------------------------------------------------------------------------
# info
# --------------------------------------------------------------------------

def summarize(created_unix_ns: int, records: list[FileRec]) -> dict[str, object]:
    frames = [r for r in records if isinstance(r, FrameRec)]
    gaps = [r for r in records if isinstance(r, GapRec)]
    reboots = [r for r in records if isinstance(r, RebootRec)]
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
    """

    def __init__(self, fh: IO[bytes], next_seq: int | None = None) -> None:
        self.fh = fh
        self.next_seq = next_seq
        self.boot_id: int | None = None
        self.frames = 0
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
                self.fh.flush()
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
            self.fh.write(encode_frame_rec(r.seq, r.esp_time_us, pc_time_ns, r.raw))
            self.frames += 1
            self.next_seq = (r.seq + 1) & SEQ_MASK
        self.fh.flush()
        return True

def _recv_exact(sock: socket.socket, n: int) -> bytes:
    buf = bytearray()
    while len(buf) < n:
        chunk = sock.recv(n - len(buf))
        if not chunk:
            raise ConnectionError("connection closed by peer")
        buf += chunk
    return bytes(buf)

def read_batch(sock: socket.socket) -> tuple[BatchHeader, list[Record]]:
    """Read one batch from a socket (blocking, honours the socket timeout)."""
    hdr = parse_batch_header(_recv_exact(sock, BATCH_HDR_LEN))
    recs: list[Record] = []
    for i in range(hdr.count):
        head = _recv_exact(sock, REC_HDR_LEN)
        seq, esp, ln = struct.unpack("<IQH", head)
        try:
            raw = _recv_exact(sock, ln)
        except ConnectionError as e:
            raise ProtocolError(f"record {i} cut off after header: {e}") from e
        recs.append(Record(seq, esp, raw))
    return hdr, recs

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
        backoff = backoff_initial
        while not stop.is_set() and (max_frames is None or w.frames < max_frames):
            from_seq = w.next_seq if w.next_seq is not None else 0
            try:
                with socket.create_connection((host, port), timeout=timeout) as sock:
                    sock.settimeout(timeout)
                    sock.sendall(encode_request(from_seq))
                    log.info("connected to %s:%d, from_seq=%d", host, port, from_seq)
                    while not stop.is_set() and (max_frames is None or w.frames < max_frames):
                        hdr, recs = read_batch(sock)
                        backoff = backoff_initial
                        if not w.add_batch(hdr, recs, time.time_ns(), from_seq):
                            break  # device rebooted: reconnect from seq 0 of the new boot
            except (OSError, ProtocolError) as e:
                log.warning("connection lost: %s; retry in %.1fs", e, backoff)
                if stop.wait(backoff):
                    break
                backoff = min(backoff * 2, backoff_max)
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
    out = args.output or time.strftime("ld2410_%Y%m%d_%H%M%S.ldrec")
    stop = threading.Event()
    try:
        w = record(args.host, args.port, out, stop)
    except KeyboardInterrupt:
        stop.set()
        log.info("interrupted")
        return 0
    _out(f"{out}: {w.frames} frames, {w.gaps} gaps")
    return 0

def cmd_export_csv(args: argparse.Namespace) -> int:
    _, recs = _load(args.file)
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

