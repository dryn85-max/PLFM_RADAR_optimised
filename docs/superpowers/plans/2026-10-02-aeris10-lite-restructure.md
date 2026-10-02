# AERIS-10 Lite Restructure Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: superpowers:subagent-driven-development. One task per
> subagent, sequential (every task moves files the next one depends on). Steps use `- [ ]`.

**Spec:** `docs/superpowers/specs/2026-10-02-aeris10-lite-restructure.md` (read it first).

## Global constraints

- Branch `claude/eloquent-mendel-dqv36n` (restarted from `main` at 623086b). Never push to `main`.
- Moves only with `git mv`; never delete content. `legacy/` keeps original relative paths:
  `X` → `legacy/X`.
- After every task the full check set passes (run from repo root; restore `fpga/tb/cosim/*.csv`
  after the regression; delete any `uv.lock`):
  - `bash fpga/run_regression.sh` (cd into the dir as the script expects) → 38/38
  - `make -C firmware test && make -C firmware && make -C firmware DIAG=0 clean all && make -C firmware ADAR_COUNT=4 clean all`
  - legacy F7 tests: `make -C legacy/9_Firmware/9_1_Microcontroller/tests clean all` (path changes in Task 3)
  - `uv run ruff check .`, py_compile over `*.py` (skip `.git __pycache__ .venv venv docs`)
  - `QT_QPA_PLATFORM=offscreen uv run pytest host/test_v7.py` (path changes in Task 4)
  - cross-layer: `uv run pytest tests/cross_layer/test_cross_layer_contract.py` (from Task 5)
  Before a task changes a path, the checks use the paths valid at that point.
- AGENTS.md rules apply (line endings preserved — `git mv` does not touch them; commit trailers
  from the session).
- Historical files under `docs/superpowers/specs|plans|notes` are not rewritten.

## Task 1: `legacy/` for upstream-only top-level material, `hardware/datasheets/`

- [ ] `git mv` into `legacy/` (same relative path): `1_Project_Description/`,
  `2_Functional Diagram & Interconnection Matrices/`, `3_Power Management/`,
  `4_Schematics and Boards Layout/`, `5_Simulations/`, `6_Application Notes/`, `8_Utils/`.
- [ ] Datasheets: `git mv` these from `7_Components Datasheets and Application notes/` to
  `hardware/datasheets/`: `ADAR1000.pdf`, `adtr1107.pdf`, `ads7830.pdf`, `tmp35_36_37.pdf`,
  `DS_FT2232H.pdf`. Everything else in that folder → `legacy/7_Components Datasheets and Application notes/`.
  Then fix references to the moved datasheets in code comments/READMEs under `9_Firmware/…/g0b1`
  and `AGENTS.md` (path string only).
- [ ] Docs site: `git mv` `docs/*.html`, `docs/*.pdf`, `docs/.nojekyll`, `docs/assets/`, `docs/artifacts/`
  → `legacy/docs-site/` (keep their internal relative layout). `docs/superpowers/` stays.
- [ ] `legacy/README.md`: what upstream is (link https://github.com/NawfalMotii79/PLFM_RADAR,
  baseline commit `b46dd71`), table: each moved item → one-line reason it is not used by
  AERIS-10 Lite; note that `legacy/` is reference only, not built (except the F7 unit tests kept
  in CI per the G0B1 spec, added in Task 3).
- [ ] Checks; commit `restructure: move upstream-only material to legacy/, own datasheets to hardware/`.

## Task 2: `fpga/`

- [ ] `git mv 9_Firmware/9_2_FPGA fpga`. Then move Xilinx-only items back out to legacy with their
  original path: `fpga/scripts/` → `legacy/9_Firmware/9_2_FPGA/scripts/`,
  `fpga/constraints/` → `legacy/9_Firmware/9_2_FPGA/constraints/`,
  `fpga/radar_system_top_te07*_dev.v` → `legacy/9_Firmware/9_2_FPGA/`. Check nothing in `PROD_RTL`,
  testbenches or `run_regression.sh` uses them (grep); `formal/` stays in `fpga/`.
- [ ] Fix every reference to `9_Firmware/9_2_FPGA` / relative climbs (`../../`) in: CI workflow,
  `fpga/README.md`, `fpga/tb/**/*.py|*.v|*.sh`, `9_Firmware/9_3_GUI/v7/software_fpga.py`
  (`parents[2]/9_2_FPGA/tb/cosim/real_data` → repo-root `fpga/tb/cosim/real_data`, computed so it
  stays correct after Task 4 moves the GUI to `host/v7/`), cross-layer `contract_parser.py`
  `FPGA_DIR`, `pyproject.toml`, `AGENTS.md`, `BACKLOG.md`, `9_Firmware/9_1_Microcontroller/g0b1/README.md`.
  Also update `fpga/README.md` limitation (d) "stale board files" to point at `legacy/`.
- [ ] Checks (regression from `fpga/`, cross-layer still at old path); commit.

## Task 3: `firmware/` and legacy F7

- [ ] `git mv 9_Firmware/9_1_Microcontroller/g0b1 firmware`; `git mv` the F7 tree
  (`9_1_1_C_Cpp_Libraries`, `9_1_2_C_Cpp_Algorithms`, `9_1_3_C_Cpp_Code`, `tests`) to
  `legacy/9_Firmware/9_1_Microcontroller/` (relative `../9_1_1…` includes in the F7 tests keep
  working because the siblings move together — verify).
- [ ] Fix references: CI (`mcu-g0b1` working-directory → `firmware`; `mcu-tests` →
  `legacy/9_Firmware/9_1_Microcontroller/tests`, job renamed "Legacy F7 MCU tests"),
  `firmware/README.md`, `firmware/tools/fetch_cube.sh` (repo-relative paths), `firmware/tests/*`
  (any `../../` to repo files such as the FPGA top for DIG checks), cross-layer `MCU_DIR`
  (temporarily keep pointing at the legacy F7 tree until Task 5), `AGENTS.md`, `BACKLOG.md`.
- [ ] Checks; commit.

## Task 4: `host/` and `tools/`

- [ ] `git mv` into `host/`: `9_Firmware/9_3_GUI/v7/`, `GUI_V7_PyQt.py`, `radar_protocol.py`,
  `test_v7.py`, `requirements_v7.txt`, `smoke_test.py` (verify it uses only these), and any file
  `test_v7.py`/`v7` import (check with grep; e.g. `test_radar_data.csv` if used). Everything else
  in `9_3_GUI` (Tk GUI V65 + its test, V5/V6/PyQt_Map, other requirements, `adi_agc_analysis.py`
  unless host uses it) → `legacy/9_Firmware/9_3_GUI/`.
- [ ] `git mv 9_Firmware/tools tools` (merge with `tools/` if it exists).
- [ ] Fix: CI python job (pytest `host/test_v7.py` only; the Tk test moves to legacy and is not
  run — say so in `legacy/README.md`), `pyproject.toml` per-file-ignores (`host/v7/hardware.py`),
  `host/v7/software_fpga.py` path (already repo-root based from Task 2 — re-verify),
  cross-layer `GUI_DIR`, docs references. Add `host/README.md` (how to install `requirements_v7.txt`
  and run `GUI_V7_PyQt.py`, protocol compatibility note, BACKLOG pointer for 4-channel adaptation).
- [ ] Checks; commit.

## Task 5: cross-layer tests retargeted (spec R3)

- [ ] `git mv 9_Firmware/tests/cross_layer tests/cross_layer`; remove the now-empty `9_Firmware/`
  (must be empty; if anything is left, stop and report).
- [ ] Re-point `contract_parser.py` and `test_cross_layer_contract.py`: FPGA → `fpga/`,
  GUI → `host/`, MCU → `firmware/`. For every MCU-side contract that parsed F7 `main.cpp`,
  `main.h`, `ADAR1000_Manager.cpp`: rewrite against the G0B1 equivalents
  (`Core/hal/pins.h` + `pins_table.c` for DIG0–7 pins/directions; `Core/app/fpga_if.c`/`agc.c`
  for DIG6 read + 2-frame debounce + AGC enable; `Core/drivers/adar1000.c` VM_I/VM_Q tables for
  `adar1000_vm_reference.py`). Keep FPGA↔GUI contracts unchanged. Any check with no G0B1
  equivalent: delete it with a comment in the test file and a line in the commit message saying
  why. Mutation-check at least the DIG6 debounce and the VM-table checks (break the firmware in a
  temp copy, test must fail).
- [ ] CI cross-layer job path. Checks; commit.

## Task 6: documentation

- [ ] `git mv BOM_OPTIMIZATION_REPORT.md docs/bom-optimization.md`.
- [ ] New root `README.md` (spec R4), rewritten `CONTRIBUTING.md`, `AGENTS.md` and `BACKLOG.md`
  path updates (+ BACKLOG: "adapt host GUI to 4-channel prototype / G0B1 STATUS line").
- [ ] `docs/architecture.md`, `docs/bring-up.md`, `docs/decisions.md` (spec R5). Sources:
  `fpga/README.md`, `firmware/README.md`, `BACKLOG.md`, `docs/bom-optimization.md`, git log of
  PR #2 commit messages, `docs/superpowers/specs/*`. Decisions to include (with dates): FPGA
  INTERNAL_W=25; receive-window buffer; 4×64 kept; every chirp processed; G0B1 Q1–Q3 (`auto`,
  el ±60°, gain direct write); safe PA/LNA bias; watchdog/NMI latch; EN_PA PA9→PC8; docs in
  git; this restructure (legacy/, semantic names, v7 GUI, Markdown, AERIS-10 Lite, English).
- [ ] Checks; commit.

## Task 7: path gate and final checks

- [ ] `tools/check_paths.sh` (spec R6): `git ls-files` minus `legacy/` and `docs/superpowers/`,
  grep for the old roots; exit 1 with the offending lines. Add as a step to the CI python job.
  Fix any remaining hits.
- [ ] Run the full check set; commit.

## Final review

Opus, whole branch. High-risk: CI path correctness (every job), cross-layer retargeting (no
contract silently lost), `software_fpga.py` golden path, legacy F7 tests still running, README /
docs factual accuracy vs code.
