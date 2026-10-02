/* Which time source the live page shows: GPS while it has a fix, else SNTP. Both are
 * recorded as time_sync records (the host chooses); this only decides what is shown. */
#ifndef TIME_SOURCE_H
#define TIME_SOURCE_H

#include <stdbool.h>
#include <stdint.h>

#define TSRC_GPS_FRESH_US 5000000 /* a GPS time_sync this recent counts as "has a fix" */

typedef enum {
    TSRC_NONE = 0,
    TSRC_GPS = 1,  /* values equal the time_sync record source codes (TIME_SRC_*) */
    TSRC_SNTP = 2
} time_source_t;

/* gps_age_us: age of the latest GPS time_sync, negative if there was none.
 * have_sntp: at least one SNTP sync happened. */
time_source_t time_source_decide(int64_t gps_age_us, bool have_sntp);

#endif
