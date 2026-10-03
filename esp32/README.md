# AERIS-10 Lite MVP: ESP32-S3 + HLK-LD2410C

A bench MVP built from parts on hand, independent of the RF chain, the FPGA
board and the STM32 firmware (none of them is part of it). An ESP32-S3 reads
the HLK-LD2410C, a ready 24 GHz FMCW presence radar, over UART and offers:

- a **live web page** served by the ESP32 (presence, distances, energies,
  9 + 9 gate energies in engineering mode);
- a **recording server** (TCP 5410) from which a PC tool stores the raw
  LD2410C frames, with sequence numbers and timestamps, and survives Wi-Fi
  drops;
- a **settings page** for the LD2410C (`/ld2410`, only from the ESP32's own
  access point), an **RGB status LED** and **BOOT-button actions** (access
  point on demand, Wi-Fi reset);
- optional **GPS** (GY-NEO6MV2, u-blox NEO-6M) and **IMU** (GY-BMI160, 6-axis,
  no magnetometer): UTC time and position, and absolute radar tilt (pitch and
  roll), shown on the live page and stored in the same recording as separate
  typed records (recording protocol v3). A missing GPS or IMU never affects the
  LD2410C path.

Specification and plan: [spec](../docs/superpowers/specs/2026-10-02-esp32-ld2410-mvp.md),
[plan](../docs/superpowers/plans/2026-10-02-esp32-ld2410-mvp.md); GPS and IMU:
[spec](../docs/superpowers/specs/2026-10-02-esp32-gps-imu.md),
[plan](../docs/superpowers/plans/2026-10-02-esp32-gps-imu.md). System context:
[docs/architecture.md](../docs/architecture.md).

> **Status: host tests and a CI build only. The only hardware-confirmed item is
> the LD2410C engineering frame layout (two real captures, 2026-10-02); the GPS
> and IMU parts have never been run on the boards.** Everything not verifiable from the repository is marked
> **VERIFY**; the list is at the end.

## Hardware

| Part | Notes |
|---|---|
| ESP32-S3-DevKitC-1 v1.1, module N32R8V | 32 MB octal flash, 8 MB octal PSRAM (VERIFY on the real board) |
| HLK-LD2410C | 24 GHz FMCW presence radar, UART 256000 8N1 |
| GY-NEO6MV2 | u-blox NEO-6M GPS module, UART 9600 8N1, 3.3 V logic (optional) |
| GY-BMI160 | Bosch BMI160 6-axis IMU (accelerometer + gyroscope), I2C (optional) |
| On-board RGB LED | addressable (WS2812-type) LED of the DevKitC-1 v1.1, data on GPIO38; no wiring |
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

### GPS and IMU wiring

GY-NEO6MV2 (UART2):

| GPS module pin | ESP32-S3 DevKit | Note |
|---|---|---|
| VCC | 3V3 | 3.3 V supply (see the power note) |
| GND | G | common ground |
| TX | GPIO5 | UART2 RX of the ESP32 |
| RX | GPIO4 | UART2 TX of the ESP32 (the UBX NAV-TIMEUTC poll; **required** for the UTC verification, see "GPS") |

GY-BMI160 (I2C, 400 kHz):

| IMU board pin | ESP32-S3 DevKit | Note |
|---|---|---|
| 3V3 / VIN | 3V3 | 3.3 V supply |
| GND | G | common ground |
| SCL | GPIO9 | I2C clock |
| SDA | GPIO8 | I2C data |
| CS | 3V3 | selects I2C mode (CS low would select SPI) |
| SA0 (SDO) | GND | I2C address 0x68; the firmware also probes 0x69 |

GPIO4, 5, 8 and 9 were chosen as free pins: not the LD2410C pins (17, 18), not
native USB (19, 20), not BOOT (0). That they are not strapping or PSRAM pins on
the DevKitC-1 v1.1 is **VERIFY** against the ESP32-S3 datasheet and the board
pinout.

**POWER NOTE.** Both modules run from the DevKit **3V3** pin; their logic is 3.3 V.
Only the LD2410C is on 5 V (see the warning above). Never connect two 5 V
sources together, and do not feed 5 V into a 3.3 V module. The internal
pull-ups of GPIO8/9 (about 45 kOhm) are enabled as a fallback; the GY-BMI160
normally has its own pull-ups. Whether 400 kHz works with them is **VERIFY**
(fall back to 100 kHz via `IMU_I2C_HZ` in `main/imu_task.c` if not).

## Build, flash, monitor

ESP-IDF **v5.5.5** is pinned (the CI container is `espressif/idf:v5.5.5`).
With ESP-IDF installed and exported:

```
cd esp32
idf.py set-target esp32s3
idf.py build flash monitor
```

`sdkconfig.defaults` is committed (octal flash and PSRAM, the ESP-IDF "single factory app
(large)" partition table: NVS at 0x9000 as before, 1.5 MB app, since the 1 MB app of the plain
table was 97 % full; `app_main.c` stops the build with `#error` while a stale `sdkconfig` still
selects another table; console on
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
  timeout). If it connects, only STA is active and the AP is off; the AP (and
  with it `/wifi` and `/ld2410`) is brought up on demand with the BOOT button
  (see "AP on demand"). If it does not connect, the
  AP starts alongside (AP+STA) and the STA keeps retrying in the background
  (backoff 2 s doubling to 30 s). A dropped STA link is also retried.
- On the STA network the board is reachable as `http://aeris-mvp.local`
  (mDNS, VERIFY) or by the STA IP from the console.
- **BOOT released after 5 to 10 s while the firmware is running** erases the
  stored STA credentials (not the AP password) and reboots into AP mode; see
  "BOOT button and RGB LED". Do **not** hold BOOT at power-up or reset: GPIO0
  low at reset selects the ROM download mode.

## BOOT button and RGB LED

The BOOT button (GPIO0) acts **on release**, by how long it was held, and the
RGB LED shows the zone while it is held. The button is polled every 50 ms; the
zone logic is `components/core/boot_btn.[ch]` (host-tested). Exact boundaries:
2000 ms is already the blue zone, 5000 ms the red zone, 10000 ms the cancel
zone.

| Held | LED while held | Action on release |
|---|---|---|
| under 2 s | off | nothing |
| 2 s to under 5 s | blue | AP on demand ("AP on demand" below), then 3 blue flashes |
| 5 s to under 10 s | red | 3 red flashes, then the STA credentials are erased (the AP password is kept) and the board restarts |
| 10 s or more | off | cancelled, nothing happens |

A button that is already down when the firmware starts is ignored until it has
been released once. **Never hold BOOT while resetting or powering up**: GPIO0
low at reset selects the ROM download mode (the same rule as for flashing).
The blue flashes appear only when the AP could be started (not when Wi-Fi was
busy: then press again).

RGB LED in normal operation (`components/core/status_led.[ch]`, host-tested;
one `status_led` task owns the LED). Highest priority first:

| Priority | State | LED |
|---|---|---|
| 1 | BOOT held | zone colour of the table above |
| 2 | action confirmation | 3 flashes (100 ms on, 100 ms off) in the action's colour |
| 3 | AP only (no stored credentials, or the STA did not connect: fallback AP) | blue steady |
| 4 | AP on demand | blue slow blink (1 Hz, 50 %) |
| 5 | STA connected, no AP | off |

The brightness is 5 % of full scale (`RGB_LED_BRIGHTNESS_PCT`, the LED is very
bright). The LED is the on-board addressable RGB LED of the DevKitC-1 v1.1 on
**GPIO38** (`RGB_LED_GPIO` in `main/rgb_led.h`;
`hardware/datasheets/esp_dev_kits_en_master_esp32s3-3540495.pdf`, ch. 1
ESP32-S3-DevKitC-1, "Description of Components" p. 4 and "Hardware Revision
Details" p. 5). **Boards of version v1.0 use GPIO48**: change that one constant
for them. It is driven by the ESP-IDF RMT TX driver (GRB order). A failing LED
initialisation is logged and everything else keeps working.

## AP on demand

Daily work is on the home Wi-Fi (STA), where the AP is off. To reach `/wifi` or
`/ld2410` (both are served only to clients of the AP):

1. Hold **BOOT** for 2 to 5 s (LED blue) and release. The LED flashes blue 3
   times and then blinks blue slowly. The console prints the line
   `AP password: ...  AP SSID: AERIS-MVP-XXXX  AP IP: 192.168.4.1 (AP on demand)`
   again (open the console with `idf.py monitor`).
2. Join `AERIS-MVP-XXXX` with that password (the same WPA2 password as always)
   and open **http://192.168.4.1/ld2410** or **/wifi**.

The board stays on the home Wi-Fi meanwhile (AP+STA; the AP uses the STA's
channel, so the AP channel is the router's, not 1, while the on-demand AP is
up). The on-demand AP switches itself off **10 minutes after the last client
disconnected** (`AP_DEMAND_IDLE_MS`; the timer starts when the AP starts and at
each disconnect of the last client, and never runs while a client is
connected); the LED goes dark. Another 2 to 5 s press while the AP is up
restarts the timer. If the switch-off finds Wi-Fi busy (a scan or connect
attempt) it retries after 5 s. The **fallback AP** (no credentials, or the STA
did not connect) is unchanged: it never times out and the LED stays steady
blue.

## LD2410C settings page (`/ld2410`)

Served only to clients of the AP (on demand or fallback); requests from the STA
side get 404, like `/wifi`. There is no password: the AP's WPA2 password and
physical access to the board are the protection (owner decision 2026-10-02).
The live page and the recording port stay open on the home network. The page
links to `/wifi` and back. Source of the numbers below:
`hardware/datasheets/Protocolo_comunicacion_serial_LD2410C.pdf` (Hi-Link
"HLK-LD2410C serial communication protocol" V1.00, the text is English despite
the file name; page numbers are the document's "Page N / 23").

The page first reads the module (firmware version and parameters). If that
fails it shows an error text and no form. Fields:

- **Max moving gate** and **max still gate** (2 to 8): the farthest gate in
  which a moving or still target is reported. One gate is **0.75 m**: with 2,
  only targets within 1.5 m count (§1.2.2, p. 6; the command section p. 10 gives
  2 to 8, §1.2.2 says 1 to 8, the firmware follows the command section).
- **No-one duration** (0 to 65535 s): after the last detection the module keeps
  reporting "occupied" for this long before it reports "unoccupied" (§1.2.2,
  p. 7).
- **Sensitivity table**, one row per gate 0 to 8 (range of the gate shown), moving
  and still, 0 to 100 (§1.2.2, p. 6-7): a target counts when its energy (0 to
  100) is above the value; **100 means the gate is ignored**. The **still
  sensitivity of gates 0 and 1 cannot be set** (Table 7, p. 15): it is shown
  read-only. The factory defaults of Table 7 are shown next to each row, and
  the factory max gates (8/8) and duration (5 s) under the table.
- **Save** writes only the values that differ from a fresh read, then reads
  the module back and shows what is now in it. The values are **stored in the
  module** (they survive a power cycle). **Data frames pause while the module is
  being configured** (about 30 ms for a one-gate write on the bench, 2026-10-02;
  the frame counter continues); each
  change is logged once on the console as `setting: ... old -> new`. Changes
  are **not recorded** in the `.ldrec` file (recording protocol unchanged).
- **Bluetooth off**: switches the module's Bluetooth off and restarts it
  (`0x00A4` then `0x00A3`; §2.2.12, p. 16). Without this anyone nearby could
  change the settings with the Hi-Link phone app. Switching it back on is not
  offered (the document is inconsistent about that value).
- **Restart module** (`0x00A3`, §2.2.11, p. 15; browser confirmation).
- **Factory reset** (`0x00A2`, effective after a restart, so the firmware
  restarts the module as well; §2.2.10, p. 14; browser confirmation). Gates,
  duration and sensitivities return to the defaults of Table 7 (p. 15). The
  document does not say whether the Bluetooth setting is reset too: check with
  the Bluetooth switch-off afterwards.

After a restart the module comes back in normal mode and the firmware
re-enables engineering mode by itself after about 3 s of normal frames. Other
tasks never touch UART1: the page submits a request to the LD2410C task, which
executes it between frame reads (see `docs/architecture.md`), with a 5 s limit;
a busy or silent module gives an error text on the page.

## Live page

`GET /` serves one self-contained page (no external resources). It opens a
WebSocket to `/ws`, which pushes the **latest snapshot** as JSON at 10 Hz. A
slow client only gets the newest snapshot (no per-client queue; up to 4
clients). The page shows:

- sensor link status (`no_data`, `ok`, `lost` after more than 1 s without a
  frame) and WebSocket status (reconnects automatically);
- LD2410C frame number and frames per second. Both come from `frame_no`, a
  dedicated counter of decoded LD2410C data frames (u32, wraps). The snapshot
  also carries `seq`, the shared ring sequence for recording correlation; it
  advances for GPS, IMU and time records too, so it is not a frame count;
- presence state (none / moving / still / both);
- moving distance and energy, still distance and energy, detection distance;
- bar charts of the 9 moving and 9 still gate energies (0-100), only when
  engineering mode was acknowledged, otherwise "engineering mode
  unavailable". Whenever normal-mode frames (data type `0x02`) keep arriving for
  more than 3 s (no ACK at boot, or the module reset itself after a power
  glitch and restarted in normal mode), the enable sequence is re-run, at most
  once per 10 s and without a limit over time; each attempt and its result is
  logged. Data reception continues meanwhile; engineering mode counts as
  enabled only while engineering frames arrive;
- a **GPS card**: badge (`fix`, `no fix`, `no data` = module silent for more than
  3 s, `not connected` = no byte ever received), fix state with the GGA fix
  quality, satellites and HDOP, latitude, longitude (7 decimals), altitude,
  speed (km/h), course, UTC (only when time and date are valid), a note under the
  UTC (`(leap seconds not yet known)` while the receiver reports its UTC as not
  valid, `(not verified: no UBX answer)` when it does not answer the UBX poll;
  nothing when verified) and the **time source** currently shown (`GPS`, `SNTP`
  or `none`);
- a **Tilt card**: badge (`ok`, `no data`, `error`, `not connected`), pitch
  (nose up +) and roll (right side down +) in degrees with two decimals, and a
  level indicator (artificial horizon; the ring turns to the "level" colour when
  both angles are within 1 degree). The IMU counts as `error` when its last
  record is older than 1 s.

Values that are not valid show `-`; the snapshot JSON carries `null` for them
(top-level keys `gps`, `imu`, `time_source`; `gps.utc_state` is `valid`,
`not_valid`, `no_ubx` or `null` whenever the GPS link is not `ok`, i.e. no good NMEA line in the last 3 s). The page needs no new connection:
the same 10 Hz WebSocket snapshot carries everything.

The console logs `ws client added fd=.. slot=..` / `ws client removed fd=..`
for each WebSocket client, a warning for each failed send, and every 10 s
`live: clients=N sent=N skipped_inflight=N queue_fail=N` (counts for those
10 s; one client normally shows `sent=100`).

Other 10 s status lines (all also appear without the sensor, so a wiring
problem is visible in the console):

- `gps`: `no data from the module (not connected?)`; or `ok|silent, no epoch
  yet, lines good N bad N dropped N`; or `ok|silent, fix Q, sats N, hdop H.HH,
  lines good N bad N dropped N` (`fix Q` is the GGA fix quality, 0 = no fix;
  `bad` counts checksum and malformed-line errors, `dropped` overlong or
  abandoned lines).
- `gps` UTC line (only while the GPS link is `ok`, i.e. a good NMEA line in the last 3 s; absent otherwise): `utc valid, ubx good N bad_ck N
  skipped N bad_len N`, `utc not valid, ubx good ...`, or, as a warning, `utc no ubx:
  no NAV-TIMEUTC answer (GPS time is not verified), ubx good ...` (UBX frame counters
  since boot).
- `imu`: `no BMI160 on I2C (SDA 8, SCL 9, 0x68/0x69; not connected?), i2c errors
  N, probe timeouts N, bus resets N, last probe NACK|TIMEOUT|none yet`; or `ok|error, pitch X.X deg, roll Y.Y deg, i2c errors N, reinits N`
  (or `no attitude yet`). One-off lines: `BMI160 at 0x68 configured (+-4 g,
  +-500 deg/s, 100 Hz)`, `... consecutive I2C failures ..., re-initialising`,
  `0x.. answers, but CHIP_ID is ... not a BMI160`, `ERR_REG 0x.. after
  configuration`.
- `sntp`: one line `SNTP time YYYY-MM-DDTHH:MM:SSZ` per synchronisation and every
  60 s (STA connected with internet access only).

The footer links to the web console log (`/log`, next section).

## Web console log (`/log`)

`GET /log` shows the firmware's console log on a phone, so the stand can be
watched away from the USB cable (for example on a power bank on the balcony).

- **Open it.** On the home Wi-Fi: **http://aeris-mvp.local/log** (or the
  device IP). Without home Wi-Fi, or while it is down: start the AP on demand
  (BOOT 2-5 s, see "AP on demand"), join `AERIS-MVP-XXXX` and open
  **http://192.168.4.1/log**. The page is served on both STA and AP, like the
  live page. It is also linked from the footer of the live page, `/wifi` and
  `/ld2410`.
- **Page.** Dark monospace view that follows the newest line and stops
  following while you scroll up. Buttons **Copy** (clipboard API, falls back to
  `execCommand`; if the browser blocks both, the text is selected so it can be
  copied by hand) and **Select all**. A status line shows the byte count, a
  connection error, and "older lines lost" after a gap.
- **What is captured.** Only `ESP_LOGx` output. A hook on `esp_log_set_vprintf`
  still sends every line to the USB console unchanged and also appends it to a
  16 KB ring in internal RAM, with ANSI colour sequences removed. Output written
  with `printf` is **not** captured: the boot banner and the
  `AP password: ...` line appear on the USB console only. As defence in depth, a
  line containing `AP password:` would also be dropped by the hook.
- **Limits.** The ring keeps the newest 16 KB (older lines are overwritten).
  A formatted line of 256 bytes or more (colour codes and newline included) is
  cut to 251 bytes plus `...\n`. The log is not persistent: a reboot clears it.
  The stream offset starts at a random value on every boot, so a page left open
  across an ESP32 reboot gets the gap marker ("older lines lost") instead of
  splicing two boots. No authentication, no log level control.
- **Polling.** The page requests `GET /log/data?from=N` once per second (at once
  again while a response is full, and after 2 s on an error). The reply is
  `text/plain`, at most 4096 bytes, with headers `X-Log-Next: <offset>` (the
  `from` for the next request) and `X-Log-Gap: 0|1` (1 = the requested bytes
  were already overwritten, so some lines were lost). Without `from` the reply
  starts at the oldest byte held. The browser keeps the last 200000 characters
  and trims older text from the top.

## GPS (GY-NEO6MV2)

UART2, 9600 8N1, module TX -> GPIO5, module RX -> GPIO4 (wiring above). The
module keeps its factory configuration, **no UBX configuration: the firmware only
polls** (u-blox 6 Receiver Description, `hardware/datasheets/u-blox6_ReceiverDescrProtSpec_(GPS.G6-SW-10018)_Public.pdf`)
and reads the default NMEA output. The `gps` task (core 1,
priority 4, below the LD2410C task) feeds `components/core/gps_rx.[ch]`, a byte
router that sends NMEA text to `components/core/nmea.[ch]` (decodes **RMC** and
**GGA** sentences of talker IDs `GP` and `GN`; a checksum is required; other
sentences are ignored) and UBX binary frames to `components/core/ubx.[ch]`.

- **UTC verification (UBX NAV-TIMEUTC).** Once per second the task sends the
  8-byte poll `B5 62 01 21 00 00 22 67` (NAV-TIMEUTC, class 0x01 id 0x21, empty
  payload; §27.2 p. 86) on UART2 TX (GPIO4; short write, never blocks the
  reader). The answer (§35.13 p. 179-180, 20-byte payload) carries a `valid`
  byte whose bit 2 is **validUTC**. Frame layout §23 p. 84, checksum (8-bit
  Fletcher over class, id, length and payload) §26 p. 85-86. The framer keeps a
  payload of at most `UBX_MAX_PAYLOAD` = 64 bytes; longer frames up to
  `UBX_SKIP_MAX` = 512 bytes are skipped by their length, a length above that is a
  false sync (e.g. a stray `B5 62` in NMEA text). A `$` inside a UBX frame never
  reaches the NMEA parser. UTC state:
  - `valid`: the latest NAV-TIMEUTC answer is under 3 s old and has validUTC = 1.
    GPS `time_sync` records are written and `gps_fix.flags` bit 4 (UTC verified) is set.
  - `not valid`: the latest answer under 3 s old has validUTC = 0 (the receiver
    does not yet know the leap-second count), or no fresh answer yet while the
    5 s limit below has not passed (also during the first 5 s after the talk start). **No GPS `time_sync` is written** and bit 4 is
    clear; the `gps_fix` records keep coming.
  - `no ubx`: no NAV-TIMEUTC answer for more than 5 s (counted from the last
    answer or from the talk start, whichever is later; the talk start is the first
    good NMEA line after the link was unknown, i.e. never talked or silent for more
    than 3 s, so a module coming back after a silence waits 5 s for a new answer
    before the fallback; with no answer at all: the TX wire is missing, or the module
    does not take UBX input). **Fallback:** GPS
    `time_sync` records are written as before (time unverified), bit 4 stays
    clear, the 10 s log line warns and the live page says `(not verified: no UBX
    answer)`.

  The wire GPIO4 -> module RX is therefore **required** for verification (it is
  connected on the bench, owner 2026-10-03). Without a UBX answer the behaviour is
  as before the change.

  Known limits (accepted, not fixed): a UBX frame damaged on the line (overrun,
  noise) can swallow the start of the next NMEA line, which is then lost; a corrupted
  length of 65-512 skips up to 514 bytes, about 0.5 s of NMEA at 9600 Bd. The resync
  replay is recursive, its depth bounded by the frame length (measured: up to 11
  levels, and only with crafted input).

- **One `gps_fix` record per NMEA epoch** (RMC and GGA with the same UTC time
  field; about 1 Hz), written **also without a fix**, with the validity flags
  clear, so the recording shows the fix state over time.
- **Validity rules (conservative).** Position, time and date are flagged valid
  only when the RMC status is `A` (and the NMEA 2.3 mode field, when present, is
  not `N`); GGA position and altitude need fix quality > 0. An empty field is
  "not valid" (flag clear, value 0). `utc_unix_ms` is non-zero only when time
  and date are both valid. Leap second 60 is rejected.
- **No fix.** The `gps_fix` records keep coming with flags 0 (and no `time_sync`
  record). **Time from an RMC sentence with status `V` is never used**: a
  receiver without a fix can report a free-running RTC time or the GPS epoch date
  (possible with this module; **VERIFY**), which must not become a
  UTC time stamp. The live page shows `no fix` (module talking) or `no data`
  (nothing valid for more than 3 s) or `not connected` (no byte ever received).
- **Cold start.** The first fix after power-up typically takes about half a
  minute to a few minutes with a clear sky (u-blox quotes about 27 s cold start
  for the NEO-6M; from memory, not in the repository, **VERIFY**). Without sky
  view it never comes. **Place the antenna near a window** (or outdoors, flat,
  with the ceramic patch facing up); a fix at a desk deep inside a building is
  not to be expected. Bench, 2026-10-02: first fix about 7 min after
  power-up (the module was moved from the desk to the balcony meanwhile), 4
  satellites, HDOP about 5 at first; position, altitude and UTC correct.
- **UTC off by whole seconds right after the first fix.** On the bench
  (2026-10-02) the GPS UTC was **3 s ahead** of SNTP for the first 5.6 min after
  the first fix (335 consecutive syncs), then correct. Likely cause: until the
  receiver has decoded the GPS-UTC leap-second count from the satellites
  (broadcast every 12.5 min) it uses a default built into its firmware (from
  memory, **VERIFY**); NMEA has no flag for this. **Handled in the firmware
  (owner decision 2026-10-03):** GPS time is used only while NAV-TIMEUTC reports
  validUTC = 1 (see "UTC verification" above), also without Wi-Fi. The host
  conversion still **rejects a GPS `time_sync` that differs from the nearest SNTP
  sync of the same boot by more than 1 s** (owner decision 2026-10-02) as a second
  line of defence (it also covers old recordings); `info` prints how many were
  rejected.
- A one-line GPS status is logged every 10 s (see "Live page").

## Time sources and accuracy

Every recorded ESP32 time stamp is `esp_timer` microseconds since boot. UTC
comes from `time_sync` records `{utc_unix_us, source}` whose record `esp_time_us`
is the matching ESP32 time. There are two sources:

| Source | Code | When a record is written | Accuracy |
|---|---|---|---|
| GPS | 1 | on every RMC with status `A` and valid time and date (about 1 Hz), **only while the UTC state is `valid` or `no ubx`** (never while `not valid`, see "GPS"); stamped when the parser completes the RMC line | no PPS on the board: the UTC value belongs to the second boundary but the sentence arrives later (a 70-character RMC takes about 70 ms at 9600 Bd, plus the receiver's output delay). Measured on the bench (2026-10-02, 1413 syncs over 23 min, against SNTP): the GPS stamp is **128 ms late** (median), p1..p99 119..136 ms, no drift; not corrected in the recording. The whole-second error after the first fix is excluded by the UTC verification (see "GPS"); in the `no ubx` fallback it can still occur |
| SNTP | 2 | on each synchronisation with `pool.ntp.org` (ESP-IDF `esp_netif_sntp`), started once the STA link has an IP address; needs the home Wi-Fi with internet access | typically tens of ms over the internet (not measured separately; the GPS figures above are relative to it); the time is the ESP32 system clock right after the SNTP callback |

**Priority.** The live page shows GPS while the latest GPS `time_sync` is less
than 5 s old, otherwise SNTP once at least one SNTP sync happened, otherwise
`none`. This only decides what is *shown*: **both sources are recorded**. The PC
tool converts a record's `esp_time_us` to UTC with the nearest GPS `time_sync`
record of the same boot within +-2 s of it (GPS has priority; GPS syncs more than
1 s off the nearest SNTP sync of the boot are rejected first); otherwise with the
nearest preceding `time_sync` of any source (if none precedes it, the first
following one; none at all: empty), and writes that source into the
`time_source` column. Since SNTP syncs hourly by default, the firmware re-emits an
SNTP `time_sync` every 60 s once SNTP has synced, so a recording always has one. A reboot starts a new time base.

## IMU (GY-BMI160) and tilt

The `imu` task (core 1, priority 3, below the LD2410C and GPS tasks) reads the
BMI160 over I2C (400 kHz, address 0x68, 0x69 also probed) every 10 ms, **about
100 Hz** (sensor ODR 100 Hz, accelerometer +-4 g, gyroscope +-500 deg/s). Every
10 samples (**10 Hz**, a single constant) it writes one `imu` record: the mean
raw acceleration (mg) and angular rate (0.1 deg/s) per axis, the current
pitch and roll (0.01 deg), the number of averaged samples and a status byte
(bit0 = data valid). Tilt is **absolute**, relative to the horizon (complementary
filter on the gravity direction, gyro weight 0.98; no magnetometer, so no
azimuth). There is no "zero tilt" button. A missing sensor is probed every 1 s for the
first ~10 s (after boot or after a sensor loss), then every 10 s;
5 failed reads in a row trigger a re-initialisation; errors and re-inits are
counted and logged. The IDF `i2c.master` log tag is silenced (it printed two
errors per probe); the 10 s `imu` line reports the last probe instead:
`NACK` = no device answering (check wiring and power); `TIMEOUT` = bus held
low or no pull-ups (check SDA/SCL wiring, pull-ups and that the module is
powered: an unpowered module clamps the lines). The line also shows probe
timeouts and bus resets.

**Axis and sign conventions** (exactly as in `components/core/tilt.h`). Body
frame = the radar frame: **+X = radar boresight (forward), +Y = left, +Z = up**,
right-handed. Level and at rest the accelerometer reads (0, 0, +1000 mg).

- `pitch` positive = **nose up** (boresight above the horizon), range -90..+90 deg.
- `roll` positive = **right side down** (left side up), range (-180, +180] deg.
- Static: `pitch = atan2(ax, hypot(ay, az))`, `roll = atan2(ay, az)`. At pitch
  +-90 deg roll is undefined; the previous roll is kept there.
- An accelerometer sample with a magnitude outside 0.5 g to 1.5 g (free fall,
  shock) is not used for correction; the gyro still propagates.

**Default mounting (owner's bench, 2026-10-02):** the body frame is defined by
the breadboard stand, not by the LD2410C antenna (which faces the ceiling): the
**nose (+X) is the short breadboard edge with the power module and the GPS
antenna**, +Z up, +Y left when looking towards the nose. Pitch is positive with
the nose up, roll positive with the right side down. The GY-BMI160 board lies
flat with the **chip facing down** and turned 90° about the vertical, so the
mapping is `body X = +sensor Y`, `body Y = +sensor X`, `body Z = -sensor Z`
(`SRC` 1, 0, 2 and `SIGN` +1, +1, -1 in `components/core/tilt.h`; right-handed).
For another mounting (for example the radar on a mast) redefine the nose and
edit the six constants (`body[i] = TILT_MAP_SIGN_i * sensor[TILT_MAP_SRC_i]`,
source index 0 = X, 1 = Y, 2 = Z), then rebuild; keep the mapping right-handed
(an odd number of sign flips or swaps mirrors the frame and gives wrong angles).

### BMI160 registers used (all VERIFY)

The BMI160 datasheet (Bosch BST-BMI160-DS000) is **not** in
`hardware/datasheets/`; every value below is from `main/imu_task.c`, written
from memory of that datasheet, and is **unverified** (BACKLOG). Check each against
the datasheet before trusting the sensor data.

| Item | Value |
|---|---|
| I2C address | 0x68 (SA0/SDO low), 0x69 (high) |
| 0x00 CHIP_ID | 0xD1 expected; any other value = "not a BMI160", device rejected |
| 0x02 ERR_REG | read once after configuration (read-clear), leftovers logged |
| 0x03 PMU_STATUS | mask 0x3C, 0x14 = accelerometer and gyroscope in normal mode; polled up to 20 x 10 ms |
| 0x0C..0x17 DATA | 12-byte burst: gyro X/Y/Z (0x0C..0x11) then accelerometer X/Y/Z (0x12..0x17), little-endian int16 |
| 0x40 ACC_CONF | 0x28 (ODR 100 Hz, normal filter) |
| 0x41 ACC_RANGE | 0x05 (+-4 g) |
| 0x42 GYR_CONF | 0x28 (ODR 100 Hz, normal filter) |
| 0x43 GYR_RANGE | 0x02 (+-500 deg/s) |
| 0x7E CMD | 0xB6 soft reset, 0x11 accelerometer normal mode, 0x15 gyroscope normal mode |
| Delays | 100 ms after reset, 10 ms after acc normal, 85 ms after gyro normal, 5 ms after each configuration write (each is read back and compared) |
| Sensitivity | accelerometer 8192 LSB/g at +-4 g; gyroscope 65.6 LSB/(deg/s) at +-500 deg/s |

## Recorder (PC side)

`host/ld2410_rec.py` uses only the Python standard library:

```
uv run python host/ld2410_rec.py record aeris-mvp.local -o run.ldrec   # --port 5410 is the default
uv run python host/ld2410_rec.py export-csv run.ldrec -o run.csv       # without -o: CSV to stdout
uv run python host/ld2410_rec.py export-csv run.ldrec -o run.csv --gps gps.csv --imu imu.csv
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
  converted with the nearest GPS `time_sync` of the same boot within +-2 s (GPS
  syncs more than 1 s off SNTP are rejected), else the
  nearest preceding `time_sync` of any source (if none precedes it, the following one;
  none at all: empty); `time_source` is `gps` or
  `sntp`. `gps_*` is the latest fix recorded before the frame, `pitch_deg` /
  `roll_deg` the latest IMU sample (empty while the IMU status is invalid); both are
  forgotten at a reboot. `gps_lat`/`gps_lon` are empty without a position,
  `gps_alt_m` without an altitude, `gps_utc` without valid time and date. GPS fixes,
  IMU samples and time syncs have no rows in the main CSV; `--gps` / `--imu` write
  them to their own CSV files (`GPS_CSV_COLUMNS` / `IMU_CSV_COLUMNS` in
  `host/ld2410_rec.py`; a damaged payload gives a row with `error` set).
- `info` prints the frame count, sequence range, PC and ESP durations, reboots,
  gaps, the number of gps/imu/time_sync records, the GPS fix ratio (fixes with a
  valid position and fix quality > 0, over all gps records), `gps utc verified: N`
  (fixes with flags bit 4 set) and the time-sync sources.

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
| 1 `gps_fix` (32 B) | `utc_unix_ms i64` (0 if time/date invalid), `lat_e7 i32`, `lon_e7 i32`, `alt_cm i32`, `speed_cmps u16` (saturating), `course_cdeg u16`, `hdop_x100 u16`, `sats u8`, `fix_quality u8` (GGA value), `flags u8` (bit0 time valid, bit1 date valid, bit2 position valid, bit3 altitude valid, bit4 UTC verified by UBX NAV-TIMEUTC validUTC; older recorders ignore it), `reserved u8[3] = 0` |
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

Source: `hardware/datasheets/Protocolo_comunicacion_serial_LD2410C.pdf`
(Hi-Link V1.00; frame layout §2.3 p. 18-20, command section 2.2 p. 9-17). The
layout below is what the code implements; it is checked by synthetic vectors
and, for the engineering frame, by two real captures (confirmed on hardware
2026-10-02; the document's engineering example with intra-frame length 0x23 =
35 bytes matches the real frames).

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
  configuration is changed at start; changes are made only from `/ld2410`.
- **Settings commands** (all inside enable-config ... end-config, which is also
  sent after a failure): `0x0060` max gates and no-one duration (§2.2.3, p. 10),
  `0x0061` read parameters (§2.2.4, p. 11), `0x0064` gate sensitivity (§2.2.7,
  p. 12-13), `0x00A0` firmware version (§2.2.8, p. 13), `0x00A2` factory reset
  (§2.2.10, p. 14), `0x00A3` restart (§2.2.11, p. 15), `0x00A4` Bluetooth
  (§2.2.12, p. 16). After a restart or factory reset no end-config is sent (the
  module is rebooting). Not used: distance resolution 0.2 m (`0x00AA`/`0x00AB`,
  BACKLOG), baud rate, Bluetooth password.

## Host tests

```
make -C esp32/tests test
```

Plain C11 modules in `esp32/components/core/` (parser, frame decoder, command
codec and ACK decoders, LD2410C settings form parser, BOOT button zone tracker,
status LED colour logic, web console log ring (`log_ring`: offsets, gaps, ANSI strip, secret-line filter), UBX framer and NAV-TIMEUTC decoder (`ubx`), NMEA/UBX byte router (`gps_rx`), ring buffer, recording protocol, snapshot JSON, Wi-Fi
form and password helpers, WebSocket slot table), built with `-Wall -Wextra -Werror` and
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
- The real LD2410C frame rate.
- Engineering frame layout: confirmed on hardware 2026-10-02 (two real frames
  in the shared vectors) and against the Hi-Link document (§2.3 p. 18-20; the
  engineering example 0x23 = 35-byte payload matches the real frames). The
  command ACK sequence is documented there too (§2.2, p. 9-17), but the
  settings commands have not been run on a real module. Maximum payload length
  64 is still VERIFY; the other shared test vectors are synthetic.
- Settings cycle **confirmed on the owner's bench 2026-10-02** (module firmware
  V2.44.25070917): read version and parameters (factory values of Table 7
  read back exactly; the still sensitivity of gates 0/1 reads 0), a gate 3
  write 30 -> 60 read back and kept over a power cycle, Bluetooth off +
  restart (ACKs ok, engineering mode re-enabled automatically after about
  3 s), data pause about 30 ms for a one-gate write, engineering mode survives
  end-config. The firmware version shown (`%08x` of the little-endian minor,
  "25070917") reads as a build date, so the document's example "22091615" for
  bytes `16 15 09 22` is taken as a typo. Still open: a `0x0064` write for gate
  0 or 1 (still value sent unchanged) was not exercised.
- RGB LED, BOOT zones and AP on demand **confirmed on the owner's bench
  2026-10-02** (GPIO38, colours 2 s / 5 s / 10 s, flashes, steady blue in
  AP-only mode, AP on demand with the STA link up, switch-off 10 min after the
  last client, 5-10 s release erases the credentials). The WS2812 part number
  and its timing are still not in a document (they work as set in
  `main/rgb_led.c`).
- WebSocket close handling (slot removal, reconnect of the page).
- Recording server preemption (a new client replacing the old one) and the
  5 s send/receive timeouts; there is no TCP keepalive (BACKLOG).
- mDNS (`aeris-mvp.local`) on the home network.
- A 64-character hexadecimal Wi-Fi password (WPA2 treats it as a raw PSK; the
  63-character ASCII case is the normal one).
- Task stack sizes (LD2410C task 6144 B, recording tasks 4096 B, HTTP server
  8192 B, main task 6144 B, BOOT monitor 4096 B, status LED task 3072 B, GPS task 4096 B, IMU task
  5120 B) under real load.
- The AP password stays stable across reboots and a credential reset.
- GPS wiring and pins (GPIO4/5 UART2, GPIO8/9 I2C) on the real boards, and that
  these GPIOs are free of strapping/PSRAM functions on the DevKitC-1 v1.1.
- GY-NEO6MV2: 9600 Bd factory output (RMC and GGA present, `GN` or `GP`
  talker), no-fix behaviour (time and date reported with status `V`), cold
  start time at the owner's window, NEO-6M datasheet not in
  `hardware/datasheets/`.
- GPS time accuracy without PPS: measured on the bench 2026-10-02 (128 ms
  late against SNTP, see "Time sources and accuracy").
- UBX UTC verification **confirmed on the bench 2026-10-03** (balcony, module
  after a long power-off): the NEO-6M answers the NAV-TIMEUTC poll on UART1 in
  its default configuration (about one answer per second, `bad_ck 0`); fix at
  about 100 s after boot, `utc not valid` (validUTC 0) for about 4.5 min after
  the fix with no GPS `time_sync` and the time source staying SNTP, then
  `utc valid` and the time source GPS. This also confirms the cause of the
  whole-second error seen on 2026-10-02 (leap seconds not yet known). Still
  open: the `utc no ubx` fallback with the GPIO4 wire unplugged.
- SNTP: sync cadence (the ESP-IDF default interval is assumed; a 36 min
  recording showed one real sync and the 60 s re-emits), and its own accuracy
  on the home network.
- GY-BMI160: every register value, delay and sensitivity in "BMI160 registers
  used" (datasheet not in `hardware/datasheets/`); board pull-ups at 400 kHz;
  address 0x68 with SA0 to GND; I2C mode with CS to 3V3.
- Axis mapping and sign conventions (pitch nose up +, roll right side down +)
  on the real mounting; `TILT_MAP_*` defaults; filter weight 0.98 and a
  possible gyro offset drift (no bias calibration).
- Snapshot JSON size and live-page behaviour with GPS and IMU at 10 Hz on the
  real hardware.
- `/log` page: live log on a phone confirmed on the bench 2026-10-03 (no AP
  password line in it). Still open: the Copy button on iOS Safari over plain
  http (the clipboard API needs a secure context, so the `execCommand`
  fallback or Select all is expected to be used).
- Stack use of the log hook (256 B line buffer plus the `va_list` copy) in
  small-stack tasks (GPS 4096 B, status LED 3072 B, recording tasks 4096 B)
  under real load.
