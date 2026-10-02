"""Tests for host/ld2410_rec.py (shared vectors in esp32/tests/vectors)."""

from __future__ import annotations

import contextlib
import csv
import io
import json
import socket
import struct
import threading
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


@pytest.mark.parametrize("name", ["frame_normal", "frame_engineering"])
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
    assert names == {"frame_normal", "frame_engineering", "ack_ok", "ack_fail", "batch_gap_wrap"}


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
    assert lr.encode_request(0x01020304) == b"LDRQ\x01\x00\x00\x00\x04\x03\x02\x01"
    assert lr.encode_request(0) == b"LDRQ" + b"\x01\x00\x00\x00" + b"\x00" * 4
    assert len(lr.encode_request(0xFFFFFFFF)) == lr.REQ_LEN


def test_batch_gap_wrap_vector():
    raw, exp = vec("batch_gap_wrap")
    hdr, recs = lr.parse_batch(raw)
    assert (hdr.version, hdr.flags, hdr.first_seq, hdr.count) == (
        exp["version"],
        exp["flags"],
        exp["first_seq"],
        exp["count"],
    )
    assert hdr.gap
    assert [(r.seq, r.esp_time_us, r.raw.hex()) for r in recs] == [
        (r["seq"], r["esp_time_us"], r["raw_hex"]) for r in exp["records"]
    ]
    assert recs[1].esp_time_us > 2**32
    for r in recs:
        lr.decode_data_frame(r.raw)


def test_batch_errors():
    raw, _ = vec("batch_gap_wrap")
    with pytest.raises(lr.ProtocolError, match="short"):
        lr.parse_batch_header(raw[:15])
    with pytest.raises(lr.ProtocolError, match="magic"):
        lr.parse_batch_header(b"XXXX" + raw[4:])
    with pytest.raises(lr.ProtocolError, match="version"):
        lr.parse_batch_header(raw[:4] + b"\x02" + raw[5:])
    with pytest.raises(lr.ProtocolError, match="reserved"):
        lr.parse_batch_header(raw[:6] + b"\x01\x00" + raw[8:])
    with pytest.raises(lr.ProtocolError, match="exceeds"):
        lr.parse_batch(raw[:-1])
    with pytest.raises(lr.ProtocolError, match="truncated"):
        lr.parse_batch(raw[: lr.BATCH_HDR_LEN + 5])
    with pytest.raises(lr.ProtocolError, match="trailing"):
        lr.parse_batch(raw + b"\x00")
    bad_len = bytearray(raw)
    bad_len[16 + 12 : 16 + 14] = b"\xff\xff"  # record length beyond data
    with pytest.raises(lr.ProtocolError, match="exceeds"):
        lr.parse_batch(bytes(bad_len))
    assert lr.parse_batch(raw[:4] + b"\x01\x00\x00\x00" + raw[8:12] + b"\x00\x00\x00\x00")[1] == []


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
        else:
            out += lr.encode_frame_rec(r.seq, r.esp_time_us, r.pc_time_ns, r.raw)
    return bytes(out)


def test_file_round_trip_and_layout():
    recs = [
        lr.FrameRec(0xFFFFFFFE, 10, 111, NORMAL_RAW),
        lr.GapRec(0xFFFFFFFF, 3, 222),
        lr.FrameRec(3, 2**33, 333, ENG_RAW),
    ]
    data = build_file(recs)
    assert data[:8] == b"LDREC1\x00\x00"
    assert data[8:12] == b"\x01\x00\x00\x00"
    assert struct.unpack_from("<Q", data, 12)[0] == 1234
    assert data[20] == 0  # first record is a frame
    created, back = lr.parse_file(data)
    assert created == 1234
    assert back == recs
    assert back[1].missing == 4


def test_file_errors_and_truncation():
    good = build_file([lr.FrameRec(1, 2, 3, NORMAL_RAW), lr.FrameRec(2, 3, 4, NORMAL_RAW)])
    with pytest.raises(lr.ProtocolError, match="short"):
        lr.parse_file(good[:10])
    with pytest.raises(lr.ProtocolError, match="magic"):
        lr.parse_file(b"XXXXXXXX" + good[8:])
    with pytest.raises(lr.ProtocolError, match="version"):
        lr.parse_file(good[:8] + b"\x02" + good[9:])
    with pytest.raises(lr.ProtocolError, match="unknown record type"):
        lr.parse_file(good + b"\x07")
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
    n, e, g, bad = rows
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


def test_info_summary():
    recs = [
        lr.FrameRec(1, 1_000_000, 10**9, NORMAL_RAW),
        lr.GapRec(2, 5, 2 * 10**9),
        lr.FrameRec(5, 3_000_000, 3 * 10**9, NORMAL_RAW),
    ]
    s = lr.summarize(0, recs)
    assert (s["frames"], s["first_seq"], s["last_seq"]) == (2, 1, 5)
    assert s["gaps"] == [(2, 5, 3)] and s["missing_frames"] == 3
    assert s["duration_s"] == 2.0 and s["esp_duration_s"] == 2.0
    assert lr.summarize(0, [])["first_seq"] is None


def test_cli_info_and_csv(tmp_path, capsys):
    f = tmp_path / "a.ldrec"
    f.write_bytes(build_file([lr.FrameRec(1, 2, 3, NORMAL_RAW)]))
    assert lr.main(["info", str(f)]) == 0
    assert "frames:        1" in capsys.readouterr().out
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


def batch(first_seq: int, seqs: list[int], flags: int = 0) -> bytes:
    out = struct.pack("<4sBBHIHH", b"LDRB", 1, flags, 0, first_seq, len(seqs), 0)
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
                    assert req[:4] == b"LDRQ"
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
