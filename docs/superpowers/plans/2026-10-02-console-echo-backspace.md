# Console echo and line editing Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: superpowers:subagent-driven-development. Single task.

**Spec:** `docs/superpowers/specs/2026-10-02-console-echo-backspace.md` (read it first).

## Task 1: echo + line editing in `firmware/Core/app/cmd.c`

**Files:** `firmware/Core/app/cmd.c`, `cmd.h` (if a getter for tests is needed),
`firmware/tests/test_cmd.c` (and `tests/mocks/mock_uart.*` only if the UART TX
capture needs a helper), `firmware/README.md` (command table + a line on echo/editing),
`docs/decisions.md` (owner decisions 1–2 with date 2026-10-02), `BACKLOG.md`
(remove/close the "no echo and no Backspace handling" item).

- [ ] Tests first (`test_cmd.c`), driving `cmd_feed()` byte by byte and capturing
  UART TX via the mock:
  - echo on (default after `cmd_init`): `"st"` → TX `"st"`; `"atus\r"` → TX
    `"atus\r\n"` then the `STATUS …` reply.
  - CRLF: `"status\r\n"` → exactly one reply, no extra `"\r\n"` echo for the LF.
  - LF alone ends a line (echo `"\r\n"`).
  - BS and DEL: `"stx\x7f" "atus\r"` → executes `status`; TX contains `"\b \b"`;
    BS on empty line → no TX; BS in overflow → no TX, line still `ERR too_long`.
  - control chars (e.g. 0x01, 0x07, 0x09) and bytes ≥ 0x80 dropped (no TX, not in line).
  - escape sequences dropped: `"\x1b[A"`, `"\x1b[1;5C"`, `"\x1bOP"`, `"\x1bx"`; a
    `\r` inside `"\x1b[1"` aborts the sequence and ends the line.
  - over-long line: chars after `CMD_MAX_LINE` not echoed.
  - `echo off` → `OK`, then typing produces no echo, `\r` produces no `"\r\n"` echo
    (reply only); `echo on` restores; `echo` → `ECHO on|off`; `echo maybe`, `echo on x`
    → `ERR args`; latched (`stop` first) → `echo off` still `OK`, `status` works,
    others `ERR latched`.
  - `cmd_init()` resets echo to on.
- [ ] Implement in `cmd.c` (`s_echo`, `s_last_cr`, small ESC state machine), add
  `echo` to `cmd_exec` before the latched check (next to `status`).
- [ ] Gates: `make -C firmware test`, `make -C firmware`, `make -C firmware DIAG=0 clean all`,
  `make -C firmware ADAR_COUNT=4 clean all`, `bash tools/check_paths.sh`,
  `uv run ruff check .`; update memory figures in `firmware/README.md` and
  `docs/architecture.md` if size-check numbers change.
- [ ] Commit.

## Final review

Opus, branch-level. High-risk: no change to existing command semantics/replies
(host GUI/tests rely on reply lines), latched-state behaviour, buffer bounds in the
line editor (BS at 0, overflow), escape-state machine cannot swallow a line end.
