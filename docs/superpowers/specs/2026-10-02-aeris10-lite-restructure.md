# Spec: Restructure the repository as AERIS-10 Lite

Date: 2026-10-02. Owner: Andrii. Status: approved in brainstorming (2026-10-02).

## Context

The fork still has the upstream AERIS-10 layout (`1_Project_Description` … `9_Firmware`, an HTML
docs site, upstream README/CONTRIBUTING). The project itself has diverged: 4-channel prototype
(1× ADAR1000 + 4× ADTR1107), vendor-neutral RTL for a Cyclone board with a 12-bit CMOS ADC,
STM32G0B1 firmware, unchanged host USB protocol. Most upstream material (F7 firmware, Xilinx flows,
16-channel boards, GaN/waveguide simulations, docs site) does not apply.

## Decisions (owner, brainstorming 2026-10-02)

1. Upstream-only material moves to `legacy/`, keeping its **original relative paths**
   (`legacy/<old path>`), with `legacy/README.md` explaining what it is and why it is unused.
   Nothing is deleted.
2. Our code gets semantic top-level names: `fpga/`, `firmware/`, `host/`, `hardware/`, `docs/`,
   `tools/`, `tests/`, `legacy/`.
3. Host: the PyQt v7 GUI (`v7/`, `GUI_V7_PyQt.py`, `radar_protocol.py`, `test_v7.py`,
   `requirements_v7.txt`) moves to `host/`; the old Tk GUI and older GUI versions go to `legacy/`.
   Adapting the GUI to the 4-channel prototype / G0B1 STATUS line is a BACKLOG item, not part of
   this work.
4. Documentation is Markdown in `docs/`; the upstream HTML site and PDFs go to `legacy/`.
5. Project name: **AERIS-10 Lite**. Upstream attribution and licences (MIT software,
   CERN-OHL-P hardware, `Licence` file) are kept.
6. Language of README and `docs/`: English (`docs/bom-optimization.md` stays Russian as a
   historical document).

## Target layout

```
README.md  AGENTS.md  CLAUDE.md  BACKLOG.md  CONTRIBUTING.md  Licence  pyproject.toml
fpga/                 <- 9_Firmware/9_2_FPGA (minus Xilinx-only scripts/constraints/te07* tops)
firmware/             <- 9_Firmware/9_1_Microcontroller/g0b1
host/                 <- v7 GUI, radar_protocol.py, test_v7.py, requirements_v7.txt (+ smoke_test.py)
tests/cross_layer/    <- 9_Firmware/tests/cross_layer, retargeted to fpga/ firmware/ host/
tools/                <- 9_Firmware/tools
hardware/datasheets/  <- datasheets of parts we use (ADAR1000, ADTR1107, ADS7830, TMP35/36/37, FT2232H)
docs/                 architecture.md, bring-up.md, decisions.md, bom-optimization.md, superpowers/
legacy/               README.md + everything upstream-only under its original path
```

## Requirements

- R1. All moves with `git mv` (history preserved). No file content is lost.
- R2. Everything keeps working at the new paths: `fpga/run_regression.sh` (38/38), `firmware`
  `make test` / `make` / `make DIAG=0` / `make ADAR_COUNT=4`, host `pytest host/test_v7.py`,
  legacy F7 tests (`legacy/9_Firmware/9_1_Microcontroller/tests`, still in CI — required by the
  G0B1 spec), `uv run ruff check .`, py_compile over the tree, CI workflow.
- R3. Cross-layer contract tests are retargeted from the F7 sources to the active code:
  MCU side = `firmware/` (DIG0–7 roles/directions from `Core/hal/pins.h`/`pins_table.c`,
  DIG6 2-frame debounce in `Core/app/fpga_if.c`/`agc.c`, ADAR1000 VM tables in
  `Core/drivers/adar1000.c`), FPGA side = `fpga/`, GUI side = `host/`. Every contract the old
  tests checked is either re-pointed at the active equivalent or explicitly dropped with a reason
  (e.g. F7-only constructs). No test is silently deleted.
- R4. Root `README.md` describes AERIS-10 Lite (what it is, block diagram in text, status — nothing
  tested on hardware, quick start per component, repo map, upstream attribution, licences);
  `CONTRIBUTING.md` is rewritten for this fork (branches from `main`, `sfw`, make/regression
  commands, CI); `AGENTS.md`, `BACKLOG.md`, module READMEs and spec/plan cross-references use the
  new paths (historical specs/plans under `docs/superpowers/` are NOT rewritten).
- R5. `docs/architecture.md` (system block diagram, interfaces FPGA↔MCU DIG0–7, USB, SPI/I²C,
  clocks, rates/widths summary, resource budgets with links to module READMEs),
  `docs/bring-up.md` (hardware requirements + bring-up checklist from BACKLOG/READMEs),
  `docs/decisions.md` (every owner decision so far with date and reason),
  `docs/bom-optimization.md` (moved `BOM_OPTIMIZATION_REPORT.md`). Facts only from the repo;
  unverified items marked VERIFY.
- R6. A path gate (`tools/check_paths.sh`, run in CI) fails if any tracked file outside `legacy/`
  and `docs/superpowers/` references the old roots (`9_Firmware`, `9_1_Microcontroller`,
  `9_2_FPGA`, `9_3_GUI`, `7_Components`, `4_Schematics`, `BOM_OPTIMIZATION_REPORT`).

## Non-goals

- No RTL, firmware or protocol behaviour change; no GUI feature work.
- No new hardware design files.

## Acceptance

CI green on the PR; path gate passes; all R2 commands pass locally; `legacy/README.md` lists every
moved top-level item with a one-line reason.
