/* Host tests for the time source priority decision. */
#include "tinytest.h"
#include "time_source.h"

#define S 1000000

static void test_none(void)
{
    TT_ASSERT_EQ(TSRC_NONE, time_source_decide(-1, false));
    TT_ASSERT_EQ(TSRC_NONE, time_source_decide(6 * S, false)); /* stale GPS, no SNTP */
}

static void test_gps_priority(void)
{
    TT_ASSERT_EQ(TSRC_GPS, time_source_decide(0, false));
    TT_ASSERT_EQ(TSRC_GPS, time_source_decide(0, true)); /* GPS wins over SNTP */
    TT_ASSERT_EQ(TSRC_GPS, time_source_decide(TSRC_GPS_FRESH_US, true));
}

static void test_fallback_sntp(void)
{
    TT_ASSERT_EQ(TSRC_SNTP, time_source_decide(-1, true));                         /* never GPS */
    TT_ASSERT_EQ(TSRC_SNTP, time_source_decide(TSRC_GPS_FRESH_US + 1, true));  /* lost fix */
    TT_ASSERT_EQ(TSRC_SNTP, time_source_decide(-5, true));                         /* any negative = never */
}

int main(void)
{
    TT_RUN(test_none);
    TT_RUN(test_gps_priority);
    TT_RUN(test_fallback_sntp);
    return TT_RESULT();
}
