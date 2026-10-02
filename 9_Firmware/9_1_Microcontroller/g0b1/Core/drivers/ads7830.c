#include <errno.h>
#include "ads7830.h"
#include "hal_i2c.h"
#include "hal_time.h"

#define ADS7830_SD_SINGLE   0x80
#define ADS7830_PD_MASK     0x0C
#define ADS7830_CONV_MS     1      /* upstream conversion delay, kept */

uint8_t ads7830_command(uint8_t ch, uint8_t pd_bits)
{
    uint8_t sel = (uint8_t)((ch >> 1) | ((ch & 1u) << 2));   /* C2 C1 C0, Table 2 */
    return (uint8_t)(ADS7830_SD_SINGLE | (sel << 4) | pd_bits);
}

int ads7830_read_se(uint8_t addr7, uint8_t ch, uint8_t pd_bits)
{
    uint8_t cmd, raw = 0;
    int rc;
    if (addr7 < ADS7830_ADDR_MIN || addr7 > ADS7830_ADDR_MAX || ch > 7 ||
        (pd_bits & (uint8_t)~ADS7830_PD_MASK) != 0) {
        return -EINVAL;
    }
    cmd = ads7830_command(ch, pd_bits);
    rc = i2c_write(addr7, &cmd, 1);
    if (rc != 0) {
        return rc < 0 ? rc : -EIO;
    }
    delay_ms(ADS7830_CONV_MS);
    rc = i2c_read(addr7, &raw, 1);
    if (rc != 0) {
        return rc < 0 ? rc : -EIO;
    }
    return raw;
}
