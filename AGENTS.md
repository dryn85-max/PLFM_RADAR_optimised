# AGENTS.md

Guidance for AI coding agents working in this repository. For what the
project is, see [README.md](README.md); for contribution rules,
[CONTRIBUTING.md](CONTRIBUTING.md); for the hardware/BOM background,
[BOM_OPTIMIZATION_REPORT.md](BOM_OPTIMIZATION_REPORT.md).

## Development process: superpowers

The project is developed by two people: one with Claude (Claude Code /
Claude desktop), one with OpenAI Codex. Both use the
[superpowers](https://github.com/obra/superpowers) skills.

- **Medium and complex tasks always go through superpowers**:
  brainstorming → spec → plan → subagent-driven development (SDD) → final
  review. Trivial one-line fixes may skip the ceremony.
- **Execution is always SDD** (subagent-driven development). When the plan
  is ready, don't ask "subagents or this session?" — the answer is SDD.
- **All discussion happens in brainstorming.** Once the brainstorming
  design is agreed, it counts as approval for the spec, the plan and the
  implementation — don't stop to ask for approval at each later stage.
  Stop only for the things listed under "Working with the owner" below.

Specs and plans written by superpowers live in `docs/superpowers/specs/` and
`docs/superpowers/plans/`. Track follow-up work that is out of scope for the
current plan in a `BACKLOG.md` at the repo root (create it when first needed).

### Model per stage

**Before SDD — the main session's own model.** Brainstorming, the spec and
the plan run in the main session, on whatever model the owner picked for it
(Sonnet or Opus — the owner decides, to keep development cost down). Don't
dispatch subagents for them and don't ask to switch models. The main session
also coordinates SDD.

**Inside SDD — subagents with an explicit model.** Always pick the model
explicitly when dispatching a subagent:

| SDD stage (subagent) | Claude | Codex |
|---|---|---|
| Implementation, tests, per-task reviews, fix rounds 1–3 | Sonnet | GPT-5.6 Terra |
| Mechanical tasks: copy code given verbatim in the plan/spec into a predefined place | Haiku | GPT-5.6 Luna |
| Fix rounds 4–5 (a finding still open after 3 rounds) | Opus | GPT-5.6 Sol |
| Root-cause investigation of a bug whose cause is unknown (systematic-debugging); the fix itself follows the implementation row | Opus | GPT-5.6 Sol |
| Final whole-branch review | Opus | GPT-5.6 Sol |

The final review's brief must name the high-risk tasks of the branch
(clock-domain crossings, reset behaviour, fixed-point width/overflow,
protocol/opcode contracts with the host and STM32) so the reviewer gives them
focused attention — instead of a separate top-tier per-task review for them.

GPT-5.5 is not used.

## Working with the owner

- Communicate in Russian.
- **Do only what was asked.** Don't add behaviour changes nobody requested,
  even "obvious" improvements — propose them instead and let the owner
  decide.
- **Ask before design decisions.** When there is more than one reasonable
  way to do something (bit widths, architecture, interfaces, limits), present
  the options with a recommendation and wait for a choice before writing
  code. In the superpowers flow this happens during brainstorming.
- **Never ask for or accept secrets in chat** (API keys, tokens, licence
  keys).
- Anything outward-facing or hard to reverse — merging to `main`, releases,
  force-pushes, deleting hardware design files (`4_Schematics and Boards
  Layout/`, datasheets) — needs the owner's explicit go-ahead.

## Git workflow

- `main` is for production releases only; topic branches are created from
  `develop` (see CONTRIBUTING.md). Work on the branch you were assigned and
  never push to `main` directly.
- Changes reach `develop`/`main` only through a PR, and only when the owner
  asks for one.
- CI (`.github/workflows/ci-tests.yml`) must be green before merging.
- Commit messages are descriptive; end them with the attribution lines the
  session provides.

## Development

- **Package installs must use the `sfw` prefix** (supply-chain mandate from
  CONTRIBUTING.md): `sfw uv pip install <pkg>`, `sfw npm install <pkg>`,
  `sfw cargo <cmd>`. Never run bare `pip install`.
- FPGA (Verilog-2001, Icarus): `cd 9_Firmware/9_2_FPGA && bash run_regression.sh`.
  The RTL (`radar_system_top.v`) is the single source of truth for opcode
  values, bit widths and reset defaults. No SystemVerilog, no `$clog2`.
- Python (GUI/scripts/tests): `uv run ruff check .`;
  `cd 9_Firmware/9_3_GUI && uv run pytest test_GUI_V65_Tk.py test_v7.py -v`.
- MCU: `cd 9_Firmware/9_1_Microcontroller/tests && make clean && make`.
- Cross-layer contracts:
  `uv run pytest 9_Firmware/tests/cross_layer/test_cross_layer_contract.py -v`.
- Write the failing test first. Testbenches must be adversarial: boundary
  conditions, reset mid-operation, unexpected input sequences.
- Run the full relevant suite locally before every commit; no raw AI output
  is committed unread (CONTRIBUTING.md, "AI Usage Policy").

## Things to know

- Do not commit generated outputs (`*.vvp`, `*.vcd`, Vivado/Quartus projects,
  bitstreams, build logs) — see `.gitignore`.
- System-level invariants (across module, process and chip boundaries) must
  hold after every change: FPGA ↔ STM32 DIG0–7 GPIO contract, host USB packet
  format, opcode tables shared between RTL, GUI and tests.
