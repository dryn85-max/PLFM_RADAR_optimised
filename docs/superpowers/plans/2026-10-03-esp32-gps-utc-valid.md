# ESP32 GPS UTC validity (UBX NAV-TIMEUTC): Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: superpowers:subagent-driven-development. One task per
> subagent, sequential (never two agents in one working tree). Steps use `- [ ]`.

**Spec:** `docs/superpowers/specs/2026-10-03-esp32-gps-utc-valid.md` (read it first).

## Global constraints

- Branch `claude/eloquent-mendel-dqv36n` (at `main`). Never push to `main`. CI runs on a draft PR
  opened by the coordinator; ESP-IDF is not available locally.
- AGENTS.md rules: failing test first, adversarial tests, line endings preserved, commit trailers
  from the session, delete any `uv.lock`, no third-party GPL code (write the UBX parser from the
  u-blox document, cite section/page).
- The recording payload layouts are shared by `esp32/components/core/rec_payload.[ch]`,
  `host/ld2410_rec.py` and `esp32/tests/vectors/`: change all three together.
- Check set after every task: `make -C esp32/tests test`, `QT_QPA_PLATFORM=offscreen uv run pytest
  host -q`, `uv run ruff check .`, `bash tools/check_paths.sh`; CI for tasks touching `esp32/main`.

## Task 1: UBX core + NMEA/UBX router (spec R1, R2)
- [ ] `components/core/ubx.[ch]`: incremental frame parser, Fletcher checksum, poll encoder,
  NAV-TIMEUTC decoder.
- [ ] `components/core/gps_rx.[ch]` (or inside ubx): byte router between UBX framing and
  `nmea_feed`, using the NMEA parser state (only outside an NMEA line).
- [ ] Tests: the poll bytes `B5 62 01 21 00 00 22 67`; a synthetic NAV-TIMEUTC frame (valid 0x07 and
  0x03) built with the checksum; split at every byte; garbage, bad checksum, oversized length,
  `$` and `0xB5` inside a UBX payload, UBX frame between two NMEA lines, NMEA results unchanged.

## Task 2: recording flag, host and snapshot (spec R4, R5 JSON)
- [ ] `GPS_FLAG_UTC_VERIFIED` 0x10 in `rec_payload.h`; vector `gps_fix_utc_verified` via
  `gen_vectors.py`; C and Python vector tests; host constant and `info` count; snapshot JSON
  `gps.utc_state` + tests.

## Task 3: GPS task integration and live page (spec R3, R5)
- [ ] `main/gps_task.c`: router, 1 s poll on UART2 TX, UTC state machine with the 3 s / 5 s
  constants, time_sync gating, flag bit 4, 10 s log, snapshot field; `web_page.html` GPS card text.

## Task 4: docs (spec R6)

## Final review
Opus, whole branch. High-risk: UBX/NMEA demultiplexing (no lost or corrupted NMEA lines, bounds),
the UTC state machine and the fallback (no GPS time while not valid; correct fallback timing),
UART TX from the GPS task (non-blocking), recording flag compatibility (C, Python, vectors).
