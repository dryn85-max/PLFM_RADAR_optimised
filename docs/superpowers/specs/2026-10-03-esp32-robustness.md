# Spec: ESP32 MVP robustness bundle (lock file, WebSocket stall, AP retries, keepalive, GPS delay)

Date: 2026-10-03. Owner: Andrii. Status: approved in brainstorming (2026-10-03).

## Context

Five small BACKLOG items of the ESP32 MVP track (`BACKLOG.md`, "ESP32 MVP track"), done as one
cycle. The tilt zero button was discussed and **postponed** (owner, 2026-10-03): the recording
already carries the raw accelerations, so a mounting offset can be removed on the host; the
item stays in BACKLOG until the rotating radar.

## Decisions (owner, brainstorming 2026-10-03, "все a")

1. `esp32/dependencies.lock` is taken from the CI build (ESP-IDF v5.5.5) and committed; the
   `.gitignore` line goes.
2. A stalled live-page WebSocket client: the send timeout of WebSocket sockets drops from 5 s
   to 1 s; a client whose frame send fails is closed (already today); the page reconnects by
   itself (already today).
3. STA reconnect attempts pause while at least one client is connected to the ESP32 AP, and
   resume when the last client leaves.
4. TCP keepalive on the recording port 5410: idle 5 s, interval 2 s, 3 probes (a vanished
   client is noticed after about 11 s).
5. The GPS `time_sync` record is corrected by the measured 128 ms RMC reception delay.

## Requirements

### R1. dependencies.lock
- Get the lock file produced by `idf.py build` in CI (`espressif/idf:v5.5.5`, target esp32s3),
  commit it unchanged as `esp32/dependencies.lock`, remove `esp32/dependencies.lock` from
  `.gitignore`. Any temporary CI step used to obtain it is removed again in the same branch.
- CI must stay green with the committed file (the build must not rewrite it; check
  `git status --porcelain esp32/dependencies.lock` is empty after the CI build, as a CI step).
- README: the lock is committed; a local build with another ESP-IDF version may rewrite it;
  do not commit such a rewrite.

### R2. WebSocket send timeout
- Only live-page WebSocket sockets get `SO_SNDTIMEO` = 1 s (one constant in `live.c`), set when
  the client is registered (post-handshake). Other HTTP responses (pages, `/update`, `/log`)
  keep the httpd default (5 s).
- The existing failure path stays: send error -> warning log -> `httpd_sess_trigger_close`.
- A failing `setsockopt` is logged and the client is kept (5 s timeout then).

### R3. STA retries paused while an AP client is connected
- Applies whenever an AP runs (fallback or on demand) and STA is wanted.
- While the AP client count is > 0: no new `esp_wifi_connect()` from the retry path or the
  disconnect handler; a connect already in flight is left to finish. Pausing and resuming are
  logged once each (`STA retries paused (AP client connected)` / `... resumed`).
- Owner decision 2026-10-03 after the final review (M1): even while paused, one STA attempt is
  made every 10 min (`STA_PAUSE_FORCE_MS`), so a device that stays on the AP forever cannot keep
  the board off the home network; the pause itself continues and the attempt is logged
  (`STA retry while paused (every 10 min)`).
- When the count drops to 0 and a retry was suppressed, a retry is scheduled after the minimum
  backoff (2 s); the backoff state is otherwise unchanged.
- `STA_START` with an AP client already connected cannot happen at boot (no AP yet); no
  special handling.
- Scans from `/wifi` are user-initiated and stay allowed.
- Saving credentials on `/wifi` restarts the board (unchanged), so the new credentials are
  tried at once.

### R4. TCP keepalive on port 5410
- On every accepted recording socket: `SO_KEEPALIVE` 1, `TCP_KEEPIDLE` 5 s, `TCP_KEEPINTVL`
  2 s, `TCP_KEEPCNT` 3 (constants in `rec_srv.c`). A failing `setsockopt` is logged once per
  connection; the connection is still served.

### R5. GPS time_sync delay correction
- Re-derived sign: the ESP time stamp of a GPS `time_sync` is taken when the RMC line
  completes, `d` = 128 ms after the UTC instant it names (bench 2026-10-02, against SNTP: GPS
  late by 128 ms, p1..p99 119..136 ms). The host maps `utc(t) = utc_sync + (t - t_sync)`, so
  the record must carry `utc_unix_us = utc_rmc + d` for the same stamp `t_sync`.
- Constant `GPS_RMC_DELAY_US 128000` in `gps_task.c` with this derivation in a comment; applied
  only to `time_sync` records. `gps_fix` records and the live page keep the raw RMC time.
- The host SNTP cross-check (PR #8, 1 s limit) is unchanged.

### R6. Docs
- `esp32/README.md` (lock file, WebSocket timeout, AP retry pause, keepalive, GPS delay
  correction and its VERIFY item: re-measure the residual against SNTP on the bench),
  `docs/decisions.md` (decisions 1-5 and the tilt postponement), `BACKLOG.md`: close the four
  ESP32 items done here (lock, WebSocket stall, STA retries vs fallback AP, TCP keepalive) and
  the 128 ms question; the tilt item gets "postponed until the rotating radar (owner
  2026-10-03)".

## Non-goals
No tilt zero, no per-client send task, no change of the recording protocol, no PPS.

## Acceptance
CI green (the lock file check included); host tests pass; owner bench check: a phone left on
the live page and put to sleep no longer slows other page loads for more than about 1 s; a
phone connected to the fallback AP keeps its link while fixing credentials; killing the
recorder's network (Wi-Fi off on the Mac) frees the recording port within about 15 s
(`rec_srv` log line `client closed` / `send failed`); the GPS-vs-SNTP difference in a recording is near 0 instead of +128 ms.
