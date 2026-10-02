# AERIS-10 Lite

AERIS-10 Lite is a reduced, low-cost prototype of the open-source AERIS-10
pulse-LFM phased-array radar: **4 receive/transmit channels instead of 16**, a
single development-board LO, and a vendor-neutral FPGA design that targets a
Cyclone development board. It is a fork of
[NawfalMotii79/PLFM_RADAR](https://github.com/NawfalMotii79/PLFM_RADAR)
("AERIS-10"); see [Upstream and attribution](#upstream-and-attribution).

> **Status: simulation and host tests only. Nothing has been tested on
> hardware.** The FPGA regression, the firmware host tests, the host GUI tests
> and the cross-layer contract tests pass; no board has been powered up. The PLL
> register tables in the firmware are **placeholders**, so the LO will not lock
> until real register exports are added (see [BACKLOG.md](BACKLOG.md) and
> [docs/bring-up.md](docs/bring-up.md)).

## What the prototype is

| Block | Part / choice |
|---|---|
| Beamformer | 1x ADAR1000 (4 channels) |
| Front ends | 4x ADTR1107 (LNA + PA + T/R switch) |
| LO | single PLL evaluation board: ADF4372 or LMX2594 (selected at build time, placeholder tables) |
| FPGA | Intel/Altera Cyclone development board, **board TBD**; the RTL is vendor-neutral Verilog-2001 (no vendor primitives, inferred DSP and RAM) |
| ADC | 12-bit CMOS, 100 MSPS, 20 MHz IF |
| DAC | 8-bit, chirp generation (120 MHz DAC clock) |
| Host link | FT232H / FT2232H in 245 synchronous FIFO mode (USB 2.0) |
| MCU | ST NUCLEO-G0B1RE (STM32G0B1RET6, Cortex-M0+, 64 MHz) |
| Host software | PyQt6 V7 GUI (upstream, unchanged; not yet adapted to 4 channels) |

## Block diagram

```
                       +---------------------------+
   host PC  <--USB2--> | FT232H / FT2232H (245 FIFO)|
 (PyQt6 GUI)           +-------------+-------------+
                                     | 8-bit bus
                         +-----------v-----------+      DIG0..DIG4 (MCU -> FPGA)
   ADC 12 bit  -------->  |   Cyclone dev board   | <------------------------+
   100 MSPS / 20 MHz IF   |  vendor-neutral RTL   |      DIG5..DIG7 (FPGA -> MCU)
   DAC 8 bit   <--------  |  DDC, matched filter, | -----------------------+ |
   (chirp)                |  Doppler, CFAR, USB   |                        | |
                          +-----------------------+                        | |
                                                                  +--------v-v--------+
   LO PLL eval board  <-- SPI1 (8 MHz) --------------------------+  NUCLEO-G0B1RE     |
   (ADF4372 / LMX2594)                                            |  firmware: power   |
   ADAR1000 (4 ch)    <-- SPI2 (16 MHz) --------------------------+  sequencing, beam, |
   ADTR1107 x4 (T/R)  <-- ADAR1000 TR / bias outputs              |  AGC outer loop,   |
   ADS7830 temperature <-- I2C1 (100 kHz)                         |  faults, UART      |
                                                                  +----------+---------+
                                          USART2 via ST-LINK VCP (115200 8N1) |
                                                                    text commands / STATUS
```

More detail (interfaces, DIG0-7 table, clocks, budgets):
[docs/architecture.md](docs/architecture.md).

## Quick start

All commands run from the repository root unless stated. Package installs
**must** use the `sfw` prefix (see [CONTRIBUTING.md](CONTRIBUTING.md)).

### FPGA (`fpga/`)

Needs `iverilog`; the golden-vector generators need Python with numpy.

```
(cd fpga && bash run_regression.sh)    # lint, vendor-neutrality and resource gates, 38 testbenches
git checkout -- fpga/tb/cosim          # the script rewrites tracked CSVs; restore them
```

Module guide, signal chain, rates/widths, resource budget and hand-off list:
[fpga/README.md](fpga/README.md).

### Firmware (`firmware/`)

Needs `arm-none-eabi-gcc` (13.2.1 used), `gcc`, `make`; `st-flash` or
`STM32_Programmer_CLI` for flashing.

```
cd firmware
make test                              # host unit tests (ADAR_COUNT=1 and 4)
make                                   # build ELF/BIN/HEX and run the size check
make DIAG=0 clean all                  # build without diagnostic logging
make ADAR_COUNT=4 clean all            # build for four ADAR1000 devices
make flash                             # ST-LINK (make flash PROGRAMMER=cube for STM32_Programmer_CLI)
```

Pin map, serial command interface, fault model and memory report:
[firmware/README.md](firmware/README.md).

### Host (`host/`)

```
sfw uv pip install -r host/requirements_v7.txt
python host/GUI_V7_PyQt.py             # GUI
python host/smoke_test.py              # board bring-up script, mock mode (--live for real FT2232H)
QT_QPA_PLATFORM=offscreen uv run pytest host/test_v7.py -v
```

See [host/README.md](host/README.md).

### Cross-layer contract tests (`tests/cross_layer/`)

```
uv run pytest tests/cross_layer/test_cross_layer_contract.py -v
```

These check that the FPGA RTL, the G0B1 firmware and the host GUI agree on
opcodes, the USB packet format and the DIG0-7 GPIO contract.

### Lint

```
uv run ruff check .
```

## Repository map

| Path | Contents |
|---|---|
| `fpga/` | Vendor-neutral Verilog RTL, testbenches, golden vectors, regression script ([README](fpga/README.md)) |
| `firmware/` | STM32G0B1 (NUCLEO-G0B1RE) bare-metal C firmware, host unit tests ([README](firmware/README.md)) |
| `host/` | V7 PyQt6 GUI, USB protocol layer, smoke test ([README](host/README.md)) |
| `tests/cross_layer/` | Cross-layer contract tests (RTL / firmware / host) |
| `tools/` | Helper scripts (`uart_capture.py`) |
| `hardware/datasheets/` | Datasheets of the parts the project uses (ADAR1000, ADTR1107, ADS7830, TMP35/36/37, FT2232H) |
| `docs/` | [architecture](docs/architecture.md), [bring-up](docs/bring-up.md), [decisions](docs/decisions.md), [BOM analysis](docs/bom-optimization.md) (historical, Russian), `superpowers/` (specs and plans) |
| `legacy/` | Upstream material Lite does not use, under its original relative paths ([README](legacy/README.md)) |
| `BACKLOG.md` | Follow-up work and open verification items |
| `AGENTS.md`, `CLAUDE.md` | Guidance for AI coding agents |
| `Licence` | CERN-OHL-P v2 text |

## Documentation

- [docs/architecture.md](docs/architecture.md) - system design, interfaces, clocks, budgets
- [docs/bring-up.md](docs/bring-up.md) - hardware requirements and ordered bring-up checklist
- [docs/decisions.md](docs/decisions.md) - dated log of owner decisions
- [docs/bom-optimization.md](docs/bom-optimization.md) - historical analysis of the upstream design (Russian)
- [docs/superpowers/](docs/superpowers/) - specs and plans of the work done so far
- [BACKLOG.md](BACKLOG.md) - open items
- Module READMEs: [fpga](fpga/README.md), [firmware](firmware/README.md), [host](host/README.md), [legacy](legacy/README.md)

## Upstream and attribution

AERIS-10 Lite is a fork of
[NawfalMotii79/PLFM_RADAR](https://github.com/NawfalMotii79/PLFM_RADAR)
("AERIS-10: Open Source Pulse Linear Frequency Modulated Phased Array Radar").
The fork baseline is upstream commit `b46dd71`. Upstream material that Lite does
not use (the 16-channel hardware design, Xilinx flows, older GUIs, the HTML
site) is kept in [`legacy/`](legacy/README.md) with its history. Thanks to the
upstream author and contributors.

## Licences

The project uses different licences for hardware and software, as upstream does:

- **Hardware** designs and documentation: CERN Open Hardware Licence Version 2 -
  Permissive (CERN-OHL-P v2). The full text is in the [`Licence`](Licence) file.
- **Software** (firmware, FPGA RTL, host code, scripts): MIT, where not
  otherwise specified.
