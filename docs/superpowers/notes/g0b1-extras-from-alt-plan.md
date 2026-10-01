# G0B1 firmware — extras salvaged from the alternative (unused) plan

Source: an independently written plan for the same spec
(`docs/superpowers/specs/2026-10-01-firmware-g0b1-port.md`), superseded by the plan that was
actually implemented on `claude/eloquent-mendel-dqv36n`. Everything else in that plan is covered
(or done better) by the branch. These three items are not, and each maps to a BACKLOG entry.

**Adapt before use:** the alternative plan put ADAR1000 on SPI1 (PA5/6/7) and the PLL on SPI2;
the branch has ADAR1000 on **SPI2 (PB13/14/15)** and the PLL on **SPI1 (PB3/4/5)**. Pin names in
the code below follow the alternative plan's `pins.h`; rename to the branch's `pins.h` symbols.
Line numbers refer to the alternative plan as it existed on 2026-10-01 (file deleted).

---

## 1. Nucleo-G0B1RE connector column for the README wiring table (BACKLOG: "fill in the Nucleo connector column")

Derived from UM2324 (Nucleo-64 user manual); rows marked **VERIFY** need a check against the
manual / board before trusting. Re-map the SPI rows to the branch's assignment.

### Nucleo-G0B1RE pin plan (authoritative copy lives in `pins.h`, Task 3)

| Signal | MCU pin | Nucleo header | Note |
|---|---|---|---|
| USART2_TX / RX | PA2 / PA3 | ST-LINK VCP | **VERIFY** SB solder bridges in UM2324 §6.8 (PA2/PA3 routed to ST-LINK by default, not to CN9/CN10) |
| SPI1 SCK / MISO / MOSI (ADAR1000) | PA5 / PA6 / PA7 | CN5 D13 / D12 / D11 | **CONFLICT**: PA5 also drives user LED LD4 (UM2324 §6.5); LED will flicker with SCK — harmless, or open the LD4 solder bridge |
| SPI2 SCK / MISO / MOSI (PLL) | PB13 / PB14 / PB15 | CN10 Morpho | free |
| I2C1 SCL / SDA (ADS7830) | PB8 / PB9 | CN5 D15 / D14 | **VERIFY** SB that may tie PB8/PB9 to A4/A5 (PC1/PC0) — must be open because PC0/PC1 are DIG0/DIG1 |
| FPGA DIG0..DIG7 | PC0..PC7 | CN7/CN10 (PC0=A5, PC1=A4, PC4=D1, PC5=D0, PC6/PC7 Morpho) | one port → atomic read of `GPIOC->IDR & 0xFF` |
| ADAR_CS0..3 | PB0 / PB1 / PB2 / PB10 | CN5 D10 / CN10 / CN10 / D6 | CS1..3 only used when `ADAR_COUNT > 1` |
| PLL_CS / PLL_CE / PLL_LD(in) | PB12 / PB11 / PB4 | CN10 / CN10 / CN9 D5 | |
| EN_FPGA / EN_LO / EN_VDD_SW / EN_VSS_SW | PC11 / PC10 / PC9 / PC8 | CN7 Morpho | PC10/PC11 are also LPUART1 alt — unused |
| EN_ADAR / EN_ADTR | PB5 / PB7 | CN9 D4 / CN10 | |
| LED_STATUS | PC12 | CN7 Morpho | external LED (LD4/PA5 is taken by SCK) |
| avoided | PC13 (B1 button), PC14/PC15 (LSE X2 — **VERIFY** X2 fitted on G0B1RE per UM2324 §6.7.2), PF0/PF1 (HSE pads), PA13/PA14 (SWD), PA11/PA12 (USB, later task), PA9/PA10 (shared with PA11/PA12 via SB on G0) | | |

---

## 2. `tests/test_pins.c` — reserved-pin and DIG-port invariants (new BACKLOG item)

Host test asserting that the pin table never uses lines that are reserved on the Nucleo-G0B1RE
(PC13 B1 button, PC14/PC15 LSE, PF0/PF1 HSE, PA13/PA14 SWD, PA11/PA12 USB and the PA9/PA10 pair
bridged to them) and that FPGA DIG0..7 occupy PC0..PC7 in order (atomic single-port read).
Extracted from the alternative plan's Task 3 (lines 834–1033); the surrounding `pins.h` /
`hal_gpio` code there is the alternative layout — keep only the test and point it at the branch's
`PIN_TABLE`.

### Task 3: `pins.h` table and `hal_gpio` target implementation

**Files:**
- Modify: `Core/hal/pins.c` (replace the placeholder table)
- Create: `Core/hal/hal_gpio_g0.c`
- Test: `tests/test_pins.c`

**Interfaces:**
- Consumes: `pin_def_t`, `gpio_t` (Task 2).
- Produces: `PIN_TABLE[]` with the final Nucleo mapping; target `gpio_*` functions; `gpio_read_fpga_dig()` reading `GPIOC->IDR & 0xFF`.

- [ ] **Step 1: Write the failing pin-table test**

`tests/test_pins.c`:

```c
#include "test_framework.h"
#include "mocks.h"
#include "hal_gpio.h"

static void test_fpga_dig_pins_are_pc0_to_pc7_in_order(void)
{
    for (int i = 0; i < 8; i++) {
        const pin_def_t *p = &PIN_TABLE[PIN_FPGA_DIG0 + i];
        TEST_ASSERT_EQ(p->port, 2);     /* GPIOC */
        TEST_ASSERT_EQ(p->pin, i);
    }
    TEST_ASSERT_EQ(PIN_TABLE[PIN_FPGA_DIG5].mode, PIN_MODE_IN_PULLDOWN);
    TEST_ASSERT_EQ(PIN_TABLE[PIN_FPGA_DIG6].mode, PIN_MODE_IN_PULLDOWN);
    TEST_ASSERT_EQ(PIN_TABLE[PIN_FPGA_DIG7].mode, PIN_MODE_IN_PULLDOWN);
    TEST_ASSERT_EQ(PIN_TABLE[PIN_FPGA_DIG4].mode, PIN_MODE_OUT);
}

static void test_no_two_signals_share_a_pin(void)
{
    for (int i = 0; i < PIN_COUNT; i++)
        for (int j = i + 1; j < PIN_COUNT; j++)
            TEST_ASSERT(!(PIN_TABLE[i].port == PIN_TABLE[j].port && PIN_TABLE[i].pin == PIN_TABLE[j].pin));
}

static void test_reserved_nucleo_pins_unused(void)
{
    /* PC13 button, PC14/PC15 LSE, PA13/PA14 SWD, PA11/PA12 USB, PF0/PF1 HSE pads */
    static const uint8_t reserved[][2] = { {2,13},{2,14},{2,15},{0,13},{0,14},{0,11},{0,12},{5,0},{5,1} };
    for (int i = 0; i < PIN_COUNT; i++)
        for (size_t r = 0; r < sizeof reserved / sizeof reserved[0]; r++)
            TEST_ASSERT(!(PIN_TABLE[i].port == reserved[r][0] && PIN_TABLE[i].pin == reserved[r][1]));
}

static void test_enables_and_cs_default_safe(void)
{
    /* rails off, chip selects idle high, FPGA held in reset (reset_n low), mixers off */
    TEST_ASSERT_EQ(PIN_TABLE[PIN_EN_FPGA].init_level, false);
    TEST_ASSERT_EQ(PIN_TABLE[PIN_EN_ADTR].init_level, false);
    TEST_ASSERT_EQ(PIN_TABLE[PIN_ADAR_CS0].init_level, true);
    TEST_ASSERT_EQ(PIN_TABLE[PIN_PLL_CS].init_level, true);
    TEST_ASSERT_EQ(PIN_TABLE[PIN_FPGA_DIG4].init_level, false);
    TEST_ASSERT_EQ(PIN_TABLE[PIN_FPGA_DIG3].init_level, false);
    gpio_init_all();
    TEST_ASSERT(mock_gpio_level(PIN_ADAR_CS0));
    TEST_ASSERT(!mock_gpio_level(PIN_EN_LO));
}

static void test_read_fpga_dig_packs_bits(void)
{
    mock_gpio_set_read(PIN_FPGA_DIG5, true);
    mock_gpio_set_read(PIN_FPGA_DIG7, true);
    TEST_ASSERT_EQ(gpio_read_fpga_dig() & 0xE0, 0xA0);
}

int main(void)
{
    RUN_TEST(test_fpga_dig_pins_are_pc0_to_pc7_in_order);
    RUN_TEST(test_no_two_signals_share_a_pin);
    RUN_TEST(test_reserved_nucleo_pins_unused);
    RUN_TEST(test_enables_and_cs_default_safe);
    RUN_TEST(test_read_fpga_dig_packs_bits);
    TEST_MAIN_END();
}
```

- [ ] **Step 2: Run to verify it fails**

Run: `make test`
Expected: `test_pins` fails on `test_fpga_dig_pins_are_pc0_to_pc7_in_order` (placeholder table is all zeros) and `test_no_two_signals_share_a_pin`.

- [ ] **Step 3: Write the real pin table**

`Core/hal/pins.c`:

```c
#include "pins.h"
/* NUCLEO-G0B1RE mapping. Port codes: 0=A 1=B 2=C 3=D 5=F. AF numbers from STM32G0B1 datasheet Table 13 (DS13560):
 * USART2 on PA2/PA3 = AF1, SPI1 on PA5/6/7 = AF0, SPI2 on PB13/14/15 = AF0, I2C1 on PB8/PB9 = AF6. */
#define A 0
#define B 1
#define C 2
const pin_def_t PIN_TABLE[PIN_COUNT] = {
    [PIN_UART_TX]   = { A, 2,  PIN_MODE_AF, 1, false, "USART2_TX (ST-LINK VCP)" },
    [PIN_UART_RX]   = { A, 3,  PIN_MODE_AF, 1, false, "USART2_RX (ST-LINK VCP)" },
    [PIN_SPI1_SCK]  = { A, 5,  PIN_MODE_AF, 0, false, "SPI1_SCK  D13 (shares LD4)" },
    [PIN_SPI1_MISO] = { A, 6,  PIN_MODE_AF, 0, false, "SPI1_MISO D12" },
    [PIN_SPI1_MOSI] = { A, 7,  PIN_MODE_AF, 0, false, "SPI1_MOSI D11" },
    [PIN_SPI2_SCK]  = { B, 13, PIN_MODE_AF, 0, false, "SPI2_SCK  CN10" },
    [PIN_SPI2_MISO] = { B, 14, PIN_MODE_AF, 0, false, "SPI2_MISO CN10" },
    [PIN_SPI2_MOSI] = { B, 15, PIN_MODE_AF, 0, false, "SPI2_MOSI CN10" },
    [PIN_I2C1_SCL]  = { B, 8,  PIN_MODE_AF, 6, false, "I2C1_SCL  D15" },
    [PIN_I2C1_SDA]  = { B, 9,  PIN_MODE_AF, 6, false, "I2C1_SDA  D14" },
    [PIN_FPGA_DIG0] = { C, 0,  PIN_MODE_OUT, 0, false, "DIG0 new_chirp (toggle)" },
    [PIN_FPGA_DIG1] = { C, 1,  PIN_MODE_OUT, 0, false, "DIG1 new_elevation (toggle)" },
    [PIN_FPGA_DIG2] = { C, 2,  PIN_MODE_OUT, 0, false, "DIG2 new_azimuth (toggle)" },
    [PIN_FPGA_DIG3] = { C, 3,  PIN_MODE_OUT, 0, false, "DIG3 mixers_enable" },
    [PIN_FPGA_DIG4] = { C, 4,  PIN_MODE_OUT, 0, false, "DIG4 fpga_reset_n (held low at boot)" },
    [PIN_FPGA_DIG5] = { C, 5,  PIN_MODE_IN_PULLDOWN, 0, false, "DIG5 agc_saturation (in)" },
    [PIN_FPGA_DIG6] = { C, 6,  PIN_MODE_IN_PULLDOWN, 0, false, "DIG6 agc_enable (in)" },
    [PIN_FPGA_DIG7] = { C, 7,  PIN_MODE_IN_PULLDOWN, 0, false, "DIG7 spare (in)" },
    [PIN_ADAR_CS0]  = { B, 0,  PIN_MODE_OUT, 0, true,  "ADAR1000 #0 CS (D10)" },
    [PIN_ADAR_CS1]  = { B, 1,  PIN_MODE_OUT, 0, true,  "ADAR1000 #1 CS" },
    [PIN_ADAR_CS2]  = { B, 2,  PIN_MODE_OUT, 0, true,  "ADAR1000 #2 CS" },
    [PIN_ADAR_CS3]  = { B, 10, PIN_MODE_OUT, 0, true,  "ADAR1000 #3 CS (D6)" },
    [PIN_PLL_CS]    = { B, 12, PIN_MODE_OUT, 0, true,  "PLL CS/SEN" },
    [PIN_PLL_CE]    = { B, 11, PIN_MODE_OUT, 0, false, "PLL CE / chip enable" },
    [PIN_PLL_LD]    = { B, 4,  PIN_MODE_IN_PULLDOWN, 0, false, "PLL lock detect (D5)" },
    [PIN_EN_FPGA]   = { C, 11, PIN_MODE_OUT, 0, false, "EN FPGA rails" },
    [PIN_EN_LO]     = { C, 10, PIN_MODE_OUT, 0, false, "EN LO/PLL eval board" },
    [PIN_EN_VDD_SW] = { C, 9,  PIN_MODE_OUT, 0, false, "EN +3V3 VDD_SW (ADTR1107)" },
    [PIN_EN_VSS_SW] = { C, 8,  PIN_MODE_OUT, 0, false, "EN -3V3 VSS_SW (ADTR1107)" },
    [PIN_EN_ADAR]   = { B, 5,  PIN_MODE_OUT, 0, false, "EN ADAR1000 3V3/5V0 (D4)" },
    [PIN_EN_ADTR]   = { B, 7,  PIN_MODE_OUT, 0, false, "EN ADTR1107 PA/LNA rails" },
    [PIN_LED_STATUS]= { C, 12, PIN_MODE_OUT, 0, false, "status LED (external)" },
};
```

Upstream mapping these replace: F7 `main.h:78-85` (ADAR CS on PA0–PA3), `main.h:94-123` (EN_P_* on PE/PG), FPGA DIG0–7 on PD8–PD15 (`main.cpp:487,526,557,837,1976,2192,2201`, `main.h:146-151`).

- [ ] **Step 4: Write the target GPIO implementation**

`Core/hal/hal_gpio_g0.c`:

```c
#include "hal_gpio.h"
#include "stm32g0xx_hal.h"

static GPIO_TypeDef *const PORTS[] = { GPIOA, GPIOB, GPIOC, GPIOD, NULL, GPIOF };

void gpio_init_all(void)
{
    __HAL_RCC_GPIOA_CLK_ENABLE(); __HAL_RCC_GPIOB_CLK_ENABLE();
    __HAL_RCC_GPIOC_CLK_ENABLE(); __HAL_RCC_GPIOD_CLK_ENABLE(); __HAL_RCC_GPIOF_CLK_ENABLE();
    for (int i = 0; i < PIN_COUNT; i++) {
        const pin_def_t *p = &PIN_TABLE[i];
        GPIO_InitTypeDef init = {0};
        init.Pin   = (uint16_t)(1u << p->pin);
        init.Speed = GPIO_SPEED_FREQ_HIGH;
        init.Pull  = GPIO_NOPULL;
        switch (p->mode) {
        case PIN_MODE_OUT:
            HAL_GPIO_WritePin(PORTS[p->port], init.Pin, p->init_level ? GPIO_PIN_SET : GPIO_PIN_RESET);
            init.Mode = GPIO_MODE_OUTPUT_PP; break;
        case PIN_MODE_IN:          init.Mode = GPIO_MODE_INPUT; break;
        case PIN_MODE_IN_PULLDOWN: init.Mode = GPIO_MODE_INPUT; init.Pull = GPIO_PULLDOWN; break;
        case PIN_MODE_AF:          init.Mode = GPIO_MODE_AF_PP; init.Alternate = p->af; break;
        }
        HAL_GPIO_Init(PORTS[p->port], &init);
    }
}

void gpio_write(gpio_t pin, bool level)
{
    const pin_def_t *p = &PIN_TABLE[pin];
    PORTS[p->port]->BSRR = level ? (1u << p->pin) : (1u << (p->pin + 16));   /* atomic set/reset */
}

bool gpio_read(gpio_t pin)
{
    const pin_def_t *p = &PIN_TABLE[pin];
    return (PORTS[p->port]->IDR & (1u << p->pin)) != 0;
}

void gpio_toggle(gpio_t pin) { gpio_write(pin, !gpio_read(pin)); }

uint8_t gpio_read_fpga_dig(void) { return (uint8_t)(GPIOC->IDR & 0xFFu); }
```

- [ ] **Step 5: Run tests and ARM build**

Run: `make test && make`
Expected: `test_pins.c: 5/5 passed`; ARM build OK (`hal_gpio_g0.c` is compiled into the ELF via the `Core/hal/*.c` wildcard; HAL GPIO functions now pulled in — size grows by ~1 KB).

- [ ] **Step 6: Commit**

```bash
git add 9_Firmware/9_1_Microcontroller/g0b1
git commit -m "feat(g0b1): Nucleo-G0B1RE pin table and GPIO HAL with atomic DIG0-7 read

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>"
```

---


---

## 3. `tools/regtable_to_h.py` — TICS Pro / ADI ACE export → `pll_tables/*.h` (BACKLOG: "Real PLL register exports")

Converter plus a well-formedness test, so the real LMX2594 (113 words R112..R0) and ADF4372
tables can be generated mechanically instead of typed. Extracted from the alternative plan's
Task 6, Step 4 (lines 2113–2184). The word formats match the branch's `pll_lo` driver
(LMX2594 `addr7<<16 | data16`, ADF4372 `addr15<<8 | data8`, MSB first) — confirm against
`Core/drivers/pll_lo.h` before wiring it in, and drop `PLL_TABLE_PLACEHOLDER` once a real export
is checked in.

- [ ] **Step 4: Write the table converter and the two table headers**

`tools/regtable_to_h.py`:

```python
#!/usr/bin/env python3
"""Convert a vendor register export to a pll_tables header.
TICS Pro (LMX2594): File > Export > "Hex Registers" gives lines like  R112\t0x700000
ADI ADF4372 eval software: Register Map > Save gives lines like      0x0000  0x18   (addr, data)
Usage: tools/regtable_to_h.py --chip lmx2594|adf4372 --name LMX2594_10500MHZ in.txt > Core/drivers/pll_tables/lmx2594_10500MHz.h
"""
import argparse, re, sys

def parse(chip, lines):
    out = []
    for ln in lines:
        ln = ln.strip()
        if not ln or ln.startswith(("#", "//")):
            continue
        if chip == "lmx2594":
            m = re.match(r"R(\d+)\s+0x([0-9A-Fa-f]{6})", ln)
            if m: out.append(int(m.group(2), 16))          # frame already contains addr(8)+data(16)
        else:
            m = re.match(r"0x([0-9A-Fa-f]{4})\s+0x([0-9A-Fa-f]{2})", ln)
            if m: out.append((int(m.group(1), 16) & 0x7FFF) << 8 | int(m.group(2), 16))
    return out

def main():
    ap = argparse.ArgumentParser(); ap.add_argument("--chip", required=True); ap.add_argument("--name", required=True); ap.add_argument("src")
    a = ap.parse_args()
    regs = parse(a.chip, open(a.src))
    if not regs: sys.exit("no registers parsed")
    print("/* generated by tools/regtable_to_h.py from %s -- do not edit */" % a.src)
    print("#include \"pll_lo.h\"")
    print("static const uint32_t %s_REGS[%d] = {" % (a.name, len(regs)))
    for i in range(0, len(regs), 6):
        print("    " + ", ".join("0x%06X" % r for r in regs[i:i+6]) + ",")
    print("};")
    delay = 1 if a.chip == "lmx2594" else 0
    print("const pll_regs_t PLL_TABLE_%s = { \"%s\", %s_REGS, %d, 500, %d };" % (a.name, a.name.lower(), a.name, len(regs), delay))

if __name__ == "__main__":
    main()
```

`Core/drivers/pll_tables/lmx2594_10500MHz.h` — **the register values must be exported from TICS Pro by the implementer** (LMX2594, 100 MHz ref from the eval board OCXO, fOUT = 10 500 MHz, LD pin = "VCO calibration + lock detect"); the plan cannot invent them. Until the export exists, check in this bootstrap table (write order R112→R0 as TICS Pro prescribes; R0 last with FCAL_EN=1) so the build and the well-formedness test pass, and mark it clearly:

```c
/* BOOTSTRAP TABLE -- replace with tools/regtable_to_h.py output from TICS Pro (LMX2594, fREF=100 MHz, fOUT=10500 MHz).
 * Only R0 (reset / FCAL_EN) is real; the chip will NOT lock with this table. */
#include "pll_lo.h"
static const uint32_t LMX2594_10500MHZ_REGS[2] = {
    0x002412,   /* R0: RESET=1 */
    0x00241C,   /* R0: RESET=0, FCAL_EN=1, MUXOUT_LD_SEL=1 (LD on MUXout pin) */
};
const pll_regs_t PLL_TABLE_LMX2594_10500MHZ = { "lmx2594_10500MHz", LMX2594_10500MHZ_REGS, 2, 500, 1 };
```

`Core/drivers/pll_tables/adf4372_10500MHz.h` — same rule, exported from the ADI ADF4372 evaluation software (ADF4372, 10 500 MHz, LD on the LOCK_DETECT pin):

```c
/* BOOTSTRAP TABLE -- replace with tools/regtable_to_h.py output from the ADI ADF4372 eval software (fOUT=10500 MHz).
 * Only the soft-reset frame is real; the chip will NOT lock with this table. */
#include "pll_lo.h"
static const uint32_t ADF4372_10500MHZ_REGS[1] = {
    0x000081,   /* reg 0x0000: SOFT_RESET | SOFT_RESET_R (ADF4372 DS Table 12) */
};
const pll_regs_t PLL_TABLE_ADF4372_10500MHZ = { "adf4372_10500MHz", ADF4372_10500MHZ_REGS, 1, 500, 0 };
```

`pll_lo.c` must include both headers once so the tables are emitted — add at the end of `pll_lo.c`: `#include "pll_tables/adf4372_10500MHz.h"` and `#include "pll_tables/lmx2594_10500MHz.h"`. The active table is selected by `-DPLL_TABLE=PLL_TABLE_LMX2594_10500MHZ` (default) in the Makefile `DEFS` (add `-DPLL_TABLE=PLL_TABLE_LMX2594_10500MHZ` to `DEFS` and to the tests `CFLAGS`).

