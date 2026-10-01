# Review of `claude/eloquent-mendel-dqv36n` (RTL + G0B1 tracks) — 2026-10-01

Reviewed at branch head `5113534`, on macOS with Icarus Verilog 13.0 and
arm-none-eabi-gcc 14.3 (same environment as the `b46dd71` baseline, 27/27 PASS).
Nothing here was fixed by the reviewer; items are for the branch owner.

## G0B1 firmware — passes

- `make -C 9_Firmware/9_1_Microcontroller/g0b1 test`: all host tests green
  (both `ADAR_COUNT=1` and `ADAR_COUNT=4` builds).
- `make -C 9_Firmware/9_1_Microcontroller/g0b1`: ELF built, `text 21004 / data 116 / bss 1096`;
  size-check 21120 / 131072 flash, 6324 / 32768 RAM. Linker warns "LOAD segment with RWX
  permissions" (cosmetic; `-Wl,--no-warn-rwx-segments` or a `.ld` tweak silences it).
- Spot checks: no `printf` in `Core/app/*.c`; `I2C1_TIMING` used from one constant; PLL tables
  are clearly marked placeholders; ST-LINK VCP on PA2/PA3; LD4 (PA5) not shared with any bus.
- Open items are already in `BACKLOG.md`. Three further extras are in
  `docs/superpowers/notes/g0b1-extras-from-alt-plan.md` (same PR).

## RTL — one regression failure, one plan task missing

### 1. `tb/tb_mti_canceller.v` does not compile (regression 33/34)

`run_regression.sh` → `MTI Canceller (ground clutter)  COMPILE FAIL`:

```
tb/tb_mti_canceller.v:178: error: Unable to bind wire/reg/memory `dut_t12_active' in `tb_mti_canceller'
tb/tb_mti_canceller.v:193:      : A symbol with that name was declared here. Check for declaration after use.
```

Introduced by `3008f54` (Task 11, MTI history in inferable RAM). `dut_t12_active` is read in
the `always @(posedge clk)` block at line 178 and declared at line 193. Fix: move

```verilog
reg dut_t12_active;
initial dut_t12_active = 0;
```

above the first `always` that reads it (e.g. next to `reg lat_ok;` at line 170). The other 33
tests pass, including golden (a)–(d) and the synthesizable matched-filter path compiled without
`-DSIMULATION`.

### 2. Plan Task 12 not done

`docs/superpowers/plans/2026-10-01-rtl-vendor-neutral-port.md`, Task 12 (spec R6/R7 and the
Acceptance section) is absent from the branch:

- no `9_Firmware/9_2_FPGA/README.md` (signal chain with rates/widths, multiplier/RAM budget
  table, removed-modules list, board-specific hand-off);
- no `tb/golden/count_multipliers.py` (static ≤ 55 multipliers / ≤ 1 Mbit RAM gate);
- no vendor-primitive grep gate in `run_regression.sh` Phase 0
  (`grep -lE "DSP48E1|xpm_memory|IBUFDS|IDDR|ODDR|MMCME2|PLLE2|BUFG|BUFIO"` over `PROD_RTL`).

Run by hand, the grep gate over `PROD_RTL` is clean; hits remain only in
`radar_system_top_te0712_dev.v` / `radar_system_top_te0713_dev.v` (BUFG, `USE_DSP`), which are
outside `PROD_RTL` — allowed by the spec, but the README should say so.

`.github/workflows/ci-tests.yml` on the branch adds only the `mcu-g0b1` job; the
`fpga-regression` job will fail until item 1 is fixed.

### 3. Minor

- `9_Firmware/9_2_FPGA/fft_twiddle_1024.mem` is kept only for `tb/tb_range_fft_realdata.v`
  (`TWIDDLE_FILE("fft_twiddle_1024.mem")`). Either regenerate that test for 256 points or note
  the file as test-only.
- `fft_engine.v` `INTERNAL_W` default is 25 (spec says 24; commit `a5a524a` marks it
  owner-approved). Record the reason in the FPGA README so the deviation is not "fixed" later.

## Resolution (on `claude/eloquent-mendel-dqv36n`)

- 1 (MTI tb declaration after use): fixed in `bde7b29`; regression 38/38 (Icarus 12 locally).
- 2 (Task 12): done in `bde7b29` — `9_2_FPGA/README.md`, `tb/golden/count_multipliers.py`,
  vendor-primitive / resource / no-SIMULATION-lint gates in Phase 0 of `run_regression.sh`.
- 3 minor: `fft_twiddle_1024.mem` (test-only) and the `INTERNAL_W = 25` rationale are recorded in
  the FPGA README (`0eaa4eb`). The RWX linker warning appears only with binutils ≥ 2.39 and
  `--no-warn-rwx-segments` breaks older ld, so it is left as is.
- The three G0B1 extras from `g0b1-extras-from-alt-plan.md` were ported to the branch's pin layout
  in `8aba250`, `c59e6d6`, `d0a243c` (EN_PA moved PA9 → PC8).
