/* Host tests for motion_det: timing boundaries, zone/energy edges, link loss, config changes. */
#include <stdint.h>
#include <string.h>
#include "tinytest.h"
#include "motion_det.h"

#define MS(x) ((uint64_t)(x) * 1000u)

static motion_cfg_t cfg;
static motion_det_t det;
static motion_ev_t ev;

static void setup(void)
{
    motion_cfg_defaults(&cfg); /* zone 100..500, e>=30, start 1000, end 3000 */
    motion_det_init(&det);
    memset(&ev, 0xAA, sizeof ev);
}

static ld_data_t frame(uint8_t state, uint16_t dist, uint8_t energy)
{
    ld_data_t d;
    memset(&d, 0, sizeof d);
    d.target_state = state;
    d.moving_dist_cm = dist;
    d.moving_energy = energy;
    d.still_dist_cm = 300;
    d.still_energy = 99;
    return d;
}

static motion_ev_kind_t step(const ld_data_t *d, uint64_t t_ms)
{
    return motion_det_step(&det, &cfg, d, MS(t_ms), &ev);
}

static void test_defaults(void)
{
    setup();
    TT_ASSERT_EQ(1, cfg.en);
    TT_ASSERT_EQ(100, cfg.dmin_cm);
    TT_ASSERT_EQ(500, cfg.dmax_cm);
    TT_ASSERT_EQ(30, cfg.emin);
    TT_ASSERT_EQ(1000, cfg.tstart_ms);
    TT_ASSERT_EQ(3000, cfg.tend_ms);
    TT_ASSERT_EQ(0, motion_det_active(&det));
    motion_cfg_defaults(NULL); /* must not crash */
}

static void test_start_boundary(void)
{
    setup();
    ld_data_t m = frame(1, 300, 50);
    TT_ASSERT_EQ(MOTION_EV_NONE, step(&m, 5000));
    TT_ASSERT_EQ(MOTION_EV_NONE, step(&m, 5999)); /* one ms before start_ms */
    TT_ASSERT_EQ(0, motion_det_active(&det));
    TT_ASSERT_EQ(MOTION_EV_START, step(&m, 6000)); /* exactly start_ms */
    TT_ASSERT_EQ(1, motion_det_active(&det));
    TT_ASSERT_EQ(MOTION_EV_NONE, step(&m, 6100)); /* no second START */
}

static void test_start_fields(void)
{
    setup();
    ld_data_t a = frame(1, 300, 40), b = frame(1, 200, 80), c = frame(1, 400, 60);
    TT_ASSERT_EQ(MOTION_EV_NONE, step(&a, 1000));
    TT_ASSERT_EQ(MOTION_EV_NONE, step(&b, 1400));
    TT_ASSERT_EQ(MOTION_EV_START, step(&c, 2000));
    TT_ASSERT_EQ(1, ev.event_no);
    TT_ASSERT_EQ(MS(1000), ev.onset_us);
    TT_ASSERT_EQ(0, ev.duration_ms);
    TT_ASSERT_EQ(80, ev.max_energy);
    TT_ASSERT_EQ(200, ev.dist_cm);
    TT_ASSERT_EQ(200, ev.min_dist_cm);
    TT_ASSERT_EQ(400, ev.max_dist_cm);
}

static void test_gap_in_pending(void)
{
    setup();
    ld_data_t m = frame(1, 300, 50), n = frame(0, 0, 0);
    TT_ASSERT_EQ(MOTION_EV_NONE, step(&m, 1000));
    TT_ASSERT_EQ(MOTION_EV_NONE, step(&m, 1500));
    TT_ASSERT_EQ(MOTION_EV_NONE, step(&n, 1600)); /* back to IDLE */
    /* the old onset must be forgotten: 2000 is only 400 ms after the new onset at 1700 */
    TT_ASSERT_EQ(MOTION_EV_NONE, step(&m, 1700));
    TT_ASSERT_EQ(MOTION_EV_NONE, step(&m, 2000));
    TT_ASSERT_EQ(MOTION_EV_NONE, step(&m, 2699));
    TT_ASSERT_EQ(MOTION_EV_START, step(&m, 2700));
    TT_ASSERT_EQ(MS(1700), ev.onset_us);
}

static void test_non_moving_at_start_time_resets(void)
{
    setup();
    ld_data_t m = frame(1, 300, 50), n = frame(2, 300, 50);
    step(&m, 1000);
    TT_ASSERT_EQ(MOTION_EV_NONE, step(&n, 2000)); /* would have started, but not moving */
    TT_ASSERT_EQ(MOTION_EV_NONE, step(&m, 2100)); /* new onset */
    TT_ASSERT_EQ(MOTION_EV_NONE, step(&m, 3000));
    TT_ASSERT_EQ(MOTION_EV_START, step(&m, 3100));
}

static void test_end_boundary(void)
{
    setup();
    ld_data_t m = frame(1, 300, 50), n = frame(0, 0, 0);
    step(&m, 0);
    TT_ASSERT_EQ(MOTION_EV_START, step(&m, 1000));
    TT_ASSERT_EQ(MOTION_EV_NONE, step(&m, 2000)); /* last moving */
    TT_ASSERT_EQ(MOTION_EV_NONE, step(&n, 4999)); /* 2999 ms after */
    TT_ASSERT_EQ(MOTION_EV_END, step(&n, 5000));  /* exactly end_ms */
    TT_ASSERT_EQ(1, ev.event_no);
    TT_ASSERT_EQ(MS(0), ev.onset_us);
    TT_ASSERT_EQ(2000, ev.duration_ms); /* last moving - onset */
    TT_ASSERT_EQ(0, motion_det_active(&det));
    TT_ASSERT_EQ(MOTION_EV_NONE, step(&n, 9000)); /* nothing more */
}

static void test_still_frames_do_not_extend(void)
{
    setup();
    ld_data_t m = frame(1, 300, 50), s = frame(2, 300, 99);
    step(&m, 0);
    step(&m, 1000);
    TT_ASSERT_EQ(MOTION_EV_NONE, step(&s, 3999));
    TT_ASSERT_EQ(MOTION_EV_END, step(&s, 4000)); /* still frames did not keep it alive */
}

static void test_still_only_never_starts(void)
{
    setup();
    ld_data_t s = frame(2, 300, 99);
    for (uint64_t t = 0; t < 10000; t += 100)
        TT_ASSERT_EQ(MOTION_EV_NONE, step(&s, t));
    TT_ASSERT_EQ(0, motion_det_active(&det));
}

static void test_state_both_counts(void)
{
    setup();
    ld_data_t b = frame(3, 300, 50);
    step(&b, 0);
    TT_ASSERT_EQ(MOTION_EV_START, step(&b, 1000));
}

static void test_null_ends_active(void)
{
    setup();
    ld_data_t m = frame(1, 300, 50);
    step(&m, 0);
    step(&m, 1000);
    step(&m, 1500);
    TT_ASSERT_EQ(MOTION_EV_NONE, step(NULL, 4499));
    TT_ASSERT_EQ(MOTION_EV_END, step(NULL, 4500));
    TT_ASSERT_EQ(1500, ev.duration_ms);
}

static void test_null_resets_pending(void)
{
    setup();
    ld_data_t m = frame(1, 300, 50);
    step(&m, 0);
    TT_ASSERT_EQ(MOTION_EV_NONE, step(NULL, 500));
    TT_ASSERT_EQ(MOTION_EV_NONE, step(&m, 600)); /* new onset at 600 */
    TT_ASSERT_EQ(MOTION_EV_NONE, step(&m, 1599));
    TT_ASSERT_EQ(MOTION_EV_START, step(&m, 1600));
    TT_ASSERT_EQ(MS(600), ev.onset_us);
}

static void test_zone_edges(void)
{
    setup();
    ld_data_t lo = frame(1, 100, 50), hi = frame(1, 500, 50);
    ld_data_t below = frame(1, 99, 50), above = frame(1, 501, 50);
    step(&lo, 0);
    TT_ASSERT_EQ(MOTION_EV_START, step(&lo, 1000)); /* min inclusive */
    setup();
    step(&hi, 0);
    TT_ASSERT_EQ(MOTION_EV_START, step(&hi, 1000)); /* max inclusive */
    setup();
    step(&below, 0);
    TT_ASSERT_EQ(MOTION_EV_NONE, step(&below, 1000));
    TT_ASSERT_EQ(MOTION_EV_NONE, step(&below, 9000));
    setup();
    step(&above, 0);
    TT_ASSERT_EQ(MOTION_EV_NONE, step(&above, 1000));
    TT_ASSERT_EQ(MOTION_EV_NONE, step(&above, 9000));
    TT_ASSERT_EQ(0, motion_det_active(&det));
}

static void test_energy_edge(void)
{
    setup();
    ld_data_t at = frame(1, 300, 30), under = frame(1, 300, 29);
    step(&at, 0);
    TT_ASSERT_EQ(MOTION_EV_START, step(&at, 1000));
    setup();
    step(&under, 0);
    TT_ASSERT_EQ(MOTION_EV_NONE, step(&under, 1000));
    TT_ASSERT_EQ(0, motion_det_active(&det));
}

static void test_out_of_zone_frame_breaks_pending(void)
{
    setup();
    ld_data_t m = frame(1, 300, 50), out = frame(1, 600, 50);
    step(&m, 0);
    step(&out, 500);
    TT_ASSERT_EQ(MOTION_EV_NONE, step(&m, 1000)); /* onset restarted at 1000 */
    TT_ASSERT_EQ(MOTION_EV_NONE, step(&m, 1999));
    TT_ASSERT_EQ(MOTION_EV_START, step(&m, 2000));
}

static void test_disable_while_active(void)
{
    setup();
    ld_data_t m = frame(1, 300, 50);
    step(&m, 0);
    step(&m, 1000);
    step(&m, 1200);
    cfg.en = 0;
    TT_ASSERT_EQ(MOTION_EV_END, step(&m, 1300));
    TT_ASSERT_EQ(1, ev.event_no);
    TT_ASSERT_EQ(1200, ev.duration_ms);
    TT_ASSERT_EQ(0, motion_det_active(&det));
    TT_ASSERT_EQ(MOTION_EV_NONE, step(&m, 1400));
    /* re-enable: starts from scratch, event number continues */
    cfg.en = 1;
    TT_ASSERT_EQ(MOTION_EV_NONE, step(&m, 2000));
    TT_ASSERT_EQ(MOTION_EV_START, step(&m, 3000));
    TT_ASSERT_EQ(2, ev.event_no);
}

static void test_disable_pending_and_never_starts(void)
{
    setup();
    ld_data_t m = frame(1, 300, 50);
    step(&m, 0);
    cfg.en = 0;
    TT_ASSERT_EQ(MOTION_EV_NONE, step(&m, 500)); /* PENDING dropped silently */
    for (uint64_t t = 1000; t < 20000; t += 100)
        TT_ASSERT_EQ(MOTION_EV_NONE, step(&m, t));
    cfg.en = 1;
    TT_ASSERT_EQ(MOTION_EV_NONE, step(&m, 20000)); /* old onset forgotten */
    TT_ASSERT_EQ(MOTION_EV_NONE, step(&m, 20999));
    TT_ASSERT_EQ(MOTION_EV_START, step(&m, 21000));
}

static void test_cfg_change_mid_event(void)
{
    setup();
    ld_data_t m = frame(1, 300, 50), n = frame(0, 0, 0);
    step(&m, 0);
    step(&m, 1000);
    step(&m, 2000);
    cfg.tend_ms = 500; /* applies from the next frame */
    TT_ASSERT_EQ(MOTION_EV_END, step(&n, 2500));
    /* zone change while ACTIVE: the next frame is outside now, no longer extends */
    setup();
    step(&m, 0);
    step(&m, 1000);
    cfg.dmax_cm = 250;
    TT_ASSERT_EQ(MOTION_EV_NONE, step(&m, 2000)); /* not moving any more */
    TT_ASSERT_EQ(MOTION_EV_END, step(&m, 4000));  /* 3000 ms after the last moving (1000) */
    TT_ASSERT_EQ(1000, ev.duration_ms);
    /* tstart change while PENDING */
    setup();
    step(&m, 0);
    cfg.tstart_ms = 200;
    TT_ASSERT_EQ(MOTION_EV_START, step(&m, 200));
}

static void test_stats_during_active(void)
{
    setup();
    ld_data_t a = frame(1, 300, 40), hi = frame(1, 450, 90), lo = frame(1, 120, 90),
              n = frame(0, 0, 0);
    step(&a, 0);
    step(&a, 1000);
    step(&hi, 1500);
    step(&lo, 2000); /* energy tie 90: first (450) wins */
    ld_data_t big = frame(1, 200, 91);
    step(&big, 2500);
    TT_ASSERT_EQ(MOTION_EV_END, step(&n, 5500));
    TT_ASSERT_EQ(91, ev.max_energy);
    TT_ASSERT_EQ(200, ev.dist_cm);
    TT_ASSERT_EQ(120, ev.min_dist_cm);
    TT_ASSERT_EQ(450, ev.max_dist_cm);
    TT_ASSERT_EQ(2500, ev.duration_ms);
}

static void test_tie_keeps_first(void)
{
    setup();
    ld_data_t a = frame(1, 450, 90), b = frame(1, 120, 90), n = frame(0, 0, 0);
    step(&a, 0);
    step(&b, 500);
    step(&b, 1000);
    step(&n, 4000);
    TT_ASSERT_EQ(450, ev.dist_cm);
}

static void test_event_numbers_and_state_reset(void)
{
    setup();
    ld_data_t m = frame(1, 300, 50), lowe = frame(1, 300, 31), n = frame(0, 0, 0);
    for (uint32_t i = 1; i <= 3; i++) {
        uint64_t t0 = i * 100000u;
        step(&m, t0);
        TT_ASSERT_EQ(MOTION_EV_START, step(&m, t0 + 1000));
        TT_ASSERT_EQ(i, ev.event_no);
        TT_ASSERT_EQ(MOTION_EV_END, step(&n, t0 + 4000));
        TT_ASSERT_EQ(i, ev.event_no);
        TT_ASSERT_EQ(MS(t0), ev.onset_us);
        TT_ASSERT_EQ(1000, ev.duration_ms);
        /* stats of a new event do not inherit the old ones */
        TT_ASSERT_EQ(50, ev.max_energy);
    }
    step(&lowe, 500000);
    step(&lowe, 501000);
    step(&n, 510000);
    TT_ASSERT_EQ(31, ev.max_energy);
    TT_ASSERT_EQ(4, ev.event_no);
}

static void test_event_with_gap_longer_than_end(void)
{
    setup();
    ld_data_t m = frame(1, 300, 50);
    step(&m, 0);
    step(&m, 1000);
    /* no call for 10 s, then a moving frame: the old event ends, the frame is dropped */
    TT_ASSERT_EQ(MOTION_EV_END, step(&m, 11000));
    TT_ASSERT_EQ(MOTION_EV_NONE, step(&m, 11100));
    TT_ASSERT_EQ(MOTION_EV_START, step(&m, 12100));
    TT_ASSERT_EQ(2, ev.event_no);
}

static void test_now_before_onset(void)
{
    setup();
    ld_data_t m = frame(1, 300, 50);
    step(&m, 10000);
    TT_ASSERT_EQ(MOTION_EV_NONE, step(&m, 5000)); /* time went back: 0 elapsed */
    TT_ASSERT_EQ(MOTION_EV_NONE, step(&m, 10999));
    TT_ASSERT_EQ(MOTION_EV_START, step(&m, 11000));
    /* ACTIVE, then time goes back: no END, duration not negative */
    TT_ASSERT_EQ(MOTION_EV_NONE, step(NULL, 100));
    TT_ASSERT_EQ(1, motion_det_active(&det));
}

static void test_now_before_last_active(void)
{
    setup();
    ld_data_t m = frame(1, 300, 50);
    step(&m, 10000);
    step(&m, 11000);
    TT_ASSERT_EQ(MOTION_EV_NONE, step(&m, 500)); /* now < last: 0 elapsed, not an END */
    TT_ASSERT_EQ(1, motion_det_active(&det));
}

static void test_null_args(void)
{
    setup();
    ld_data_t m = frame(1, 300, 50);
    motion_det_init(NULL);
    TT_ASSERT_EQ(0, motion_det_active(NULL));
    TT_ASSERT_EQ(MOTION_EV_NONE, motion_det_step(NULL, &cfg, &m, 0, &ev));
    TT_ASSERT_EQ(MOTION_EV_NONE, motion_det_step(&det, NULL, &m, 0, &ev));
    TT_ASSERT_EQ(MOTION_EV_NONE, motion_det_step(&det, &cfg, &m, 0, NULL));
    /* nothing was recorded by the rejected calls */
    TT_ASSERT_EQ(MOTION_EV_NONE, step(&m, 1000));
    TT_ASSERT_EQ(MOTION_EV_NONE, step(&m, 1999));
    TT_ASSERT_EQ(MOTION_EV_START, step(&m, 2000));
}

static void test_ev_untouched_without_event(void)
{
    setup();
    ld_data_t m = frame(1, 300, 50);
    step(&m, 0);
    step(&m, 500);
    uint8_t pat[sizeof ev];
    memset(pat, 0xAA, sizeof pat);
    TT_ASSERT_EQ(0, memcmp(pat, &ev, sizeof ev));
}

int main(void)
{
    TT_RUN(test_defaults);
    TT_RUN(test_start_boundary);
    TT_RUN(test_start_fields);
    TT_RUN(test_gap_in_pending);
    TT_RUN(test_non_moving_at_start_time_resets);
    TT_RUN(test_end_boundary);
    TT_RUN(test_still_frames_do_not_extend);
    TT_RUN(test_still_only_never_starts);
    TT_RUN(test_state_both_counts);
    TT_RUN(test_null_ends_active);
    TT_RUN(test_null_resets_pending);
    TT_RUN(test_zone_edges);
    TT_RUN(test_energy_edge);
    TT_RUN(test_out_of_zone_frame_breaks_pending);
    TT_RUN(test_disable_while_active);
    TT_RUN(test_disable_pending_and_never_starts);
    TT_RUN(test_cfg_change_mid_event);
    TT_RUN(test_stats_during_active);
    TT_RUN(test_tie_keeps_first);
    TT_RUN(test_event_numbers_and_state_reset);
    TT_RUN(test_event_with_gap_longer_than_end);
    TT_RUN(test_now_before_onset);
    TT_RUN(test_now_before_last_active);
    TT_RUN(test_null_args);
    TT_RUN(test_ev_untouched_without_event);
    return TT_RESULT();
}
