#!/usr/bin/env python3
"""Regenerate the synthetic shared test vectors (*.bin / *.json) in this folder.

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
        + struct.pack("<H", 80) + bytes([64])
        + struct.pack("<H", 120) + bytes([50])
        + struct.pack("<H", 85)
        + bytes([0x55, 0x00])
    )


MOVING = [100, 80, 60, 40, 20, 10, 5, 2, 0]
STILL = [0, 10, 20, 30, 40, 50, 60, 70, 80]


def eng_payload():
    return (
        bytes([0x01, 0xAA, 0x03])
        + struct.pack("<H", 80) + bytes([64])
        + struct.pack("<H", 120) + bytes([50])
        + struct.pack("<H", 85)
        + bytes([8, 8])
        + bytes(MOVING) + bytes(STILL)
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
    write("ack_ok", ok, {"frame_kind": "cmd", "raw_hex": ok.hex(),
                         "ack": {"cmd": 0x00FF, "status": 0, "ok": 1}})
    bad = frame(CMD_HDR, struct.pack("<HH", 0x0162, 1), CMD_FTR)
    write("ack_fail", bad, {"frame_kind": "cmd", "raw_hex": bad.hex(),
                            "ack": {"cmd": 0x0062, "status": 1, "ok": 0}})

    recs = [(0xFFFFFFFF, 1234567, nraw), (0, 0x100000005, eraw)]
    boot = 0xA1B2C3D4
    out = b"LDRB" + struct.pack("<BBHIHHI", 2, 1, 0, 0xFFFFFFFF, len(recs), 0, boot)
    for seq, t, raw in recs:
        out += struct.pack("<IQH", seq, t, len(raw)) + raw
    write("batch_gap_wrap", out, {
        "version": 2, "flags": 1, "first_seq": 0xFFFFFFFF, "count": 2,
        "boot_id": boot,
        "records": [{"seq": s, "esp_time_us": t, "raw_hex": r.hex()}
                    for s, t, r in recs]})

    # Keep-alive from a device that rebooted: seq restarted at 0, boot_id differs
    # from the one in batch_gap_wrap (and is the largest possible value).
    out = b"LDRB" + struct.pack("<BBHIHHI", 2, 0, 0, 0, 0, 0, 0xFFFFFFFF)
    write("batch_reboot", out, {
        "version": 2, "flags": 0, "first_seq": 0, "count": 0,
        "boot_id": 0xFFFFFFFF, "records": []})

if __name__ == "__main__":
    main()
