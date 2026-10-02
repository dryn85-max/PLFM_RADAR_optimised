# ESP32 LD2410C settings + AP on demand + RGB LED: Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: superpowers:subagent-driven-development. One task per
> subagent, sequential. Steps use `- [ ]`.

**Spec:** `docs/superpowers/specs/2026-10-02-esp32-settings-led.md` (read it first).

## Global constraints

- Branch `claude/eloquent-mendel-dqv36n` (fast-forwarded to `main` at 55ab7e8). Never push to
  `main`. CI runs on the draft PR opened by the coordinator; ESP-IDF is not available locally,
  so ESP-IDF code stays minimal and uses public v5.5 APIs only.
- AGENTS.md rules: failing test first, adversarial tests, line endings preserved, commit
  trailers from the session, delete any `uv.lock`, no secrets (the AP password is printed on the
  console only, never logged elsewhere or sent to a page), no third-party GPL code (write the
  LD2410C commands from the protocol description in the spec, not from ESPHome).
- Protocol facts not in `hardware/datasheets/` are marked VERIFY in code comments and README.
- Check set after every task: `make -C esp32/tests test`, `QT_QPA_PLATFORM=offscreen uv run pytest
  host -q`, `uv run ruff check .`, `bash tools/check_paths.sh`; CI (all six jobs green) for
  tasks touching `esp32/main`.

## Task 1: core state machines (button, LED priority)

- [ ] `components/core/boot_btn.[ch]`: `bb_zone_t bb_zone(uint32_t held_ms)` with the spec R2
  boundaries; a small tracker `bb_t` fed with `bb_step(&b, pressed, dt_ms)` returning the
  action on release (`BB_ACT_NONE`, `BB_ACT_AP`, `BB_ACT_RESET`) and exposing the current
  zone while held. A press that starts while the ESP32 boots with the button already down is
  ignored until the first release (GPIO0 low at reset is download mode; a still-held button
  after boot must not trigger anything).
- [ ] `components/core/status_led.[ch]` (logic only): inputs (zone while held, AP state
  {off, on-demand, ap-only}, pending flash colour/count, time ms) -> RGB colour at that time
  (priority per spec R2; 1 Hz 50 % blink; flash = 3 x (100 ms on, 100 ms off)).
- [ ] Tests: every boundary (1999/2000, 4999/5000, 9999/10000 ms), release in each zone,
  re-press after an action, held-at-boot, uint32 time wrap in the LED timing, priority
  combinations.

## Task 2: LED driver, BOOT monitor, AP on demand (esp32/main)

- [ ] `main/rgb_led.[ch]`: RMT TX (`esp_driver_rmt`) bytes encoder, WS2812 timing, GRB, GPIO38
  constant (confirmed, see spec R1; WS2812 timing VERIFY), brightness constant ~5 %; `rgb_led_set(r,g,b)`.
- [ ] `main/status_led_task.c` (or inside `rgb_led.c`): 20 ms tick, computes the colour with
  core `status_led`, writes only on change. Setters: button zone, AP state, flash.
- [ ] `wifi_mgr.c`: BOOT monitor uses core `boot_btn` (action on release); `wifi_mgr_ap_on_demand()`;
  AP client counting; 10 min idle timer (`esp_timer`) back to `WIFI_MODE_STA` only for the
  on-demand AP; publishes the AP state to the LED. Keep the scan/connect locking rules.
- [ ] CMake `REQUIRES esp_driver_rmt`. CI green.

## Task 3: LD2410C commands and form parsing (core)

- [ ] `ld2410_cmd.[ch]`: encoders and ACK decoders per spec R4, bounds-checked.
- [ ] `components/core/ld_settings.[ch]`: settings struct, range check, form parsing
  (reuse the URL decoding of `wifi_form`; field names fixed in the header), diff of two
  settings (which commands to send), firmware-version text formatting.
- [ ] Tests: byte-exact frames for every encoder, ACK decoding incl. truncated/oversized/
  wrong-header, form parsing with missing/duplicate/out-of-range/overlong fields, diff.

## Task 4: command execution and `/ld2410` page (esp32/main)

- [ ] `ld2410_task.[ch]`: request queue (read, write diff, bluetooth off, restart, factory reset),
  executed by the task between frames with enable-config/end-config, 5 s caller timeout,
  console log old -> new.
- [ ] `http_srv.c`: `/ld2410` GET/POST (AP only), same style and chunked sending as `/wifi`,
  links between the two pages. CI green.

## Task 5: docs

- [ ] `esp32/README.md`, `docs/decisions.md` (spec decisions 1–10), `docs/architecture.md`,
  `BACKLOG.md` per spec R7. Path gate.

## Final review

Opus, whole branch. High-risk: BOOT action on release and the held-at-boot case (no accidental
credential erase); Wi-Fi mode switching APSTA <-> STA under the scan/connect locks and with
connected clients; UART ownership and the request queue (no blocking of the frame path beyond
the configuration window, timeouts, end-config always sent); LD2410C command/ACK byte layouts and
bounds (VERIFY items listed); RMT/LED init failure isolation; AP-only enforcement of `/ld2410`.
