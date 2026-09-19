/* battery_port_tab5.c — the M5Stack Tab5's battery and power (board_tab5.h,
 * docs/boards/m5stack-tab5.md): the 2S NP-F550 pack read by the INA226 at
 * 0x41, the IP2326 charger switched on IO expander 2 (P7 enable, P6 status),
 * the power-off pulse to the PMS150G on expander 2 P4, and GPIO 35.
 *
 * Measured on the bench (phase 7, 2026-09-19; the table in the board doc):
 *   - the INA226's bus input is the pack (6.6 V nearly flat, 7.5 V after a
 *     half-hour's charge); the gauge is that voltage on a 2-cell Li-ion
 *     curve. While charging the pack reads high (the charge current through
 *     its resistance), so the gauge runs ahead until the cable comes out;
 *   - charging = the shunt well NEGATIVE (-2.4 mV at the charger's steady
 *     current; +5 uV with the charger off on USB, the board fed from USB);
 *   - expander 2 P7 HIGH enables the charger (low: the current stops) - on
 *     at boot, the keeper's go-ahead; `pmic off chg` / `pmic on chg`;
 *   - expander 2 P6 read HIGH in every state: not used;
 *   - the power key is the PMS150G's own (one press on, two presses off, a
 *     long press = download mode): the firmware leaves it alone;
 *   - power-off (the director's `poweroff`, the keeper's sleep) pulses
 *     expander 2 P4 high for 100 ms: the rails drop at once.
 * Director: `pmic` dumps it all, `pmic on|off chg`. */
#include "battery_port.h"
#include "board_tab5.h"
#include "driver/gpio.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "battery";
#define INA_CONFIG   0x00
#define INA_SHUNT    0x01            /* signed, 2.5 uV / LSB */
#define INA_BUS      0x02            /* 1.25 mV / LSB */
#define INA_MFG      0xFE            /* 0x5449 "TI" */
#define INA_DIE      0xFF            /* 0x2260 */
#define PACK_CELLS   2

static i2c_master_dev_handle_t s_ina;
static int64_t s_last_us = -1;
static float s_frac; static bool s_valid;
static bool s_chg_on, s_charging;
#define CHARGING_UV (-200)           /* shunt below this: current into the pack (the bench read -2.4 mV) */

static bool rd16(uint8_t reg, uint16_t *v) {
    uint8_t b[2];
    if (i2c_master_transmit_receive(s_ina, &reg, 1, b, 2, 100) != ESP_OK) return false;
    *v = (uint16_t)(b[0] << 8 | b[1]);
    return true;
}
static bool wr16(uint8_t reg, uint16_t v) {
    uint8_t b[3] = { reg, (uint8_t)(v >> 8), (uint8_t)v };
    return i2c_master_transmit(s_ina, b, 3, 100) == ESP_OK;
}

/* one Li-ion cell, rested-ish, mV -> fraction (a common discharge curve;
   under load the pack reads low, so the gauge leans pessimistic) */
static float cell_frac(int mv) {
    static const int16_t pt[][2] = { { 3000, 0 }, { 3450, 5 }, { 3680, 10 }, { 3740, 20 }, { 3770, 30 }, { 3790, 40 },
                                     { 3820, 50 }, { 3870, 60 }, { 3920, 70 }, { 3980, 80 }, { 4060, 90 }, { 4200, 100 } };
    if (mv <= pt[0][0]) return 0;
    for (size_t i = 1; i < sizeof pt / sizeof pt[0]; i++)
        if (mv <= pt[i][0])
            return (pt[i - 1][1] + (float)(mv - pt[i - 1][0]) * (pt[i][1] - pt[i - 1][1]) / (pt[i][0] - pt[i - 1][0])) / 100.0f;
    return 1;
}

static void chg_enable(bool on) {
    esp_io_expander_handle_t x = board_iox2();
    if (!x) return;
    esp_io_expander_set_level(x, TAB5_IOX2_CHG_EN, on);
    esp_io_expander_set_output_mode(x, TAB5_IOX2_CHG_EN, IO_EXPANDER_OUTPUT_MODE_PUSH_PULL);   /* board_tab5.c iox_out */
    esp_io_expander_set_dir(x, TAB5_IOX2_CHG_EN, IO_EXPANDER_OUTPUT);
    s_chg_on = on;
}
bool battery_port_init(i2c_master_bus_handle_t bus) {
    if (!bus || i2c_master_probe(bus, TAB5_ADDR_INA226, 50) != ESP_OK) { ESP_LOGW(TAG, "no INA226 at 0x%02x", TAB5_ADDR_INA226); return false; }
    i2c_device_config_t cfg = { .dev_addr_length = I2C_ADDR_BIT_LEN_7, .device_address = TAB5_ADDR_INA226, .scl_speed_hz = 400000 };
    if (i2c_master_bus_add_device(bus, &cfg, &s_ina) != ESP_OK) { s_ina = NULL; return false; }
    uint16_t mfg = 0, die = 0; rd16(INA_MFG, &mfg); rd16(INA_DIE, &die);
    /* continuous shunt + bus, 1.1 ms conversions, 16-sample average (~35 ms per reading) */
    bool ok = wr16(INA_CONFIG, 0x4527);
    chg_enable(true);
    vTaskDelay(pdMS_TO_TICKS(60));                      /* the first averaged conversion (the bench's boot read 1213 mV before it) */
    ESP_LOGI(TAG, "INA226 at 0x%02x (id %04x/%04x)%s; charging enabled (expander 2 P7 high)",
             TAB5_ADDR_INA226, mfg, die, ok ? "" : ", config write FAILED");
    battery_port_dump();
    return true;
}

int battery_port_vbat_mv(void) {
    uint16_t v;
    if (!s_ina || !rd16(INA_BUS, &v)) return 0;
    return (int)(v * 125 / 100);                        /* 1.25 mV / LSB */
}
static int shunt_uv(void) {
    uint16_t v;
    if (!s_ina || !rd16(INA_SHUNT, &v)) return 0;
    return (int16_t)v * 5 / 2;                          /* 2.5 uV / LSB, signed */
}

bool battery_port_read(float *frac, bool *charging) {
    if (!s_ina) return false;
    int64_t now = esp_timer_get_time();
    if (s_last_us < 0 || now - s_last_us > 5 * 1000000) {
        s_last_us = now;
        int mv = battery_port_vbat_mv();
        s_valid = mv > 4000 && mv < 9500;               /* a 2S pack, not a floating input */
        if (s_valid) { s_frac = cell_frac(mv / PACK_CELLS); s_charging = shunt_uv() < CHARGING_UV; }
    }
    if (!s_valid) return false;
    *frac = s_frac; *charging = s_charging;
    return true;
}

void battery_port_dump(void) {
    if (!s_ina) { ESP_LOGW(TAG, "no INA226"); return; }
    uint16_t cfg = 0; rd16(INA_CONFIG, &cfg);
    int mv = battery_port_vbat_mv(), uv = shunt_uv();
    ESP_LOGI(TAG, "pack %d mV (%d mV/cell, gauge %d%%) | shunt %+d uV: %s | charger %s (expander 2 P7) | INA226 config %04x",
             mv, mv / PACK_CELLS, (int)(cell_frac(mv / PACK_CELLS) * 100 + 0.5f), uv,
             uv < CHARGING_UV ? "CHARGING" : uv > -CHARGING_UV ? "on battery" : "on USB, not charging",
             s_chg_on ? "enabled" : "DISABLED", cfg);
}

/* the director's rail switch, here the charger's: `pmic on|off chg` */
bool battery_port_set_rail(const char *name, bool on) {
    if (!name || strcmp(name, "chg")) return false;
    chg_enable(on);
    ESP_LOGI(TAG, "charging %s (expander 2 P7 %s)", on ? "enabled" : "disabled", on ? "HIGH" : "low");
    vTaskDelay(pdMS_TO_TICKS(300));                     /* the charger's reaction, then a reading */
    battery_port_dump();
    return true;
}
void battery_port_trim_rails(void) {}                   /* no PMIC rails on this board */

/* the power-off pulse to the PMS150G (expander 2 P4): high for 100 ms (bench, 2026-09-19) */
bool battery_port_poweroff(void) {
    esp_io_expander_handle_t x = board_iox2();
    if (!x) return false;
    ESP_LOGW(TAG, "power-off: expander 2 P4 pulsed high for 100 ms");
    esp_io_expander_set_level(x, TAB5_IOX2_PWROFF, 0);
    esp_io_expander_set_output_mode(x, TAB5_IOX2_PWROFF, IO_EXPANDER_OUTPUT_MODE_PUSH_PULL);
    esp_io_expander_set_dir(x, TAB5_IOX2_PWROFF, IO_EXPANDER_OUTPUT);
    esp_io_expander_set_level(x, TAB5_IOX2_PWROFF, 1);
    vTaskDelay(pdMS_TO_TICKS(100));
    esp_io_expander_set_level(x, TAB5_IOX2_PWROFF, 0);
    return true;
}

/* the power key belongs to the PMS150G (one press on, two presses off): no
   key events for the tank. `keytime` traces GPIO 35 (BOOT, shared with the
   PMS150G) - the tank holds still while it runs */
void battery_port_key_init(void) {
    gpio_config_t io = { .pin_bit_mask = 1ULL << TAB5_BOOT_GPIO, .mode = GPIO_MODE_INPUT, .pull_up_en = GPIO_PULLUP_ENABLE };
    gpio_config(&io);
}
int battery_port_key_poll(void) { return 0; }
void battery_port_key_trace(int seconds) {
    int64_t end = esp_timer_get_time() + (int64_t)seconds * 1000000, edge = esp_timer_get_time();
    int lv = gpio_get_level(TAB5_BOOT_GPIO), n = 0;
    ESP_LOGI(TAG, "key trace for %d s (the tank holds still): GPIO35 now %d. Tap the power key ONCE (two presses power off)", seconds, lv);
    while (esp_timer_get_time() < end) {
        int v = gpio_get_level(TAB5_BOOT_GPIO); int64_t now = esp_timer_get_time();
        if (v != lv) {
            n++;
            ESP_LOGI(TAG, "key trace: GPIO35 %d -> %d after %lld ms", lv, v, (now - edge) / 1000);
            lv = v; edge = now;
        }
        vTaskDelay(pdMS_TO_TICKS(2));
    }
    ESP_LOGI(TAG, "key trace over: %d edges on GPIO35", n);
}
