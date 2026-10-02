# legacy/

Back to the [root README](../README.md).

Reference material inherited from the upstream project
[NawfalMotii79/PLFM_RADAR](https://github.com/NawfalMotii79/PLFM_RADAR)
(baseline upstream commit `b46dd71`) that AERIS-10 Lite does not use.

Everything here was moved with `git mv`, so history is preserved, and keeps its
original relative path (`X` became `legacy/X`). `legacy/` is reference only and
is not built. The one exception: the STM32F7 firmware unit tests in
`9_Firmware/9_1_Microcontroller/tests/` are kept in CI (job "Legacy F7 MCU tests")
as required by the G0B1 spec; run them with
`make -C legacy/9_Firmware/9_1_Microcontroller/tests clean all`.

## Moved items

| Item | Why it is not used by AERIS-10 Lite |
|---|---|
| `1_Project_Description/` | Upstream project description (docx); describes the XC7A100T/upstream design, not Lite. |
| `2_Functional Diagram & Interconnection Matrices/` | Upstream RADAR_V6 block diagram and interconnection matrices for the original 10.5 GHz / multi-board hardware. |
| `3_Power Management/` | Upstream power-board spreadsheet and schematic; Lite has its own power tree. |
| `4_Schematics and Boards Layout/` | Upstream schematics, Gerbers, BOM/CPL for the original boards; Lite hardware is a different design. |
| `5_Simulations/` | Upstream antenna, filter and radar simulations for the original RF chain. |
| `6_Application Notes/` | Upstream application note (UG-290) for the original hardware. |
| `7_Components Datasheets and Application notes/` | Datasheets of parts Lite does not use. The five we do use (ADAR1000, ADTR1107, ADS7830, TMP35/36/37, FT2232H) live in `hardware/datasheets/`. |
| `8_Utils/` | Upstream images, Eagle CAD libraries, mechanical drawings and helper scripts for the original hardware. |
| `docs-site/` (`*.html`, `*.pdf`, `assets/`, `artifacts/`, `.nojekyll`) | Upstream GitHub Pages site, reports and Xilinx TE0713 bring-up artifacts; they describe the upstream system. Lite's own plans and specs stay in `docs/superpowers/`. |
| `9_Firmware/9_1_Microcontroller/` | Upstream STM32F7 firmware (libraries, algorithms, application code) and its unit tests. Kept as reference for the G0B1 port; the unit tests under `tests/` still run in CI as "Legacy F7 MCU tests" per the G0B1 spec. |
| `9_Firmware/9_2_FPGA/` | Xilinx-only parts of the upstream FPGA tree: build scripts, constraints (XC7A50T/XC7A200T boards) and the TE0712/TE0713 development tops. Lite's FPGA design is in `fpga/`. |
| `9_Firmware/9_3_GUI/` | Older GUIs (V5, V6, V6.5 Tk, PyQt map) with their requirements, `adi_agc_analysis.py` (needs `host/v7/agc_sim.py` on its import path) and `test_radar_data.csv`. `test_GUI_V65_Tk.py` is no longer run in CI. Lite uses the V7 GUI in `host/`. |
