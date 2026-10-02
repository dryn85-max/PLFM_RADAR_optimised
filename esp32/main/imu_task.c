#include "imu_task.h"

#include <inttypes.h>
#include <string.h>

#include "driver/i2c_master.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "ld2410_task.h"
#include "rec_payload.h"
#include "tilt.h"

/* ---- Wiring and bus ------------------------------------------------------ */
#define IMU_I2C_PORT I2C_NUM_0
#define IMU_PIN_SDA 8
#define IMU_PIN_SCL 9
#define IMU_I2C_HZ 400000
#define IMU_I2C_TIMEOUT_MS 20
/* The internal pull-ups (about 45 kOhm) are enabled as a fallback; GY-BMI160
 * boards normally carry their own pull-ups, which then dominate.
 * VERIFY on the real board (bus rise time at 400 kHz; fall back to 100 kHz if
 * the board has no external pull-ups). */
#define IMU_INTERNAL_PULLUP true

/* ---- Timing and policy ---------------------------------------------------- */
#define IMU_SAMPLE_PERIOD_MS 10 /* ~100 Hz polling, matches ODR 100 Hz */
#define IMU_RECORD_SAMPLES 10   /* 10 samples = one 100 ms record (10 Hz) */
#define IMU_FAIL_LIMIT 5        /* consecutive failed reads before the sensor is re-initialised */
#define IMU_BACKOFF_MS 1000     /* wait before a re-probe/re-init (also while absent) */
#define IMU_LOG_US 10000000
#define IMU_WARN_EVERY 30       /* rounds (x IMU_BACKOFF_MS) between repeated warnings */
#define IMU_DT_MIN_S 0.005f     /* measured sample interval is clamped to this window */
#define IMU_DT_MAX_S 0.030f
/* Core 1 with the LD2410C (prio 5) and GPS (prio 4) tasks, but below both: the
 * LD2410C task preempts it at any time. Core 1 has no Wi-Fi/lwIP tasks, so the
 * 10 ms sampling grid is not disturbed by their bursts. Load: a 12-byte burst
 * (~0.3 ms at 400 kHz, interrupt driven, the task blocks meanwhile) per 10 ms. */
#define IMU_TASK_PRIO 3
#define IMU_TASK_CORE 1
#define IMU_TASK_STACK 5120 /* float printf in the 10 s log line */

/* ---- BMI160 (Bosch BST-BMI160-DS000). Every value below: VERIFY, the BMI160
 * datasheet is not in hardware/datasheets/. Section numbers are from memory of
 * the datasheet's section 2.11 "Register description" / section 1 specification. */
/* I2C 7-bit address: SDO low = 0x68, SDO high = 0x69 (Sec. 3.1 I2C interface).
 * VERIFY: BMI160 datasheet not in hardware/datasheets/ */
#define BMI160_ADDR_SDO_LOW 0x68
#define BMI160_ADDR_SDO_HIGH 0x69
/* Reg 0x00 CHIP_ID, fixed value 0xD1 (Sec. 2.11.1).
 * VERIFY: BMI160 datasheet not in hardware/datasheets/ */
#define BMI160_REG_CHIP_ID 0x00
#define BMI160_CHIP_ID_VALUE 0xD1
/* Reg 0x02 ERR_REG, error flags, read-clear (Sec. 2.11.2).
 * VERIFY: BMI160 datasheet not in hardware/datasheets/ */
#define BMI160_REG_ERR_REG 0x02
/* Reg 0x03 PMU_STATUS: bits [5:4] acc_pmu_status, [3:2] gyr_pmu_status,
 * value 0b01 = normal mode (Sec. 2.11.3, Table "PMU_STATUS").
 * VERIFY: BMI160 datasheet not in hardware/datasheets/ */
#define BMI160_REG_PMU_STATUS 0x03
#define BMI160_PMU_MASK_ACC_GYR 0x3C
#define BMI160_PMU_ACC_GYR_NORMAL 0x14 /* acc 01 at [5:4], gyr 01 at [3:2] */
/* Reg 0x0C..0x17 DATA: GYR_X_L/H, GYR_Y_L/H, GYR_Z_L/H (0x0C..0x11) then
 * ACC_X_L/H, ACC_Y_L/H, ACC_Z_L/H (0x12..0x17), little-endian int16, one
 * 12-byte burst with auto-increment (Sec. 2.11.4-2.11.5, Table 21 register map).
 * VERIFY: BMI160 datasheet not in hardware/datasheets/ */
#define BMI160_REG_DATA_GYR 0x0C
#define BMI160_DATA_LEN 12
/* Reg 0x40 ACC_CONF: acc_odr[3:0] = 0b1000 (100 Hz), acc_bwp[6:4] = 0b010
 * (normal filter), acc_us[7] = 0 (Sec. 2.11.17, ODR table). 0x28.
 * VERIFY: BMI160 datasheet not in hardware/datasheets/ */
#define BMI160_REG_ACC_CONF 0x40
#define BMI160_ACC_CONF_100HZ_NORMAL 0x28
/* Reg 0x41 ACC_RANGE: 0b0011 = +-2 g, 0b0101 = +-4 g, 0b1000 = +-8 g,
 * 0b1100 = +-16 g (Sec. 2.11.18).
 * VERIFY: BMI160 datasheet not in hardware/datasheets/ */
#define BMI160_REG_ACC_RANGE 0x41
#define BMI160_ACC_RANGE_4G 0x05
/* Reg 0x42 GYR_CONF: gyr_odr[3:0] = 0b1000 (100 Hz), gyr_bwp[5:4] = 0b10
 * (normal filter) (Sec. 2.11.19). 0x28.
 * VERIFY: BMI160 datasheet not in hardware/datasheets/ */
#define BMI160_REG_GYR_CONF 0x42
#define BMI160_GYR_CONF_100HZ_NORMAL 0x28
/* Reg 0x43 GYR_RANGE: 0b000 = +-2000, 0b001 = +-1000, 0b010 = +-500,
 * 0b011 = +-250, 0b100 = +-125 deg/s (Sec. 2.11.20).
 * VERIFY: BMI160 datasheet not in hardware/datasheets/ */
#define BMI160_REG_GYR_RANGE 0x43
#define BMI160_GYR_RANGE_500DPS 0x02
/* Reg 0x7E CMD (write only, Sec. 2.11.38): 0xB6 soft reset, 0x11 acc normal
 * mode, 0x15 gyr normal mode.
 * VERIFY: BMI160 datasheet not in hardware/datasheets/ */
#define BMI160_REG_CMD 0x7E
#define BMI160_CMD_SOFTRESET 0xB6
#define BMI160_CMD_ACC_NORMAL 0x11
#define BMI160_CMD_GYR_NORMAL 0x15
/* Delays (Sec. 1 Table "electrical/timing specification" and Sec. 2.11.38 CMD
 * notes): soft reset needs about 1 ms before the next access (10 ms power-on);
 * acc suspend -> normal about 3.8 ms; gyro suspend -> normal about 55 ms (up to
 * 80 ms). Generous values are used and PMU_STATUS is polled instead of trusting
 * them. VERIFY: BMI160 datasheet not in hardware/datasheets/ */
#define BMI160_DELAY_AFTER_RESET_MS 100
#define BMI160_DELAY_ACC_NORMAL_MS 10
#define BMI160_DELAY_GYR_NORMAL_MS 85
#define BMI160_PMU_POLL_MS 10
#define BMI160_PMU_POLL_MAX 20
#define BMI160_DELAY_AFTER_CONF_MS 5
/* Sensitivity at +-4 g: 8192 LSB/g (Sec. 1 Table 2 accelerometer: +-2g 16384,
 * +-4g 8192, +-8g 4096, +-16g 2048). mg = raw * 1000 / 8192.
 * VERIFY: BMI160 datasheet not in hardware/datasheets/ */
#define BMI160_ACC_LSB_PER_G 8192
/* Sensitivity at +-500 deg/s: 65.6 LSB/(deg/s) (Sec. 1 Table 3 gyroscope: 2000:
 * 16.4, 1000: 32.8, 500: 65.6, 250: 131.2, 125: 262.4). 0.1 deg/s = raw * 10 /
 * 65.6 = raw * 1000 / 656.
 * VERIFY: BMI160 datasheet not in hardware/datasheets/ */
#define BMI160_GYR_LSB_PER_DPS_X10 656 /* 65.6 * 10 */

#define MS_TICKS(ms) (pdMS_TO_TICKS(ms) > 0 ? pdMS_TO_TICKS(ms) : 1)

static const char *TAG = "imu";

static i2c_master_bus_handle_t s_bus;
static i2c_master_dev_handle_t s_dev; /* IMU task only */

static SemaphoreHandle_t s_mtx; /* guards s_snap and s_link */
static imu_snapshot_t s_snap;
static imu_link_t s_link;
static bool s_ever_seen;

static void set_link(imu_link_t l)
{
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    s_link = l;
    xSemaphoreGive(s_mtx);
}

static void count_error(void)
{
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    s_snap.i2c_errors++;
    xSemaphoreGive(s_mtx);
}

static esp_err_t reg_read(uint8_t reg, uint8_t *buf, size_t n)
{
    return i2c_master_transmit_receive(s_dev, &reg, 1, buf, n, IMU_I2C_TIMEOUT_MS);
}

static esp_err_t reg_write(uint8_t reg, uint8_t val)
{
    const uint8_t b[2] = {reg, val};
    return i2c_master_transmit(s_dev, b, sizeof(b), IMU_I2C_TIMEOUT_MS);
}

static void close_device(void)
{
    if (s_dev != NULL) {
        i2c_master_bus_rm_device(s_dev);
        s_dev = NULL;
    }
}

/* Probe 0x68 then 0x69, keep the device handle of the first BMI160 that answers
 * with the right CHIP_ID. Returns the address, or 0. */
static uint8_t find_sensor(void)
{
    const uint8_t addrs[2] = {BMI160_ADDR_SDO_LOW, BMI160_ADDR_SDO_HIGH};
    for (int i = 0; i < 2; i++) {
        if (i2c_master_probe(s_bus, addrs[i], IMU_I2C_TIMEOUT_MS) != ESP_OK) {
            continue;
        }
        const i2c_device_config_t cfg = {
            .dev_addr_length = I2C_ADDR_BIT_LEN_7,
            .device_address = addrs[i],
            .scl_speed_hz = IMU_I2C_HZ,
        };
        if (i2c_master_bus_add_device(s_bus, &cfg, &s_dev) != ESP_OK) {
            s_dev = NULL;
            continue;
        }
        uint8_t id = 0;
        if (reg_read(BMI160_REG_CHIP_ID, &id, 1) == ESP_OK && id == BMI160_CHIP_ID_VALUE) {
            return addrs[i];
        }
        static unsigned s_wrong_id_rounds;
        if (s_wrong_id_rounds++ % IMU_WARN_EVERY == 0) {
            ESP_LOGW(TAG, "0x%02X answers, but CHIP_ID is 0x%02X (expected 0x%02X): not a BMI160",
                     addrs[i], (unsigned)id, BMI160_CHIP_ID_VALUE);
        }
        close_device();
    }
    return 0;
}

static esp_err_t write_checked(uint8_t reg, uint8_t val)
{
    esp_err_t err = reg_write(reg, val);
    if (err != ESP_OK) {
        return err;
    }
    vTaskDelay(MS_TICKS(BMI160_DELAY_AFTER_CONF_MS));
    uint8_t rb = 0;
    err = reg_read(reg, &rb, 1);
    if (err != ESP_OK) {
        return err;
    }
    if (rb != val) {
        ESP_LOGW(TAG, "reg 0x%02X reads back 0x%02X, wrote 0x%02X", reg, (unsigned)rb, (unsigned)val);
        return ESP_ERR_INVALID_RESPONSE;
    }
    return ESP_OK;
}

/* Soft reset, acc + gyro normal mode, +-4 g, +-500 deg/s, ODR 100 Hz. */
static esp_err_t configure(void)
{
    esp_err_t err = reg_write(BMI160_REG_CMD, BMI160_CMD_SOFTRESET);
    if (err != ESP_OK) {
        return err;
    }
    vTaskDelay(MS_TICKS(BMI160_DELAY_AFTER_RESET_MS));
    uint8_t id = 0;
    err = reg_read(BMI160_REG_CHIP_ID, &id, 1);
    if (err != ESP_OK) {
        return err;
    }
    if (id != BMI160_CHIP_ID_VALUE) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    err = reg_write(BMI160_REG_CMD, BMI160_CMD_ACC_NORMAL);
    if (err != ESP_OK) {
        return err;
    }
    vTaskDelay(MS_TICKS(BMI160_DELAY_ACC_NORMAL_MS));
    err = reg_write(BMI160_REG_CMD, BMI160_CMD_GYR_NORMAL);
    if (err != ESP_OK) {
        return err;
    }
    vTaskDelay(MS_TICKS(BMI160_DELAY_GYR_NORMAL_MS));
    uint8_t pmu = 0;
    for (int i = 0; i < BMI160_PMU_POLL_MAX; i++) {
        err = reg_read(BMI160_REG_PMU_STATUS, &pmu, 1);
        if (err != ESP_OK) {
            return err;
        }
        if ((pmu & BMI160_PMU_MASK_ACC_GYR) == BMI160_PMU_ACC_GYR_NORMAL) {
            break;
        }
        vTaskDelay(MS_TICKS(BMI160_PMU_POLL_MS));
    }
    if ((pmu & BMI160_PMU_MASK_ACC_GYR) != BMI160_PMU_ACC_GYR_NORMAL) {
        ESP_LOGW(TAG, "PMU_STATUS 0x%02X: acc/gyr did not reach normal mode", (unsigned)pmu);
        return ESP_ERR_TIMEOUT;
    }
    err = write_checked(BMI160_REG_ACC_CONF, BMI160_ACC_CONF_100HZ_NORMAL);
    if (err == ESP_OK) {
        err = write_checked(BMI160_REG_ACC_RANGE, BMI160_ACC_RANGE_4G);
    }
    if (err == ESP_OK) {
        err = write_checked(BMI160_REG_GYR_CONF, BMI160_GYR_CONF_100HZ_NORMAL);
    }
    if (err == ESP_OK) {
        err = write_checked(BMI160_REG_GYR_RANGE, BMI160_GYR_RANGE_500DPS);
    }
    if (err == ESP_OK) {
        uint8_t e = 0; /* clear the read-clear error register; report leftovers */
        if (reg_read(BMI160_REG_ERR_REG, &e, 1) == ESP_OK && e != 0) {
            ESP_LOGW(TAG, "ERR_REG 0x%02X after configuration", (unsigned)e);
        }
    }
    return err;
}

/* round(raw * num / den), den > 0 */
static int32_t scale_round(int32_t raw, int32_t num, int32_t den)
{
    int64_t v = (int64_t)raw * num;
    int64_t half = den / 2;
    return (int32_t)((v >= 0) ? (v + half) / den : -((-v + half) / den));
}

static int16_t le16(const uint8_t *p)
{
    return (int16_t)(uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

/* One 12-byte burst -> mapped body-axis mg and 0.1 deg/s. */
static esp_err_t read_sample(int32_t acc_body[3], int32_t gyr_body[3])
{
    uint8_t b[BMI160_DATA_LEN];
    esp_err_t err = reg_read(BMI160_REG_DATA_GYR, b, sizeof(b));
    if (err != ESP_OK) {
        return err;
    }
    int32_t g[3], a[3];
    for (int i = 0; i < 3; i++) {
        g[i] = scale_round(le16(&b[2 * i]), 1000, BMI160_GYR_LSB_PER_DPS_X10);
        a[i] = scale_round(le16(&b[6 + 2 * i]), 1000, BMI160_ACC_LSB_PER_G);
    }
    tilt_map_axes(a, acc_body);
    tilt_map_axes(g, gyr_body);
    return ESP_OK;
}

static const char *link_name(imu_link_t l)
{
    return l == IMU_LINK_OK ? "ok" : (l == IMU_LINK_ERROR ? "error" : "absent");
}

static void log_status(const tilt_t *t)
{
    imu_snapshot_t s;
    imu_link_t l = imu_status();
    bool have = imu_get_snapshot(&s);
    if (!have) {
        xSemaphoreTake(s_mtx, portMAX_DELAY);
        s = s_snap;
        xSemaphoreGive(s_mtx);
    }
    if (l == IMU_LINK_ABSENT) {
        ESP_LOGI(TAG, "no BMI160 on I2C (SDA %d, SCL %d, 0x68/0x69; not connected?), i2c errors %"
                 PRIu32, IMU_PIN_SDA, IMU_PIN_SCL, s.i2c_errors);
    } else if (t != NULL && tilt_valid(t)) {
        ESP_LOGI(TAG, "%s, pitch %.1f deg, roll %.1f deg, i2c errors %" PRIu32 ", reinits %" PRIu32,
                 link_name(l), (double)tilt_pitch_deg(t), (double)tilt_roll_deg(t), s.i2c_errors,
                 s.reinits);
    } else {
        ESP_LOGI(TAG, "%s, no attitude yet, i2c errors %" PRIu32 ", reinits %" PRIu32, link_name(l),
                 s.i2c_errors, s.reinits);
    }
}

/* Samples until IMU_FAIL_LIMIT consecutive read failures. */
static void sample_loop(int64_t *last_log)
{
    tilt_t tilt;
    tilt_init(&tilt, TILT_ALPHA_DEFAULT, IMU_SAMPLE_PERIOD_MS / 1000.0f);
    int fails = 0;
    int in_window = 0;
    int64_t prev_us = 0;
    TickType_t wake = xTaskGetTickCount();

    for (;;) {
        vTaskDelayUntil(&wake, MS_TICKS(IMU_SAMPLE_PERIOD_MS));
        int32_t a[3], g[3];
        esp_err_t err = read_sample(a, g);
        int64_t now = esp_timer_get_time();
        if (err != ESP_OK) {
            count_error();
            if (++fails >= IMU_FAIL_LIMIT) {
                ESP_LOGW(TAG, "%d consecutive I2C failures (%s), re-initialising", fails,
                         esp_err_to_name(err));
                return;
            }
            continue;
        }
        fails = 0;
        if (prev_us != 0) { /* real interval, so a late tick does not skew the gyro integral */
            float dt = (float)(now - prev_us) * 1e-6f;
            tilt_set_dt(&tilt, dt < IMU_DT_MIN_S ? IMU_DT_MIN_S : (dt > IMU_DT_MAX_S ? IMU_DT_MAX_S : dt));
        }
        prev_us = now;
        tilt_update(&tilt, a, g);
        if (++in_window >= IMU_RECORD_SAMPLES) {
            in_window = 0;
            rec_imu_t rec;
            tilt_take_record(&tilt, &rec);
            uint8_t buf[REC_IMU_LEN];
            if (rec_imu_encode(buf, &rec) == 0) {
                int rc = ld2410_ring_push((uint64_t)now, REC_TYPE_IMU, buf, sizeof(buf), NULL);
                if (rc != 0) {
                    ESP_LOGW(TAG, "ring push failed (%d)", rc);
                }
            }
            xSemaphoreTake(s_mtx, portMAX_DELAY);
            s_snap.valid = true;
            s_snap.rec = rec;
            s_snap.time_us = (uint64_t)now;
            xSemaphoreGive(s_mtx);
        }
        if (now - *last_log >= IMU_LOG_US) {
            *last_log = now;
            log_status(&tilt);
        }
    }
}

static void imu_task(void *arg)
{
    (void)arg;
    int64_t last_log = esp_timer_get_time();
    unsigned miss = 0; /* consecutive rounds without a working sensor */
    for (;;) {
        if (miss >= IMU_FAIL_LIMIT) { /* bus may be wedged (SDA held low): reset before re-probing */
            esp_err_t rerr = i2c_master_bus_reset(s_bus);
            if (rerr != ESP_OK && (miss - IMU_FAIL_LIMIT) % IMU_WARN_EVERY == 0) {
                ESP_LOGW(TAG, "i2c_master_bus_reset failed: %s", esp_err_to_name(rerr));
            }
        }
        uint8_t addr = find_sensor();
        if (addr != 0) {
            esp_err_t err = configure();
            if (err == ESP_OK) {
                xSemaphoreTake(s_mtx, portMAX_DELAY);
                s_ever_seen = true;
                s_snap.addr = addr;
                s_snap.reinits++;
                s_link = IMU_LINK_OK;
                xSemaphoreGive(s_mtx);
                miss = 0;
                ESP_LOGI(TAG, "BMI160 at 0x%02X configured (+-4 g, +-500 deg/s, 100 Hz)", addr);
                sample_loop(&last_log);
                miss = IMU_FAIL_LIMIT - 1; /* read failures: reset the bus before the next probe */
            } else {
                count_error();
                if (miss % IMU_WARN_EVERY == 0) { /* first failure, then every 30 s */
                    ESP_LOGW(TAG, "BMI160 configuration failed: %s", esp_err_to_name(err));
                }
            }
            close_device();
        }
        miss++;
        xSemaphoreTake(s_mtx, portMAX_DELAY);
        s_link = s_ever_seen ? IMU_LINK_ERROR : IMU_LINK_ABSENT;
        xSemaphoreGive(s_mtx);

        vTaskDelay(MS_TICKS(IMU_BACKOFF_MS));
        int64_t now = esp_timer_get_time();
        if (now - last_log >= IMU_LOG_US) {
            last_log = now;
            log_status(NULL);
        }
    }
}

esp_err_t imu_start(void)
{
    s_mtx = xSemaphoreCreateMutex();
    if (s_mtx == NULL) {
        return ESP_ERR_NO_MEM;
    }
    memset(&s_snap, 0, sizeof(s_snap));
    s_link = IMU_LINK_ABSENT;
    s_ever_seen = false;
    s_dev = NULL;

    const i2c_master_bus_config_t bus_cfg = {
        .i2c_port = IMU_I2C_PORT,
        .sda_io_num = IMU_PIN_SDA,
        .scl_io_num = IMU_PIN_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = IMU_INTERNAL_PULLUP,
    };
    esp_err_t err = i2c_new_master_bus(&bus_cfg, &s_bus);
    if (err != ESP_OK) {
        return err;
    }
    BaseType_t ok = xTaskCreatePinnedToCore(imu_task, "imu", IMU_TASK_STACK, NULL, IMU_TASK_PRIO,
                                            NULL, IMU_TASK_CORE);
    return ok == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}

bool imu_get_snapshot(imu_snapshot_t *out)
{
    if (out == NULL || s_mtx == NULL) {
        return false;
    }
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    *out = s_snap;
    xSemaphoreGive(s_mtx);
    return out->valid;
}

imu_link_t imu_status(void)
{
    if (s_mtx == NULL) {
        return IMU_LINK_ABSENT;
    }
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    imu_link_t l = s_link;
    xSemaphoreGive(s_mtx);
    return l;
}
