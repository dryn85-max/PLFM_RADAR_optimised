# Shared test vectors

Used by the C host tests (`esp32/tests/test_*.c`) and, later, by
`host/test_ld2410_rec.py`.

**All vectors are synthetic, VERIFY against a real capture.** There is no
real LD2410C capture yet; the bytes are built from the documented layout
(spec R3, plan "Fixed byte layouts") by `gen_vectors.py`, independently of
the C code. Regenerate with `python3 gen_vectors.py` (output is deterministic).

Each `NAME.bin` is the exact byte stream; `NAME.json` is the expected decode.
JSON keys mirror the C struct field names (`ld_data_t`, `ld_ack_t`,
`rec_batch_hdr_t`). `raw_hex` is the same bytes as the `.bin`, lower-case hex.

| Vector | Content | JSON |
|---|---|---|
| `frame_normal` | data frame, type 0x02, 13-byte payload | `frame_kind:"data"`, `data{data_type, engineering, target_state, moving_dist_cm, moving_energy, still_dist_cm, still_energy, detect_dist_cm, max_moving_gate, max_still_gate, moving_gate_energy[9], still_gate_energy[9]}`; gate fields are 0 for a normal frame |
| `frame_engineering` | data frame, type 0x01, 35-byte payload incl. 2 module-specific bytes | same keys |
| `ack_ok` | ACK of enable-config (0x01FF), status 0 | `frame_kind:"cmd"`, `ack{cmd, status, ok}`; `cmd` has the 0x0100 flag removed |
| `ack_fail` | ACK of enable-engineering (0x0162), status 1 | same keys |
| `batch_gap_wrap` | recording batch (version 2, boot_id 0xA1B2C3D4), GAP flag, first_seq 0xFFFFFFFF, 2 records (seq 0xFFFFFFFF then 0, second esp_time_us above 2^32) wrapping the sequence | `version, flags, first_seq, count, boot_id, records[{seq, esp_time_us, raw_hex}]` |
| `batch_reboot` | keep-alive batch of a rebooted device: first_seq 0, count 0, boot_id 0xFFFFFFFF (differs from `batch_gap_wrap`) | same keys, `records` empty |

Record layout in a batch: `seq u32, esp_time_us u64, len u16, raw[len]`, little-endian;
batch header 20 bytes `"LDRB", version u8 = 2, flags u8, reserved u16, first_seq u32, count u16, reserved u16, boot_id u32` (boot_id is random per ESP32 boot, never 0).
