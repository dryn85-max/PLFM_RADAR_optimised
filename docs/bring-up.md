# Bring-up

Nothing in AERIS-10 Lite has been run on hardware. This document compiles the
hardware requirements and an ordered bring-up checklist from the module READMEs
and the backlog. Each item names its source; follow the link for the reasoning.
Items that could not be verified from the repository are marked **VERIFY**.

Sources: [firmware/README.md](../firmware/README.md) (firmware),
[fpga/README.md](../fpga/README.md) (FPGA, "Hand-off" section),
[BACKLOG.md](../BACKLOG.md).

## Hardware requirements (before first power-up)

1. **External pull-downs on all `EN_*` lines (EN_FPGA, EN_LO, EN_ADAR,
   EN_ADTR_VDD_SW, EN_ADTR_VSS_SW, EN_LNA, EN_PA) and on DIG0..DIG4.** MCU pins
   are high-Z during reset and flashing; the FPGA reset (DIG4) must not float
   released. Source: [firmware/README.md, Hardware requirements](../firmware/README.md#wiring-corehalpinsh-corehalhal_gpioc),
   BACKLOG "Hardware bring-up checklist". Note: the upstream FPGA reset has an
   internal pull-up in the old Xilinx constraints (`legacy/9_Firmware/9_2_FPGA/constraints/xc7a50t_ftg256.xdc:121`);
   what the Cyclone board does is **VERIFY**.
2. **Nucleo solder bridges (UM2324):** PB8/PB9 (I2C1, Arduino D15/D14) must NOT be
   tied to A4/A5 (PC1/PC0 = DIG1/DIG0): verify the bridges are open. PA2/PA3 stay
   routed to the ST-LINK VCP. Exact bridge names **VERIFY vs UM2324**. Source:
   firmware/README.md, BACKLOG.
3. **ADAR1000 `PA_ON` (pin B3) must not be pulled low** on the board (internal
   100 kohm pull-up to the 1.8 V LDO; low forces the PA bias OFF values in TR-pin
   mode; ADAR1000 DS p. 39, Table 16). The MCU does not drive it. Source:
   firmware/README.md, BACKLOG "Idq calibration".
4. **ADTR1107 `CTRL_SW` must be driven by the ADAR1000 `TR_SW_POS` output**;
   confirm on the schematic (the `SW_DRV_TR_STATE` polarity fix depends on it).
   Source: BACKLOG, firmware/README.md "Upstream defects fixed".
5. **Connector column of the wiring table checked against UM2324** (all "VERIFY vs
   UM2324" entries), together with pin AF numbers and I2C `TIMINGR` `0x10B17DB5`
   against the STM32G0B1 datasheet. Source: firmware/README.md, BACKLOG.
6. **Real PLL register exports** (placeholders do not lock). See step 3 below.
7. **FPGA board choice and wrapper:** Cyclone dev board is TBD; a top-level
   wrapper, pin assignments and constraints are needed before any FPGA bring-up
   (see step 7). Source: fpga/README.md "Hand-off".

## Ordered checklist

Tick items on the bench; record anything unexpected in `BACKLOG.md`.

### 1. Power (rails, scope)

- [ ] With RF parts not yet populated or rails disconnected, power the board and
  check the rail order with a scope: base rails (FPGA, LO, ADAR, VDD_SW, VSS_SW)
  first, then LNA 3V3, then PA 5 V (the ADTR1107 needs a negative `VGG_PA` before
  `VDD_PA`). Source: [firmware/README.md, Fault model](../firmware/README.md#fault-model).
- [ ] Measure the `CTRL_SW` level (must be receive) **before** the PA rail rises.
  Source: BACKLOG.
- [ ] Check that the chip-select lines are not back-powering unpowered chips:
  `hal_gpio_init()` idles CS high before the rails come up (known gap, BACKLOG
  "CS lines idle high before the rails are up").
- [ ] Check that all pull-downs hold the `EN_*` lines and DIG0..DIG4 low while the
  MCU is in reset or being flashed (requirement 1).

### 2. MCU flash and VCP

- [ ] `cd firmware && make test && make`, then `make flash` (ST-LINK).
  Source: [firmware/README.md, Build, flash, test](../firmware/README.md#build-flash-test).
- [ ] Open the ST-LINK virtual COM port, 115200 8N1 (USART2, PA2/PA3). Capture the
  log with `python tools/uart_capture.py -p <port>` (give the port explicitly;
  **VERIFY** that the tool's log format matches `diag_log.h`).
- [ ] The boot log must report the placeholder PLL tables; `status` must answer
  (`STATUS lock=0 ... fault=1` until the PLL is real). Source: firmware/README.md
  "Warnings", "Serial command interface".
- [ ] Deliberate watchdog drill: a watchdog reset leaves `fault=13` latched; only a
  power cycle clears it. Also drill `stop` (latched e-stop) and over-temperature
  handling. Source: BACKLOG "Hardware bring-up checklist", firmware/README.md
  "Fault model".

### 3. PLL export and lock

- [ ] Export the register table from TICS Pro (LMX2594, "Hex Registers") or ADI ACE
  (ADF4372) for the 10.5 GHz LO setting (symbol names `LMX2594_10500MHZ` /
  `ADF4372_10500MHZ`).
- [ ] Convert with `firmware/tools/regtable_to_h.py` (run from `firmware/`), for
  example:
  `tools/regtable_to_h.py --chip lmx2594 --name LMX2594_10500MHZ --settle-ms 10 -o Core/drivers/pll_tables/lmx2594_10500MHz.h tics_export.txt`.
  Do not hand-type the 113 LMX2594 words. Source:
  [firmware/README.md, PLL register tables](../firmware/README.md#pll-register-tables-from-vendor-exports),
  BACKLOG "Real PLL register exports".
- [ ] Rebuild, flash, check the lock-detect (PB6, `lock=1` in `status`) and look at
  SPI1 waveforms (8 MHz) and the 100 ms lock timeout. `FAULT_PLL_LOCK` is
  non-latched and keeps RF rails off.

### 4. ADAR1000 scratchpad and SPI/I2C waveforms

- [ ] Confirm the ADAR1000 scratchpad write/read works; a failure raises
  `FAULT_ADAR_COMM` (code 2, non-latched). Check SPI2 waveforms (16 MHz, mode 0) and
  I2C1 (100 kHz, ADS7830 at `0x48`, temperature on channel 0). Source:
  firmware/README.md "Fault model", BACKLOG "Hardware bring-up checklist".
- [ ] Verify the temperature reading (`temp=` in deci-degC, `temp_err=0`) and
  that the `HAL`-level SPI/I2C timeouts do not trip.
- [ ] Check `tx` / `rx` / `auto` mode switching with a scope on TR/`CTRL_SW`
  (`mode=` in `status`); `gain <ch> <val>` and `beam <az> <el>` (el -60..60).

### 5. Idq calibration of the PA gate bias

- [ ] Firmware uses safe values (PA ON = PA OFF = `0x5D`, -1.75 V, PA pinched;
  LNA ON `0x00`, OFF `0x68`; all <= `0x6A` = -2.0 V, enforced by
  `_Static_assert` in `Core/drivers/adar1000.h`). Step the PA bias up from
  pinch-off while measuring Idq; the datasheet example is ON `0x39` for about
  220 mA and OFF `0x85`. Then raise `kPaBiasOperational` (the `kBiasDacMaxSafe`
  limit only with the owner's agreement). Source: BACKLOG "Idq calibration",
  [firmware/README.md, Fault model](../firmware/README.md#fault-model).
- [ ] Confirm `PA_ON` is high during this (requirement 3).

### 6. DIG0-7 against the FPGA

- [ ] With the FPGA board connected, check DIG0..DIG7 against the table in
  [architecture.md](architecture.md#fpga---mcu-dig0-7): DIG0..DIG3 toggles/levels
  from the MCU, DIG4 as FPGA reset, DIG5/DIG6 back to the MCU, DIG7 low.
  Source: BACKLOG "Hardware bring-up checklist".
- [ ] E-stop drill: DIG lines go low before `EN_FPGA`. Source: firmware/README.md
  "Fault model".

### 7. FPGA board

- [ ] Choose the Cyclone board; write the top wrapper (ADC data clock as
  `clk_100m`, PLL for the 120 MHz DAC clock, FT232H clock forwarding), pin
  assignments and I/O standards, SDC with ADC input delays. Create the Quartus
  project and add the `.mem` files (twiddles, reference spectra, TX chirp LUT);
  confirm ROM/RAM inference. Source: fpga/README.md "Hand-off", BACKLOG "Quartus
  project, pin assignments and SDC".
- [ ] Add CDC synchronizer constraints/attributes and the PLL/reset sequencing.
  Source: fpga/README.md "Hand-off" (M4), BACKLOG "CDC synchronizer constraints".
- [ ] Compare the real DSP / RAM / Fmax report with the static budget (38 / 46
  multipliers, 236 kbit). Source: BACKLOG.

### 8. ADC / DAC phase

- [ ] Close the ADC clock phase (DCO to sample clock) with input delay constraints
  and, if needed, a PLL phase shift; the RTL captures on a single edge.
- [ ] Close the DAC forwarded-clock timing (launched on the same edge as the data;
  use a phase-shifted forwarded clock and output delay constraints).
- [ ] Validate overlap-save alignment (reference vs signal segment) with real
  12-bit / 100 MSPS data; all current checks use synthetic chirps. Check
  `mf_overrun` and the long-chirp PRI against the 0.44 ms busy time. Source:
  fpga/README.md "Hand-off" (M6), BACKLOG "DAC forwarded clock and ADC clock phase",
  "Real-data validation", "Check the matched-filter busy time".

### 9. Host link

- [ ] Connect the FT232H/FT2232H (245 synchronous FIFO), run
  `python host/smoke_test.py --live` (self-test opcode `0x30`, result `0x31`).
  Source: [host/README.md](../host/README.md).
- [ ] Start the GUI (`python host/GUI_V7_PyQt.py`). It is the unchanged upstream
  V7 and not adapted to 4 channels or the `STATUS` line (BACKLOG).
- [ ] Back-pressure test: check that the `0x55` footer survives host stalls on the
  FT2232H path (BACKLOG, owner decision pending).
