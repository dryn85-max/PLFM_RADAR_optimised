# Spec: console echo and line editing (firmware)

Date: 2026-10-02. Owner: Andrii. Status: approved in brainstorming (2026-10-02).

## Context

First bench run (BACKLOG, "First bench run"): the USART2 command line has no echo
and no Backspace handling — a typo corrected with Backspace reaches the parser as
garbage (`status` was answered `ERR latched`). `cmd_feed()` (`firmware/Core/app/cmd.c`)
appends every byte except CR/LF to the line.

## Owner decisions

1. Echo is **on by default**; new command `echo on` / `echo off` switches it;
   `echo` alone reports `ECHO on` / `ECHO off`. `echo` is accepted while a fault is
   latched (like `status`). Echo state is not persisted (on after every reset).
2. Line editing: BS (0x08) and DEL (0x7F) delete the last character; all other
   control characters and ANSI escape sequences (e.g. arrows `ESC [ A`) are
   discarded and never enter the line.

## Requirements

- R1 Echo: when on, every byte accepted into the line is echoed as-is; CR or LF
  that ends a line echoes `\r\n` before the reply; a LF immediately following a CR
  is ignored entirely (no blank line, no echo) — CRLF terminals give one line.
- R2 Backspace: BS/DEL on a non-empty line removes the last char and, if echo is
  on, sends `\b \b`; on an empty line it does nothing (no echo). During an
  over-long line (overflow state) BS/DEL do nothing.
- R3 Discarded input: bytes < 0x20 other than CR, LF, BS, and bytes >= 0x80 are
  dropped (no echo). ESC (0x1B) starts a sequence that is dropped up to and
  including its final byte: `ESC [` … final byte 0x40–0x7E (CSI), `ESC O x` (SS3),
  otherwise `ESC` + one byte. A CR/LF inside a sequence aborts it and is handled
  normally.
- R4 Over-long line: characters beyond `CMD_MAX_LINE` are not echoed; behaviour of
  `ERR too_long` unchanged.
- R5 `echo` command: `echo` → `ECHO on|off`; `echo on|off` → `OK`; any other
  argument or extra tokens → `ERR args`; works while latched. Documented in
  `firmware/README.md` (command table) and `docs/decisions.md`.
- R6 No change to other commands, replies, line length, or `cmd_exec()` semantics
  for existing commands. Replies are still sent after the echo of the line end.
- R7 Echo output goes through `uart_write()` (same as replies); no printf.

## Acceptance

Host tests (TDD) in `firmware/tests/test_cmd.c` cover R1–R5 (echo on/off byte
streams, CRLF, BS/DEL incl. empty line and overflow, control chars, CSI/SS3/ESC+1
sequences, CR inside a sequence, latched `echo`); all existing tests pass
(`ADAR_COUNT=1` and `4`); `make`, `make DIAG=0`, `make ADAR_COUNT=4` build;
CI green; owner verifies in `screen`.
