#!/usr/bin/env python3
"""Regenerate the synthetic shared test vectors (*.bin / *.json) in this folder.

Recording protocol v3 (typed records) and the gps_fix / imu / time_sync payloads are
built here from the plan "Fixed layouts" (docs/superpowers/plans/2026-10-02-esp32-gps-imu.md).
The two real LD2410C captures are hand-written files, only read here (batch_v3_mixed).

Bytes are built from the documented layout only (spec R3, plan "Fixed byte
layouts"), independently of the C code.  SYNTHETIC: VERIFY against a real
LD2410C capture.  Run: python3 gen_vectors.py
"""

import json
import struct
from pathlib import Path

HERE = Path(__file__).resolve().parent

DATA_HDR = bytes([0xF4, 0xF3, 0xF2, 0xF1])
DATA_FTR = bytes([0xF8, 0xF7, 0xF6, 0xF5])
CMD_HDR = bytes([0xFD, 0xFC, 0xFB, 0xFA])
CMD_FTR = bytes([0x04, 0x03, 0x02, 0x01])


def frame(hdr, payload, ftr):
    return hdr + struct.pack("<H", len(payload)) + payload + ftr


def normal_payload():
    return (
        bytes([0x02, 0xAA, 0x03])
        + struct.pack("<H", 80)
        + bytes([64])
        + struct.pack("<H", 120)
        + bytes([50])
        + struct.pack("<H", 85)
        + bytes([0x55, 0x00])
    )


MOVING = [100, 80, 60, 40, 20, 10, 5, 2, 0]
STILL = [0, 10, 20, 30, 40, 50, 60, 70, 80]


def eng_payload():
    return (
        bytes([0x01, 0xAA, 0x03])
        + struct.pack("<H", 80)
        + bytes([64])
        + struct.pack("<H", 120)
        + bytes([50])
        + struct.pack("<H", 85)
        + bytes([8, 8])
        + bytes(MOVING)
        + bytes(STILL)
        + bytes([0x11, 0x22])  # module-specific bytes, skipped by the decoder
        + bytes([0x55, 0x00])
    )


def write(name, raw, meta):
    (HERE / (name + ".bin")).write_bytes(raw)
    (HERE / (name + ".json")).write_text(json.dumps(meta, indent=2) + "\n")


def data_meta(engineering, raw):
    d = {
        "data_type": 1 if engineering else 2,
        "engineering": 1 if engineering else 0,
        "target_state": 3,
        "moving_dist_cm": 80,
        "moving_energy": 64,
        "still_dist_cm": 120,
        "still_energy": 50,
        "detect_dist_cm": 85,
        "max_moving_gate": 8 if engineering else 0,
        "max_still_gate": 8 if engineering else 0,
        "moving_gate_energy": MOVING if engineering else [0] * 9,
        "still_gate_energy": STILL if engineering else [0] * 9,
    }
    return {"frame_kind": "data", "raw_hex": raw.hex(), "data": d}


def main():
    nraw = frame(DATA_HDR, normal_payload(), DATA_FTR)
    write("frame_normal", nraw, data_meta(False, nraw))
    eraw = frame(DATA_HDR, eng_payload(), DATA_FTR)
    write("frame_engineering", eraw, data_meta(True, eraw))

    ok = frame(CMD_HDR, struct.pack("<HH", 0x01FF, 0), CMD_FTR)
    write(
        "ack_ok",
        ok,
        {"frame_kind": "cmd", "raw_hex": ok.hex(), "ack": {"cmd": 0x00FF, "status": 0, "ok": 1}},
    )
    bad = frame(CMD_HDR, struct.pack("<HH", 0x0162, 1), CMD_FTR)
    write(
        "ack_fail",
        bad,
        {"frame_kind": "cmd", "raw_hex": bad.hex(), "ack": {"cmd": 0x0062, "status": 1, "ok": 0}},
    )

    # --- recording protocol v3 -------------------------------------------
    recs = [(0xFFFFFFFF, 1234567, 0, nraw), (0, 0x100000005, 0, eraw)]
    boot = 0xA1B2C3D4
    write("batch_gap_wrap", batch(1, 0xFFFFFFFF, boot, recs), batch_meta(1, 0xFFFFFFFF, boot, recs))

    # Keep-alive from a device that rebooted: seq restarted at 0, boot_id differs
    # from the one in batch_gap_wrap (and is the largest possible value).
    write("batch_reboot", batch(0, 0, 0xFFFFFFFF, []), batch_meta(0, 0, 0xFFFFFFFF, []))

    # --- record payloads (one vector per type and edge) ---------------------
    # utc 2026-10-02T12:34:56.789Z = 1790944496789 ms, Berlin-ish position
    gps = {
        "gps_fix_nominal": {
            "utc_unix_ms": 1790944496789,
            "lat_e7": 525200000,
            "lon_e7": 134050000,
            "alt_cm": 3475,
            "speed_cmps": 1234,
            "course_cdeg": 27150,
            "hdop_x100": 95,
            "sats": 9,
            "fix_quality": 1,
            "flags": 0x0F,
        },
        # southern + western hemisphere, below sea level, no altitude flag
        "gps_fix_southwest": {
            "utc_unix_ms": 1790944497000,
            "lat_e7": -338688197,
            "lon_e7": -1224194155,
            "alt_cm": -1250,
            "speed_cmps": 0,
            "course_cdeg": 0,
            "hdop_x100": 210,
            "sats": 4,
            "fix_quality": 2,
            "flags": 0x07,
        },
        # UBX-verified UTC: all four NMEA flags plus bit 4 (GPS_FLAG_UTC_VERIFIED)
        "gps_fix_utc_verified": {
            "utc_unix_ms": 1790944498250,
            "lat_e7": 525200000,
            "lon_e7": 134050000,
            "alt_cm": 3475,
            "speed_cmps": 0,
            "course_cdeg": 0,
            "hdop_x100": 80,
            "sats": 10,
            "fix_quality": 1,
            "flags": 0x1F,
        },
        # no fix: everything invalid, utc 0
        "gps_fix_nofix": {
            "utc_unix_ms": 0,
            "lat_e7": 0,
            "lon_e7": 0,
            "alt_cm": 0,
            "speed_cmps": 0,
            "course_cdeg": 0,
            "hdop_x100": 9999,
            "sats": 0,
            "fix_quality": 0,
            "flags": 0x00,
        },
        # coordinate and integer extremes, maxed counters
        "gps_fix_extremes": {
            "utc_unix_ms": 2**63 - 1,
            "lat_e7": -900000000,
            "lon_e7": -1800000000,
            "alt_cm": -(2**31),
            "speed_cmps": 65535,
            "course_cdeg": 35999,
            "hdop_x100": 65535,
            "sats": 255,
            "fix_quality": 255,
            "flags": 0xFF,
        },
    }
    for name, g in gps.items():
        pl = gps_payload(g)
        write(name, pl, {"record_type": 1, "payload_hex": pl.hex(), "gps_fix": g})
    imus = {
        "imu_nominal": {
            "acc_mg": [12, -34, 1001],
            "gyr_ddps": [5, -6, 7],
            "pitch_cdeg": -1234,
            "roll_cdeg": 4567,
            "n_samples": 10,
            "status": 1,
        },
        "imu_extremes": {
            "acc_mg": [-32768, 32767, -1],
            "gyr_ddps": [32767, -32768, 0],
            "pitch_cdeg": -9000,
            "roll_cdeg": 18000,
            "n_samples": 255,
            "status": 0,
        },
    }
    for name, m in imus.items():
        pl = imu_payload(m)
        write(name, pl, {"record_type": 2, "payload_hex": pl.hex(), "imu": m})
    syncs = {
        "time_sync_gps": {"utc_unix_us": 1790944496000000, "source": 1},
        "time_sync_sntp": {"utc_unix_us": 1790944500123456, "source": 2},
        "time_sync_edge": {"utc_unix_us": -(2**63), "source": 255},
    }
    for name, t in syncs.items():
        pl = struct.pack("<qB", t["utc_unix_us"], t["source"])
        write(name, pl, {"record_type": 3, "payload_hex": pl.hex(), "time_sync": t})

    # Mixed batch: seq wraps; frame (real capture 0), gps_fix, imu, time_sync, frame
    # (real capture 1). The two real frames are hand-written files, read here.
    real0 = (HERE / "frame_engineering_real_0.bin").read_bytes()
    real1 = (HERE / "frame_engineering_real_1.bin").read_bytes()
    mixed = [
        (0xFFFFFFFD, 5000000, 0, real0),
        (0xFFFFFFFE, 5000100, 1, gps_payload(gps["gps_fix_nominal"])),
        (0xFFFFFFFF, 5100000, 2, imu_payload(imus["imu_nominal"])),
        (0, 5100050, 3, struct.pack("<qB", 1790944496000000, 1)),
        (1, 0x1_0000_0000 + 7, 0, real1),
    ]
    boot = 0x0BADCAFE
    write(
        "batch_v3_mixed", batch(0, 0xFFFFFFFD, boot, mixed), batch_meta(0, 0xFFFFFFFD, boot, mixed)
    )


def gps_payload(g):
    return struct.pack(
        "<qiiiHHHBBB3x",
        g["utc_unix_ms"],
        g["lat_e7"],
        g["lon_e7"],
        g["alt_cm"],
        g["speed_cmps"],
        g["course_cdeg"],
        g["hdop_x100"],
        g["sats"],
        g["fix_quality"],
        g["flags"],
    )


def imu_payload(m):
    return struct.pack(
        "<6hhhBB",
        *m["acc_mg"],
        *m["gyr_ddps"],
        m["pitch_cdeg"],
        m["roll_cdeg"],
        m["n_samples"],
        m["status"],
    )


def batch(flags, first_seq, boot, recs):
    out = b"LDRB" + struct.pack("<BBHIHHI", 3, flags, 0, first_seq, len(recs), 0, boot)
    for seq, t, ty, payload in recs:
        out += struct.pack("<IQBH", seq, t, ty, len(payload)) + payload
    return out


def batch_meta(flags, first_seq, boot, recs):
    return {
        "version": 3,
        "flags": flags,
        "first_seq": first_seq,
        "count": len(recs),
        "boot_id": boot,
        "records": [
            {"seq": s, "esp_time_us": t, "type": ty, "raw_hex": p.hex()} for s, t, ty, p in recs
        ],
    }


if __name__ == "__main__":
    main()
