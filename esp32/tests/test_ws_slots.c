/* Host tests for ws_slots: client slot table with per-client in-flight flag. */
#include <errno.h>
#include "tinytest.h"
#include "ws_slots.h"

static void test_add_remove_basic(void)
{
    ws_slots_t t; ws_slots_init(&t);
    TT_ASSERT_EQ(0, ws_slots_count(&t));
    int a = ws_slots_add(&t, 10), b = ws_slots_add(&t, 11);
    TT_ASSERT(a >= 0 && b >= 0 && a != b);
    TT_ASSERT_EQ(2, ws_slots_count(&t));
    TT_ASSERT_EQ(10, ws_slots_fd(&t, a));
    TT_ASSERT_EQ(0, ws_slots_remove(&t, 10));
    TT_ASSERT_EQ(-1, ws_slots_fd(&t, a));
    TT_ASSERT_EQ(1, ws_slots_count(&t));
}

static void test_duplicate_fd_is_idempotent(void)
{
    ws_slots_t t; ws_slots_init(&t);
    int a = ws_slots_add(&t, 5);
    TT_ASSERT_EQ(a, ws_slots_add(&t, 5));
    TT_ASSERT_EQ(1, ws_slots_count(&t));
}

static void test_full_table(void)
{
    ws_slots_t t; ws_slots_init(&t);
    for (int i = 0; i < WS_SLOTS_MAX; i++) TT_ASSERT(ws_slots_add(&t, 100 + i) >= 0);
    TT_ASSERT_EQ(-ENOSPC, ws_slots_add(&t, 999));
    TT_ASSERT_EQ(WS_SLOTS_MAX, ws_slots_count(&t));
    TT_ASSERT_EQ(0, ws_slots_remove(&t, 100));
    TT_ASSERT(ws_slots_add(&t, 999) >= 0);
}

static void test_remove_unknown_and_invalid(void)
{
    ws_slots_t t; ws_slots_init(&t);
    TT_ASSERT_EQ(-ENOENT, ws_slots_remove(&t, 42));
    TT_ASSERT_EQ(-EINVAL, ws_slots_add(&t, -1));
    TT_ASSERT_EQ(-EINVAL, ws_slots_remove(&t, -1));
    TT_ASSERT_EQ(-1, ws_slots_fd(&t, -1));
    TT_ASSERT_EQ(-1, ws_slots_fd(&t, WS_SLOTS_MAX));
    ws_slots_end(&t, 99); /* out of range: must not crash */
    ws_slots_end(&t, -3);
    TT_ASSERT_EQ(-ENOENT, ws_slots_remove(&t, 0)); /* fd 0 is valid but absent */
    TT_ASSERT(ws_slots_add(&t, 0) >= 0);           /* fd 0 is a valid key */
    TT_ASSERT_EQ(0, ws_slots_remove(&t, 0));
    TT_ASSERT_EQ(-ENOENT, ws_slots_remove(&t, 0)); /* double remove */
}

static void test_inflight_latest_only(void)
{
    ws_slots_t t; ws_slots_init(&t);
    int a = ws_slots_add(&t, 7);
    TT_ASSERT_EQ(1, ws_slots_begin(&t, a));      /* first send allowed */
    TT_ASSERT_EQ(0, ws_slots_begin(&t, a));      /* previous not complete: skip */
    ws_slots_end(&t, a);
    TT_ASSERT_EQ(1, ws_slots_begin(&t, a));
    ws_slots_end(&t, a);
    ws_slots_end(&t, a);                          /* double end harmless */
    TT_ASSERT_EQ(1, ws_slots_begin(&t, a));
}

static void test_begin_on_inactive(void)
{
    ws_slots_t t; ws_slots_init(&t);
    TT_ASSERT_EQ(0, ws_slots_begin(&t, 0));
    TT_ASSERT_EQ(0, ws_slots_begin(&t, -1));
    TT_ASSERT_EQ(0, ws_slots_begin(&t, WS_SLOTS_MAX));
}

static void test_removed_while_inflight_slot_not_reused(void)
{
    ws_slots_t t; ws_slots_init(&t);
    int a = ws_slots_add(&t, 1);
    TT_ASSERT_EQ(1, ws_slots_begin(&t, a));
    TT_ASSERT_EQ(0, ws_slots_remove(&t, 1));
    TT_ASSERT_EQ(-1, ws_slots_fd(&t, a));         /* queued work sees an inactive slot */
    TT_ASSERT_EQ(0, ws_slots_begin(&t, a));
    /* fill the rest; the busy slot must not be handed out */
    for (int i = 0; i < WS_SLOTS_MAX - 1; i++) TT_ASSERT(ws_slots_add(&t, 50 + i) != a);
    TT_ASSERT_EQ(-ENOSPC, ws_slots_add(&t, 80));
    ws_slots_end(&t, a);
    TT_ASSERT_EQ(a, ws_slots_add(&t, 80));
    TT_ASSERT_EQ(1, ws_slots_begin(&t, a));       /* fresh client starts not in flight? */
}

int main(void)
{
    TT_RUN(test_add_remove_basic);
    TT_RUN(test_duplicate_fd_is_idempotent);
    TT_RUN(test_full_table);
    TT_RUN(test_remove_unknown_and_invalid);
    TT_RUN(test_inflight_latest_only);
    TT_RUN(test_begin_on_inactive);
    TT_RUN(test_removed_while_inflight_slot_not_reused);
    return TT_RESULT();
}
