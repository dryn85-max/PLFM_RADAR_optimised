# Spec: GPS UTC validity from UBX NAV-TIMEUTC (ESP32 MVP)

Date: 2026-10-03. Owner: Andrii. Status: approved in brainstorming (2026-10-03).

## Context

After a cold start the NEO-6M reports UTC off by whole seconds until it has decoded the
GPS-UTC leap-second count (bench 2026-10-02: +3 s for 5.6 min after the first fix). NMEA has no
flag for it. Today only the host conversion catches it, and only when SNTP syncs exist
(PR #8). Without Wi-Fi the error stays in the recording and on the live page.

Source: `hardware/datasheets/u-blox6_ReceiverDescrProtSpec_(GPS.G6-SW-10018)_Public.pdf`
(u-blox 6 Receiver Description including Protocol Specification, GPS.G6-SW-10018-F; page
numbers below are the document's "Page N of 210").

## Decisions (owner, brainstorming 2026-10-03)

1. Fix the cause in the firmware (option C): poll UBX **NAV-TIMEUTC** and use GPS time only while
   its **validUTC** flag is set. The module's configuration is not changed (poll only), so the
   "factory configuration" rule stays in spirit; the "no UBX" rule is relaxed to "no UBX
   configuration".
2. If the module does not answer the UBX poll (TX wire missing, UBX input disabled): keep today's
   behaviour (GPS time used unverified) and warn in the 10 s log line and on the live page.
3. The recording marks verified fixes: `gps_fix.flags` **bit 4 = UTC verified by UBX**
   (backward compatible: older recorders ignore the bit).
4. The GPIO4 (ESP32 TX) -> GY-NEO6MV2 RX wire is connected on the bench (owner, 2026-10-03).

## Requirements

### R1. UBX framing (core, C11, host-tested)
- Packet (§23, p. 84): `0xB5 0x62`, class u8, id u8, length u16 LE (payload only), payload,
  `CK_A CK_B`. Checksum (§26, p. 85-86): 8-bit Fletcher over class, id, length and payload.
- Incremental parser: any chunking, resync after garbage or a bad checksum, payload length
  bounded (frames longer than the buffer are skipped by length and counted), counters for good,
  bad-checksum and skipped frames.
- Poll encoder: NAV-TIMEUTC poll = empty payload (§27.2, p. 86):
  `B5 62 01 21 00 00 22 67` (checksum computed, also a test vector).
- NAV-TIMEUTC decoder (§35.13, p. 179-180): class 0x01, id 0x21, payload exactly 20 bytes:
  iTOW u4, tAcc u4, nano i4, year u2, month u1, day u1, hour u1, min u1, sec u1, valid x1;
  `valid` bit 0 validTOW, bit 1 validWKN, **bit 2 validUTC** (p. 180).

### R2. NMEA/UBX demultiplexing (core)
- The GPS UART carries NMEA text and UBX binary. A byte router feeds UBX framing when a
  `0xB5 0x62` start is seen **outside an NMEA line**, and the NMEA parser otherwise; bytes of a UBX
  frame never reach the NMEA parser (a `$` inside a UBX payload must not start an NMEA line).
  Existing NMEA behaviour and tests stay unchanged.

### R3. GPS task (esp32/main)
- Send the NAV-TIMEUTC poll once per second on UART2 TX (GPIO4); never block the reader (short
  write timeout).
- UTC state: `VALID` (latest NAV-TIMEUTC within 3 s has validUTC = 1), `NOT_VALID` (latest within
  3 s has validUTC = 0), `NO_UBX` (no NAV-TIMEUTC for more than 5 s since the GPS started talking).
- GPS `time_sync` records are written only in `VALID`, or in `NO_UBX` (fallback, decision 2).
  The GPS fix record keeps being written every epoch as today; its bit 4 is set only in `VALID`.
- The 10 s log line adds `utc valid|not valid|no ubx`; `no ubx` is a warning.

### R4. Recording and host
- `gps_fix.flags` bit 4 `GPS_FLAG_UTC_VERIFIED` (0x10) in `rec_payload.h` and
  `host/ld2410_rec.py`; a shared test vector with the bit set; the GPS CSV keeps the raw `flags`
  column (no new column); `info` reports the number of fixes with verified UTC.
- The host SNTP check of PR #8 stays as a second line of defence.

### R5. Live page
- Snapshot JSON `gps` gains `utc_state` ("valid", "not_valid", "no_ubx"). The GPS card shows the
  UTC with "(leap seconds not yet known)" in `not_valid` and "(not verified: no UBX answer)" in
  `no_ubx`. The time source shown follows the existing rule (it already depends on GPS
  `time_sync` records, which are now gated).

### R6. Docs
- `esp32/README.md` (GPS section, time sources, wiring note: the TX wire is now used),
  `docs/decisions.md` (decisions 1-4), BACKLOG: close the "GPS UTC wrong by whole seconds" item
  except the 128 ms correction question.

## Non-goals
No UBX configuration (CFG-*), no change of the NMEA output, no PPS, no 128 ms correction.

## Acceptance
CI green; host tests (C and Python) pass; owner bench check: after a cold start (module without
backup power, or a long power-off) the log shows `utc not valid` for some minutes, no GPS
`time_sync` meanwhile, then `utc valid` and GPS time; with the TX wire unplugged the log shows
`no ubx` and GPS time is used as before.
