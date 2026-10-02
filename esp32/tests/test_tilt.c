/* Host tests for the tilt filter and the 10 Hz averager. */
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "tinytest.h"
#include "tilt.h"

#define PI_F 3.14159265358979f
#define PI_D 3.14159265358979323846
#define D2R (PI_F / 180.0f)

/* Accelerometer reading (mg) of a static board at the given pitch / roll. */
static void acc_for(double pitch_deg, double roll_deg, int32_t a[3])
{
    double p = pitch_deg * PI_D / 180.0, r = roll_deg * PI_D / 180.0;
    a[0] = (int32_t)lround(1000.0 * sin(p));
    a[1] = (int32_t)lround(1000.0 * cos(p) * sin(r));
    a[2] = (int32_t)lround(1000.0 * cos(p) * cos(r));
}

static const int32_t ZERO3[3] = {0, 0, 0};

static int near(double a, double b, double tol) { return fabs(a - b) <= tol; }

static double wrap180(double d)
{
    while (d > 180.0) d -= 360.0;
    while (d <= -180.0) d += 360.0;
    return d;
}

static void check_static(double pitch, double roll, double exp_pitch, double exp_roll)
{
    tilt_t t;
    int32_t a[3];
    tilt_init(&t, 0.98f, 0.01f);
    acc_for(pitch, roll, a);
    tilt_update(&t, a, ZERO3);
    TT_ASSERT(tilt_valid(&t));
    TT_ASSERT(!isnan(tilt_pitch_deg(&t)) && !isnan(tilt_roll_deg(&t)));
    TT_ASSERT(near(tilt_pitch_deg(&t), exp_pitch, 0.05));
    TT_ASSERT(near(wrap180(tilt_roll_deg(&t) - exp_roll), 0.0, 0.05));
}

static void test_static_pitch(void)
{
    const double p[] = {0, 30, -30, 89.9, -89.9, 90, -90};
    for (unsigned i = 0; i < sizeof(p) / sizeof(p[0]); i++) {
        check_static(p[i], 0, p[i], 0);
    }
}

static void test_static_roll_and_combined(void)
{
    check_static(0, 45, 0, 45);
    check_static(0, -45, 0, -45);
    check_static(20, -40, 20, -40);
    check_static(-60, 70, -60, 70);
    check_static(0, 179, 0, 179);
    check_static(0, -179, 0, -179);
    check_static(0, 180, 0, 180); /* +-180 are the same angle */
}

static void test_sign_convention(void)
{
    tilt_t t;
    int32_t a[3] = {500, 0, 866}; /* boresight up -> pitch positive */
    tilt_init(&t, 0.98f, 0.01f);
    tilt_update(&t, a, ZERO3);
    TT_ASSERT(tilt_pitch_deg(&t) > 29.0f);
    int32_t b[3] = {0, 500, 866}; /* left side up == right side down -> roll positive */
    tilt_init(&t, 0.98f, 0.01f);
    tilt_update(&t, b, ZERO3);
    TT_ASSERT(tilt_roll_deg(&t) > 29.0f);
}

static void test_not_valid_before_data(void)
{
    tilt_t t;
    tilt_init(&t, 0.98f, 0.01f);
    TT_ASSERT(!tilt_valid(&t));
    TT_ASSERT(tilt_pitch_deg(&t) == 0.0f && tilt_roll_deg(&t) == 0.0f);
}

/* Gyro-only (alpha = 1): constant rate over 1 s from level. */
static void test_gyro_only_step(void)
{
    tilt_t t;
    int32_t lvl[3] = {0, 0, 1000};
    int32_t gx[3] = {100, 0, 0};  /* +10 deg/s about +X: right side down */
    tilt_init(&t, 1.0f, 0.01f);
    tilt_update(&t, lvl, ZERO3);
    for (int i = 0; i < 100; i++) tilt_update(&t, lvl, gx);
    TT_ASSERT(near(tilt_roll_deg(&t), 10.0, 0.05));
    TT_ASSERT(near(tilt_pitch_deg(&t), 0.0, 0.05));

    int32_t gy[3] = {0, -100, 0}; /* -10 deg/s about +Y (left): nose up */
    tilt_init(&t, 1.0f, 0.01f);
    tilt_update(&t, lvl, ZERO3);
    for (int i = 0; i < 100; i++) tilt_update(&t, lvl, gy);
    TT_ASSERT(near(tilt_pitch_deg(&t), 10.0, 0.05));
    TT_ASSERT(near(tilt_roll_deg(&t), 0.0, 0.05));

    int32_t gz[3] = {0, 0, 300}; /* yaw rate only: level stays level */
    tilt_init(&t, 1.0f, 0.01f);
    tilt_update(&t, lvl, ZERO3);
    for (int i = 0; i < 200; i++) tilt_update(&t, lvl, gz);
    TT_ASSERT(near(tilt_pitch_deg(&t), 0.0, 0.05) && near(tilt_roll_deg(&t), 0.0, 0.05));
}

/* A gyro bias against a level accelerometer settles at alpha*rate*dt/(1-alpha). */
static void test_complementary_steady_state(void)
{
    tilt_t t;
    int32_t lvl[3] = {0, 0, 1000};
    int32_t gx[3] = {100, 0, 0};
    tilt_init(&t, 0.98f, 0.01f);
    tilt_update(&t, lvl, ZERO3);
    for (int i = 0; i < 2000; i++) tilt_update(&t, lvl, gx);
    TT_ASSERT(near(tilt_roll_deg(&t), 4.9, 0.3));
}

static void test_accel_only_convergence(void)
{
    tilt_t t;
    int32_t lvl[3] = {0, 0, 1000};
    int32_t a[3];
    acc_for(30, -20, a);
    tilt_init(&t, 0.98f, 0.01f);
    tilt_update(&t, lvl, ZERO3);
    for (int i = 0; i < 600; i++) tilt_update(&t, a, ZERO3);
    TT_ASSERT(near(tilt_pitch_deg(&t), 30.0, 0.1));
    TT_ASSERT(near(tilt_roll_deg(&t), -20.0, 0.1));
}

/* Gyro integration through +-180 roll wraps instead of running to 181. */
static void test_roll_wrap_by_gyro(void)
{
    tilt_t t;
    int32_t a[3], gx[3] = {100, 0, 0};
    acc_for(0, 179, a);
    tilt_init(&t, 1.0f, 0.01f);
    tilt_update(&t, a, ZERO3);
    for (int i = 0; i < 20; i++) tilt_update(&t, a, gx); /* +2 deg */
    TT_ASSERT(near(tilt_roll_deg(&t), -179.0, 0.1));
    int32_t gn[3] = {-100, 0, 0};
    for (int i = 0; i < 20; i++) tilt_update(&t, a, gn);
    TT_ASSERT(near(tilt_roll_deg(&t), 179.0, 0.1));
}

/* Rotating through the pole: no NaN, pitch folds back, roll flips by 180. */
static void test_through_the_pole(void)
{
    tilt_t t;
    int32_t a[3], gy[3] = {0, -100, 0}; /* nose up 10 deg/s */
    acc_for(80, 0, a);
    tilt_init(&t, 1.0f, 0.01f);
    tilt_update(&t, a, ZERO3);
    for (int i = 0; i < 200; i++) {
        tilt_update(&t, a, gy);
        TT_ASSERT(!isnan(tilt_pitch_deg(&t)) && !isnan(tilt_roll_deg(&t)));
        TT_ASSERT(fabsf(tilt_pitch_deg(&t)) <= 90.0f);
    }
    TT_ASSERT(near(tilt_pitch_deg(&t), 80.0, 0.1)); /* 80 + 20 = 100 -> 80 */
    TT_ASSERT(fabsf(tilt_roll_deg(&t)) > 179.5f);
}

/* At the pole the roll is held, not reset or NaN, while g converges to +X. */
static void test_gimbal_hold(void)
{
    tilt_t t;
    int32_t a[3], up[3] = {1000, 0, 0};
    acc_for(0, 40, a);
    tilt_init(&t, 0.9f, 0.01f);
    tilt_update(&t, a, ZERO3);
    for (int i = 0; i < 3000; i++) tilt_update(&t, up, ZERO3);
    TT_ASSERT(near(tilt_pitch_deg(&t), 90.0, 0.01));
    TT_ASSERT(near(tilt_roll_deg(&t), 40.0, 0.5));
    TT_ASSERT(!isnan(tilt_roll_deg(&t)));
}

static void test_invalid_input(void)
{
    tilt_t t;
    int32_t a[3];
    /* zero vector before init: stays invalid */
    tilt_init(&t, 0.98f, 0.01f);
    tilt_update(&t, ZERO3, ZERO3);
    TT_ASSERT(!tilt_valid(&t));
    TT_ASSERT(tilt_pitch_deg(&t) == 0.0f && tilt_roll_deg(&t) == 0.0f);
    int32_t huge[3] = {100000, 0, 0}; /* far outside 0.5..1.5 g */
    tilt_update(&t, huge, ZERO3);
    TT_ASSERT(!tilt_valid(&t));
    /* later samples work */
    acc_for(25, 10, a);
    tilt_update(&t, a, ZERO3);
    TT_ASSERT(tilt_valid(&t));
    TT_ASSERT(near(tilt_pitch_deg(&t), 25.0, 0.1) && near(tilt_roll_deg(&t), 10.0, 0.1));
    /* zero / shock accel after init: attitude stays (alpha = 0.98 filter, no jump) */
    for (int i = 0; i < 50; i++) tilt_update(&t, ZERO3, ZERO3);
    tilt_update(&t, huge, ZERO3);
    TT_ASSERT(near(tilt_pitch_deg(&t), 25.0, 0.1) && near(tilt_roll_deg(&t), 10.0, 0.1));
    TT_ASSERT(tilt_valid(&t));
}

static void test_config_clamps(void)
{
    tilt_t t;
    int32_t lvl[3] = {0, 0, 1000}, gx[3] = {100, 0, 0};
    tilt_init(&t, 5.0f, 0.0f); /* alpha -> 1, dt -> default */
    TT_ASSERT(t.alpha == 1.0f && t.dt == TILT_DT_DEFAULT);
    tilt_update(&t, lvl, ZERO3);
    for (int i = 0; i < 100; i++) tilt_update(&t, lvl, gx);
    TT_ASSERT(near(tilt_roll_deg(&t), 10.0, 0.05));
    tilt_init(&t, -1.0f, NAN);
    TT_ASSERT(t.alpha == 0.0f && t.dt == TILT_DT_DEFAULT);
    tilt_set_dt(&t, 0.02f);
    TT_ASSERT(t.dt == 0.02f);
    tilt_set_dt(&t, -3.0f);
    TT_ASSERT(t.dt == TILT_DT_DEFAULT);
    /* alpha 0: the accelerometer wins at once */
    int32_t a[3];
    acc_for(30, 0, a);
    tilt_update(&t, lvl, ZERO3);
    tilt_update(&t, a, gx);
    TT_ASSERT(near(tilt_pitch_deg(&t), 30.0, 0.05));
}

static void test_map_axes(void)
{
    int32_t s[3] = {1, -2, 3}, b[3];
    tilt_map_axes(s, b);
    TT_ASSERT_EQ(TILT_MAP_SIGN_X * s[TILT_MAP_SRC_X], b[0]);
    TT_ASSERT_EQ(TILT_MAP_SIGN_Y * s[TILT_MAP_SRC_Y], b[1]);
    TT_ASSERT_EQ(TILT_MAP_SIGN_Z * s[TILT_MAP_SRC_Z], b[2]);
}

static void test_averaging(void)
{
    tilt_t t;
    rec_imu_t r;
    int32_t lvl[3] = {0, 0, 1000};
    tilt_init(&t, 0.98f, 0.01f);
    for (int i = 0; i < 10; i++) {
        int32_t a[3] = {i * 10, -i * 10, 1000};
        int32_t g[3] = {i, -2 * i, 100};
        tilt_update(&t, a, g);
    }
    tilt_take_record(&t, &r);
    TT_ASSERT_EQ(10, r.n_samples);
    TT_ASSERT_EQ(IMU_STATUS_VALID, r.status);
    TT_ASSERT_EQ(45, r.acc_mg[0]);   /* mean 45.0 */
    TT_ASSERT_EQ(-45, r.acc_mg[1]);
    TT_ASSERT_EQ(1000, r.acc_mg[2]);
    TT_ASSERT_EQ(5, r.gyr_ddps[0]);  /* mean 4.5 -> 5 (ties away from zero) */
    TT_ASSERT_EQ(-9, r.gyr_ddps[1]); /* mean -9.0 */
    TT_ASSERT_EQ(100, r.gyr_ddps[2]);
    TT_ASSERT(r.pitch_cdeg == (int16_t)lroundf(tilt_pitch_deg(&t) * 100.0f));
    TT_ASSERT(r.roll_cdeg == (int16_t)lroundf(tilt_roll_deg(&t) * 100.0f));

    /* window restarts */
    tilt_update(&t, lvl, ZERO3);
    tilt_update(&t, lvl, ZERO3);
    tilt_take_record(&t, &r);
    TT_ASSERT_EQ(2, r.n_samples);
    TT_ASSERT_EQ(1000, r.acc_mg[2]);
    TT_ASSERT_EQ(0, r.gyr_ddps[0]);

    /* empty window */
    tilt_take_record(&t, &r);
    TT_ASSERT_EQ(0, r.n_samples);
    TT_ASSERT_EQ(0, r.status);
    TT_ASSERT(r.acc_mg[2] == 0 && r.gyr_ddps[2] == 0);

    /* negative half rounds away from zero */
    int32_t g1[3] = {-1, 0, 0}, g0[3] = {0, 0, 0};
    tilt_update(&t, lvl, g1);
    tilt_update(&t, lvl, g0);
    tilt_take_record(&t, &r);
    TT_ASSERT_EQ(-1, r.gyr_ddps[0]); /* -0.5 -> -1 */
}

static void test_n_samples_saturates(void)
{
    tilt_t t;
    rec_imu_t r;
    int32_t lvl[3] = {0, 0, 1000};
    tilt_init(&t, 0.98f, 0.01f);
    for (int i = 0; i < 300; i++) tilt_update(&t, lvl, ZERO3);
    tilt_take_record(&t, &r);
    TT_ASSERT_EQ(255, r.n_samples);
    TT_ASSERT_EQ(1000, r.acc_mg[2]); /* mean uses the true count */
}

static void test_int16_saturation(void)
{
    tilt_t t;
    rec_imu_t r;
    tilt_init(&t, 0.98f, 0.01f);
    int32_t a[3] = {100000, -100000, 32767};
    int32_t g[3] = {2000000000, -2000000000, -40000};
    for (int i = 0; i < 4; i++) tilt_update(&t, a, g);
    tilt_take_record(&t, &r);
    TT_ASSERT_EQ(32767, r.acc_mg[0]);
    TT_ASSERT_EQ(-32768, r.acc_mg[1]);
    TT_ASSERT_EQ(32767, r.acc_mg[2]);
    TT_ASSERT_EQ(32767, r.gyr_ddps[0]);
    TT_ASSERT_EQ(-32768, r.gyr_ddps[1]);
    TT_ASSERT_EQ(-32768, r.gyr_ddps[2]);
    TT_ASSERT_EQ(4, r.n_samples);
}

static void test_record_angles(void)
{
    tilt_t t;
    rec_imu_t r;
    int32_t a[3];
    acc_for(12.34, -56.78, a);
    tilt_init(&t, 0.98f, 0.01f);
    tilt_update(&t, a, ZERO3);
    tilt_take_record(&t, &r);
    TT_ASSERT(near(r.pitch_cdeg, 1234, 8));
    TT_ASSERT(near(r.roll_cdeg, -5678, 8));
    acc_for(0, 180, a);
    tilt_init(&t, 0.98f, 0.01f);
    tilt_update(&t, a, ZERO3);
    tilt_take_record(&t, &r);
    TT_ASSERT(r.roll_cdeg == 18000 || r.roll_cdeg == -18000);
}

int main(void)
{
    TT_RUN(test_static_pitch);
    TT_RUN(test_static_roll_and_combined);
    TT_RUN(test_sign_convention);
    TT_RUN(test_not_valid_before_data);
    TT_RUN(test_gyro_only_step);
    TT_RUN(test_complementary_steady_state);
    TT_RUN(test_accel_only_convergence);
    TT_RUN(test_roll_wrap_by_gyro);
    TT_RUN(test_through_the_pole);
    TT_RUN(test_gimbal_hold);
    TT_RUN(test_invalid_input);
    TT_RUN(test_config_clamps);
    TT_RUN(test_map_axes);
    TT_RUN(test_averaging);
    TT_RUN(test_n_samples_saturates);
    TT_RUN(test_int16_saturation);
    TT_RUN(test_record_angles);
    return TT_RESULT();
}
