/* imu_port_tab5.c — the M5Stack Tab5's BMI270 IMU (I2C 0x68; board_tab5.h,
 * docs/boards/m5stack-tab5.md) as an orientation sensor: accel at 25 Hz,
 * +-2 g, read through Espressif's espressif/bmi270 driver (it uploads Bosch's
 * config file) and kept in the QMI8658 port's counts (16384 = 1 g), so the
 * thresholds below are the 1.8's, bench-tuned. Polled ~4x/s from the tank
 * task; the inverted flag flips only after the gravity component along the
 * panel's landscape-vertical axis has clearly pointed the other way for 3
 * consecutive polls, and holds its last state while the device lies flat
 * (no axis dominant), so the screen never flaps on a table. */
#include "imu_port.h"
#include "board_tab5.h"
#include "bmi270.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/* which accel axis is "up" when the tank is held right side up (SD slot at
 * the bottom): TAB5_IMU_UP_AXIS / TAB5_IMU_UP_SIGN, measured on the bench
 * (board_tab5.h). Not measured yet (-1): the screen never flips. */
#define IMU_UP_AXIS TAB5_IMU_UP_AXIS
#define IMU_UP_SIGN TAB5_IMU_UP_SIGN
#define IMU_FLIP_ON (TAB5_IMU_UP_AXIS >= 0)

#define POLL_INTERVAL_US   250000
/* 2026-08-31: was 8192 (0.5 g) - that only fired within ~60 deg of vertical,
 * so a device reclined on its back (bench pose: up-axis carries ~0.25 g)
 * never flipped. Now ~0.21 g, but the up-axis must also DOMINATE the other
 * in-screen axis, so lying flat or held sideways still holds last state. */
#define FLIP_THRESH        3500   /* ~0.21 g at +-2g full scale (16384 counts/g) */
#define FLIP_HOLD_POLLS    3      /* ~750 ms the other way up before flipping */
/* motion = the sum over healthy axes of |a - a_prev| between two polls
 * (250 ms apart). A table reads a few tens of counts of noise; a hand
 * holding still a few hundred; a pick-up thousands. */
#define MOTION_THRESH      220    /* ~0.013 g */
#define IMU_MOTION_HOLD_US 1000000

static const char *TAG = "imu";
static bmi270_handle_t *s_dev;
static i2c_master_bus_handle_t s_bus;
static bool s_inverted;
static int s_streak __attribute__((unused));              /* consecutive polls voting for a flip */
static int64_t s_next_us;
static int16_t s_prev[3]; static bool s_have_prev;
static int64_t s_moved_us; static int s_motion; static int16_t s_last[3];
static int64_t s_handled_us; static bool s_prev_moved;   /* two polls in a row over the threshold */

/* create (the driver soft-resets the chip and uploads its config file) +
 * start. The chip sits on an always-on rail and keeps whatever state it fell
 * into across reboots (the QMI8658's lesson, 2026-08-31): never trust its
 * power-on state - every init and every wake starts it from scratch. */
static bool imu_reset_config(void) {
    if (s_dev) { bmi270_delete(s_dev); s_dev = NULL; }
    const bmi270_driver_config_t dcfg = { .addr = TAB5_ADDR_BMI270, .interface = BMI270_USE_I2C, .i2c_bus = s_bus };
    if (bmi270_create(&dcfg, &s_dev) != ESP_OK) { s_dev = NULL; return false; }
    const bmi270_config_t cfg = { .acce_odr = BMI270_ACC_ODR_25_HZ, .acce_range = BMI270_ACC_RANGE_2_G,
                                  .gyro_odr = BMI270_GYR_ODR_25_HZ, .gyro_range = BMI270_GYR_RANGE_2000_DPS };
    if (bmi270_start(s_dev, &cfg) != ESP_OK) { bmi270_delete(s_dev); s_dev = NULL; return false; }
    return true;
}
/* one sample in the QMI8658 port's counts: 16384 = 1 g */
static bool read_counts(int16_t a[3]) {
    float g[3];
    if (bmi270_get_acce_data(s_dev, &g[0], &g[1], &g[2]) != ESP_OK) return false;
    for (int i = 0; i < 3; i++) {
        float c = g[i] * 16384.0f;
        a[i] = (int16_t)(c > 32767 ? 32767 : c < -32768 ? -32768 : c);
    }
    return true;
}

bool imu_port_init(i2c_master_bus_handle_t bus) {
    if (!bus) return false;
    if (i2c_master_probe(bus, TAB5_ADDR_BMI270, 50) != ESP_OK) { ESP_LOGW(TAG, "no BMI270 at 0x%02x", TAB5_ADDR_BMI270); return false; }
    s_bus = bus;
    if (!imu_reset_config()) { ESP_LOGW(TAG, "BMI270 init failed"); return false; }
    if (IMU_FLIP_ON) ESP_LOGI(TAG, "BMI270 up at 0x%02x: orientation axis %c%c", TAB5_ADDR_BMI270,
                              IMU_UP_SIGN > 0 ? '+' : '-', "XYZ"[IMU_FLIP_ON ? IMU_UP_AXIS : 0]);
    else ESP_LOGW(TAG, "BMI270 up at 0x%02x: its axes are not measured yet (board_tab5.h) - the screen will not flip; "
                       "director `imu` traces the raw axes", TAB5_ADDR_BMI270);
    return true;
}

void imu_port_poll(int64_t now_us) {
    if (!s_dev || now_us < s_next_us) return;
    s_next_us = now_us + POLL_INTERVAL_US;
    int16_t a[3];
    if (!read_counts(a)) return;
    static int logged;
    if (logged < 3) { logged++; ESP_LOGI(TAG, "g=[%d %d %d] inverted=%d", a[0], a[1], a[2], (int)s_inverted); }
    /* handling: movement since the last poll, railed channels ignored */
    if (s_have_prev) {
        int m = 0;
        for (int i = 0; i < 3; i++) {
            if (a[i] <= -32000 || a[i] >= 32000 || s_prev[i] <= -32000 || s_prev[i] >= 32000) continue;
            int d = a[i] - s_prev[i]; m += d < 0 ? -d : d;
        }
        s_motion = m;
        bool moved = m > MOTION_THRESH;
        if (moved) s_moved_us = now_us;
        if (moved && s_prev_moved) s_handled_us = now_us;
        s_prev_moved = moved;
    }
    for (int i = 0; i < 3; i++) { s_prev[i] = a[i]; s_last[i] = a[i]; }
    s_have_prev = true;
    /* railed axis = a channel latched at full scale. Found 2026-08-31: X and
     * Z pegged at +-32767 while Y tracked reality, with clean comms, clean
     * config readback, soft reset no help - damaged channels on the MEMS die.
     * Work with what's healthy: the flip only needs the UP axis. A railed
     * other axis just skips the dominance guard; only a railed UP axis
     * disables the flip (and we keep nudging the chip with soft resets in
     * case it is recoverable stiction rather than damage). */
#define RAILED(x) ((x) <= -32000 || (x) >= 32000)
    if (!IMU_FLIP_ON) return;                      /* axes not measured: motion only */
#if TAB5_IMU_UP_AXIS >= 0
    static int s_bad; static int64_t s_gate; static bool s_warned;
    if (RAILED(a[IMU_UP_AXIS])) {
        if (++s_bad >= 12 && now_us > s_gate) {              /* ~3 s railed */
            ESP_LOGW(TAG, "up axis railed (g=[%d %d %d]) - soft reset", a[0], a[1], a[2]);
            imu_reset_config();
            s_bad = 0; s_gate = now_us + 5000000;
        }
        return;
    }
    s_bad = 0;
    int v = a[IMU_UP_AXIS] * IMU_UP_SIGN;
    /* the other IN-SCREEN axis (TAB5_IMU_FACE_AXIS is out of the glass): the up-axis must
     * carry more of gravity than it, or we are sideways/flat - hold state.
     * Skipped when that axis is railed - one good axis is enough to flip. */
    int other = a[3 - IMU_UP_AXIS - TAB5_IMU_FACE_AXIS];   /* the in-screen axis that is not "up" */
    if (RAILED(other) && !s_warned) {
        s_warned = true;
        ESP_LOGW(TAG, "axis %c railed (sensor damage?) - flip runs on the up axis alone",
                 "XYZ"[3 - IMU_UP_AXIS - TAB5_IMU_FACE_AXIS]);
    }
    bool dominant = RAILED(other) || (v > 0 ? v : -v) > (other > 0 ? other : -other);
    bool wants_flip = dominant && (s_inverted ? (v > FLIP_THRESH) : (v < -FLIP_THRESH));
    s_streak = wants_flip ? s_streak + 1 : 0;      /* flat / sideways: hold state */
    if (s_streak >= FLIP_HOLD_POLLS) {
        s_inverted = !s_inverted; s_streak = 0;
        ESP_LOGI(TAG, "orientation: %s", s_inverted ? "inverted" : "upright");
    }
#endif
}

bool imu_port_inverted(void) { return s_inverted; }
void imu_port_last(int16_t out[3], int *motion) { for (int i = 0; i < 3; i++) out[i] = s_last[i]; if (motion) *motion = s_motion; }
bool imu_port_handled(void) { return s_handled_us && esp_timer_get_time() - s_handled_us < IMU_MOTION_HOLD_US; }
bool imu_port_moving(void) { return s_moved_us && esp_timer_get_time() - s_moved_us < IMU_MOTION_HOLD_US; }
int  imu_port_motion(void) { return s_motion; }

/* drowse bracket (see imu_port.h). Sleep: sensors off, chip quiesced while
 * the neighbouring rails cycle. Wake: never trust what the chip did in the
 * dark - full soft reset + reconfigure. */
void imu_port_sleep(void) {
    if (s_dev) (void)bmi270_stop(s_dev);
}
void imu_port_wake(void) {
    if (!s_bus) return;
    if (!imu_reset_config()) ESP_LOGW(TAG, "wake reconfig failed");
}
