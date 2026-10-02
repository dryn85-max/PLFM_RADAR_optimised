# Contributing to AERIS-10 Lite

AERIS-10 Lite is a fork of [NawfalMotii79/PLFM_RADAR](https://github.com/NawfalMotii79/PLFM_RADAR).
This guide covers what you need to get a change reviewed and merged here. For
what the project is, see [README.md](README.md); AI coding agents should also
read [AGENTS.md](AGENTS.md).

## Branches and pull requests

- `main` is the only long-lived branch of this fork. (Upstream also has a
  `develop` branch; it does not exist here.)
- Create topic branches from `main`. Never push to `main` directly.
- Changes reach `main` only through a pull request.
- CI (`.github/workflows/ci-tests.yml`) must be green before merging.
- Commit messages are descriptive. Keep generated outputs (`*.vvp`, `*.vcd`,
  Vivado/Quartus projects, bitstreams, build logs, `uv.lock`) out of version
  control; see `.gitignore`.

## Security mandate: package installation

Due to supply chain attack risks, **ALL package installations MUST use the
`sfw` (secure firewall) prefix**.

- Python: `sfw uv pip install <package>` (do not use raw pip)
- Node/JS: `sfw npm install <package>`
- Rust/Cargo: `sfw cargo <command>`

Never run bare package installation commands without the `sfw` prefix.

## Repository layout

| Path | Contents |
|------|----------|
| `fpga/` | Vendor-neutral Verilog RTL, testbenches, golden vectors, `run_regression.sh` |
| `firmware/` | STM32G0B1 (NUCLEO-G0B1RE) firmware and host unit tests |
| `host/` | V7 PyQt6 GUI, protocol layer, smoke test |
| `tests/cross_layer/` | Python system invariant / contract tests |
| `tools/` | Helper scripts |
| `hardware/datasheets/` | Datasheets of the parts used |
| `docs/` | Architecture, bring-up, decisions, BOM analysis, specs and plans |
| `legacy/` | Upstream-only material, reference only (see `legacy/README.md`) |

## Code standards and tooling

- **Python (host, scripts, tests):** `uv` for dependency management, `ruff` for
  linting (`uv run ruff check .` must be clean), `pytest` for tests.
- **Verilog (FPGA):** Verilog-2001 for Icarus Verilog (`iverilog`); no
  SystemVerilog, no `$clog2`, no vendor primitives or attributes (the regression
  has a grep gate). The RTL (`radar_system_top.v`) is the single source of truth
  for opcode values, bit widths, reset defaults and valid ranges; all other
  layers must align to it. Testbenches must include **adversarial validation**:
  boundary conditions, race conditions, unexpected input sequences, and reset
  mid-operation. Add a `#1` delay after `@(posedge clk)` before driving DUT
  inputs with blocking assignments (or use non-blocking assignments).
- **C (MCU firmware):** `make test` in `firmware/` runs the host unit tests with
  recording mocks; `make`, `make DIAG=0` and `make ADAR_COUNT=4` must all build
  (`arm-none-eabi-gcc`).
- **System-level invariants:** whenever you change code, check that invariants
  across module, process and chip boundaries still hold: the FPGA to STM32
  DIG0-7 GPIO contract, the host USB packet format, and the opcode tables shared
  by RTL, GUI and tests.
- Write the failing test first.
- Hardware facts (register bits, pin functions, polarities) are checked against
  the datasheets in `hardware/datasheets/` and cited by table and page in code
  comments. Anything that cannot be verified there is marked `VERIFY` and listed
  in `BACKLOG.md`.

### Line endings

Several RTL files mix CRLF and LF. Never rewrite a whole file's line endings.
Before committing, `git diff --stat` and `git diff --ignore-cr-at-eol --stat`
must report the same line counts.

## AI usage policy

The use of AI is permitted, but the quality and control of the codebase must not
depend on the agents: it depends on the maintainer pushing the changes, who is
fully responsible for the code they commit.

1. **Human accountability.** The committing engineer is fully responsible for
   AI-generated code as if they wrote it. Every PR must be understood and
   defensible by a human.
2. **Mandatory review.** No raw AI output may be committed unread. AI code must
   pass the same review bar as hand-written code.
3. **Full CI before commit.** All AI-assisted changes must pass the complete CI
   suite locally (lint, unit, regression, cross-layer) before commit.

Agent-specific rules (development process, models, git workflow) are in
[AGENTS.md](AGENTS.md).

## Running the test suites

CI runs these jobs on every PR; run them locally before pushing.

### 1. Python lint and tests

```bash
uv run ruff check .
QT_QPA_PLATFORM=offscreen uv run pytest host/test_v7.py host/test_radar_protocol.py -v
```

### 2. FPGA regression

```bash
cd fpga
bash run_regression.sh
cd .. && git checkout -- fpga/tb/cosim
```

The regression (lint, vendor-neutrality and resource gates, golden tests, 38
testbenches) rewrites tracked co-sim CSVs: always restore `fpga/tb/cosim/*.csv`.
It does not touch `fpga/tb/golden/`: the receiver golden is compared against the
committed `fpga/tb/golden/golden_doppler.mem`, which changes only when a receiver
change legitimately alters the output (re-blessing procedure in AGENTS.md,
"Things to know"), after two runs give the same md5.

### 3. Firmware (STM32G0B1)

```bash
cd firmware
make test
make
make DIAG=0 clean all
make ADAR_COUNT=4 clean all
```

### 4. Legacy F7 MCU tests

The upstream STM32F7 unit tests are reference only (do not modify them) but still
run in CI:

```bash
cd legacy/9_Firmware/9_1_Microcontroller/tests
make clean && make
```

### 5. Cross-layer contract tests

```bash
uv run pytest tests/cross_layer/test_cross_layer_contract.py -v
```

## CI checklist

| Job | What it checks |
|----|---------------|
| `python-tests` | path gate (`tools/check_paths.sh`), ruff clean, py_compile, `host/test_v7.py` and `host/test_radar_protocol.py` green |
| `mcu-tests` | legacy F7 unit tests, `make test` exits 0 |
| `mcu-g0b1` | firmware `make test`, `make`, `make DIAG=0`, `make ADAR_COUNT=4` |
| `fpga-regression` | `run_regression.sh` exits 0, golden vectors reproduce |
| `cross-layer-tests` | contract tests green |

## Checklist before push

- [ ] `bash tools/check_paths.sh` - no references to the old layout
- [ ] `uv run ruff check .` - no lint errors
- [ ] `QT_QPA_PLATFORM=offscreen uv run pytest host/test_v7.py host/test_radar_protocol.py -v` - all pass
- [ ] `(cd fpga && bash run_regression.sh)` - all phases pass; `git checkout -- fpga/tb/cosim` afterwards
- [ ] `cd firmware && make test && make && make DIAG=0 clean all && make ADAR_COUNT=4 clean all` - pass
- [ ] `cd legacy/9_Firmware/9_1_Microcontroller/tests && make clean && make` - pass
- [ ] `uv run pytest tests/cross_layer/test_cross_layer_contract.py` - pass
- [ ] `git diff --check` - no whitespace issues; line-ending counts match (see above)
- [ ] no generated outputs and no `uv.lock` staged
- [ ] PR targets `main`

## Questions?

Open a GitHub issue; discussion is visible to everyone.
