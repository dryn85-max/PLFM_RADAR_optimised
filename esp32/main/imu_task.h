/* IMU link (GY-BMI160 on I2C): sampling task, tilt filter, 10 Hz ring producer,
 * latest snapshot. Axis and sign conventions: components/core/tilt.h. */
#ifndef IMU_TASK_H
#define IMU_TASK_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "rec_payload.h"

typedef enum {
    IMU_LINK_ABSENT = 0, /* no BMI160 has answered since boot (not connected / wrong address) */
    IMU_LINK_ERROR = 1,  /* it answered before, but reads are failing or it is being re-initialised */
    IMU_LINK_OK = 2      /* configured and delivering samples */
} imu_link_t;

typedef struct {
    bool valid;         /* false until the first 100 ms record */
    rec_imu_t rec;      /* latest record: averaged acc/gyro, pitch/roll in 0.01 deg, status */
    uint64_t time_us;   /* esp_timer_get_time() at the end of its window */
    uint8_t addr;       /* 7-bit I2C address in use (0 if none yet) */
    uint32_t i2c_errors; /* failed I2C transfers (reads/configuration), since boot */
    uint32_t reinits;   /* completed (re-)initialisations of the sensor */
} imu_snapshot_t;

/* Create the I2C bus and start the IMU task. Needs ld2410_start() to have
 * created the ring. Safe without a sensor attached (status stays IMU_LINK_ABSENT). */
esp_err_t imu_start(void);

/* Copy the latest record. Returns false until the first one. */
bool imu_get_snapshot(imu_snapshot_t *out);

imu_link_t imu_status(void);

#endif
