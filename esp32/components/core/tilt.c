#include "tilt.h"

#include <math.h>
#include <string.h>

#define RAD2DEG 57.295779513082321f
#define DDPS_TO_RADPS 0.0017453292519943296f /* 0.1 deg/s -> rad/s */
#define ACC_MIN_MG 500.0f                    /* usable |a|: 0.5 g .. 1.5 g */
#define ACC_MAX_MG 1500.0f
#define GIMBAL_EPS 1e-3f /* hypot(gy, gz) below this: roll undefined, hold it */
#define BLEND_MIN 1e-3f  /* |alpha*g + (1-alpha)*u| below this: take u */

static float clamp_dt(float dt)
{
    return (dt > 0.0f && dt < 10.0f) ? dt : TILT_DT_DEFAULT; /* false for NaN */
}

void tilt_set_dt(tilt_t *t, float dt_s)
{
    t->dt = clamp_dt(dt_s);
}

void tilt_init(tilt_t *t, float alpha, float dt_s)
{
    memset(t, 0, sizeof(*t));
    t->alpha = (alpha >= 0.0f) ? (alpha <= 1.0f ? alpha : 1.0f) : 0.0f; /* NaN -> 0 */
    t->dt = clamp_dt(dt_s);
    t->g[2] = 1.0f;
}

void tilt_map_axes(const int32_t sensor[3], int32_t body[3])
{
    body[0] = TILT_MAP_SIGN_X * sensor[TILT_MAP_SRC_X];
    body[1] = TILT_MAP_SIGN_Y * sensor[TILT_MAP_SRC_Y];
    body[2] = TILT_MAP_SIGN_Z * sensor[TILT_MAP_SRC_Z];
}

static void normalize3(float v[3])
{
    float n = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    if (n > 0.0f) {
        v[0] /= n;
        v[1] /= n;
        v[2] /= n;
    }
}

/* dg/dt = -w x g: rotate g by the rotation vector -w*dt (Rodrigues). */
static void propagate(tilt_t *t, const int32_t gyr_ddps[3])
{
    float w[3] = {(float)gyr_ddps[0] * DDPS_TO_RADPS, (float)gyr_ddps[1] * DDPS_TO_RADPS,
                  (float)gyr_ddps[2] * DDPS_TO_RADPS};
    float wn = sqrtf(w[0] * w[0] + w[1] * w[1] + w[2] * w[2]);
    float theta = wn * t->dt;
    if (!(theta > 1e-9f)) {
        return;
    }
    float k[3] = {-w[0] / wn, -w[1] / wn, -w[2] / wn};
    float c = cosf(theta), s = sinf(theta);
    float kg = k[0] * t->g[0] + k[1] * t->g[1] + k[2] * t->g[2];
    float cr[3] = {k[1] * t->g[2] - k[2] * t->g[1], k[2] * t->g[0] - k[0] * t->g[2],
                   k[0] * t->g[1] - k[1] * t->g[0]};
    for (int i = 0; i < 3; i++) {
        t->g[i] = t->g[i] * c + cr[i] * s + k[i] * kg * (1.0f - c);
    }
    normalize3(t->g);
}

static void update_angles(tilt_t *t)
{
    float hyp = hypotf(t->g[1], t->g[2]);
    t->pitch_deg = atan2f(t->g[0], hyp) * RAD2DEG;
    if (hyp > GIMBAL_EPS) {
        t->roll_deg = atan2f(t->g[1], t->g[2]) * RAD2DEG;
    } /* else: pole, keep the previous roll */
}

void tilt_update(tilt_t *t, const int32_t acc_mg[3], const int32_t gyr_ddps[3])
{
    for (int i = 0; i < 3; i++) {
        t->sum_acc[i] += acc_mg[i];
        t->sum_gyr[i] += gyr_ddps[i];
    }
    t->n++;

    if (t->init) {
        propagate(t, gyr_ddps);
    }
    float a[3] = {(float)acc_mg[0], (float)acc_mg[1], (float)acc_mg[2]};
    float an = sqrtf(a[0] * a[0] + a[1] * a[1] + a[2] * a[2]);
    if (an >= ACC_MIN_MG && an <= ACC_MAX_MG) {
        for (int i = 0; i < 3; i++) {
            a[i] /= an;
        }
        if (!t->init) {
            memcpy(t->g, a, sizeof(a));
            t->init = true;
        } else {
            float b[3];
            for (int i = 0; i < 3; i++) {
                b[i] = t->alpha * t->g[i] + (1.0f - t->alpha) * a[i];
            }
            float bn = sqrtf(b[0] * b[0] + b[1] * b[1] + b[2] * b[2]);
            if (bn > BLEND_MIN) {
                for (int i = 0; i < 3; i++) {
                    t->g[i] = b[i] / bn;
                }
            } else {
                memcpy(t->g, a, sizeof(a));
            }
        }
    }
    if (t->init) {
        update_angles(t);
    }
}

bool tilt_valid(const tilt_t *t)
{
    return t->init;
}

float tilt_pitch_deg(const tilt_t *t)
{
    return t->pitch_deg;
}

float tilt_roll_deg(const tilt_t *t)
{
    return t->roll_deg;
}

static int16_t mean_i16(int64_t sum, uint32_t n)
{
    if (n == 0) {
        return 0;
    }
    int64_t half = (int64_t)n / 2;
    int64_t q = (sum >= 0) ? (sum + half) / (int64_t)n : -((-sum + half) / (int64_t)n);
    if (q > INT16_MAX) {
        return INT16_MAX;
    }
    if (q < INT16_MIN) {
        return INT16_MIN;
    }
    return (int16_t)q;
}

static int16_t cdeg_i16(float deg)
{
    float v = roundf(deg * 100.0f);
    if (!(v < 32767.0f)) { /* also catches NaN */
        return v > 0.0f ? INT16_MAX : 0;
    }
    if (v < -32768.0f) {
        return INT16_MIN;
    }
    return (int16_t)v;
}

void tilt_take_record(tilt_t *t, rec_imu_t *out)
{
    memset(out, 0, sizeof(*out));
    for (int i = 0; i < 3; i++) {
        out->acc_mg[i] = mean_i16(t->sum_acc[i], t->n);
        out->gyr_ddps[i] = mean_i16(t->sum_gyr[i], t->n);
    }
    if (t->init) {
        out->pitch_cdeg = cdeg_i16(t->pitch_deg);
        out->roll_cdeg = cdeg_i16(t->roll_deg);
    }
    out->n_samples = (t->n > 255u) ? 255u : (uint8_t)t->n;
    out->status = (t->init && t->n > 0) ? IMU_STATUS_VALID : 0u;
    memset(t->sum_acc, 0, sizeof(t->sum_acc));
    memset(t->sum_gyr, 0, sizeof(t->sum_gyr));
    t->n = 0;
}
