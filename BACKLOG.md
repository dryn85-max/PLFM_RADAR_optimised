# Backlog

Follow-up work that is out of scope for the current plans.

## G0B1 firmware track

- [ ] **Real PLL register exports.** Replace the placeholder tables in
  `9_Firmware/9_1_Microcontroller/g0b1/Core/drivers/pll_tables/` with exports from
  TICS Pro (LMX2594) / ADI ACE (ADF4372), remove `PLL_TABLE_PLACEHOLDER`. Until
  then lock fails and the unit reports `FAULT_PLL_LOCK`.
  Generate the headers with `9_Firmware/9_1_Microcontroller/g0b1/tools/regtable_to_h.py`
  (usage in the g0b1 README; it emits `PLL_TABLE_PLACEHOLDER 0`); do not hand-type
  the 113 LMX2594 words.
- [ ] **Verify pins, AF numbers and I2C TIMINGR** (`0x10B17DB5`) against the
  STM32G0B1 datasheet and UM2324; fill in the "Nucleo connector" column of the
  README wiring table.
- [ ] **Confirm on the schematic** that ADTR1107 `CTRL_SW` is driven by the
  ADAR1000 `TR_SW_POS` output (the `SW_DRV_TR_STATE` polarity fix depends on it).
- [ ] **Idq calibration of the PA gate bias on hardware.** The ADTR1107
  contains the PA. Firmware uses safe values (PA ON = PA OFF = `0x5D`, -1.75 V,
  PA pinched; LNA ON `0x00`, OFF `0x68`; all <= `0x6A` = -2.0 V, enforced by
  `_Static_assert` in `Core/drivers/adar1000.h`). Find the real ON value for the
  target quiescent current (DS example: ON `0x39` -> ~220 mA, OFF `0x85`) by
  stepping the bias up from pinch-off while measuring Idq, then raise
  `kPaBiasOperational` (and the `kBiasDacMaxSafe` limit only with the owner's
  agreement). Check that the ADAR1000 `PA_ON` pin is not pulled low on the board.
- [ ] **CS lines idle high before the rails are up.** `hal_gpio_init()` sets the
  ADAR/PLL chip selects high at boot while the chip supplies are still off, which
  can back-power the unpowered chips. Safer: CS low until the supply is on
  (`SEQ_BASE_UP` already raises them after the supply; only the initial level in
  `hal_gpio.c` and the first-frame edge need handling). The e-stop and shutdown
  already drive them low after the supply is cut.
- [ ] **ADAR1000 soft reset is chip-0-global** (matters for `ADAR_COUNT` > 1):
  a reset issued for one device resets the others.
- [ ] **USB-CDC on PA11/PA12** (host link; `RadarSettings` binary packet).
- [ ] **GPS/IMU wiring** (`um982_gps.c` is compiled but not wired).
- [ ] **GUI parser for `STATUS`** lines.
- [ ] **Hardware bring-up checklist** (first power-up, rail order with a scope,
  DIG0..7 against the FPGA, SPI/I2C waveforms, lock detect, IWDG/e-stop drills).
  Include: **external pull-downs fitted on all `EN_*` lines and DIG0..DIG4**
  (MCU pins are high-Z during reset/flashing; the FPGA `reset_n` has an internal
  `PULLUP`, `xc7a50t_ftg256.xdc:121`); ADAR1000 `PA_ON` not pulled low;
  `CTRL_SW` level (receive) measured before the PA rail rises; a deliberate
  watchdog reset leaves `fault=13` latched; **Nucleo
  solder bridges: PB8/PB9 (I2C1) must NOT be tied to A4/A5 (PC1/PC0 = DIG1/DIG0) -
  verify they are open; PA2/PA3 stay routed to the ST-LINK VCP**; connector
  column of the README wiring table checked against UM2324.

## RTL track

(Reserved: the RTL track adds its items here.)
