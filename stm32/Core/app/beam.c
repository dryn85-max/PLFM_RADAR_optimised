#include <errno.h>
#include "beam.h"
#include "adar1000.h"
#include "sin_lut.h"

/* sin(el) in Q15 for integer degrees -90..90 (caller range-checks). */
static int32_t sin_q15(int el_deg)
{
    return el_deg >= 0 ? (int32_t)SIN_Q15[el_deg] : -(int32_t)SIN_Q15[-el_deg];
}

int beam_phase_indices(int el_deg, uint8_t idx[ADAR_COUNT * 4])
{
    int32_t s;
    int n;
    if (el_deg < -90 || el_deg > 90) {
        return -EINVAL;
    }
    s = sin_q15(el_deg);
    for (n = 0; n < ADAR_COUNT * 4; n++) {
        /* |n*s| <= 15*32767 fits int32. >> of a negative value is an arithmetic
         * shift on every supported compiler (gcc/clang, arm-none-eabi-gcc). */
        int32_t q6 = ((int32_t)n * s) >> 3;
        idx[n] = (uint8_t)(((q6 + 32) >> 6) & 127);
    }
    return 0;
}

int beam_apply(int el_deg)
{
    uint8_t idx[ADAR_COUNT * 4];
    int g, rc = beam_phase_indices(el_deg, idx);
    if (rc != 0) {
        return rc;
    }
    for (g = 0; g < ADAR_COUNT * 4; g++) {
        uint8_t dev = (uint8_t)(g / 4), ch = (uint8_t)(g % 4);
        rc = adar_set_rx_phase(dev, ch, idx[g]);
        if (rc != 0) return rc;
        rc = adar_set_tx_phase(dev, ch, idx[g]);
        if (rc != 0) return rc;
    }
    return 0;
}
