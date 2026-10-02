#include "thermal.h"
#include <errno.h>
#include "ads7830.h"
#include "config.h"
#include "diag_log.h"
#include "fault.h"
#include "hal_time.h"

#define THERMAL_ADS_ADDR   0x48
#define THERMAL_ADS_CH     0
#define THERMAL_VREF_MV    2500
#define THERMAL_FULL_SCALE 256   /* ADS7830 LSB = Vref / 256 (8-bit, ADS7830 DS) */

static uint32_t g_last_ms;
static int16_t  g_deci_c;
static int      g_err;

int thermal_read_deci_c(int16_t *deci_c)
{
    int32_t mv, d;
    int raw;
    if (deci_c == NULL) {
        return -EINVAL;
    }
    raw = ads7830_read_se(THERMAL_ADS_ADDR, THERMAL_ADS_CH, ADS7830_PD_REF_ON_ADC_ON);
    if (raw < 0) {
        return raw;
    }
    mv = ((int32_t)raw * THERMAL_VREF_MV) / THERMAL_FULL_SCALE;   /* <= 2500 */
    d = (mv * 10 + 10) / 20;                                       /* 20 mV/degC, rounded */
    *deci_c = (int16_t)d;
    return 0;
}

void thermal_init(void)
{
    g_last_ms = millis();
    g_deci_c = 0;
    g_err = 0;
}

void thermal_tick(void)
{
    int16_t t;
    int rc;
    if ((uint32_t)(millis() - g_last_ms) < THERMAL_PERIOD_MS) {
        return;
    }
    g_last_ms = millis();
    rc = thermal_read_deci_c(&t);
    g_err = rc;
    if (rc != 0) {
        DIAG_WARN("THM", "sensor read failed (%d)", rc);
        return;
    }
    g_deci_c = t;
    if (t >= OVERTEMP_DECI_C) {
        fault_raise(FAULT_OVERTEMP);
    }
}

int16_t thermal_last(void)     { return g_deci_c; }
int     thermal_last_err(void) { return g_err; }
