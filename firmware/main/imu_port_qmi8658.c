/* imu_port_qmi8658.c — QMI8658 6-axis IMU (I2C 0x6B, alt 0x6A) as an
 * orientation sensor: accel only at 31.25 Hz, gyro off. Polled ~4x/s from the
 * tank task. The frame turns (rotate.h: quarter turns for a square tank, half
 * turns for a rectangular one) only after gravity has clearly pointed down a
 * new edge of the screen for 3 consecutive polls, and holds its last turn
 * while the device lies flat (no in-screen axis dominant), so the screen
 * never flaps on a table. */
#include "imu_port.h"
#include "rotate.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/* which accel axis is "up" when the tank is held right side up. The boot log
 * prints the live vector ("imu: g=[x y z]") — if the flip is wrong or dead,
 * hold the device upright, read which axis carries ~1 g, and fix these two. */
#define IMU_UP_AXIS 1        /* 0=X 1=Y 2=Z. AMOLED-2.16: the schematic's silkscreen puts +Y toward */
#define IMU_UP_SIGN (+1)     /* the keys (USB down = upright), so upright-in-hand = +Y ~16k (the 1.8 was -Y) */
/* ... and which is "right" (the quarter turns, square tank only). The same
 * silkscreen, drawn as seen from the display side, has +X to the right and Z
 * out of the glass - a right-handed set with the +Y the bench confirmed - so
 * right-edge-up = +X ~16k. ASSUMED, not yet seen on the bench: if a quarter
 * turn puts the floor on the ceiling, flip the sign. */
#define IMU_RIGHT_AXIS 0
#define IMU_RIGHT_SIGN (+1)
/* a square tank's quarter turns: the winning in-screen axis must carry 25%
 * more of gravity than the other, so a device held near 45 degrees keeps its
 * turn instead of flapping between two (a half-turn-only tank needs none -
 * it compares the up axis with the other as before) */
#define QUARTER_MARGIN_PCT 125

#define QMI8658_ADDR       0x6B
#define QMI8658_ADDR_ALT   0x6A
#define REG_WHO_AM_I       0x00   /* reads 0x05 */
#define REG_CTRL1          0x02
#define REG_CTRL2          0x03
#define REG_CTRL7          0x08
#define REG_RESET          0x60   /* write 0xB0 = soft reset */
#define REG_AX_L           0x35
#define WHO_AM_I_VAL       0x05

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
static i2c_master_dev_handle_t s_dev;
static int  s_rot;                /* the frame's quarter turns (rotate.h) */
static int  s_cand = -1;          /* the turn the polls are voting for */
static int  s_streak;             /* consecutive polls voting for it */
static int64_t s_next_us;
static int16_t s_prev[3]; static bool s_have_prev;
static int64_t s_moved_us; static int s_motion; static int16_t s_last[3];
static int64_t s_handled_us; static bool s_prev_moved;   /* two polls in a row over the threshold */

static bool wr8(uint8_t reg, uint8_t val) {
    uint8_t buf[2] = { reg, val };
    return i2c_master_transmit(s_dev, buf, 2, 100) == ESP_OK;
}
static bool rdn(uint8_t reg, uint8_t *val, size_t n) {
    return i2c_master_transmit_receive(s_dev, &reg, 1, val, n, 100) == ESP_OK;
}

/* soft reset + full config. The chip sits on an always-on rail, so it keeps
 * whatever state it fell into across reboots and reflashes - 2026-08-31 it
 * was found latched with two axes railed at full scale (garbage that only a
 * reset clears; only a full PMIC power-off ever power-cycles it). Never
 * trust its power-on state. */
static bool imu_reset_config(void) {
    bool rst = wr8(REG_RESET, 0xB0);
    vTaskDelay(pdMS_TO_TICKS(25));
    bool ok = wr8(REG_CTRL1, 0x40)   /* address auto-increment for burst reads */
           && wr8(REG_CTRL2, 0x08)   /* accel +-2g, 31.25 Hz */
           && wr8(REG_CTRL7, 0x01);  /* accel on, gyro off */
    uint8_t c1 = 0xEE, c2 = 0xEE, c7 = 0xEE;   /* readback: is it even listening? */
    rdn(REG_CTRL1, &c1, 1); rdn(REG_CTRL2, &c2, 1); rdn(REG_CTRL7, &c7, 1);
    ESP_LOGI(TAG, "reset %s, ctrl readback 1=0x%02x 2=0x%02x 7=0x%02x (want 40/08/01)",
             rst ? "acked" : "NACKED", c1, c2, c7);
    return ok;
}

bool imu_port_init(i2c_master_bus_handle_t bus) {
    if (!bus) return false;
    uint8_t addr = QMI8658_ADDR;
    if (i2c_master_probe(bus, addr, 50) != ESP_OK) {
        addr = QMI8658_ADDR_ALT;
        if (i2c_master_probe(bus, addr, 50) != ESP_OK) { ESP_LOGW(TAG, "no QMI8658"); return false; }
    }
    i2c_device_config_t cfg = { .dev_addr_length = I2C_ADDR_BIT_LEN_7,
                                .device_address = addr, .scl_speed_hz = 400000 };
    if (i2c_master_bus_add_device(bus, &cfg, &s_dev) != ESP_OK) return false;
    uint8_t who = 0;
    if (!rdn(REG_WHO_AM_I, &who, 1) || who != WHO_AM_I_VAL) {
        ESP_LOGW(TAG, "QMI8658 whoami 0x%02x (want 0x05)", who);
        s_dev = NULL; return false;
    }
    if (!imu_reset_config()) { ESP_LOGW(TAG, "QMI8658 config failed"); s_dev = NULL; return false; }
    ESP_LOGI(TAG, "QMI8658 up at 0x%02x: orientation axis %c%s", addr,
             IMU_UP_SIGN > 0 ? '+' : '-', IMU_UP_AXIS == 0 ? "X" : IMU_UP_AXIS == 1 ? "Y" : "Z");
    return true;
}

void imu_port_poll(int64_t now_us) {
    if (!s_dev || now_us < s_next_us) return;
    s_next_us = now_us + POLL_INTERVAL_US;
    uint8_t raw[6];
    if (!rdn(REG_AX_L, raw, 6)) return;
    int16_t a[3] = { (int16_t)(raw[0] | raw[1] << 8),
                     (int16_t)(raw[2] | raw[3] << 8),
                     (int16_t)(raw[4] | raw[5] << 8) };
    static int logged;
    if (logged < 3) { logged++; ESP_LOGI(TAG, "g=[%d %d %d] turned %d deg", a[0], a[1], a[2], s_rot * 90); }
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
     * Work with what's healthy: the half turn only needs the UP axis. A
     * railed right axis skips the dominance guard and the quarter turns; only
     * a railed UP axis stops the turning (and we keep nudging the chip with
     * soft resets in case it is recoverable stiction rather than damage). */
#define RAILED(x) ((x) <= -32000 || (x) >= 32000)
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
    /* gravity in the screen's plane (Z is out of the glass): u along the
     * screen's up, r along its right; the accel reads UP, so u > 0 = upright,
     * r > 0 = the right edge on top (the image turns a quarter clockwise to
     * stand on the left edge). The axis carrying more of it picks the edge;
     * neither past FLIP_THRESH (flat) or neither clearly ahead (45 degrees on
     * a square tank) holds the turn. A rectangular tank never takes an odd
     * one: sideways it holds, exactly the old half-turn-only flip. */
    int u = a[IMU_UP_AXIS] * IMU_UP_SIGN, r_raw = a[IMU_RIGHT_AXIS];
    bool r_ok = !RAILED(r_raw);
    if (!r_ok && !s_warned) {
        s_warned = true;
        ESP_LOGW(TAG, "axis %c railed (sensor damage?) - half turns only, on the up axis alone", "XYZ"[IMU_RIGHT_AXIS]);
    }
    int r = r_ok ? r_raw * IMU_RIGHT_SIGN : 0;
    int au = u < 0 ? -u : u, ar = r < 0 ? -r : r;
    bool quarter = ROTATE_QUARTER_OK && r_ok;
    int margin = quarter ? QUARTER_MARGIN_PCT : 100;
    int cand = -1;
    if (!r_ok || au * 100 > ar * margin)            cand = u > FLIP_THRESH ? 0 : u < -FLIP_THRESH ? 2 : -1;
    else if (quarter && ar * 100 > au * margin)     cand = r > FLIP_THRESH ? 1 : r < -FLIP_THRESH ? 3 : -1;
    if (cand >= 0 && cand != s_rot) { s_streak = cand == s_cand ? s_streak + 1 : 1; s_cand = cand; }
    else s_streak = 0;                              /* flat / sideways / settled: hold the turn */
    if (s_streak >= FLIP_HOLD_POLLS) {
        s_rot = cand; s_streak = 0;
        ESP_LOGI(TAG, "orientation: turned %d deg (g=[%d %d %d])", s_rot * 90, a[0], a[1], a[2]);
    }
}

int imu_port_rotation(void) { return s_rot; }
void imu_port_last(int16_t out[3], int *motion) { for (int i = 0; i < 3; i++) out[i] = s_last[i]; if (motion) *motion = s_motion; }
bool imu_port_handled(void) { return s_handled_us && esp_timer_get_time() - s_handled_us < IMU_MOTION_HOLD_US; }
bool imu_port_moving(void) { return s_moved_us && esp_timer_get_time() - s_moved_us < IMU_MOTION_HOLD_US; }
int  imu_port_motion(void) { return s_motion; }

/* drowse bracket (see imu_port.h). Sleep: sensors off, chip quiesced while
 * the neighbouring rails cycle. Wake: never trust what the chip did in the
 * dark - full soft reset + reconfigure. */
void imu_port_sleep(void) {
    if (s_dev) (void)wr8(REG_CTRL7, 0x00);
}
void imu_port_wake(void) {
    if (!s_dev) return;
    if (!imu_reset_config()) ESP_LOGW(TAG, "wake reconfig failed");
}
