# host/

Back to the [root README](../README.md).

Host application for AERIS-10 Lite: the upstream V7 PyQt6 radar GUI and its
protocol layer, taken over unchanged.

| File | Role |
|---|---|
| `GUI_V7_PyQt.py` | Entry point; opens the `RadarDashboard` main window. |
| `v7/` | GUI package: dashboard, map widget, workers, processing, replay, hardware access, software FPGA model (`software_fpga.py`), AGC simulation (`agc_sim.py`). |
| `radar_protocol.py` | Pure-logic USB protocol layer (packet parsing, command building, `FT2232HConnection`, `FT601Connection`); no GUI dependencies. |
| `smoke_test.py` | Board bring-up script: sends opcode 0x30 (self-test) and reads the result with 0x31. Mock mode by default, `--live` for real FT2232H hardware. |
| `test_v7.py` | Unit tests for the V7 GUI package (models, processing, workers, software FPGA). |
| `test_radar_protocol.py` | Unit tests for `radar_protocol.py`: packet parsing, command building, opcodes, AGC status, recorder, acquisition. No Qt or Tk needed. |
| `requirements_v7.txt` | Python dependencies. |

## Install

```
sfw uv pip install -r host/requirements_v7.txt
```

PyQt6, PyQt6-WebEngine, numpy and matplotlib are required; pyusb, pyftdi,
scipy, scikit-learn, filterpy and crcmod are optional (the GUI degrades
gracefully without them).

## Run

```
python host/GUI_V7_PyQt.py
python host/smoke_test.py            # mock mode, no hardware
python host/smoke_test.py --live     # real FT2232H
```

## Tests

```
QT_QPA_PLATFORM=offscreen uv run pytest host/test_v7.py host/test_radar_protocol.py -v
```

`v7/software_fpga.py` and some tests read the golden reference and co-sim data
from `fpga/tb/cosim/real_data` and `fpga/*.mem`; the repository root is found by
walking up to `pyproject.toml`.

## USB protocol

The host protocol is unchanged relative to upstream: data packets
(`0xAA` ... `0x55`), status packets (`0xBB` ... `0x55`) and 4-byte commands
`{opcode, addr, value_hi, value_lo}`, over FT2232H (USB 2.0, via pyftdi) or
FT601 (USB 3.0, via ftd3xx). The `fpga/` design keeps these formats, so this
GUI works with it; the opcode tables are checked against the RTL by the
cross-layer contract tests.

## Not adapted yet

The GUI still assumes the upstream radar configuration. Adapting it to the
4-channel prototype and to the G0B1 firmware `STATUS` line is tracked in
[`BACKLOG.md`](../BACKLOG.md).
