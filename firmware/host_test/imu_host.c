/* imu_host.c - host check of the IMU driver's screen turns
 * (main/imu_port_qmi8658.c + common/rotate.h), gravity fed through stand-in
 * ESP-IDF headers (stub/). Build + run from firmware/host_test, square tank
 * and rectangular:
 *   cc -std=c11 -Istub -I../main -I../../common -o imu_host imu_host.c ../main/imu_port_qmi8658.c && ./imu_host
 *   cc -std=c11 -Istub -include stub_rect/tank.h -I../main -I../../common -o imu_host_rect imu_host.c ../main/imu_port_qmi8658.c && ./imu_host_rect
 * Checks: each edge down turns the frame to stand on it after 3 polls (a
 * square tank; a rectangular one takes half turns only and holds sideways);
 * a glance shorter than that, lying flat and a 45-degree hold keep the turn;
 * a railed right axis leaves the half turns working. IMU_RIGHT_SIGN's
 * direction is the driver's ASSUMPTION - this pins the logic, the bench the
 * sign. */
#include <stdio.h>
#include <string.h>
#include "imu_port.h"
#include "rotate.h"

int host_log;
static int64_t s_now;
static int16_t s_acc[3];                           /* what the accel reads: world-up, 16384 = 1 g */
int64_t esp_timer_get_time(void) { return s_now; }
esp_err_t i2c_master_probe(i2c_master_bus_handle_t b, uint16_t a, int t) { (void)b; (void)t; return a == 0x6B ? ESP_OK : ESP_FAIL; }
esp_err_t i2c_master_bus_add_device(i2c_master_bus_handle_t b, const i2c_device_config_t *c, i2c_master_dev_handle_t *d) {
    (void)b; (void)c; *d = (i2c_master_dev_handle_t)1; return ESP_OK; }
esp_err_t i2c_master_transmit(i2c_master_dev_handle_t d, const uint8_t *buf, size_t n, int t) { (void)d; (void)buf; (void)n; (void)t; return ESP_OK; }
esp_err_t i2c_master_transmit_receive(i2c_master_dev_handle_t d, const uint8_t *w, size_t wn, uint8_t *r, size_t rn, int t) {
    (void)d; (void)wn; (void)t;
    switch (w[0]) {
    case 0x00: r[0] = 0x05; break;                 /* WHO_AM_I */
    case 0x02: r[0] = 0x40; break;                 /* the config readback */
    case 0x03: r[0] = 0x08; break;
    case 0x08: r[0] = 0x01; break;
    case 0x35: for (size_t i = 0; i < rn / 2; i++) { r[2 * i] = (uint8_t)s_acc[i]; r[2 * i + 1] = (uint8_t)((uint16_t)s_acc[i] >> 8); } break;
    default: memset(r, 0, rn);
    }
    return ESP_OK;
}

static int fails;
#define CHECK(cond, ...) do { if (!(cond)) { fails++; printf("FAIL: " __VA_ARGS__); printf("\n"); } } while (0)
/* hold a pose for n polls (250 ms apart) */
static void hold(int x, int y, int z, int n) {
    s_acc[0] = (int16_t)x; s_acc[1] = (int16_t)y; s_acc[2] = (int16_t)z;
    for (int i = 0; i < n; i++) { s_now += 250000; imu_port_poll(s_now); }
}
#define G 16384
/* the screen's own axes (the driver's IMU_UP_* / IMU_RIGHT_*: +Y up, +X right) */
#define UPRIGHT      0,  G, 0
#define UPSIDE_DOWN  0, -G, 0
#define RIGHT_UP     G,  0, 0                      /* turned a quarter anticlockwise: the right edge on top */
#define LEFT_UP     -G,  0, 0
#define FLAT         0,  0, G

int main(void) {
    const bool sq = ROTATE_QUARTER_OK;
    printf("imu_host: %s tank %dx%d\n", sq ? "square" : "rectangular", TANK_W, TANK_H);
    CHECK(imu_port_init((i2c_master_bus_handle_t)1), "init");
    hold(UPRIGHT, 4);                  CHECK(imu_port_rotation() == 0, "upright -> %d", imu_port_rotation());
    hold(RIGHT_UP, 2);                 CHECK(imu_port_rotation() == 0, "a 2-poll glance with the right edge up turned it");
    hold(RIGHT_UP, 1);                 CHECK(imu_port_rotation() == (sq ? 1 : 0), "right edge up -> %d (want %d)", imu_port_rotation(), sq ? 1 : 0);
    hold(FLAT, 8);                     CHECK(imu_port_rotation() == (sq ? 1 : 0), "lying flat did not hold the turn");
    hold(LEFT_UP, 3);                  CHECK(imu_port_rotation() == (sq ? 3 : 0), "left edge up -> %d (want %d)", imu_port_rotation(), sq ? 3 : 0);
    hold(UPSIDE_DOWN, 3);              CHECK(imu_port_rotation() == 2, "upside down -> %d", imu_port_rotation());
    hold(RIGHT_UP, 3);                 CHECK(imu_port_rotation() == (sq ? 1 : 2), "right edge up from upside down -> %d", imu_port_rotation());
    hold(UPRIGHT, 3);                  CHECK(imu_port_rotation() == 0, "back upright -> %d", imu_port_rotation());
    hold(11585, 11585, 0, 8);          CHECK(imu_port_rotation() == 0, "45 degrees turned it (%d)", imu_port_rotation());
    hold(8192, 14189, 0, 8);           CHECK(imu_port_rotation() == 0, "30 degrees off upright turned it (%d)", imu_port_rotation());
    hold(14189, 8192, 0, 3);           CHECK(imu_port_rotation() == (sq ? 1 : 0), "60 degrees toward the right edge -> %d", imu_port_rotation());
    hold(UPRIGHT, 3);
    hold(3000, 0, 16000, 8);           CHECK(imu_port_rotation() == 0, "reclined, under the threshold, turned it");
    hold(32767, -G, 0, 3);             CHECK(imu_port_rotation() == 2, "a railed right axis stopped the half turn (%d)", imu_port_rotation());
    hold(32767, 2000, 0, 3);           CHECK(imu_port_rotation() == 2, "a railed right axis turned it a quarter (%d)", imu_port_rotation());
    hold(32767, G, 0, 3);              CHECK(imu_port_rotation() == 0, "railed right axis, upright again -> %d", imu_port_rotation());
    printf(fails ? "imu_host: %d FAILED\n" : "imu_host ok\n", fails);
    return fails != 0;
}
