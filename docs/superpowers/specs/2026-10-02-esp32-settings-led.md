# Spec: LD2410C settings page, AP on demand and RGB LED status (ESP32 MVP)

Date: 2026-10-02. Owner: Andrii. Status: approved in brainstorming (2026-10-02).

## Context

The ESP32-S3 + LD2410C MVP (`esp32/`, PR #6, GPS/IMU in PR #7) has no way to configure the
LD2410C, and its AP comes up only when the STA link fails or after the 5 s BOOT reset (which
erases the home Wi-Fi credentials). The live page has no authentication.

## Decisions (owner, brainstorming 2026-10-02)

1. PA temperature sensor: **not needed now** (no part that can overheat on the bench).
2. Protection of settings: **no password; settings pages only from the ESP32 AP** (like
   `/wifi`). The AP's WPA2 password and physical access to the board are the protection. The
   live page and the recording port stay open on the home network.
3. Daily work stays on the **home Wi-Fi (STA)**; the AP is brought up **on demand** by a short
   BOOT hold.
4. **BOOT button, action on release**, with RGB LED feedback while held:
   0–2 s nothing; 2–5 s blue (release: AP on demand); 5–10 s red (release: erase STA
   credentials and reboot, as today); > 10 s LED off (release: cancel, nothing happens).
5. **LED status in normal operation**: STA connected: off; AP on demand: blue slow blink;
   AP only (no credentials or STA failed): blue steady; action confirmation: 3 fast flashes in
   the action's colour. Low brightness (about 5 %).
6. The on-demand AP turns off **10 min after the last client disconnects** (never while a
   client is connected).
7. LD2410C settings in scope: maximum moving/still gate, "no one" duration, per-gate
   moving/still sensitivity, Bluetooth off, firmware version and current parameters (read),
   factory reset, module restart. **Not now:** distance resolution 0.2 m (changes the gate
   size; needs a recording-protocol change together with a "config" record), baud rate.
8. **Bluetooth of the LD2410C is switched off** (once, from the page); it would otherwise let
   anyone nearby change the settings with the Hi-Link phone app.
9. The recording protocol is **unchanged**; settings changes are logged on the console only.
   A "config" record and the 0.2 m resolution go to BACKLOG (one protocol change, v4,
   together with the azimuth of the rotating radar).
10. Rotating radar: purpose = rehearse mechanics and software for the future radar; **step
    mode** (move, settle, take frames); drive **B**: 28BYJ-48 + ULN2003 + home sensor (Hall or
    slotted optical), **360° back and forth** (no slip ring). Hardware not on hand: a **separate
    later cycle**; recorded in BACKLOG with the shopping list.

## Requirements

### R1. RGB LED
- The DevKitC-1 v1.1 on-board addressable RGB LED (WS2812-type), data on **GPIO38**
  (`hardware/datasheets/esp_dev_kits_en_master_esp32s3-3540495.pdf`, ch. 1 ESP32-S3-DevKitC-1,
  "Description of Components", p. 4: "Addressable RGB LED, driven by GPIO38"; "Hardware Revision
  Details", p. 5: the initial version uses GPIO48, v1.1 GPIO38). One constant. GPIO38 is not a
  strapping pin and not one of GPIO35-37 taken by the octal flash/PSRAM (same document, p. 4-5).
  The WS2812 part number and its timing are not in the document: timing VERIFY.
- Driven with the ESP-IDF v5.5 RMT TX driver (`esp_driver_rmt`) and a small bytes encoder
  (WS2812 timing, GRB order), no new managed component. Brightness about 5 % (one constant).
- One `status_led` task owns the LED; others only set state. A failing LED init is logged and
  everything else keeps working.

### R2. BOOT button (GPIO0) and LED feedback
- Hardware-independent state machine in `components/core/` (host-tested): input = pressed /
  released + elapsed ms; output = current zone (`NONE` 0–2 s, `AP` 2–5 s, `RESET` 5–10 s,
  `CANCEL` > 10 s, boundaries: a hold of exactly 2000 ms is `AP`, 5000 ms `RESET`, 10000 ms
  `CANCEL`) and, on release, the action of the zone (`NONE`/`CANCEL` -> none).
  Debounce: the existing 50 ms polling.
- LED priority (also core, host-tested): button held (zone colour) > confirmation flashes >
  AP-only steady blue > AP-on-demand slow blink (1 Hz, 50 %) > off.
- Release in `AP`: start the AP on demand (R3) and flash blue 3 times. If the AP is already
  running (fallback or on demand) the timer is restarted, flashes as well.
- Release in `RESET`: flash red 3 times, then erase the STA credentials and restart (today's
  behaviour, now on release instead of at 5 s; the AP password is kept).

### R3. AP on demand
- `wifi_mgr_ap_on_demand()`: STA mode -> `WIFI_MODE_APSTA` with the existing AP config
  (SSID `AERIS-MVP-XXXX`, WPA2, stored random password); the STA link stays up (the AP uses
  the STA's channel). Log the AP SSID and IP; the AP password line is printed again.
- Count AP clients from `WIFI_EVENT_AP_STACONNECTED` / `_STADISCONNECTED`. Turn the on-demand
  AP off (back to `WIFI_MODE_STA`) when no client has been connected for **10 min** (one
  constant); a timer started at AP start and on each last-client disconnect, cancelled while a
  client is connected.
- The fallback AP (no credentials / STA failed) is unchanged and never times out.
- `/wifi` and the new `/ld2410` work on the on-demand AP exactly as on the fallback AP
  (AP-only check by the socket's local address, existing `http_srv_req_on_ap`).

### R4. LD2410C commands (core, host-tested; protocol facts VERIFY against the Hi-Link
"LD2410C Serial Communication Protocol" — not in `hardware/datasheets/` yet)
- Encoders: set max gates and duration (`0x0060`: words 0x0000 max moving gate, 0x0001 max
  still gate, 0x0002 no-one duration s, each `u16 word + u32 value`), read parameters
  (`0x0061`), set gate sensitivity (`0x0064`: 0x0000 gate (0..8), 0x0001 moving, 0x0002
  still), read firmware version (`0x00A0`), factory reset (`0x00A2`), restart (`0x00A3`),
  Bluetooth on/off (`0x00A4`, value 0x0000 off / 0x0001 on; effective after restart).
- Decoders: read-parameters ACK (`0xAA` header, max gate N, max moving gate, max still gate,
  N+1 moving sensitivities, N+1 still sensitivities, duration u16) and firmware-version ACK
  (type u16, major u16, minor u32; shown as `V<major hi>.<major lo>.<minor hex>`). Bounds
  checked; malformed ACKs rejected.
- Value limits: gates 2..8, duration 0..65535 s, sensitivity 0..100 (VERIFY; the manual may
  fix the still sensitivity of gates 0 and 1 — if so they are shown read-only).
- `LD_CMD_MAX_VALUE` grows to fit the 18-byte values.

### R5. Command execution in the LD2410C task
- The LD2410C task owns UART1. Other tasks submit a request (queue) and wait for the result
  with a timeout (5 s). The task runs: enable-config, the commands, end-config, each ACK
  checked (existing `send_cmd`); on any failure it still sends end-config.
- Data frames pause during configuration (about up to 1 s, VERIFY); the frame counter simply
  continues. After restart or factory reset the existing engineering-mode auto-recovery
  re-enables engineering mode.
- Every change is logged once on the console: old -> new values (from a read before and after).

### R6. `/ld2410` page (AP only, same style as `/wifi`)
- GET: reads firmware version and parameters (R5) and shows a form: max moving gate, max
  still gate, no-one duration, a 9-row table of moving/still sensitivity, and buttons
  **Save**, **Bluetooth off**, **Restart module**, **Factory reset** (browser confirm on the
  last two). If the read fails: error text, no form.
- POST (form-urlencoded, length-limited, parsed and range-checked in core with the existing
  URL decoding): Save writes only the values that differ from a fresh read; the result page
  shows OK or the failing command. Requests from the STA side get 404.
- A link to `/ld2410` on the `/wifi` page and vice versa; the live page gets no link (it is
  served on STA).

### R7. Docs and BACKLOG
- `esp32/README.md` (button table, LED table, AP on demand, `/ld2410`, VERIFY items),
  `docs/decisions.md` (decisions 1–10), `docs/architecture.md`, BACKLOG: close the settings
  page and authentication items (decision 2), drop the PA temperature item from the ESP32
  track, add: protocol v4 (config record, 0.2 m resolution, azimuth), rotating radar details
  (decision 10 with shopping list), LD2410C protocol PDF into `hardware/datasheets/`.

## Non-goals
No password/HTTPS, no change to the live page data or the recording protocol, no 0.2 m
resolution, no baud-rate change, no motor code.

## Acceptance
CI green; host tests pass (button state machine, LED priority, command encoders/decoders,
form parsing); owner bench check: LED colours while holding BOOT, AP on demand appears and
disappears after 10 min without clients, `/ld2410` reads the module, changes a gate
sensitivity (visible in the live gate behaviour and after a power cycle), Bluetooth off (the
Hi-Link app no longer finds the module), 5 s reset still works, > 10 s cancels.
