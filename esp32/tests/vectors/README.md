# Shared test vectors

Used by the C host tests (`esp32/tests/test_*.c`) and, later, by
`host/test_ld2410_rec.py`.

**Most vectors are synthetic**: the bytes are built from the documented layout
(spec R3, plan "Fixed byte layouts") by `gen_vectors.py`, independently of
the C code. Regenerate with `python3 gen_vectors.py` (output is deterministic).
The exceptions are `frame_engineering_real_0` and `_1`: **real captures**,
hand-written, not produced by `gen_vectors.py` (see below).

Each `NAME.bin` is the exact byte stream; `NAME.json` is the expected decode.
JSON keys mirror the C struct field names (`ld_data_t`, `ld_ack_t`,
`rec_batch_hdr_t`). `raw_hex` is the same bytes as the `.bin`, lower-case hex.

| Vector | Content | JSON |
|---|---|---|
| `frame_normal` | data frame, type 0x02, 13-byte payload | `frame_kind:"data"`, `data{data_type, engineering, target_state, moving_dist_cm, moving_energy, still_dist_cm, still_energy, detect_dist_cm, max_moving_gate, max_still_gate, moving_gate_energy[9], still_gate_energy[9]}`; gate fields are 0 for a normal frame |
| `frame_engineering` | data frame, type 0x01, 35-byte payload incl. 2 module-specific bytes | same keys |
| `frame_engineering_real_0` | **real capture**, engineering frame, type 0x01, 35-byte payload incl. 2 module-specific bytes (`a5 01`), seq 0 (first frame of the recording) | same keys |
| `frame_engineering_real_1` | **real capture**, engineering frame, same layout, module-specific bytes `a4 01`, seq 1 | same keys |
| `ack_ok` | ACK of enable-config (0x01FF), status 0 | `frame_kind:"cmd"`, `ack{cmd, status, ok}`; `cmd` has the 0x0100 flag removed |
| `ack_fail` | ACK of enable-engineering (0x0162), status 1 | same keys |
| `batch_gap_wrap` | recording batch (version 2, boot_id 0xA1B2C3D4), GAP flag, first_seq 0xFFFFFFFF, 2 records (seq 0xFFFFFFFF then 0, second esp_time_us above 2^32) wrapping the sequence | `version, flags, first_seq, count, boot_id, records[{seq, esp_time_us, raw_hex}]` |
| `batch_reboot` | keep-alive batch of a rebooted device: first_seq 0, count 0, boot_id 0xFFFFFFFF (differs from `batch_gap_wrap`) | same keys, `records` empty |

Record layout in a batch: `seq u32, esp_time_us u64, len u16, raw[len]`, little-endian;
batch header 20 bytes `"LDRB", version u8 = 2, flags u8, reserved u16, first_seq u32, count u16, reserved u16, boot_id u32` (boot_id is random per ESP32 boot, never 0).

## Real captures

`frame_engineering_real_0` / `_1` are the first two frames of the owner's bench
recording of 2026-10-02: sensor HLK-LD2410C on an ESP32-S3 bench setup,
engineering mode, UART 256000 Bd. The bytes are unmodified; the expected decode
in the JSON was derived by hand, byte by byte. The two module-specific bytes
before the `55 00` tail (`a5 01` / `a4 01`) are skipped by the decoder; they are
probably the light sensor value and the OUT pin state (unverified
interpretation). The engineering frame layout, including these 2 module-specific
bytes, is **confirmed on hardware** by these captures. The Hi-Link manual is
still not in `hardware/datasheets/`.
