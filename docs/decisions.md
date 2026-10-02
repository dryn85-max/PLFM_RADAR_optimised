# Decisions log

Dated log of owner decisions for AERIS-10 Lite, newest last within each date
group. Each entry has a one-line reason taken from the repository (commit
messages, specs, module READMEs); "reason not recorded" means the repository
does not state one. SHAs are commits on this fork. PR #2 is the merge
`623086b` (vendor-neutral RTL port, track A, and STM32G0B1 firmware port, track
B). Fork baseline: upstream `b46dd71`.

## 2026-10-01 - firmware (STM32G0B1)

| Decision | Reason | Commit |
|---|---|---|
| **Q1: `auto` command** returns all ADAR1000 devices to TR-pin mode (FPGA owns TX/RX switching); `tx` / `rx` force SPI mode for bench tests. | Upstream misused `TR_SOURCE` (reg 0x031 bit 2 selects SPI vs TR-pin, not "TX"); pin mode is the normal operating mode, so there must be a way back to it. | `a935263` |
| **Q2: elevation limited to +-60 degrees** (`beam <az> <el>`, `el` -60..60; `az` -180..180, stored only). | Reason not recorded in the repository beyond the command contract; the array is steered electronically in elevation only. | `a935263` |
| **Q3: `gain <ch> <val>` is a direct write** of the RX VGA register (0..127) that also updates the AGC cache. | Keeps the AGC's record of what was written equal to the hardware; the value sticks only while the FPGA AGC is disabled (DIG6 low), otherwise the AGC overwrites it within 250 ms. | `a935263` |
| **Safe PA/LNA bias:** PA ON = PA OFF = `0x5D` (-1.75 V, PA pinched), LNA ON `0x00`, LNA OFF `0x68`; every PA/LNA bias constant limited to `0x6A` (-2.0 V) by `_Static_assert`. | The real quiescent current is unknown until Idq is calibrated on hardware; start pinched so the PA cannot be damaged. | `f1aa4c1` |
| **Boot gains:** TX VGA `0x7F` on all channels, RX VGA = AGC base (30); ADAR set to SPI RX mode right after safe bias, before the RF rails come up. | `agc.written[]` must match the hardware; `CTRL_SW` must be in receive before the PA rail rises. | `f1aa4c1` |
| **Watchdog / NMI reset latches `FAULT_WATCHDOG` (13)**; NMI and unexpected interrupts call `fault_panic()`; the latch is stored before the e-stop and the first code is kept. | A hung or crashed MCU must come back in the safe, rails-off state instead of re-energising the PA; upstream's `Error_Handler` reset and re-energised the rails. | `acd8e76` |
| **`EN_PA` moved from PA9 to PC8.** | Removes the PA9/PA11 pad remap risk on STM32G0 (approved by the coordinator). | `8aba250` |
| **E-stop and shutdown drive DIG0-2 and DIG4 low before `EN_FPGA`; chip selects and CE go low only after the chip's supply is off.** | A high MCU output into an unpowered FPGA/ADAR1000/PLL would back-power it through its input protection. | `a935263`, `acd8e76` |

## 2026-10-01 - FPGA (vendor-neutral RTL)

| Decision | Reason | Commit |
|---|---|---|
| **Range FFT `INTERNAL_W` = 25** (spec said 24); the 16-point Doppler FFT keeps 24. Do not revert. | Full-scale I and Q reach sqrt(2) * 2^23 and wrap a 24-bit word; golden test (c) fails with 24 bits. | `a5a524a` |
| **Matched-filter segmenter buffers the whole receive window** (1024 x 16 RAM; long chirp 928 samples, short chirp 13) and then processes the four segments from the buffer. | Samples arriving while segments 0..2 were processed were dropped, so only segment 0 was contiguous (owner-approved design change). | `59a1397` |
| **Keep four 64-bin range sets per long chirp** (one per matched-filter segment), as upstream did. | Pre-existing upstream behaviour, not a regression of the port; the open design question (merge, select or gate) stays in BACKLOG. | `5b22a38` |
| **Process every chirp** (both toggle edges of `mc_new_chirp`). | Upstream detected only the rising edge of a toggle, so every second chirp was silently dropped (an upstream defect). | `59a1397`, documented in `5b22a38` |
| Keep the USB packet format unchanged (so `mf_overrun` and ADC overrange stay invisible to the host for now). | Protocol-affecting changes need an owner decision on the status-word layout (BACKLOG). | `5b22a38` |

## 2026-10-02

| Decision | Reason | Commit |
|---|---|---|
| **ISC004 lint patch included in PR #2** (parenthesised implicit string concatenations in `compare_doppler.py` and `v7/dashboard.py`). | Four pre-existing findings that also fail on `main`; no behaviour change; included in PR #2 at the owner's request. | `6872d2b` |
| **`docs/superpowers/` (specs, plans, notes) is kept in git.** | Owner decision in the restructure brainstorming (reason not recorded beyond that); historical specs and plans are not rewritten when paths change. | `de18760` |
| **Restructure into AERIS-10 Lite layout:** upstream-only material moves to `legacy/` keeping its original relative paths (nothing deleted). | The project has diverged from upstream (4-channel prototype, vendor-neutral RTL, G0B1 firmware); most upstream material no longer applies. History is preserved with `git mv`. | `de18760`, `3b30597` |
| **Semantic top-level names:** `fpga/`, `firmware/`, `host/`, `hardware/`, `docs/`, `tools/`, `tests/`, `legacy/`. | Replace the numbered upstream layout (`1_Project_Description` ... `9_Firmware`) with names that say what is inside. | `cc4fe56`, `c05835b`, `a02a7ca`, `429da93` |
| **V7 GUI moves to `host/`; older GUIs go to `legacy/`.** Adapting the GUI to the 4-channel prototype and the G0B1 `STATUS` line is a BACKLOG item. | V7 is the GUI the prototype uses; the adaptation is explicitly out of scope of the restructure. | `a02a7ca` |
| **Documentation is Markdown in `docs/`;** the upstream HTML site and PDFs go to `legacy/docs-site/`. | The upstream HTML docs site describes the upstream system and does not apply (restructure spec context). | `de18760`, `3b30597` |
| **Project name: AERIS-10 Lite;** upstream attribution and licences (MIT software, CERN-OHL-P hardware, `Licence` file) kept. | The project has diverged into a 4-channel prototype (restructure spec context). | `de18760` |
| **Language of README and `docs/` is English;** `docs/bom-optimization.md` stays Russian as a historical document. | It is a historical analysis of the upstream design. | `de18760` |

## Process decisions (earlier)

| Date | Decision | Reason | Commit |
|---|---|---|---|
| 2026-10-01 | Development follows superpowers (brainstorming, spec, plan, subagent-driven development, final review) with explicit models per stage; `main` only via PR. | Defined in [AGENTS.md](../AGENTS.md); fitted to this fork (no `develop` branch) in `97bbfa2`. | `007a8f5`, `97bbfa2` |
