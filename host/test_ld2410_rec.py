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
from dataclasses import asdict, replace
from pathlib import Path

import ld2410_rec as lr
import pytest

VEC = Path(__file__).resolve().parent.parent / "esp32" / "tests" / "vectors"


def vec(name: str) -> tuple[bytes, dict]:
    return (VEC / f"{name}.bin").read_bytes(), json.loads((VEC / f"{name}.json").read_text())


GPS_VECS = ["gps_fix_nominal", "gps_fix_southwest", "gps_fix_nofix", "gps_fix_extremes"]
IMU_VECS = ["imu_nominal", "imu_extremes"]
SYNC_VECS = ["time_sync_gps", "time_sync_sntp", "time_sync_edge"]
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
        "batch_v3_mixed",
        *GPS_VECS,
        *IMU_VECS,
        *SYNC_VECS,
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
    assert lr.encode_request(0x01020304) == b"LDRQ\x03\x00\x00\x00\x04\x03\x02\x01"
    assert lr.encode_request(0) == b"LDRQ" + b"\x03\x00\x00\x00" + b"\x00" * 4
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
    assert hdr.version == 3 and hdr.boot_id == 0xA1B2C3D4
    assert hdr.gap
    assert [(r.seq, r.esp_time_us, r.rtype, r.raw.hex()) for r in recs] == [
        (r["seq"], r["esp_time_us"], r["type"], r["raw_hex"]) for r in exp["records"]
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
    for old_version in (0, 1, 2, 4):  # only v3 is accepted
        with pytest.raises(lr.ProtocolError, match="version"):
            lr.parse_batch_header(raw[:4] + bytes([old_version]) + raw[5:])
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
    bad_len[20 + 13 : 20 + 15] = b"\xff\xff"  # record length beyond data
    with pytest.raises(lr.ProtocolError, match="exceeds"):
        lr.parse_batch(bytes(bad_len))
    empty = raw[:4] + b"\x03\x00\x00\x00" + raw[8:12] + b"\x00\x00\x00\x00" + raw[16:20]
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
        elif isinstance(r, lr.GpsRec):
            out += lr.encode_sensor_rec(lr.REC_GPS, r.seq, r.esp_time_us, r.pc_time_ns, r.payload)
        elif isinstance(r, lr.ImuRec):
            out += lr.encode_sensor_rec(lr.REC_IMU, r.seq, r.esp_time_us, r.pc_time_ns, r.payload)
        elif isinstance(r, lr.TimeSyncRec):
            out += lr.encode_sensor_rec(
                lr.REC_TIME_SYNC, r.seq, r.esp_time_us, r.pc_time_ns, r.payload
            )
        elif isinstance(r, lr.UnknownRec):
            out += lr.encode_unknown_rec(r.wire_type, r.seq, r.esp_time_us, r.pc_time_ns, r.payload)
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
    assert data[8:12] == b"\x03\x00\x00\x00"
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


def batch(
    first_seq: int,
    seqs: list[int],
    flags: int = 0,
    boot: int = BOOT_A,
    other: dict[int, tuple[int, bytes]] | None = None,
) -> bytes:
    """A v3 batch; seqs in `other` carry (wire type, payload) instead of a frame."""
    out = struct.pack("<4sBBHIHHI", b"LDRB", 3, flags, 0, first_seq, len(seqs), 0, boot)
    for s in seqs:
        rtype, raw = (other or {}).get(s, (0, None))
        raw = frame_for(s) if raw is None else raw
        out += struct.pack("<IQBH", s, 1000 + s, rtype, len(raw)) + raw
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
                    assert req[:5] == b"LDRQ\x03"
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
    v2 = bytearray(batch(0, [0]))
    v2[4] = 2  # a v2 device (older firmware) is refused too, not misparsed
    script = [[bytes(old)], [bytes(v2)], [batch(0, [0])]]
    srv, recs = run_recorder(tmp_path, script, 1)
    assert srv.requests == [0, 0, 0]
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
            body = b"".join(struct.pack("<IQBH", seq + i, i, 0, len(raw)) + raw for i in range(20))
            hdr = struct.pack("<4sBBHIHHI", b"LDRB", 3, 0, 0, seq, 20, 0, BOOT_A)
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


# ---- protocol v3: payloads, typed records ------------------------------------


def jtuple(d: dict) -> dict:
    return {k: tuple(v) if isinstance(v, list) else v for k, v in d.items()}


@pytest.mark.parametrize("name", GPS_VECS)
def test_gps_fix_vectors(name):
    raw, exp = vec(name)
    assert exp["record_type"] == lr.WIRE_GPS_FIX and raw.hex() == exp["payload_hex"]
    assert len(raw) == lr.GPS_FIX_LEN == 32
    g = lr.decode_gps_fix(raw)
    assert asdict(g) == exp["gps_fix"]
    assert lr.encode_gps_fix(g) == raw


@pytest.mark.parametrize("name", IMU_VECS)
def test_imu_vectors(name):
    raw, exp = vec(name)
    assert exp["record_type"] == lr.WIRE_IMU and raw.hex() == exp["payload_hex"]
    assert len(raw) == lr.IMU_LEN == 18
    m = lr.decode_imu(raw)
    assert asdict(m) == jtuple(exp["imu"])
    assert lr.encode_imu(m) == raw


@pytest.mark.parametrize("name", SYNC_VECS)
def test_time_sync_vectors(name):
    raw, exp = vec(name)
    assert exp["record_type"] == lr.WIRE_TIME_SYNC and raw.hex() == exp["payload_hex"]
    assert len(raw) == lr.TIME_SYNC_LEN == 9
    t = lr.decode_time_sync(raw)
    assert asdict(t) == exp["time_sync"]
    assert lr.encode_time_sync(t) == raw


def test_payload_edge_values():
    g = lr.decode_gps_fix(vec("gps_fix_southwest")[0])
    assert (g.lat_e7, g.lon_e7, g.alt_cm) == (-338688197, -1224194155, -1250)
    assert g.pos_valid and not g.alt_valid and g.time_valid
    x = lr.decode_gps_fix(vec("gps_fix_extremes")[0])
    assert (x.lat_e7, x.lon_e7, x.alt_cm) == (-900000000, -1800000000, -(2**31))
    assert x.utc_unix_ms == 2**63 - 1 and x.speed_cmps == 65535 and x.sats == 255
    n = lr.decode_gps_fix(vec("gps_fix_nofix")[0])
    assert not (n.pos_valid or n.has_fix) and n.utc_unix_ms == 0
    m = lr.decode_imu(vec("imu_extremes")[0])
    assert m.acc_mg == (-32768, 32767, -1) and m.gyr_ddps == (32767, -32768, 0) and not m.valid
    assert lr.decode_time_sync(vec("time_sync_edge")[0]).utc_unix_us == -(2**63)


def test_payload_hand_derived_bytes():
    # independent of the vectors; same bytes as the C test test_explicit_layout
    exp = (
        bytes([254, 255, 255, 255, 255, 255, 255, 255])  # utc_unix_ms = -2
        + bytes([255, 255, 255, 255, 4, 3, 2, 1])  # lat_e7 = -1, lon_e7 = 0x01020304
        + bytes([0, 255, 255, 255, 6, 5, 8, 7])  # alt_cm = -256, speed, course
        + bytes([10, 9, 11, 12, 13, 0, 0, 0])  # hdop, sats, fix_quality, flags, reserved
    )
    g = lr.GpsFix(-2, -1, 0x01020304, -256, 0x0506, 0x0708, 0x090A, 0x0B, 0x0C, 0x0D)
    assert lr.encode_gps_fix(g) == exp and lr.decode_gps_fix(exp) == g
    m = lr.ImuSample((1, -2, 3), (-4, 5, -6), -7, 0x0809, 10, 1)
    expi = bytes([1, 0, 0xFE, 0xFF, 3, 0, 0xFC, 0xFF, 5, 0, 0xFA, 0xFF, 0xF9, 0xFF, 9, 8, 10, 1])
    assert lr.encode_imu(m) == expi and lr.decode_imu(expi) == m
    t = lr.TimeSync(-3, 2)
    expt = bytes([0xFD] + [0xFF] * 7 + [2])
    assert lr.encode_time_sync(t) == expt and lr.decode_time_sync(expt) == t


def test_payload_wrong_lengths_and_reserved():
    for n in range(45):
        for dec, size in (
            (lr.decode_gps_fix, 32),
            (lr.decode_imu, 18),
            (lr.decode_time_sync, 9),
        ):
            if n == size:
                dec(bytes(n))
            else:
                with pytest.raises(lr.PayloadError, match="expected"):
                    dec(bytes(n))
    good = bytearray(vec("gps_fix_nominal")[0])
    for i in (29, 30, 31):
        bad = bytearray(good)
        bad[i] = 1
        with pytest.raises(lr.PayloadError, match="reserved"):
            lr.decode_gps_fix(bytes(bad))
    # PayloadError is not a ProtocolError: a damaged payload never kills a recording
    assert not issubclass(lr.PayloadError, lr.ProtocolError)


def test_speed_saturation():
    base = lr.decode_gps_fix(vec("gps_fix_nominal")[0])
    for given, want in ((0, 0), (65534, 65534), (65535, 65535), (65536, 65535), (10**9, 65535)):
        out = lr.decode_gps_fix(lr.encode_gps_fix(replace(base, speed_cmps=given)))
        assert out.speed_cmps == want
        assert (out.course_cdeg, out.hdop_x100, out.sats) == (
            base.course_cdeg,
            base.hdop_x100,
            base.sats,
        )


def test_batch_v3_mixed_vector():
    raw, exp = vec("batch_v3_mixed")
    hdr, recs = lr.parse_batch(raw)
    assert (hdr.version, hdr.flags, hdr.first_seq, hdr.count, hdr.boot_id) == (
        3,
        0,
        0xFFFFFFFD,
        5,
        0x0BADCAFE,
    )
    assert [r.rtype for r in recs] == [0, 1, 2, 3, 0]
    assert [r.seq for r in recs] == [0xFFFFFFFD, 0xFFFFFFFE, 0xFFFFFFFF, 0, 1]  # one shared seq
    assert [(r.seq, r.esp_time_us, r.rtype, r.raw.hex()) for r in recs] == [
        (j["seq"], j["esp_time_us"], j["type"], j["raw_hex"]) for j in exp["records"]
    ]
    # the real captures travel unmodified as type 0 records
    assert recs[0].raw == vec("frame_engineering_real_0")[0]
    assert recs[4].raw == vec("frame_engineering_real_1")[0]
    for r in (recs[0], recs[4]):
        assert lr.decode_data_frame(r.raw).engineering == 1
    assert recs[1].raw == vec("gps_fix_nominal")[0]
    assert recs[2].raw == vec("imu_nominal")[0]
    assert lr.decode_time_sync(recs[3].raw) == lr.decode_time_sync(vec("time_sync_gps")[0])
    assert recs[4].esp_time_us > 2**32
    nxt = (hdr.first_seq + hdr.count) & lr.SEQ_MASK
    assert nxt == 2 and lr.seq_delta(recs[4].seq, recs[0].seq) == 4


def test_unknown_wire_type_is_kept_not_fatal():
    b = batch(0, [0, 1, 2], other={1: (9, b"\x01\x02\x03"), 2: (0xFF, b"")})
    _, recs = lr.parse_batch(b)
    assert [(r.rtype, r.raw) for r in recs] == [(0, frame_for(0)), (9, b"\x01\x02\x03"), (255, b"")]
    out = lr.encode_wire_record(recs[1], 77)
    data = lr.encode_file_header(1) + out + lr.encode_wire_record(recs[2], 78)
    _, back = lr.parse_file(data)
    assert back == [
        lr.UnknownRec(1, 1001, 77, 9, b"\x01\x02\x03"),
        lr.UnknownRec(2, 1002, 78, 255, b""),
    ]
    assert lr.csv_rows(back) == []  # no row, no crash


def test_wire_to_file_mapping_and_v3_file_round_trip():
    pl = {1: vec("gps_fix_nominal")[0], 2: vec("imu_nominal")[0], 3: vec("time_sync_gps")[0]}
    recs = [
        lr.FrameRec(0xFFFFFFFE, 10, 111, NORMAL_RAW),
        lr.GpsRec(0xFFFFFFFF, 11, 112, pl[1]),
        lr.ImuRec(0, 12, 113, pl[2]),
        lr.TimeSyncRec(1, 2**33, 114, pl[3]),
        lr.UnknownRec(2, 13, 115, 42, b"zz"),
        lr.GapRec(3, 9, 116),
        lr.RebootRec(1, 2, 117),
    ]
    data = build_file(recs)
    created, back = lr.parse_file(data)
    assert created == 1234 and back == recs
    # layouts: type, seq u32, esp u64, pc u64, len u16, payload
    gps = lr.encode_sensor_rec(lr.REC_GPS, 5, 6, 7, pl[1])
    assert gps[0] == 3 and gps[1:] == struct.pack("<IQQH", 5, 6, 7, 32) + pl[1]
    assert lr.encode_sensor_rec(lr.REC_IMU, 5, 6, 7, pl[2])[0] == 4
    assert lr.encode_sensor_rec(lr.REC_TIME_SYNC, 5, 6, 7, pl[3])[0] == 5
    unk = lr.encode_unknown_rec(42, 5, 6, 7, b"zz")
    assert unk == b"\x06" + struct.pack("<IQQBH", 5, 6, 7, 42, 2) + b"zz"
    for wire, ftype in ((0, 0), (1, 3), (2, 4), (3, 5), (4, 6), (200, 6)):
        assert lr.encode_wire_record(lr.Record(1, 2, wire, b"\x00" * 10), 3)[0] == ftype
    with pytest.raises(ValueError, match="not a sensor"):
        lr.encode_sensor_rec(0, 1, 2, 3, b"")


def test_v3_file_truncation_of_new_records():
    gps = lr.encode_sensor_rec(lr.REC_GPS, 1, 2, 3, vec("gps_fix_nominal")[0])
    good = lr.encode_file_header(1) + lr.encode_frame_rec(0, 1, 2, NORMAL_RAW)
    for cut in (1, 10, len(gps) - 1):
        with pytest.raises(lr.ProtocolError, match=r"truncated|exceeds"):
            lr.parse_file(good + gps[:cut])
        _, part = lr.parse_file(good + gps[:cut], strict=False)
        assert len(part) == 1
    unk = lr.encode_unknown_rec(9, 1, 2, 3, b"abc")
    for cut in (1, 15, len(unk) - 1):
        _, part = lr.parse_file(good + unk[:cut], strict=False)
        assert len(part) == 1
    assert len(lr.parse_file(good + gps + unk)[1]) == 3


# ---- v2 files stay readable -------------------------------------------------


def build_v2_file() -> bytes:
    """A version 2 file built byte by byte with the OLD layout (not via the v3 writer)."""
    out = b"LDREC1\x00\x00" + struct.pack("<HHQ", 2, 0, 987654321)
    out += struct.pack("<BIQQH", 0, 0xFFFFFFFE, 10, 111, len(NORMAL_RAW)) + NORMAL_RAW
    out += struct.pack("<BIIQ", 1, 0xFFFFFFFF, 3, 222)  # gap
    out += struct.pack("<BIQQH", 0, 3, 2**33, 333, len(ENG_RAW)) + ENG_RAW
    out += struct.pack("<BIIQ", 2, 0xAAAA, 0xBBBB, 444)  # reboot
    out += struct.pack("<BIQQH", 0, 0, 5, 555, len(NORMAL_RAW)) + NORMAL_RAW
    return out


def test_v2_file_is_readable():
    data = build_v2_file()
    assert data[8:10] == b"\x02\x00"
    created, recs = lr.parse_file(data)
    assert created == 987654321
    assert recs == [
        lr.FrameRec(0xFFFFFFFE, 10, 111, NORMAL_RAW),
        lr.GapRec(0xFFFFFFFF, 3, 222),
        lr.FrameRec(3, 2**33, 333, ENG_RAW),
        lr.RebootRec(0xAAAA, 0xBBBB, 444),
        lr.FrameRec(0, 5, 555, NORMAL_RAW),
    ]
    rows = to_rows(recs)
    assert [r["record"] for r in rows] == ["frame", "gap", "frame", "reboot", "frame"]
    frames = [r for r in rows if r["record"] == "frame"]
    assert all(r["frame_utc"] == "" and r["time_source"] == "" for r in frames)
    assert rows[0]["detect_dist_cm"] == "85" and rows[2]["max_moving_gate"] == "8"
    s = lr.summarize(created, recs)
    assert (s["frames"], s["gps_records"], s["imu_records"], s["time_syncs"]) == (3, 0, 0, 0)
    assert s["fix_ratio"] is None
    # truncated last record of a v2 file is tolerated like before
    _, part = lr.parse_file(data[:-3], strict=False)
    assert len(part) == 4


def test_v2_file_rejects_v3_only_types(tmp_path):
    data = build_v2_file()
    gps = lr.encode_sensor_rec(lr.REC_GPS, 1, 2, 3, vec("gps_fix_nominal")[0])
    with pytest.raises(lr.ProtocolError, match="unknown record type 3"):
        lr.parse_file(data + gps)
    with pytest.raises(lr.ProtocolError, match="unknown record type"):
        lr.parse_file(data + lr.encode_unknown_rec(9, 1, 2, 3, b""))
    f = tmp_path / "v2.ldrec"
    f.write_bytes(data)
    assert lr.main(["info", str(f)]) == 0
    assert lr.main(["export-csv", str(f), "-o", str(tmp_path / "v2.csv")]) == 0
    with pytest.raises(lr.ProtocolError, match="version"):
        lr.parse_file(data[:8] + b"\x04\x00" + data[10:])
    with pytest.raises(lr.ProtocolError, match="version"):
        lr.parse_file(data[:8] + b"\x01\x00" + data[10:])


# ---- UTC conversion ---------------------------------------------------------

U0 = 1790944496_000_000  # 2026-10-02T12:34:56Z in unix microseconds


def sync_rec(seq, esp, utc_us, source=1, pc=1):
    return lr.TimeSyncRec(seq, esp, pc, lr.encode_time_sync(lr.TimeSync(utc_us, source)))


def frame_at(seq, esp):
    return lr.FrameRec(seq, esp, 10 * seq, NORMAL_RAW)


def utc_of(recs, which):
    rows = [r for r in lr.csv_rows(recs) if r["record"] == "frame"]
    return (rows[which]["frame_utc"], rows[which]["time_source"])


def test_format_utc_us():
    assert lr.format_utc_us(U0) == "2026-10-02T12:34:56.000000Z"
    assert lr.format_utc_us(U0 + 1) == "2026-10-02T12:34:56.000001Z"
    assert lr.format_utc_us(0) == "1970-01-01T00:00:00.000000Z"
    assert lr.format_utc_us(-1) == "1969-12-31T23:59:59.999999Z"
    assert lr.format_utc_us(2**63 - 1) == "" and lr.format_utc_us(-(2**63)) == ""
    assert lr.format_utc_us(10**30) == ""


def test_utc_nearest_preceding_sync():
    recs = [
        sync_rec(0, 1_000_000, U0, 1),
        sync_rec(1, 10_000_000, U0 + 9_000_050, 2),  # 50 us of drift, other source
        frame_at(2, 3_500_000),  # 2.5 s after A
        frame_at(3, 9_900_000),  # B is closer, but A is the nearest PRECEDING one
        frame_at(4, 10_000_000),  # exactly at B: B (preceding or equal)
        frame_at(5, 12_000_000),
    ]
    assert utc_of(recs, 0) == ("2026-10-02T12:34:58.500000Z", "gps")
    assert utc_of(recs, 1) == ("2026-10-02T12:35:04.900000Z", "gps")
    assert utc_of(recs, 2) == ("2026-10-02T12:35:05.000050Z", "sntp")
    assert utc_of(recs, 3) == ("2026-10-02T12:35:07.000050Z", "sntp")


def test_utc_fallback_to_following_sync_and_none():
    recs = [frame_at(0, 400_000), sync_rec(1, 1_000_000, U0), frame_at(2, 1_000_001)]
    assert utc_of(recs, 0) == ("2026-10-02T12:34:55.400000Z", "gps")  # 0.6 s before the sync
    assert utc_of(recs, 1) == ("2026-10-02T12:34:56.000001Z", "gps")
    assert utc_of([frame_at(0, 5)], 0) == ("", "")  # no sync at all -> empty


def test_utc_never_uses_a_sync_of_another_boot():
    recs = [
        sync_rec(0, 1_000_000, U0, 1),
        frame_at(1, 2_000_000),
        lr.RebootRec(1, 2, 5),
        frame_at(0, 100),  # new boot, no sync yet: boot 0's sync must NOT be used
        sync_rec(1, 5_000_000, U0 + 3_600_000_000, 2),  # an hour later
        frame_at(2, 6_000_000),
        lr.RebootRec(2, 3, 6),
        frame_at(0, 7_000_000),  # third boot: no sync of its own -> empty
    ]
    assert utc_of(recs, 0) == ("2026-10-02T12:34:57.000000Z", "gps")
    # boot 1: fallback to ITS following sync (5 s - 100 us before it), not boot 0's
    t1, s1 = utc_of(recs, 1)
    assert s1 == "sntp" and t1 == lr.format_utc_us(U0 + 3_600_000_000 - (5_000_000 - 100))
    assert utc_of(recs, 2) == ("2026-10-02T13:34:57.000000Z", "sntp")
    assert utc_of(recs, 3) == ("", "")
    idx = lr.TimeIndex(recs)
    assert idx.utc_us(0, 2_000_000) == (U0 + 1_000_000, 1)
    assert idx.utc_us(2, 7_000_000) is None and idx.utc_us(7, 1) is None


def test_utc_ignores_damaged_sync_and_handles_odd_values():
    bad = lr.TimeSyncRec(0, 100, 1, b"\x00" * 8)  # 8 bytes: damaged
    recs = [bad, frame_at(1, 200), sync_rec(2, 300, U0, 7), frame_at(3, 300)]
    assert utc_of(recs, 0) == ("2026-10-02T12:34:55.999900Z", "src7")  # unknown source named
    # a sync far outside the datetime range gives empty text instead of an exception
    assert utc_of([sync_rec(0, 1, 2**63 - 1), frame_at(1, 2)], 0) == ("", "")
    assert utc_of([sync_rec(0, 1, -(2**63)), frame_at(1, 2)], 0) == ("", "")


# ---- CSV: extra columns and per-sensor files ---------------------------------


def gps_rec(seq, esp, **kw):
    base = {
        "utc_unix_ms": 1790944496789,
        "lat_e7": 525200000,
        "lon_e7": 134050000,
        "alt_cm": 3475,
        "speed_cmps": 100,
        "course_cdeg": 9000,
        "hdop_x100": 95,
        "sats": 9,
        "fix_quality": 1,
        "flags": 0x0F,
    }
    base.update(kw)
    return lr.GpsRec(seq, esp, 5, lr.encode_gps_fix(lr.GpsFix(**base)))


def imu_rec(seq, esp, pitch=-1234, roll=4567, status=1):
    m = lr.ImuSample((1, 2, 3), (4, -5, 6), pitch, roll, 10, status)
    return lr.ImuRec(seq, esp, 6, lr.encode_imu(m))


def test_csv_extra_columns_latest_values():
    recs = [
        frame_at(0, 100),  # nothing recorded yet: all extra sensor columns empty
        sync_rec(1, 150, U0),
        gps_rec(2, 200),
        imu_rec(3, 210),
        frame_at(4, 1_000_150),
        gps_rec(5, 300, lat_e7=-5000000, lon_e7=-1, alt_cm=-5, flags=0x07, fix_quality=2, sats=4),
        imu_rec(6, 310, pitch=0, roll=-5, status=1),
        frame_at(7, 2_000_150),
        gps_rec(8, 400, flags=0, fix_quality=0, sats=0, utc_unix_ms=0, lat_e7=0, lon_e7=0),
        imu_rec(9, 410, pitch=999, roll=999, status=0),  # sensor says invalid
        frame_at(10, 3_000_150),
    ]
    cols = lr.CSV_COLUMNS
    assert cols[-12:] == [
        "frame_utc", "time_source", "gps_utc", "gps_lat", "gps_lon", "gps_alt_m",
        "gps_sats", "gps_hdop", "gps_fix_quality", "gps_flags", "pitch_deg", "roll_deg",
    ]  # fmt: skip
    rows = to_rows(recs)
    assert [r["record"] for r in rows] == ["frame"] * 4
    f0, f1, f2, f3 = rows
    # frame 0 precedes the sync: fallback to the following one (50 us before it)
    assert (f0["frame_utc"], f0["time_source"]) == ("2026-10-02T12:34:55.999950Z", "gps")
    for c in ("gps_utc", "gps_lat", "gps_lon", "gps_alt_m", "gps_sats", "pitch_deg", "roll_deg"):
        assert f0[c] == ""
    assert (f1["frame_utc"], f1["time_source"]) == ("2026-10-02T12:34:57.000000Z", "gps")
    assert (f1["gps_utc"], f1["gps_lat"], f1["gps_lon"], f1["gps_alt_m"]) == (
        "2026-10-02T12:34:56.789000Z",
        "52.5200000",
        "13.4050000",
        "34.75",
    )
    assert (f1["gps_sats"], f1["gps_hdop"], f1["gps_fix_quality"], f1["gps_flags"]) == (
        "9",
        "0.95",
        "1",
        "15",
    )
    assert (f1["pitch_deg"], f1["roll_deg"]) == ("-12.34", "45.67")
    # negative coordinates / small magnitudes keep their sign and leading zeros
    assert (f2["gps_lat"], f2["gps_lon"], f2["gps_alt_m"]) == ("-0.5000000", "-0.0000001", "")
    assert (f2["gps_fix_quality"], f2["gps_flags"], f2["gps_sats"]) == ("2", "7", "4")
    assert (f2["pitch_deg"], f2["roll_deg"]) == ("0.00", "-0.05")
    # no-fix record: validity flags 0 -> position/alt/utc empty; invalid imu -> empty tilt
    for c in ("gps_utc", "gps_lat", "gps_lon", "gps_alt_m", "pitch_deg", "roll_deg"):
        assert f3[c] == ""
    assert (f3["gps_fix_quality"], f3["gps_flags"], f3["gps_sats"]) == ("0", "0", "0")
    # gps/imu/time_sync records do not get rows of their own in the main CSV
    assert len(rows) == 4


def test_csv_latest_values_reset_on_reboot_and_survive_damage():
    recs = [
        gps_rec(0, 100),
        imu_rec(1, 110),
        lr.GpsRec(2, 120, 5, b"short"),  # damaged: the previous fix stays the latest
        frame_at(3, 130),
        lr.RebootRec(1, 2, 9),
        frame_at(0, 5),  # new boot: the old boot's fix and tilt must not leak in
    ]
    a, _, b = to_rows(recs)
    assert a["gps_lat"] == "52.5200000" and a["pitch_deg"] == "-12.34"
    assert b["gps_lat"] == b["gps_sats"] == b["pitch_deg"] == b["frame_utc"] == ""


def test_csv_extreme_gps_values_do_not_crash():
    ex = lr.GpsRec(0, 1, 2, vec("gps_fix_extremes")[0])
    rows = to_rows([ex, frame_at(1, 2)])
    r = rows[0]
    assert r["gps_lat"] == "-90.0000000" and r["gps_lon"] == "-180.0000000"
    assert r["gps_alt_m"] == "-21474836.48" and r["gps_utc"] == ""  # utc out of range
    assert (r["gps_sats"], r["gps_hdop"]) == ("255", "655.35")


def test_fixed_formatting():
    assert lr.fixed(0, 2) == "0.00" and lr.fixed(-1, 2) == "-0.01" and lr.fixed(-100, 2) == "-1.00"
    assert lr.fixed(123456789, 7) == "12.3456789" and lr.fixed(5, 0) == "5"
    assert lr.fixed(-(2**31), 2) == "-21474836.48"


def test_per_sensor_csv_files(tmp_path):
    recs = [
        sync_rec(0, 100, U0),
        gps_rec(1, 1_000_100),
        gps_rec(2, 2_000_100, flags=0, fix_quality=0, lat_e7=0, lon_e7=0, utc_unix_ms=0),
        lr.GpsRec(3, 3_000_100, 5, b"\x01\x02"),
        imu_rec(4, 1_100_100),
        imu_rec(5, 1_200_100, status=0),
        lr.ImuRec(6, 1_300_100, 5, b""),
        frame_at(7, 1_400_100),
        lr.UnknownRec(8, 1, 1, 77, b"x"),
    ]
    g, i = io.StringIO(), io.StringIO()
    assert lr.write_gps_csv(recs, g) == 3 and lr.write_imu_csv(recs, i) == 3
    gr = list(csv.DictReader(io.StringIO(g.getvalue())))
    assert list(gr[0].keys()) == lr.GPS_CSV_COLUMNS
    assert (gr[0]["esp_utc"], gr[0]["time_source"]) == ("2026-10-02T12:34:57.000000Z", "gps")
    assert (gr[0]["lat"], gr[0]["lon"], gr[0]["alt_m"], gr[0]["speed_mps"]) == (
        "52.5200000",
        "13.4050000",
        "34.75",
        "1.00",
    )
    assert (gr[0]["course_deg"], gr[0]["hdop"], gr[0]["fix_utc"]) == (
        "90.00",
        "0.95",
        "2026-10-02T12:34:56.789000Z",
    )
    assert gr[1]["lat"] == "" and gr[1]["fix_quality"] == "0" and gr[1]["error"] == ""
    assert "expected 32" in gr[2]["error"] and gr[2]["lat"] == ""
    ir = list(csv.DictReader(io.StringIO(i.getvalue())))
    assert list(ir[0].keys()) == lr.IMU_CSV_COLUMNS
    assert (ir[0]["acc_z_mg"], ir[0]["gyr_y_dps"], ir[0]["pitch_deg"], ir[0]["roll_deg"]) == (
        "3",
        "-0.5",
        "-12.34",
        "45.67",
    )
    assert (ir[1]["pitch_deg"], ir[1]["status"], ir[1]["n_samples"]) == ("", "0", "10")
    assert "expected 18" in ir[2]["error"]
    # empty selections still give a header
    e = io.StringIO()
    assert lr.write_gps_csv([frame_at(0, 1)], e) == 0 and e.getvalue().startswith("seq,")


def test_cli_export_with_gps_and_imu_files(tmp_path, capsys):
    f = tmp_path / "m.ldrec"
    f.write_bytes(
        build_file(
            [sync_rec(0, 100, U0), gps_rec(1, 200), imu_rec(2, 300), frame_at(3, 400)]
        )
    )
    main_csv, g_csv, i_csv = (tmp_path / n for n in ("m.csv", "g.csv", "i.csv"))
    argv = ["export-csv", str(f), "-o", str(main_csv), "--gps", str(g_csv), "--imu", str(i_csv)]
    assert lr.main(argv) == 0
    assert len(list(csv.DictReader(main_csv.open()))) == 1
    assert len(list(csv.DictReader(g_csv.open()))) == 1
    assert len(list(csv.DictReader(i_csv.open()))) == 1
    assert lr.main(["export-csv", str(f), "--gps", str(g_csv)]) == 0  # CSV to stdout, gps to file
    assert capsys.readouterr().out.startswith("seq,")


# ---- info -------------------------------------------------------------------


def test_info_gps_imu_time_stats(tmp_path, capsys):
    recs = [
        sync_rec(0, 1, U0, 1),
        sync_rec(1, 2, U0, 2),
        sync_rec(2, 3, U0, 2),
        lr.TimeSyncRec(3, 4, 1, b"bad"),  # damaged: counted as a record, not as a source
        gps_rec(4, 10),
        gps_rec(5, 20, fix_quality=0),  # position flag but quality 0: no fix
        gps_rec(6, 30, flags=0x03),  # time/date only, no position: no fix
        gps_rec(7, 40),
        imu_rec(8, 50),
        lr.UnknownRec(9, 60, 1, 9, b""),
        frame_at(10, 70),
        frame_at(11, 80),
    ]
    s = lr.summarize(0, recs)
    assert (s["frames"], s["gps_records"], s["gps_fixes"], s["imu_records"]) == (2, 4, 2, 1)
    assert s["fix_ratio"] == 0.5 and s["time_syncs"] == 4
    assert s["time_sources"] == {"gps": 1, "sntp": 2} and s["unknown_records"] == 1
    f = tmp_path / "i.ldrec"
    f.write_bytes(build_file(recs))
    assert lr.main(["info", str(f)]) == 0
    out = capsys.readouterr().out
    assert "gps records:   4" in out and "gps fix ratio: 0.500 (2 with fix)" in out
    assert "imu records:   1" in out and "time syncs:    4 (gps 1, sntp 2)" in out
    assert "unknown type:  1 records kept raw" in out
    assert "frames:        2" in out
    # a file without GPS: ratio n/a, no crash
    f.write_bytes(build_file([frame_at(0, 1)]))
    assert lr.main(["info", str(f)]) == 0
    out = capsys.readouterr().out
    assert "gps fix ratio: n/a" in out and "time syncs:    0 (none)" in out


# ---- recorder with mixed record types (one shared seq) ------------------------

GPS_PL = vec("gps_fix_nominal")[0]
IMU_PL = vec("imu_nominal")[0]
SYNC_PL = vec("time_sync_gps")[0]
MIX = {1: (1, GPS_PL), 2: (2, IMU_PL), 3: (3, SYNC_PL)}


def mix_summary(recs):
    names = {lr.FrameRec: "f", lr.GpsRec: "gps", lr.ImuRec: "imu", lr.TimeSyncRec: "sync"}
    out = []
    for r in recs:
        if isinstance(r, lr.GapRec):
            out.append(("gap", r.from_seq, r.to_seq))
        elif isinstance(r, lr.RebootRec):
            out.append(("reboot", r.old_boot_id, r.new_boot_id))
        elif isinstance(r, lr.UnknownRec):
            out.append(("unk", r.seq, r.wire_type))
        else:
            out.append((names[type(r)], r.seq))
    return out


def test_mixed_records_resume_counts_every_type(tmp_path):
    script = [
        [batch(0, [0, 1, 2, 3], other=MIX)],  # frame, gps, imu, sync; then the link drops
        [batch(4, [4, 5], other={5: MIX[1]})],
    ]
    srv, recs = run_recorder(tmp_path, script, 2)  # max_frames counts frames only
    assert srv.requests == [0, 4]  # resume = last seq + 1 over ALL record types
    assert mix_summary(recs) == [
        ("f", 0), ("gps", 1), ("imu", 2), ("sync", 3), ("f", 4), ("gps", 5),
    ]  # fmt: skip
    assert recs[1].payload == GPS_PL and recs[2].payload == IMU_PL and recs[3].payload == SYNC_PL
    assert recs[1].esp_time_us == 1001 and recs[3].pc_time_ns == recs[0].pc_time_ns
    lr.decode_gps_fix(recs[1].payload)
    lr.decode_imu(recs[2].payload)
    lr.decode_time_sync(recs[3].payload)


def test_mixed_records_gap_and_duplicates(tmp_path):
    script = [
        [batch(0, [0, 1, 2], other=MIX)],
        # replay of 1,2 is dropped as duplicate; 3 (a sync) is missing, 4 and 5 are frames:
        [batch(1, [1, 2, 4, 5], other=MIX)],
        [batch(9, [9], flags=1, other={9: MIX[2]}), batch(10, [10])],
    ]
    srv, recs = run_recorder(tmp_path, script, 4)
    assert srv.requests == [0, 3, 6]
    assert mix_summary(recs) == [
        ("f", 0), ("gps", 1), ("imu", 2), ("gap", 3, 4), ("f", 4), ("f", 5),
        ("gap", 6, 9), ("imu", 9), ("f", 10),
    ]  # fmt: skip


def test_mixed_records_seq_wrap(tmp_path):
    w = {0xFFFFFFFE: MIX[1], 0: MIX[2], 1: MIX[3]}
    script = [
        [batch(0xFFFFFFFE, [0xFFFFFFFE, 0xFFFFFFFF, 0, 1], other=w)],
        [batch(2, [2, 3])],
    ]
    srv, recs = run_recorder(tmp_path, script, 3)
    assert srv.requests == [0, 2]  # resumes right after the wrapped sync record
    assert mix_summary(recs) == [
        ("gps", 0xFFFFFFFE), ("f", 0xFFFFFFFF), ("imu", 0), ("sync", 1), ("f", 2), ("f", 3),
    ]  # fmt: skip


def test_mixed_records_reboot(tmp_path):
    script = [
        [batch(0, [0, 1, 2], boot=BOOT_A, other=MIX)],
        [batch(3, [3, 4], boot=BOOT_B)],  # other boot: discarded, ask again from 0
        [batch(0, [0, 1, 2, 3, 4], boot=BOOT_B, other=MIX)],
    ]
    srv, recs = run_recorder(tmp_path, script, 3)
    assert srv.requests == [0, 3, 0]
    assert mix_summary(recs) == [
        ("f", 0), ("gps", 1), ("imu", 2), ("reboot", BOOT_A, BOOT_B),
        ("f", 0), ("gps", 1), ("imu", 2), ("sync", 3), ("f", 4),
    ]  # fmt: skip
    # the sync after the reboot belongs to the second boot only
    rows = to_rows(recs)
    assert [r["record"] for r in rows] == ["frame", "reboot", "frame", "frame"]
    assert rows[0]["frame_utc"] == ""  # boot 0 never had a time sync
    assert rows[2]["frame_utc"] != "" and rows[2]["gps_lat"] == ""  # sync follows the frame
    assert rows[3]["frame_utc"] != "" and rows[3]["gps_lat"] == "52.5200000"


def test_unknown_wire_type_in_stream_is_recorded(tmp_path):
    script = [[batch(0, [0, 1, 2], other={1: (9, b"new"), 2: (200, b"")})], [batch(3, [3])]]
    srv, recs = run_recorder(tmp_path, script, 2)
    assert srv.requests == [0, 3]
    assert mix_summary(recs) == [("f", 0), ("unk", 1, 9), ("unk", 2, 200), ("f", 3)]
    assert recs[1].payload == b"new" and recs[2].payload == b""
