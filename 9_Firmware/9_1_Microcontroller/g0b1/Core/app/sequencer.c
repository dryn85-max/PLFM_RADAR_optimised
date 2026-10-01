#include "sequencer.h"
#include <errno.h>
#include "fault.h"
#include "hal_time.h"

const seq_step_t SEQ_BASE_UP[SEQ_BASE_UP_N] = {
    { PIN_EN_FPGA,        1, 100 },
    { PIN_EN_LO,          1, 10  },
    { PIN_PLL_CS,         1, 0   },   /* CS idles high only once the PLL is powered */
    { PIN_EN_ADAR,        1, 500 },   /* upstream settle time */
    { PIN_ADAR_CS0,       1, 0   },
    { PIN_ADAR_CS1,       1, 0   },
    { PIN_ADAR_CS2,       1, 0   },
    { PIN_ADAR_CS3,       1, 0   },
    { PIN_EN_ADTR_VDD_SW, 1, 1   },
    { PIN_EN_ADTR_VSS_SW, 1, 1   },
};

const seq_step_t SEQ_RF_UP[SEQ_RF_UP_N] = {
    { PIN_EN_LNA, 1, 2  },
    { PIN_EN_PA,  1, 50 },
};

const seq_step_t SEQ_DOWN[SEQ_DOWN_N] = {
    { PIN_FPGA_DIG3,      0, 0  },
    { PIN_EN_PA,          0, 10 },
    { PIN_EN_LNA,         0, 10 },
    { PIN_EN_ADTR_VSS_SW, 0, 1  },
    { PIN_EN_ADTR_VDD_SW, 0, 1  },
    { PIN_EN_ADAR,        0, 10 },
    /* CS low only after the chip supply is off (see sequencer.h) */
    { PIN_ADAR_CS0,       0, 0  },
    { PIN_ADAR_CS1,       0, 0  },
    { PIN_ADAR_CS2,       0, 0  },
    { PIN_ADAR_CS3,       0, 0  },
    { PIN_EN_LO,          0, 10 },
    { PIN_PLL_CE,         0, 0  },
    { PIN_PLL_CS,         0, 0  },
    /* FPGA inputs low before the FPGA loses power (as in SEQ_ESTOP) */
    { PIN_FPGA_DIG0,      0, 0  },
    { PIN_FPGA_DIG1,      0, 0  },
    { PIN_FPGA_DIG2,      0, 0  },
    { PIN_FPGA_DIG4,      0, 0  },
    { PIN_EN_FPGA,        0, 0  },
};

const seq_step_t SEQ_ESTOP[SEQ_ESTOP_N] = {
    { PIN_FPGA_DIG3,      0, 0 },
    { PIN_EN_PA,          0, 0 },
    { PIN_EN_LNA,         0, 0 },
    { PIN_EN_ADTR_VSS_SW, 0, 0 },
    { PIN_EN_ADTR_VDD_SW, 0, 0 },
    { PIN_EN_ADAR,        0, 0 },
    /* Chip selects / CE low only AFTER the chip's supply is off: a high output
     * into an unpowered ADAR1000 / PLL back-powers it through its input
     * protection (inputs must stay <= supply + 0.3 V). Low is always within the
     * abs. max. ratings, and selecting an unpowered device is harmless; this is
     * the datasheet-safe choice over leaving CS high. */
    { PIN_ADAR_CS0,       0, 0 },
    { PIN_ADAR_CS1,       0, 0 },
    { PIN_ADAR_CS2,       0, 0 },
    { PIN_ADAR_CS3,       0, 0 },
    { PIN_EN_LO,          0, 0 },
    { PIN_PLL_CE,         0, 0 },
    { PIN_PLL_CS,         0, 0 },
    /* FPGA inputs low before the FPGA loses power: a high MCU output into an
     * unpowered FPGA would back-power it through its input protection. */
    { PIN_FPGA_DIG0,      0, 0 },
    { PIN_FPGA_DIG1,      0, 0 },
    { PIN_FPGA_DIG2,      0, 0 },
    { PIN_FPGA_DIG4,      0, 0 },
    { PIN_EN_FPGA,        0, 0 },
};

void sequencer_run(const seq_step_t *steps, size_t n)
{
    size_t i;
    if (steps == NULL) {
        return;
    }
    for (i = 0; i < n; i++) {
        gpio_write(steps[i].pin, steps[i].level);
        if (steps[i].delay_ms != 0) {
            delay_ms(steps[i].delay_ms);
        }
    }
}

int sequencer_power_up(void)
{
    if (fault_is_latched()) {
        return -EPERM;
    }
    sequencer_run(SEQ_BASE_UP, SEQ_BASE_UP_N);
    return 0;
}

int sequencer_rf_up(void)
{
    if (fault_is_latched()) {
        return -EPERM;
    }
    sequencer_run(SEQ_RF_UP, SEQ_RF_UP_N);
    return 0;
}

/* Non-latched faults (PLL lock, ADAR comm): GPIO only, no SPI. The PA gate
 * bias needs no write here: PA ON = PA OFF = 0x5D (pinched, adar1000.h), so the
 * PA is already at its safe gate voltage before VDD_PA is cut. */
void sequencer_rf_off(void)
{
    gpio_write(PIN_FPGA_DIG3, 0);
    gpio_write(PIN_EN_PA, 0);
    gpio_write(PIN_EN_LNA, 0);
}

void sequencer_power_down(void)
{
    sequencer_run(SEQ_DOWN, SEQ_DOWN_N);
}

/* Fault-handler safe: only gpio_write, no delay, no log, no bus. */
void sequencer_emergency_stop(void)
{
    size_t i;
    for (i = 0; i < SEQ_ESTOP_N; i++) {
        gpio_write(SEQ_ESTOP[i].pin, SEQ_ESTOP[i].level);
    }
}
