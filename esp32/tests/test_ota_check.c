/* Host tests for ota_check: boundaries and condition order. */
#include <stdint.h>
#include "tinytest.h"
#include "ota_check.h"

static void test_not_pending(void)
{
    TT_ASSERT_EQ(OTA_ACT_NONE, ota_check_step(false, true, true, 0));
    TT_ASSERT_EQ(OTA_ACT_NONE, ota_check_step(false, true, true, OTA_VALID_AFTER_MS));
    TT_ASSERT_EQ(OTA_ACT_NONE, ota_check_step(false, false, false, OTA_ROLLBACK_AFTER_MS));
    TT_ASSERT_EQ(OTA_ACT_NONE, ota_check_step(false, false, false, UINT32_MAX));
}

static void test_valid_boundary(void)
{
    TT_ASSERT_EQ(OTA_ACT_NONE, ota_check_step(true, true, true, 0));
    TT_ASSERT_EQ(OTA_ACT_NONE, ota_check_step(true, true, true, OTA_VALID_AFTER_MS - 1));
    TT_ASSERT_EQ(OTA_ACT_MARK_VALID, ota_check_step(true, true, true, OTA_VALID_AFTER_MS));
    TT_ASSERT_EQ(OTA_ACT_MARK_VALID, ota_check_step(true, true, true, OTA_VALID_AFTER_MS + 1));
    TT_ASSERT_EQ(30000u, OTA_VALID_AFTER_MS);
}

static void test_rollback_boundary(void)
{
    TT_ASSERT_EQ(OTA_ACT_NONE, ota_check_step(true, false, false, OTA_ROLLBACK_AFTER_MS - 1));
    TT_ASSERT_EQ(OTA_ACT_ROLLBACK, ota_check_step(true, false, false, OTA_ROLLBACK_AFTER_MS));
    TT_ASSERT_EQ(OTA_ACT_ROLLBACK, ota_check_step(true, false, false, UINT32_MAX));
    TT_ASSERT_EQ(120000u, OTA_ROLLBACK_AFTER_MS);
}

static void test_missing_condition(void)
{
    /* before 30 s nothing happens whatever is down */
    TT_ASSERT_EQ(OTA_ACT_NONE, ota_check_step(true, false, true, 29999));
    TT_ASSERT_EQ(OTA_ACT_NONE, ota_check_step(true, true, false, 29999));
    /* 30 s .. 119.999 s: a missing condition only waits */
    TT_ASSERT_EQ(OTA_ACT_NONE, ota_check_step(true, false, true, 30000));
    TT_ASSERT_EQ(OTA_ACT_NONE, ota_check_step(true, true, false, 30000));
    TT_ASSERT_EQ(OTA_ACT_NONE, ota_check_step(true, false, false, 119999));
    TT_ASSERT_EQ(OTA_ACT_NONE, ota_check_step(true, false, true, 119999));
    TT_ASSERT_EQ(OTA_ACT_NONE, ota_check_step(true, true, false, 119999));
    /* from 120 s: rolled back */
    TT_ASSERT_EQ(OTA_ACT_ROLLBACK, ota_check_step(true, false, true, 120000));
    TT_ASSERT_EQ(OTA_ACT_ROLLBACK, ota_check_step(true, true, false, 120000));
}

static void test_valid_wins_late(void)
{
    /* healthy image evaluated late is never rolled back */
    TT_ASSERT_EQ(OTA_ACT_MARK_VALID, ota_check_step(true, true, true, OTA_ROLLBACK_AFTER_MS));
    TT_ASSERT_EQ(OTA_ACT_MARK_VALID, ota_check_step(true, true, true, UINT32_MAX));
}

int main(void)
{
    TT_RUN(test_not_pending);
    TT_RUN(test_valid_boundary);
    TT_RUN(test_rollback_boundary);
    TT_RUN(test_missing_condition);
    TT_RUN(test_valid_wins_late);
    return TT_RESULT();
}
