# AERIS-10 Lite MVP: ESP32-S3 + HLK-LD2410C

A bench MVP built from parts on hand, independent of the RF chain, the FPGA
board and the STM32 firmware (none of them is part of it). An ESP32-S3 reads
the HLK-LD2410C, a ready 24 GHz FMCW presence radar, over UART and offers:

- a **live web page** served by the ESP32 (presence, distances, energies,
  9 + 9 gate energies in engineering mode);
- a **recording server** (TCP 5410) from which a PC tool stores the raw
  LD2410C frames, with sequence numbers and timestamps, and survives Wi-Fi
  drops.

Specification and plan: [spec](../docs/superpowers/specs/2026-10-02-esp32-ld2410-mvp.md),
[plan](../docs/superpowers/plans/2026-10-02-esp32-ld2410-mvp.md). System context:
[docs/architecture.md](../docs/architecture.md).

> **Status: host tests and a CI build only. Nothing here has been run on the
> hardware yet.** Everything not verifiable from the repository is marked
> **VERIFY**; the list is at the end.

## Hardware

| Part | Notes |
|---|---|
| ESP32-S3-DevKitC-1 v1.1, module N32R8V | 32 MB octal flash, 8 MB octal PSRAM (VERIFY on the real board) |
| HLK-LD2410C | 24 GHz FMCW presence radar, UART 256000 8N1 |
| USB cable | to the native **USB** connector (USB-Serial-JTAG) |

### Wiring

| LD2410C pin | ESP32-S3 DevKit | Note |
|---|---|---|
| VCC | 5V pin | see the power warning |
| GND | G | common ground |
| TX | GPIO18 | UART1 RX of the ESP32 |
| RX | GPIO17 | UART1 TX of the ESP32 |
| OUT | not connected | not used by the MVP |

**POWER WARNING.** Never connect two 5 V sources together (for example the
DevKit 5V pin and a breadboard power-supply module). Either power the LD2410C
from the DevKit 5V pin, or from the supply module's 5 V rail and connect only
GND between the module and the DevKit.

**Use the native "USB" connector** (USB-Serial-JTAG, GPIO19/20) for flashing and
the console, not the "UART" one: the UART connector of the owner's board was
re-soldered and is not relied on.

## Build, flash, monitor

ESP-IDF **v5.5.5** is pinned (the CI container is `espressif/idf:v5.5.5`).
With ESP-IDF installed and exported:

```
cd esp32
idf.py set-target esp32s3
idf.py build flash monitor
```

`sdkconfig.defaults` is committed (octal flash and PSRAM, console on
USB-Serial-JTAG, WebSocket support with the post-handshake callback
(`CONFIG_HTTPD_WS_POST_HANDSHAKE_CB_SUPPORT`: from v5.5.5 on, esp_http_server
reports a new WebSocket client only through it), `CONFIG_LWIP_MAX_SOCKETS=16`: httpd uses
up to 7 sessions + listen + control, the recording server a listen socket, a
client and one transient socket); `sdkconfig`, `build/` and
`managed_components/` are git-ignored. An existing `sdkconfig` takes
precedence over `sdkconfig.defaults`: after a change to the defaults, delete
`esp32/sdkconfig` before building (`idf.py fullclean` does not remove it). The `espressif/mdns` component is fetched
at build time, pinned exactly (`main/idf_component.yml`, `==1.14.0`).

If the board does not enter download mode by itself: hold **BOOT**, tap
**RESET**, release **BOOT**, then flash again.

## First boot

1. Open the console (`idf.py monitor`). At every boot it prints one line with
   the **AP password**, the AP SSID, the AP IP and the STA IP, for example
   `AP password: ...  AP SSID: AERIS-MVP-XXXX  AP IP: 192.168.4.1 (AP on)  STA IP: 0.0.0.0`.
2. With no stored Wi-Fi credentials the ESP32 starts only its access point
   `AERIS-MVP-XXXX` (XXXX = last two bytes of the AP MAC), WPA2, channel 1, up to
   4 clients. The password is 12 random characters (hardware RNG) generated on
   the first boot and kept in NVS; it survives a Wi-Fi credential reset.
3. Join that network and open **http://192.168.4.1**.

Credentials and the STA password are never logged and never sent to the live
page; only the AP password line is printed.

## Wi-Fi setup and reset

- `/wifi` is the setup page: SSID and password (empty password = open network,
  otherwise 8-64 characters). It saves to NVS and reboots. It is served **only
  to clients connected to the AP** (the request's local address must be the AP
  IP); requests from the STA side get 404.
- Opening `/wifi` first scans for networks (active scan, about 2 s) and lists
  them above the form: SSID (HTML-escaped), signal in dBm, channel and
  open/secured, strongest first, one entry per SSID, at most 20, hidden
  networks skipped. Tapping an entry fills the SSID field (a few lines of
  inline JavaScript, no external resources); the field can still be typed by
  hand for hidden networks. SSIDs that are not valid UTF-8 or contain control
  characters are shown with `?` and are not tappable. **The scan briefly
  disrupts the AP for the connected phone (the radio leaves channel 1 while
  scanning) and the page takes 2-3 s to load.** If the scan cannot run (for
  example a STA connect attempt is in progress) the page says "Scan
  unavailable, enter SSID manually"; "rescan" reloads the page. Without stored
  credentials the radio runs in AP+STA mode with the STA idle, because the
  driver cannot scan in AP-only mode.
- On boot with stored credentials the ESP32 connects as a station (15 s
  timeout). If it connects, only STA is active and the AP is off (so `/wifi` is
  not reachable until the credentials are erased). If it does not connect, the
  AP starts alongside (AP+STA) and the STA keeps retrying in the background
  (backoff 2 s doubling to 30 s). A dropped STA link is also retried.
- On the STA network the board is reachable as `http://aeris-mvp.local`
  (mDNS, VERIFY) or by the STA IP from the console.
- **BOOT held for at least 5 s while the firmware is running** erases the stored
  STA credentials (not the AP password) and reboots into AP mode. Do **not**
  hold BOOT at power-up or reset: GPIO0 low at reset selects the ROM download
  mode.

## Live page

`GET /` serves one self-contained page (no external resources). It opens a
WebSocket to `/ws`, which pushes the **latest snapshot** as JSON at 10 Hz. A
slow client only gets the newest snapshot (no per-client queue; up to 4
clients). The page shows:

- sensor link status (`no_data`, `ok`, `lost` after more than 1 s without a
  frame) and WebSocket status (reconnects automatically);
- frame sequence number and frames per second;
- presence state (none / moving / still / both);
- moving distance and energy, still distance and energy, detection distance;
- bar charts of the 9 moving and 9 still gate energies (0-100), only when
  engineering mode was acknowledged, otherwise "engineering mode
  unavailable". If the enable sequence gets no ACK at boot, it is retried every
  5 s (at most 5 retries) while no engineering frame has been seen; data
  reception continues meanwhile.

The console logs `ws client added fd=.. slot=..` / `ws client removed fd=..`
for each WebSocket client, a warning for each failed send, and every 10 s
`live: clients=N sent=N skipped_inflight=N queue_fail=N` (counts for those
10 s; one client normally shows `sent=100`).

## Recorder (PC side)

`host/ld2410_rec.py` uses only the Python standard library:

```
uv run python host/ld2410_rec.py record aeris-mvp.local -o run.ldrec   # --port 5410 is the default
uv run python host/ld2410_rec.py export-csv run.ldrec -o run.csv       # without -o: CSV to stdout
uv run python host/ld2410_rec.py info run.ldrec
```

- `record HOST [--port PORT] [-o FILE]` writes a `.ldrec` file (default name
  `ld2410_YYYYmmdd_HHMMSS.ldrec`), reconnects forever with backoff (0.5 s up to
  10 s) and resumes from the last sequence number plus one. Stop with Ctrl-C.
  A gap (frames evicted from the ring buffer while the link was down) is logged
  and written as a gap record. If the ESP32 rebooted (`boot_id` changed) a
  reboot record is written, the sequence baseline is reset and the recorder
  asks again from sequence 0 of the new boot.
- `pc_time_ns` is taken per batch: all frames of one batch share it (use
  `esp_time_us` for the spacing of frames inside a batch). Frames evicted from
  the ring before the first request of a recording are not reported as a gap
  (there is no baseline sequence to compare with).
- `export-csv [--gps FILE] [--imu FILE]` decodes the raw frames. Columns: `seq,
  esp_time_us, pc_time_ns, data_type, target_state, moving_dist_cm, moving_energy,
  still_dist_cm, still_energy, detect_dist_cm, max_moving_gate, max_still_gate,
  move_g0..8, still_g0..8, record, gap_to_seq, error, old_boot_id, new_boot_id,
  frame_utc, time_source, gps_utc, gps_lat, gps_lon, gps_alt_m, gps_sats, gps_hdop,
  gps_fix_quality, gps_flags, pitch_deg, roll_deg`. `record` is `frame`, `gap` or
  `reboot`. `frame_utc` (ISO 8601, microseconds) is the frame's `esp_time_us`
  converted with the nearest preceding `time_sync` record of the same boot (if none
  precedes it, the following one; none at all: empty); `time_source` is `gps` or
  `sntp`. `gps_*` is the latest fix recorded before the frame, `pitch_deg` /
  `roll_deg` the latest IMU sample (empty while the IMU status is invalid); both are
  forgotten at a reboot. `gps_lat`/`gps_lon` are empty without a position,
  `gps_alt_m` without an altitude, `gps_utc` without valid time and date. GPS fixes,
  IMU samples and time syncs have no rows in the main CSV; `--gps` / `--imu` write
  them to their own CSV files (`GPS_CSV_COLUMNS` / `IMU_CSV_COLUMNS` in
  `host/ld2410_rec.py`; a damaged payload gives a row with `error` set).
- `info` prints the frame count, sequence range, PC and ESP durations, reboots,
  gaps, the number of gps/imu/time_sync records, the GPS fix ratio (fixes with a
  valid position and fix quality > 0, over all gps records) and the time-sync
  sources.

One recording client at a time: a new connection replaces the old one. The
ring buffer holds 4 MiB in PSRAM (if that allocation fails the firmware falls
back to 32 KiB of internal RAM and logs a warning), so a recording survives a
Wi-Fi drop as long as the buffer still covers the outage.

## Protocol layouts (version 3, all little-endian)

Source of truth: `esp32/components/core/rec_proto.[ch]`, `rec_payload.[ch]` and
`host/ld2410_rec.py`; all are tested against the shared vectors in
`esp32/tests/vectors/` (mostly synthetic; two engineering frames are real
captures, confirmed on hardware 2026-10-02).

**Request** (PC to ESP32, 12 B): `"LDRQ"`, `version u8 = 3`, `reserved u8[3] = 0`,
`from_seq u32`. The server closes the connection on a bad length, magic,
reserved bytes or any other version (a v2 recorder is refused, a v3 recorder
refuses a v2 device), and after 5 s without a complete request.

**Batch** (ESP32 to PC): header 20 B, then `count` records.

| Offset | Size | Field |
|---|---|---|
| 0 | 4 | `"LDRB"` |
| 4 | 1 | `version = 3` |
| 5 | 1 | `flags`: bit0 `GAP` (requested start older than the oldest stored record; the batch starts at the oldest) |
| 6 | 2 | reserved = 0 |
| 8 | 4 | `first_seq` |
| 12 | 2 | `count` |
| 14 | 2 | reserved = 0 |
| 16 | 4 | `boot_id` (random per ESP32 boot, never 0; the sequence restarts at 0 after every reboot) |

Record: `seq u32, esp_time_us u64, type u8, len u16, payload[len]` (15 B header).
All record types share one sequence. A batch with `count = 0` is a keep-alive and
is sent about once per second while idle. The client resumes from `first_seq +
count` of the last batch of the current boot. Sequence numbers wrap at 2^32.

| Type | Payload |
|---|---|
| 0 `ld2410_frame` | the raw LD2410C frame byte for byte, header to footer |
| 1 `gps_fix` (32 B) | `utc_unix_ms i64` (0 if time/date invalid), `lat_e7 i32`, `lon_e7 i32`, `alt_cm i32`, `speed_cmps u16` (saturating), `course_cdeg u16`, `hdop_x100 u16`, `sats u8`, `fix_quality u8` (GGA value), `flags u8` (bit0 time valid, bit1 date valid, bit2 position valid, bit3 altitude valid), `reserved u8[3] = 0` |
| 2 `imu` (18 B) | `acc_mg i16[3]`, `gyr_ddps i16[3]` (0.1 deg/s), `pitch_cdeg i16`, `roll_cdeg i16`, `n_samples u8`, `status u8` (bit0 data valid) |
| 3 `time_sync` (9 B) | `utc_unix_us i64`, `source u8` (1 GPS, 2 SNTP); the record's `esp_time_us` is the matching ESP32 time |

Unknown record types: the ESP32 ring, batch builder and server are type-agnostic and
forward any type byte unchanged (`rec_record_check()` classifies a type and length:
known and valid, wrong length, or unknown). The PC recorder never fails on an
unknown type: it stores the record as file type 6 (below) and `info` counts it. A
known type with a wrong payload length is stored as is and shows up as a decode error
(`error` column) instead of stopping the recording. Decoders reject nonzero reserved
bytes of `gps_fix`; undefined flag, status and source values pass through.

**File `.ldrec` (version 3)**: header 20 B = `"LDREC1\0\0"` (8), `version u16 = 3`,
`reserved u16 = 0`, `created_unix_ns u64`; then records, each starting with a
`type u8`:

| Type | Layout after the type byte |
|---|---|
| 0 frame | `seq u32, esp_time_us u64, pc_time_ns u64, len u16, raw[len]` |
| 1 gap | `from_seq u32, to_seq u32, pc_time_ns u64` (missing range `[from_seq, to_seq)`) |
| 2 reboot | `old_boot_id u32, new_boot_id u32, pc_time_ns u64` |
| 3 gps_fix, 4 imu, 5 time_sync | `seq u32, esp_time_us u64, pc_time_ns u64, len u16, payload[len]` (payload as in the table above) |
| 6 unknown | `seq u32, esp_time_us u64, pc_time_ns u64, wire_type u8, len u16, payload[len]` (a wire type this recorder does not know) |

Gap and reboot records and the sequence bookkeeping work over all record types
together. The recorder still reads version 2 files (types 0 to 2, same layouts,
no sensor columns filled in); it only writes version 3.

## LD2410C protocol summary

The Hi-Link manual is not in the repository yet (BACKLOG). The layout below is
what the code implements; it was written from the protocol description and is
checked by synthetic vectors and, for the engineering frame, by two real
captures (confirmed on hardware 2026-10-02; Hi-Link manual still not in
`hardware/datasheets/`).

- UART 256000 8N1.
- **Data frame:** header `F4 F3 F2 F1`, payload length u16, payload, footer
  `F8 F7 F6 F5`. Payload: byte 0 type (`0x01` engineering, `0x02` normal),
  byte 1 `0xAA`, target state (0 none, 1 moving, 2 still, 3 both), moving
  distance cm u16, moving energy, still distance cm u16, still energy,
  detection distance cm u16; engineering frames add max moving gate, max still
  gate, 9 moving-gate and 9 still-gate energies, then module-specific bytes
  (skipped); the payload ends with `0x55 0x00`. The parser accepts payloads up
  to 64 bytes.
- **Command frame:** header `FD FC FB FA`, length u16 (= 2 + value length),
  command word u16, value, footer `04 03 02 01`. ACK: command word | `0x0100`,
  then status u16, 0 = OK.
- **At start** the firmware sends enable-configuration (`0x00FF`, value
  `0x0001`), enable-engineering-mode (`0x0062`) and end-configuration
  (`0x00FE`), waiting up to 500 ms for each ACK. On failure it logs a warning
  and continues in normal mode (gates shown as unavailable). No other
  configuration is changed.

## Host tests

```
make -C esp32/tests test
```

Plain C11 modules in `esp32/components/core/` (parser, frame decoder, command
codec, ring buffer, recording protocol, snapshot JSON, Wi-Fi form and password
helpers, WebSocket slot table), built with `-Wall -Wextra -Werror` and
AddressSanitizer/UBSan. The Python side:
`uv run pytest host/test_ld2410_rec.py -v`.

## CI

Job `esp32-mvp` in `.github/workflows/ci-tests.yml`: `make -C esp32/tests test`,
then `idf.py set-target esp32s3 && idf.py build` in `espressif/idf:v5.5.5`.
The Python job runs `host/test_ld2410_rec.py`. The ESP-IDF build was only ever
verified by CI (no Docker/ESP-IDF in the development environment).

## Optional bench checks (oscilloscope, Fluke 199C)

- LD2410C TX pin: logic high about 3.3 V (not 5 V) before wiring it to GPIO18.
- Bit time about 3.9 us at 256000 Bd (1 / 256000 = 3.906 us).
- Ripple on the 5 V rail with Wi-Fi active (transmit bursts).

## VERIFY list

- Octal flash and octal PSRAM boot on the N32R8V module (`sdkconfig.defaults`).
- LD2410C ACK sequence and the real frame rate.
- Engineering frame layout, including the extra module-specific bytes:
  confirmed on hardware 2026-10-02 (two real frames in the shared vectors);
  Hi-Link manual still not in `hardware/datasheets/`. Maximum payload length
  64 is still VERIFY; the other shared test vectors are synthetic.
- WebSocket close handling (slot removal, reconnect of the page).
- Recording server preemption (a new client replacing the old one) and the
  5 s send/receive timeouts; there is no TCP keepalive (BACKLOG).
- mDNS (`aeris-mvp.local`) on the home network.
- A 64-character hexadecimal Wi-Fi password (WPA2 treats it as a raw PSK; the
  63-character ASCII case is the normal one).
- Task stack sizes (LD2410C task 4096 B, recording tasks 4096 B, HTTP server
  6144 B, main task 6144 B, BOOT monitor 3072 B) under real load.
- The AP password stays stable across reboots and a credential reset.
