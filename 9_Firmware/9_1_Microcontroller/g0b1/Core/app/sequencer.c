#include "sequencer.h"
#include <errno.h>
#include "fault.h"
#include "hal_time.h"

const seq_step_t SEQ_BASE_UP[SEQ_BASE_UP_N] = {
    { PIN_EN_FPGA,        1, 100 },
    { PIN_EN_LO,          1, 10  },
    { PIN_EN_ADAR,        1, 500 },   /* upstream settle time */
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
    { PIN_EN_LO,          0, 10 },
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
    { PIN_EN_LO,          0, 0 },
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
