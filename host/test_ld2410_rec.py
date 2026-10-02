"""Tests for host/ld2410_rec.py (shared vectors in esp32/tests/vectors)."""

from __future__ import annotations

import contextlib
import csv
import io
import json
import socket
import struct
import signal
import subprocess
import sys
import threading
import time
from dataclasses import asdict
from pathlib import Path

import ld2410_rec as lr
import pytest

VEC = Path(__file__).resolve().parent.parent / "esp32" / "tests" / "vectors"


def vec(name: str) -> tuple[bytes, dict]:
    return (VEC / f"{name}.bin").read_bytes(), json.loads((VEC / f"{name}.json").read_text())


NORMAL_RAW = vec("frame_normal")[0]
ENG_RAW = vec("frame_engineering")[0]


def listify(d: dict) -> dict:
    return {k: list(v) if isinstance(v, tuple) else v for k, v in d.items()}


# ---- decoder against shared vectors ----------------------------------------


@pytest.mark.parametrize("name", [
        "frame_normal",
        "frame_engineering",
        "frame_engineering_real_0",
        "frame_engineering_real_1",
    ],
)
def test_decode_data_vectors(name):
    raw, exp = vec(name)
    assert raw.hex() == exp["raw_hex"]
    assert exp["frame_kind"] == "data"
    assert listify(asdict(lr.decode_frame(raw))) == exp["data"]


@pytest.mark.parametrize("name", ["ack_ok", "ack_fail"])
def test_decode_ack_vectors(name):
    raw, exp = vec(name)
    assert exp["frame_kind"] == "cmd"
    assert asdict(lr.decode_frame(raw)) == exp["ack"]


def test_every_vector_is_covered():
    names = {p.stem for p in VEC.glob("*.json")}
    assert names == {
        "frame_normal",
        "frame_engineering",
        "frame_engineering_real_0",
        "frame_engineering_real_1",
        "ack_ok",
        "ack_fail",
        "batch_gap_wrap",
        "batch_reboot",
    }


def payload_of(raw: bytes) -> bytes:
    return raw[6:-4]


def frame_of(payload: bytes) -> bytes:
    return lr.DATA_HEADER + struct.pack("<H", len(payload)) + payload + lr.DATA_FOOTER


def test_decode_payload_rules():
    p = bytearray(payload_of(NORMAL_RAW))
    # module-specific extra bytes before the tail are accepted (normal frame too)
    lr.decode_payload(bytes(p[:-2]) + b"\x01\x02" + b"\x55\x00")
    for bad in (
        bytes(p[:12]),  # too short
        bytes([0x03]) + bytes(p[1:]),  # bad type
        bytes(p[:1]) + b"\x00" + bytes(p[2:]),  # bad marker
        bytes(p[:2]) + b"\x04" + bytes(p[3:]),  # target state > 3
        bytes(p[:-1]) + b"\x01",  # bad tail
    ):
        with pytest.raises(lr.FrameError):
            lr.decode_payload(bad)
    eng = payload_of(ENG_RAW)
    with pytest.raises(lr.FrameError):  # truncated engineering payload
        lr.decode_payload(eng[:20] + b"\x55\x00")
    with pytest.raises(lr.FrameError):
        lr.decode_payload(b"")


def test_decode_frame_rejects_bad_framing():
    for bad in (
        b"",
        NORMAL_RAW[:5],
        NORMAL_RAW[:-1],  # footer cut
        b"\x00" + NORMAL_RAW[1:],  # bad header
        NORMAL_RAW[:4] + b"\x0e\x00" + NORMAL_RAW[6:],  # length mismatch
        NORMAL_RAW + b"\x00",
    ):
        with pytest.raises(lr.FrameError):
            lr.decode_frame(bad)
    ack, _ = vec("ack_ok")
    with pytest.raises(lr.FrameError):
        lr.decode_data_frame(ack)


# ---- protocol --------------------------------------------------------------


def test_request_encoding_exact():
    assert lr.encode_request(0x01020304) == b"LDRQ\x02\x00\x00\x00\x04\x03\x02\x01"
    assert lr.encode_request(0) == b"LDRQ" + b"\x02\x00\x00\x00" + b"\x00" * 4
    assert len(lr.encode_request(0xFFFFFFFF)) == lr.REQ_LEN


def test_batch_gap_wrap_vector():
    raw, exp = vec("batch_gap_wrap")
    hdr, recs = lr.parse_batch(raw)
    assert (hdr.version, hdr.flags, hdr.first_seq, hdr.count, hdr.boot_id) == (
        exp["version"],
        exp["flags"],
        exp["first_seq"],
        exp["count"],
        exp["boot_id"],
    )
    assert hdr.version == 2 and hdr.boot_id == 0xA1B2C3D4
    assert hdr.gap
    assert [(r.seq, r.esp_time_us, r.raw.hex()) for r in recs] == [
        (r["seq"], r["esp_time_us"], r["raw_hex"]) for r in exp["records"]
    ]
    assert recs[1].esp_time_us > 2**32
    for r in recs:
        lr.decode_data_frame(r.raw)


def test_batch_reboot_vector():
    raw, exp = vec("batch_reboot")
    hdr, recs = lr.parse_batch(raw)
    assert recs == [] and len(raw) == lr.BATCH_HDR_LEN == 20
    assert (hdr.first_seq, hdr.count, hdr.boot_id, hdr.gap) == (0, 0, exp["boot_id"], False)
    assert hdr.boot_id == 0xFFFFFFFF != vec("batch_gap_wrap")[1]["boot_id"]


def test_batch_errors():
    raw, _ = vec("batch_gap_wrap")
    with pytest.raises(lr.ProtocolError, match="short"):
        lr.parse_batch_header(raw[:19])
    with pytest.raises(lr.ProtocolError, match="magic"):
        lr.parse_batch_header(b"XXXX" + raw[4:])
    with pytest.raises(lr.ProtocolError, match="version"):
        lr.parse_batch_header(raw[:4] + b"\x01" + raw[5:])
    with pytest.raises(lr.ProtocolError, match="reserved"):
        lr.parse_batch_header(raw[:6] + b"\x01\x00" + raw[8:])
    with pytest.raises(lr.ProtocolError, match="boot_id"):
        lr.parse_batch_header(raw[:16] + b"\x00\x00\x00\x00")
    with pytest.raises(lr.ProtocolError, match="exceeds"):
        lr.parse_batch(raw[:-1])
    with pytest.raises(lr.ProtocolError, match="truncated"):
        lr.parse_batch(raw[: lr.BATCH_HDR_LEN + 5])
    with pytest.raises(lr.ProtocolError, match="trailing"):
        lr.parse_batch(raw + b"\x00")
    bad_len = bytearray(raw)
    bad_len[20 + 12 : 20 + 14] = b"\xff\xff"  # record length beyond data
    with pytest.raises(lr.ProtocolError, match="exceeds"):
        lr.parse_batch(bytes(bad_len))
    empty = raw[:4] + b"\x02\x00\x00\x00" + raw[8:12] + b"\x00\x00\x00\x00" + raw[16:20]
    assert lr.parse_batch(empty)[1] == []


def test_seq_delta_wrap():
    assert lr.seq_delta(0, 0xFFFFFFFF) == 1
    assert lr.seq_delta(0xFFFFFFFF, 0) == -1
    assert lr.seq_delta(5, 5) == 0


# ---- file ------------------------------------------------------------------


def build_file(recs) -> bytes:
    out = bytearray(lr.encode_file_header(1234))
    for r in recs:
        if isinstance(r, lr.GapRec):
            out += lr.encode_gap_rec(r.from_seq, r.to_seq, r.pc_time_ns)
        elif isinstance(r, lr.RebootRec):
            out += lr.encode_reboot_rec(r.old_boot_id, r.new_boot_id, r.pc_time_ns)
        else:
            out += lr.encode_frame_rec(r.seq, r.esp_time_us, r.pc_time_ns, r.raw)
    return bytes(out)


def test_file_round_trip_and_layout():
    recs = [
        lr.FrameRec(0xFFFFFFFE, 10, 111, NORMAL_RAW),
        lr.GapRec(0xFFFFFFFF, 3, 222),
        lr.FrameRec(3, 2**33, 333, ENG_RAW),
        lr.RebootRec(0x11223344, 0xFFFFFFFF, 2**40 + 5),
        lr.FrameRec(0, 4, 444, NORMAL_RAW),
    ]
    data = build_file(recs)
    assert data[:8] == b"LDREC1\x00\x00"
    assert data[8:12] == b"\x02\x00\x00\x00"
    assert struct.unpack_from("<Q", data, 12)[0] == 1234
    assert data[20] == 0  # first record is a frame
    created, back = lr.parse_file(data)
    assert created == 1234
    assert back == recs
    assert back[1].missing == 4
    # reboot record: type 2, old u32, new u32, pc_time_ns u64
    reb = lr.encode_reboot_rec(0x11223344, 0xFFFFFFFF, 2**40 + 5)
    assert reb == b"\x02" + b"\x44\x33\x22\x11" + b"\xff\xff\xff\xff" + struct.pack("<Q", 2**40 + 5)
    assert len(reb) == 17


def test_file_errors_and_truncation():
    good = build_file([lr.FrameRec(1, 2, 3, NORMAL_RAW), lr.FrameRec(2, 3, 4, NORMAL_RAW)])
    with pytest.raises(lr.ProtocolError, match="short"):
        lr.parse_file(good[:10])
    with pytest.raises(lr.ProtocolError, match="magic"):
        lr.parse_file(b"XXXXXXXX" + good[8:])
    with pytest.raises(lr.ProtocolError, match="version"):
        lr.parse_file(good[:8] + b"\x01" + good[9:])
    with pytest.raises(lr.ProtocolError, match="unknown record type"):
        lr.parse_file(good + b"\x07")
    with pytest.raises(lr.ProtocolError, match="truncated"):
        lr.parse_file(good + lr.encode_reboot_rec(1, 2, 3)[:-1])
    _, part = lr.parse_file(good + lr.encode_reboot_rec(1, 2, 3)[:-1], strict=False)
    assert [r.seq for r in part] == [1, 2]
    with pytest.raises(lr.ProtocolError, match=r"truncated|exceeds"):
        lr.parse_file(good[:-3])
    _, part = lr.parse_file(good[:-3], strict=False)
    assert [r.seq for r in part] == [1]
    with pytest.raises(lr.ProtocolError, match="exceeds"):
        lr.parse_file(good[:20] + good[20:43] + b"\xff\xff"[:0] + good[43:-1])


# ---- CSV / info ------------------------------------------------------------


def to_rows(recs):
    buf = io.StringIO()
    lr.write_csv(recs, buf)
    return list(csv.DictReader(io.StringIO(buf.getvalue())))


def test_csv_columns_and_values():
    rows = to_rows(
        [
            lr.FrameRec(7, 99, 555, NORMAL_RAW),
            lr.FrameRec(8, 100, 556, ENG_RAW),
            lr.GapRec(9, 12, 557),
            lr.FrameRec(12, 101, 558, b"garbage"),
            lr.RebootRec(5, 6, 559),
        ]
    )
    expected = (
        ["seq", "esp_time_us", "pc_time_ns", "data_type", "target_state", "moving_dist_cm"]
        + ["moving_energy", "still_dist_cm", "still_energy", "detect_dist_cm"]
        + ["max_moving_gate", "max_still_gate"]
        + [f"move_g{i}" for i in range(9)]
        + [f"still_g{i}" for i in range(9)]
    )
    assert list(rows[0].keys())[: len(expected)] == expected
    n, e, g, bad, reb = rows
    assert (n["seq"], n["esp_time_us"], n["pc_time_ns"], n["data_type"]) == ("7", "99", "555", "2")
    assert (n["target_state"], n["moving_dist_cm"], n["still_dist_cm"]) == ("3", "80", "120")
    assert (n["detect_dist_cm"], n["max_moving_gate"], n["move_g0"], n["still_g8"]) == (
        "85",
        "",
        "",
        "",
    )
    assert n["error"] == ""
    assert (e["data_type"], e["max_moving_gate"]) == ("1", "8")
    assert (e["move_g0"], e["move_g8"]) == ("100", "0")
    assert (e["still_g0"], e["still_g8"]) == ("0", "80")
    assert (g["record"], g["seq"], g["gap_to_seq"], g["data_type"]) == ("gap", "9", "12", "")
    assert bad["error"] and bad["data_type"] == ""
    assert (reb["record"], reb["pc_time_ns"], reb["seq"]) == ("reboot", "559", "")
    assert reb["data_type"] == ""
    assert (reb["old_boot_id"], reb["new_boot_id"]) == ("5", "6")


def test_info_summary():
    recs = [
        lr.FrameRec(1, 1_000_000, 10**9, NORMAL_RAW),
        lr.GapRec(2, 5, 2 * 10**9),
        lr.RebootRec(1, 2, 25 * 10**8),
        lr.FrameRec(5, 3_000_000, 3 * 10**9, NORMAL_RAW),
    ]
    s = lr.summarize(0, recs)
    assert s["reboots"] == [(1, 2, 25 * 10**8)]
    assert (s["frames"], s["first_seq"], s["last_seq"]) == (2, 1, 5)
    assert s["gaps"] == [(2, 5, 3)] and s["missing_frames"] == 3
    assert s["duration_s"] == 2.0 and s["esp_duration_s"] == 2.0
    assert lr.summarize(0, [])["first_seq"] is None


def test_cli_info_and_csv(tmp_path, capsys):
    f = tmp_path / "a.ldrec"
    f.write_bytes(build_file([lr.FrameRec(1, 2, 3, NORMAL_RAW)]))
    assert lr.main(["info", str(f)]) == 0
    out_text = capsys.readouterr().out
    assert "frames:        1" in out_text
    assert "reboots:       0" in out_text
    out = tmp_path / "a.csv"
    assert lr.main(["export-csv", str(f), "-o", str(out)]) == 0
    assert out.read_text().startswith("seq,esp_time_us,pc_time_ns")
    f.write_bytes(b"junk")
    assert lr.main(["info", str(f)]) == 1


# ---- fake server -----------------------------------------------------------


def frame_for(seq: int) -> bytes:
    p = bytearray(payload_of(NORMAL_RAW))
    p[3] = seq & 0xFF  # make frames distinguishable
    return frame_of(bytes(p))


BOOT_A = 0xA0A0A0A1
BOOT_B = 0xB0B0B0B2


def batch(first_seq: int, seqs: list[int], flags: int = 0, boot: int = BOOT_A) -> bytes:
    out = struct.pack("<4sBBHIHHI", b"LDRB", 2, flags, 0, first_seq, len(seqs), 0, boot)
    for s in seqs:
        raw = frame_for(s)
        out += struct.pack("<IQH", s, 1000 + s, len(raw)) + raw
    return out


class FakeServer:
    """Serves one scripted reply list per connection; records each request's from_seq."""

    def __init__(self, script: list[list[bytes]]) -> None:
        self.script = script
        self.requests: list[int] = []
        self.sock = socket.socket()
        self.sock.bind(("127.0.0.1", 0))
        self.sock.listen(4)
        self.sock.settimeout(5)
        self.port = self.sock.getsockname()[1]
        self.thread = threading.Thread(target=self._run, daemon=True)
        self.thread.start()

    def _run(self) -> None:
        try:
            for replies in self.script:
                conn, _ = self.sock.accept()
                with conn:
                    conn.settimeout(5)
                    req = b""
                    while len(req) < 12:
                        chunk = conn.recv(12 - len(req))
                        if not chunk:
                            break
                        req += chunk
                    assert req[:5] == b"LDRQ\x02"
                    self.requests.append(struct.unpack_from("<I", req, 8)[0])
                    for r in replies:
                        conn.sendall(r)
                    # linger briefly so the client closes first after max_frames
                    conn.settimeout(0.2)
                    with contextlib.suppress(OSError):
                        conn.recv(1)
        except OSError:
            pass

    def close(self) -> None:
        self.thread.join(5)
        self.sock.close()


def run_recorder(tmp_path, script, max_frames):
    srv = FakeServer(script)
    path = tmp_path / "r.ldrec"
    try:
        lr.record(
            "127.0.0.1", srv.port, path, max_frames=max_frames, timeout=2, backoff_initial=0.01
        )
    finally:
        srv.close()
    _, recs = lr.parse_file(path.read_bytes())
    return srv, recs


def summary(recs):
    return [
        ("gap", r.from_seq, r.to_seq) if isinstance(r, lr.GapRec) else ("f", r.seq) for r in recs
    ]


def test_reconnect_resume_and_gap(tmp_path):
    script = [
        [batch(0, [0, 1, 2])],  # then the connection drops
        [batch(6, [6, 7], flags=1)],  # device evicted 3..5: GAP, starts at oldest stored
    ]
    srv, recs = run_recorder(tmp_path, script, 5)
    assert srv.requests == [0, 3]
    assert summary(recs) == [("f", 0), ("f", 1), ("f", 2), ("gap", 3, 6), ("f", 6), ("f", 7)]
    for r in recs:
        if isinstance(r, lr.FrameRec):
            assert r.pc_time_ns > 0 and r.esp_time_us == 1000 + r.seq
            lr.decode_data_frame(r.raw)


def test_resume_without_gap_and_keepalive(tmp_path):
    script = [
        [batch(0, [0, 1]), batch(2, [])],  # keep-alive carries next expected seq
        [batch(2, []), batch(2, [2, 3])],
    ]
    srv, recs = run_recorder(tmp_path, script, 4)
    assert srv.requests == [0, 2]
    assert summary(recs) == [("f", 0), ("f", 1), ("f", 2), ("f", 3)]


def test_skip_ahead_without_gap_flag_and_duplicates(tmp_path):
    script = [
        [batch(0, [0, 1, 2])],
        [batch(1, [1, 2, 3, 6])],  # replayed 1,2 must not be duplicated; 4,5 missing
    ]
    srv, recs = run_recorder(tmp_path, script, 5)
    assert srv.requests == [0, 3]
    assert summary(recs) == [("f", 0), ("f", 1), ("f", 2), ("f", 3), ("gap", 4, 6), ("f", 6)]


def test_seq_wrap_across_reconnect(tmp_path):
    script = [
        [batch(0xFFFFFFFE, [0xFFFFFFFE, 0xFFFFFFFF])],
        [batch(0, [0, 1])],
        [batch(5, [5], flags=1)],
    ]
    srv, recs = run_recorder(tmp_path, script, 5)
    assert srv.requests == [0, 0, 2]
    assert summary(recs) == [
        ("f", 0xFFFFFFFE),
        ("f", 0xFFFFFFFF),
        ("f", 0),
        ("f", 1),
        ("gap", 2, 5),
        ("f", 5),
    ]
    _, back = lr.parse_file((tmp_path / "r.ldrec").read_bytes())
    assert back == recs


def test_bad_server_data_reconnects(tmp_path):
    script = [
        [batch(0, [0])[:20]],  # cut mid-record
        [b"XXXX" + b"\x00" * 12],  # bad magic
        [batch(0, [0, 1])],
    ]
    srv, recs = run_recorder(tmp_path, script, 2)
    assert srv.requests == [0, 0, 0]
    assert summary(recs) == [("f", 0), ("f", 1)]


def test_stop_event_ends_recording(tmp_path):
    stop = threading.Event()
    srv = FakeServer([[batch(0, [0])]])
    path = tmp_path / "s.ldrec"
    threading.Timer(0.3, stop.set).start()
    try:
        w = lr.record("127.0.0.1", srv.port, path, stop, timeout=0.5, backoff_initial=0.01)
    finally:
        srv.close()
    assert w.frames == 1
    _, recs = lr.parse_file(path.read_bytes())
    assert summary(recs) == [("f", 0)]


def reboot_summary(recs):
    return [
        ("reboot", r.old_boot_id, r.new_boot_id)
        if isinstance(r, lr.RebootRec)
        else ("gap", r.from_seq, r.to_seq)
        if isinstance(r, lr.GapRec)
        else ("f", r.seq)
        for r in recs
    ]


def test_boot_id_unchanged_reconnect_no_reboot_record(tmp_path):
    script = [[batch(0, [0, 1])], [batch(2, [2, 3])]]
    srv, recs = run_recorder(tmp_path, script, 4)
    assert srv.requests == [0, 2]
    assert reboot_summary(recs) == [("f", 0), ("f", 1), ("f", 2), ("f", 3)]


def test_esp32_reboot_mid_recording_keeps_all_frames(tmp_path):
    # Device rebooted: the new boot's seq restarts at 0, i.e. "older" than 2.
    # Frames before and after must all be kept, with exactly one reboot record.
    script = [
        [batch(0, [0, 1, 2], boot=BOOT_A)],
        # request from 3 on a device that already has 0..4 again: boot differs ->
        # the recorder must discard this batch and ask again from 0 (0..2 are not lost)
        [batch(3, [3, 4], boot=BOOT_B)],
        [batch(0, [0, 1, 2, 3, 4], boot=BOOT_B)],
    ]
    srv, recs = run_recorder(tmp_path, script, 8)
    assert srv.requests == [0, 3, 0]
    assert reboot_summary(recs) == [
        ("f", 0),
        ("f", 1),
        ("f", 2),
        ("reboot", BOOT_A, BOOT_B),
        ("f", 0),
        ("f", 1),
        ("f", 2),
        ("f", 3),
        ("f", 4),
    ]


def test_reboot_seq_restarts_at_zero_no_wrong_drop(tmp_path):
    # The usual case: the new boot has fewer frames than the old last seq.
    script = [
        [batch(0, [0, 1, 2, 3, 4, 5], boot=BOOT_A)],
        [batch(0, [], boot=BOOT_B)],  # boot differs; requested 6, device sends from 0
        [batch(0, [0, 1], boot=BOOT_B)],
    ]
    srv, recs = run_recorder(tmp_path, script, 8)
    assert srv.requests == [0, 6, 0]
    got = reboot_summary(recs)
    assert got.count(("reboot", BOOT_A, BOOT_B)) == 1
    assert [x for x in got if x[0] == "f"] == [("f", i) for i in range(6)] + [("f", 0), ("f", 1)]
    assert not any(x[0] == "gap" for x in got)
    # file decodes, reboot record sits between the two boots' frames
    assert isinstance(recs[6], lr.RebootRec)
    assert recs[6].pc_time_ns > 0


def test_reboot_with_gap_flag_on_new_boot_is_accepted(tmp_path):
    # After the reboot the new ring already evicted its oldest frames: GAP flag, first_seq 7
    script = [
        [batch(0, [0, 1], boot=BOOT_A)],
        [batch(7, [7], flags=1, boot=BOOT_B)],  # requested 2 -> boot differs -> redo from 0
        [batch(7, [7, 8], flags=1, boot=BOOT_B)],
        [batch(9, [9], boot=BOOT_B)],  # same boot: resume from last+1, no extra reboot record
    ]
    srv, recs = run_recorder(tmp_path, script, 5)
    assert srv.requests == [0, 2, 0, 9]
    assert reboot_summary(recs) == [
        ("f", 0),
        ("f", 1),
        ("reboot", BOOT_A, BOOT_B),
        ("f", 7),
        ("f", 8),
        ("f", 9),
    ]


def test_two_reboots(tmp_path):
    script = [
        [batch(0, [0], boot=BOOT_A)],
        [batch(0, [0], boot=BOOT_B)],  # requested 1, from_seq != 0 -> redo
        [batch(0, [0], boot=BOOT_B)],
        [batch(0, [0], boot=BOOT_A)],  # another reboot, boot id differs again
        [batch(0, [0], boot=BOOT_A)],
    ]
    srv, recs = run_recorder(tmp_path, script, 3)
    assert srv.requests == [0, 1, 0, 1, 0]
    assert reboot_summary(recs) == [
        ("f", 0),
        ("reboot", BOOT_A, BOOT_B),
        ("f", 0),
        ("reboot", BOOT_B, BOOT_A),
        ("f", 0),
    ]


def test_cli_info_and_csv_with_reboot(tmp_path, capsys):
    f = tmp_path / "r.ldrec"
    f.write_bytes(
        build_file(
            [
                lr.FrameRec(1, 2, 3, NORMAL_RAW),
                lr.RebootRec(7, 9, 10),
                lr.FrameRec(0, 1, 11, NORMAL_RAW),
            ]
        )
    )
    assert lr.main(["info", str(f)]) == 0
    assert "reboots:       1" in capsys.readouterr().out
    out = tmp_path / "r.csv"
    assert lr.main(["export-csv", str(f), "-o", str(out)]) == 0
    rows = list(csv.DictReader(out.open()))
    assert [r["record"] for r in rows] == ["frame", "reboot", "frame"]


def test_server_with_other_version_batch_reconnects(tmp_path):
    old = bytearray(batch(0, [0]))
    old[4] = 1  # an old-protocol batch must be refused, not misparsed
    script = [[bytes(old)], [batch(0, [0])]]
    srv, recs = run_recorder(tmp_path, script, 1)
    assert srv.requests == [0, 0]
    assert reboot_summary(recs) == [("f", 0)]


# ---- Ctrl+C / stop promptness ------------------------------------------------


class SilentServer:
    """Accepts one connection, reads the request, then sends nothing."""

    def __init__(self) -> None:
        self.sock = socket.socket()
        self.sock.bind(("127.0.0.1", 0))
        self.sock.listen(4)
        self.port = self.sock.getsockname()[1]
        self.conns: list[socket.socket] = []
        threading.Thread(target=self._run, daemon=True).start()

    def _run(self) -> None:
        with contextlib.suppress(OSError):
            while True:
                c, _ = self.sock.accept()
                self.conns.append(c)

    def close(self) -> None:
        for c in self.conns:
            c.close()
        self.sock.close()


def test_stop_during_blocking_read_is_prompt(tmp_path):
    """stop set while recv waits (idle device, long timeout) must not wait for the timeout."""
    stop = threading.Event()
    srv = SilentServer()
    threading.Timer(0.3, stop.set).start()
    t0 = time.monotonic()
    try:
        lr.record("127.0.0.1", srv.port, tmp_path / "a.ldrec", stop, timeout=30)
    finally:
        srv.close()
    assert time.monotonic() - t0 < 1.5


def test_stop_during_backoff_is_prompt(tmp_path):
    stop = threading.Event()
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    port = s.getsockname()[1]
    s.close()  # nothing listens: connection refused -> backoff
    threading.Timer(0.3, stop.set).start()
    t0 = time.monotonic()
    lr.record("127.0.0.1", port, tmp_path / "b.ldrec", stop, backoff_initial=30, backoff_max=30)
    assert time.monotonic() - t0 < 1.5


def test_stop_mid_batch_is_prompt_and_file_clean(tmp_path):
    """A slow trickle inside one batch must not delay stop; whole records only in the file."""
    stop = threading.Event()
    full = batch(0, list(range(50)))
    srv = FakeServer([[full[:100]]])  # 20-byte header + part of the records, then silence
    path = tmp_path / "c.ldrec"
    threading.Timer(0.3, stop.set).start()
    t0 = time.monotonic()
    try:
        w = lr.record("127.0.0.1", srv.port, path, stop, timeout=30)
    finally:
        srv.close()
    assert time.monotonic() - t0 < 1.5
    assert w.frames == 0
    lr.parse_file(path.read_bytes())  # strict parse: no truncated record


def test_info_tolerates_truncated_last_record(tmp_path, capsys):
    path = tmp_path / "t.ldrec"
    path.write_bytes(
        lr.encode_file_header(1) + lr.encode_frame_rec(0, 1, 2, frame_for(0))
        + lr.encode_frame_rec(1, 2, 3, frame_for(1))[:-5]
    )
    assert lr.main(["info", str(path)]) == 0
    assert "frames:        1" in capsys.readouterr().out


def _run_cli(tmp_path, host, port, sigs, wait=1.0):
    path = tmp_path / "cli.ldrec"
    p = subprocess.Popen(
        [sys.executable, str(Path(lr.__file__)), "record", host,
         "--port", str(port), "-o", str(path)],
        stderr=subprocess.PIPE,
        text=True,
    )
    time.sleep(wait)
    t0 = time.monotonic()
    for _ in range(sigs):
        p.send_signal(signal.SIGINT)
        time.sleep(0.05)
    try:
        rc = p.wait(3)
    except subprocess.TimeoutExpired:
        p.kill()
        raise
    return rc, time.monotonic() - t0, p.stderr.read(), path


@pytest.mark.skipif(sys.platform == "win32", reason="POSIX signals")
@pytest.mark.parametrize("sigs", [1, 5])
def test_cli_sigint_during_catchup_burst(tmp_path, sigs):
    raw = frame_for(0)
    stopper = threading.Event()
    srv = socket.socket()
    srv.bind(("127.0.0.1", 0))
    srv.listen(2)

    def serve():
        c, _ = srv.accept()
        c.recv(12)
        seq = 0
        while not stopper.is_set():
            body = b"".join(struct.pack("<IQH", seq + i, i, len(raw)) + raw for i in range(20))
            hdr = struct.pack("<4sBBHIHHI", b"LDRB", 2, 0, 0, seq, 20, 0, BOOT_A)
            try:
                c.sendall(hdr + body)
            except OSError:
                return
            seq += 20

    threading.Thread(target=serve, daemon=True).start()
    try:
        rc, dt, err, path = _run_cli(tmp_path, "127.0.0.1", srv.getsockname()[1], sigs)
    finally:
        stopper.set()
        srv.close()
    assert rc == 0
    assert dt < 1.5
    assert "interrupted" in err
    lr._load(str(path))  # readable by `info` (a truncated last record is tolerated)


@pytest.mark.skipif(sys.platform == "win32", reason="POSIX signals")
def test_cli_sigint_idle_device_long_timeout(tmp_path):
    srv = SilentServer()
    try:
        rc, dt, err, _ = _run_cli(tmp_path, "127.0.0.1", srv.port, 1)
    finally:
        srv.close()
    assert rc == 0
    assert dt < 1.5
    assert "interrupted" in err


@pytest.mark.skipif(sys.platform == "win32", reason="POSIX signals")
def test_cli_sigint_during_backoff(tmp_path):
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    port = s.getsockname()[1]
    s.close()
    rc, dt, err, _ = _run_cli(tmp_path, "127.0.0.1", port, 1, wait=2.0)
    assert rc == 0
    assert dt < 1.5
    assert "interrupted" in err
