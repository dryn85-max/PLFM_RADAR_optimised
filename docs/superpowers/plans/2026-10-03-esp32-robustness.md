# ESP32 robustness bundle: Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: superpowers:subagent-driven-development. One task per
> subagent, sequential (never two agents in one working tree). Steps use `- [ ]`.

**Spec:** `docs/superpowers/specs/2026-10-03-esp32-robustness.md` (read it first).

## Global constraints

- Branch `claude/eloquent-mendel-dqv36n` (at `main` c66a8b4). Never push to `main`. CI runs on a
  draft PR opened by the coordinator; ESP-IDF is not available locally, so ESP-IDF code uses
  public v5.5 APIs only and is checked by CI.
- AGENTS.md rules: line endings preserved, commit trailers from the session, delete any
  `uv.lock`, no secrets.
- Check set after every task: `make -C esp32/tests test`, `QT_QPA_PLATFORM=offscreen uv run pytest
  host -q`, `uv run ruff check .`, `bash tools/check_paths.sh`.

## Task 1: sockets (spec R2, R4)
- [ ] `live.c`: `LIVE_WS_SEND_TIMEOUT_S 1`, `SO_SNDTIMEO` on the fd when a client is added.
- [ ] `rec_srv.c`: keepalive options on the accepted socket (include `lwip/sockets.h` /
  `netinet/tcp.h` as needed for `TCP_KEEP*`).

## Task 2: STA retry pause (spec R3)
- [ ] `wifi_mgr.c`: pause/resume in `retry_cb`, the `STA_DISCONNECTED` branch and the
  `AP_STADISCONNECTED` branch; a `s_retry_suppressed` flag; log lines; header comment updated.

## Task 3: GPS delay (spec R5)
- [ ] `gps_task.c`: `GPS_RMC_DELAY_US`, added to `utc_unix_us` of `time_sync` only.

## Task 4: lock file (spec R1) — coordinator
- [ ] Temporary CI step printing the lock; copy from the job log; commit; remove the step and the
  `.gitignore` line; add the porcelain check step.

## Task 5: docs (spec R6)

## Final review
Opus, whole branch. High-risk: Wi-Fi retry pause (event-handler vs esp_timer context, a
suppressed retry never lost, no reconnect storm when the last client leaves, interaction with
the on-demand AP idle timer and the scan lock); the sign of the GPS delay correction against
the host time mapping; socket option side effects (WebSocket timeout only on WS fds,
keepalive constants and headers).
