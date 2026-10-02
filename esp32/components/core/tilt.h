/* Tilt estimation (plain C11): complementary filter on the gravity direction,
 * plus the averager that produces the 10 Hz `imu` record values.
 *
 * Body frame (the radar frame; every input is expressed in it):
 *   +X = radar boresight (forward), +Y = left, +Z = up (right-handed).
 *   Level and at rest the accelerometer reads (0, 0, +1000 mg): it measures
 *   the reaction to gravity, i.e. the "up" direction in body axes.
 *   Gyro rates are right-handed rotations about these axes (0.1 deg/s units).
 * Angles:
 *   pitch  positive = nose up (boresight above the horizon), range [-90, +90].
 *   roll   positive = right side down (left side up), range (-180, +180].
 *   Static: pitch = atan2(ax, hypot(ay, az)), roll = atan2(ay, az).
 *
 * Filter: the state is a unit vector g = "up" in body axes (no Euler angles
 * are integrated, so there is no tan(pitch) blow-up). Per sample g is rotated
 * by the gyro (dg/dt = -w x g, exact Rodrigues rotation), then
 *   g = normalize(alpha * g + (1 - alpha) * a / |a|).
 * alpha in [0, 1] is the weight of the gyro-propagated estimate (alpha = 1
 * disables the accelerometer); time constant tau = alpha * dt / (1 - alpha).
 * The first usable accelerometer sample initialises g (no convergence wait).
 * An accelerometer sample whose magnitude is outside 0.5 g .. 1.5 g (zero
 * vector, free fall, strong shock) is not used for correction; the gyro still
 * propagates. Until the first usable sample the filter is not valid.
 *
 * Gimbal handling: at pitch +-90 deg the roll angle is undefined (ay and az
 * both ~0). When hypot(gy, gz) < 1e-3 (pitch > 89.94 deg) the previous roll is
 * kept (0 if there is none), pitch stays exact. g keeps rotating correctly
 * through the pole, so roll recovers as soon as the board leaves it.
 *
 * Averager: every tilt_update() also accumulates the raw mapped samples;
 * tilt_take_record() returns their means (rounded to nearest, ties away from
 * zero, saturated to int16), the current pitch/roll in 0.01 deg, n_samples
 * (saturated to 255) and status bit0 (valid: filter initialised and at least
 * one sample in the window), then starts a new window. */
#ifndef TILT_H
#define TILT_H

#include <stdbool.h>
#include <stdint.h>

#include "rec_payload.h"

/* Sensor-to-body axis mapping, a firmware constant: body[i] = SIGN_i * sensor[SRC_i].
 * Default = the sensor is mounted so that its X axis points along the boresight,
 * its Z axis up (so its Y axis is left, as the BMI160 axes are right-handed).
 * Change these six values for another mounting. VERIFY on the real board. */
#define TILT_MAP_SRC_X 0
#define TILT_MAP_SRC_Y 1
#define TILT_MAP_SRC_Z 2
#define TILT_MAP_SIGN_X 1
#define TILT_MAP_SIGN_Y 1
#define TILT_MAP_SIGN_Z 1

#define TILT_ALPHA_DEFAULT 0.98f
#define TILT_DT_DEFAULT 0.01f

typedef struct {
    float alpha; /* gyro weight, clamped to [0, 1] by tilt_init */
    float dt;    /* seconds per sample, > 0 */
    float g[3];  /* unit "up" vector, body axes */
    bool init;
    float pitch_deg;
    float roll_deg;
    int64_t sum_acc[3];
    int64_t sum_gyr[3];
    uint32_t n;
} tilt_t;

/* alpha is clamped to [0, 1]; dt <= 0 (or NaN) falls back to TILT_DT_DEFAULT. */
void tilt_init(tilt_t *t, float alpha, float dt_s);
/* Change dt for the following updates (same rule as tilt_init). */
void tilt_set_dt(tilt_t *t, float dt_s);

/* sensor axes -> body axes (mg or 0.1 deg/s, any int32 values). */
void tilt_map_axes(const int32_t sensor[3], int32_t body[3]);

/* One sample in body axes: acc_mg (mg), gyr_ddps (0.1 deg/s). */
void tilt_update(tilt_t *t, const int32_t acc_mg[3], const int32_t gyr_ddps[3]);

bool tilt_valid(const tilt_t *t);
float tilt_pitch_deg(const tilt_t *t);
float tilt_roll_deg(const tilt_t *t);

/* Close the averaging window: fill *out and clear the accumulators. */
void tilt_take_record(tilt_t *t, rec_imu_t *out);

#endif
