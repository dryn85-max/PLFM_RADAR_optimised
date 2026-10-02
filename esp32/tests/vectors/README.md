# Shared test vectors

Used by the C host tests (`esp32/tests/test_*.c`) and by `host/test_ld2410_rec.py`
(which also checks that every vector here is covered).

**Most vectors are synthetic**: the bytes are built from the documented layout
(spec R3, plan "Fixed byte layouts") by `gen_vectors.py`, independently of
the C code. Regenerate with `python3 gen_vectors.py` (output is deterministic).
The exceptions are `frame_engineering_real_0` and `_1`: **real captures**,
hand-written, not produced by `gen_vectors.py` (see below); the generator only reads
them to wrap them into `batch_v3_mixed`.

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
| `batch_gap_wrap` | recording batch (version 3, boot_id 0xA1B2C3D4), GAP flag, first_seq 0xFFFFFFFF, 2 type-0 records (seq 0xFFFFFFFF then 0, second esp_time_us above 2^32) wrapping the sequence | `version, flags, first_seq, count, boot_id, records[{seq, esp_time_us, type, raw_hex}]` |
| `batch_reboot` | keep-alive batch of a rebooted device: first_seq 0, count 0, boot_id 0xFFFFFFFF (differs from `batch_gap_wrap`) | same keys, `records` empty |
| `batch_v3_mixed` | batch (boot_id 0x0BADCAFE, first_seq 0xFFFFFFFD, no flags) with 5 records, one shared seq wrapping 2^32: type 0 = `frame_engineering_real_0`, type 1 = `gps_fix_nominal`, type 2 = `imu_nominal`, type 3 = `time_sync_gps`, type 0 = `frame_engineering_real_1` (esp_time_us of the last above 2^32) | same keys |
| `gps_fix_nominal` | gps_fix payload (32 B), all four valid flags (0x0F), 52.52 N 13.405 E, 34.75 m, UTC 2026-10-02T12:34:56.789Z | `record_type:1, payload_hex, gps_fix{utc_unix_ms, lat_e7, lon_e7, alt_cm, speed_cmps, course_cdeg, hdop_x100, sats, fix_quality, flags}` |
| `gps_fix_southwest` | southern and western hemisphere (negative lat/lon), negative altitude, altitude flag clear (0x07) | same keys |
| `gps_fix_nofix` | no fix: flags 0, utc 0, position 0, HDOP 99.99, 0 satellites | same keys |
| `gps_fix_extremes` | lat -90 deg, lon -180 deg, alt INT32_MIN, utc INT64_MAX, speed/HDOP 65535, sats/quality/flags 255 | same keys |
| `imu_nominal` | imu payload (18 B), valid | `record_type:2, payload_hex, imu{acc_mg[3], gyr_ddps[3], pitch_cdeg, roll_cdeg, n_samples, status}` |
| `imu_extremes` | INT16 minima/maxima, pitch -90 deg, roll 180 deg, status 0 (invalid), 255 samples | same keys |
| `time_sync_gps` | time_sync payload (9 B), source 1, 2026-10-02T12:34:56Z | `record_type:3, payload_hex, time_sync{utc_unix_us, source}` |
| `time_sync_sntp` | source 2, microsecond part 123456 | same keys |
| `time_sync_edge` | utc INT64_MIN, source 255 (undefined values pass through) | same keys |

Record layout in a batch (protocol v3): `seq u32, esp_time_us u64, type u8, len u16, payload[len]`
(15 B header), little-endian; batch header 20 bytes `"LDRB", version u8 = 3, flags u8, reserved u16, first_seq u32, count u16, reserved u16, boot_id u32` (boot_id is random per ESP32 boot, never 0). Record types: 0 LD2410C frame (the `raw_hex` is the whole raw frame, as in the frame vectors), 1 gps_fix, 2 imu, 3 time_sync. `raw_hex` of a record is its payload.

The frame vectors (`frame_*`, `ack_*`) are payload vectors of type 0 records; the other
payload vectors (`gps_fix_*`, `imu_*`, `time_sync_*`) are the bytes of types 1, 2, 3 and
are asserted equal to the payloads inside `batch_v3_mixed`. Saturation (`speed_cmps`) is
an encoder rule and is tested in the unit tests, not by a vector.

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
