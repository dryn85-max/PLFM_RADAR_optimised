# Backlog

Follow-up work that is out of scope for the current plans.

## G0B1 firmware track

- [ ] **Real PLL register exports.** Replace the placeholder tables in
  `9_Firmware/9_1_Microcontroller/g0b1/Core/drivers/pll_tables/` with exports from
  TICS Pro (LMX2594) / ADI ACE (ADF4372), remove `PLL_TABLE_PLACEHOLDER`. Until
  then lock fails and the unit reports `FAULT_PLL_LOCK`.
- [ ] **Verify pins, AF numbers and I2C TIMINGR** (`0x10B17DB5`) against the
  STM32G0B1 datasheet and UM2324; fill in the "Nucleo connector" column of the
  README wiring table.
- [ ] **Confirm on the schematic** that ADTR1107 `CTRL_SW` is driven by the
  ADAR1000 `TR_SW_POS` output (the `SW_DRV_TR_STATE` polarity fix depends on it).
- [ ] **Revisit PA bias constants before any PA is fitted.** Upstream ON `0x7F`
  (about -2.4 V) / OFF `0x20` (about -0.6 V) look swapped; the datasheet example
  uses ON `0x39` / OFF `0x85`. `kPaBiasOperational` / `kPaBiasRxSafe` in
  `Core/drivers/adar1000.h` carry the upstream values.
- [ ] **ADAR1000 soft reset is chip-0-global** (matters for `ADAR_COUNT` > 1):
  a reset issued for one device resets the others.
- [ ] **USB-CDC on PA11/PA12** (host link; `RadarSettings` binary packet).
- [ ] **GPS/IMU wiring** (`um982_gps.c` is compiled but not wired).
- [ ] **GUI parser for `STATUS`** lines.
- [ ] **Hardware bring-up checklist** (first power-up, rail order with a scope,
  DIG0..7 against the FPGA, SPI/I2C waveforms, lock detect, IWDG/e-stop drills).

## RTL track

(Reserved: the RTL track adds its items here.)
