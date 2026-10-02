#include "time_source.h"

time_source_t time_source_decide(int64_t gps_age_us, bool have_sntp)
{
    if (gps_age_us >= 0 && gps_age_us <= TSRC_GPS_FRESH_US) {
        return TSRC_GPS;
    }
    return have_sntp ? TSRC_SNTP : TSRC_NONE;
}
