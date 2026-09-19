/* rtc_port_rx8130.c — the M5Stack Tab5's RX8130CE RTC (I2C 0x32;
 * docs/boards/m5stack-tab5.md) -> system wall clock. The same contract as
 * rtc_port_pcf85063.c: at boot, a plausible time sets the system clock; a
 * lost or unset one (the voltage-low flag, or a year before 2024) is seeded
 * from the firmware build time. The progression save stamps
 * clock_port_now_unix(); nothing else needs the RTC.
 *
 * Registers (Epson RX8130CE application manual; ASSUMED until the phase-8
 * bench): 0x10-0x16 seconds, minutes, hours (24 h), weekday (one-hot, bit 0
 * = Sunday), day, month, year (BCD, 20xx); 0x1D flags, bit 1 VLF = the
 * oscillator stopped / the supply fell - the time is not to be trusted;
 * 0x1E control 0, bit 6 STOP = hold the clock while it is written. Control 1
 * (0x1F: the backup cell's charge enable among others) is left as it is. */
#include "rtc_port.h"
#include "driver/i2c_master.h"
#include "esp_log.h"
#include <string.h>
#include <sys/time.h>
#include <time.h>

#define RX8130_ADDR  0x32
#define REG_SEC      0x10
#define REG_FLAG     0x1D
#define REG_CTRL0    0x1E
#define FLAG_VLF     0x02
#define CTRL0_STOP   0x40
static const char *TAG = "rtc";
static i2c_master_dev_handle_t s_dev;

static int bcd2bin(uint8_t b) { return (b >> 4) * 10 + (b & 0x0f); }
static uint8_t bin2bcd(int v) { return (uint8_t)(((v / 10) << 4) | (v % 10)); }
static bool rd(uint8_t reg, uint8_t *b, size_t n) { return i2c_master_transmit_receive(s_dev, &reg, 1, b, n, 100) == ESP_OK; }
static bool wr1(uint8_t reg, uint8_t v) { uint8_t w[2] = { reg, v }; return i2c_master_transmit(s_dev, w, 2, 100) == ESP_OK; }

/* false = unreadable; *lost = the chip says its time cannot be trusted */
static bool rtc_read(struct tm *out, bool *lost, uint8_t *flags) {
    uint8_t b[7], f = 0;
    if (!rd(REG_SEC, b, 7) || !rd(REG_FLAG, &f, 1)) return false;
    memset(out, 0, sizeof *out);
    out->tm_sec = bcd2bin(b[0] & 0x7f); out->tm_min = bcd2bin(b[1] & 0x7f);
    out->tm_hour = bcd2bin(b[2] & 0x3f);
    out->tm_wday = 0; for (int i = 0; i < 7; i++) if (b[3] & (1 << i)) { out->tm_wday = i; break; }
    out->tm_mday = bcd2bin(b[4] & 0x3f); out->tm_mon = bcd2bin(b[5] & 0x1f) - 1;
    out->tm_year = bcd2bin(b[6]) + 100;                 /* 20xx */
    *lost = f & FLAG_VLF; *flags = f;
    return true;
}
/* STOP, the seven time registers, STOP off (the count restarts from here),
   then the voltage-low flag cleared: the time is good from now on */
static bool rtc_write(const struct tm *t) {
    uint8_t c0 = 0;
    if (!rd(REG_CTRL0, &c0, 1) || !wr1(REG_CTRL0, c0 | CTRL0_STOP)) return false;
    uint8_t w[8] = { REG_SEC, bin2bcd(t->tm_sec), bin2bcd(t->tm_min), bin2bcd(t->tm_hour), (uint8_t)(1 << (t->tm_wday % 7)),
                     bin2bcd(t->tm_mday), bin2bcd(t->tm_mon + 1), bin2bcd(t->tm_year - 100) };
    bool ok = i2c_master_transmit(s_dev, w, sizeof w, 100) == ESP_OK;
    ok &= wr1(REG_CTRL0, c0 & ~CTRL0_STOP);
    uint8_t f = 0;
    if (rd(REG_FLAG, &f, 1)) ok &= wr1(REG_FLAG, f & ~FLAG_VLF);
    return ok;
}

bool rtc_port_init(i2c_master_bus_handle_t bus) {
    if (!bus || i2c_master_probe(bus, RX8130_ADDR, 50) != ESP_OK) { ESP_LOGW(TAG, "no RX8130CE at 0x%02x: no wall clock", RX8130_ADDR); return false; }
    i2c_device_config_t cfg = { .dev_addr_length = I2C_ADDR_BIT_LEN_7, .device_address = RX8130_ADDR, .scl_speed_hz = 400000 };
    if (i2c_master_bus_add_device(bus, &cfg, &s_dev) != ESP_OK) { ESP_LOGW(TAG, "no RTC"); s_dev = NULL; return false; }
    struct tm t; bool lost = true; uint8_t flags = 0;
    bool read = rtc_read(&t, &lost, &flags);
    if (read && !lost && t.tm_year + 1900 >= 2024) {
        struct timeval tv = { .tv_sec = mktime(&t) };
        settimeofday(&tv, NULL);
        ESP_LOGI(TAG, "system clock set from the RX8130CE: %04d-%02d-%02d %02d:%02d:%02d (flags %02x)",
                 t.tm_year + 1900, t.tm_mon + 1, t.tm_mday, t.tm_hour, t.tm_min, t.tm_sec, flags);
        return true;
    }
    /* seed from build time: "Aug 21 2026" "10:15:00" */
    static const char mon[] = "JanFebMarAprMayJunJulAugSepOctNovDec";
    char ms[4] = {0}; int d, y, hh, mm, ss;
    if (read) ESP_LOGW(TAG, "RX8130CE time %s: %04d-%02d-%02d %02d:%02d:%02d, flags %02x", lost ? "LOST (voltage-low flag)" : "unset",
                       t.tm_year + 1900, t.tm_mon + 1, t.tm_mday, t.tm_hour, t.tm_min, t.tm_sec, flags);
    sscanf(__DATE__, "%3s %d %d", ms, &d, &y); sscanf(__TIME__, "%d:%d:%d", &hh, &mm, &ss);
    memset(&t, 0, sizeof t);
    t.tm_mon = (int)((strstr(mon, ms) - mon) / 3); t.tm_mday = d; t.tm_year = y - 1900;
    t.tm_hour = hh; t.tm_min = mm; t.tm_sec = ss;
    time_t secs = mktime(&t); struct timeval tv = { .tv_sec = secs }; settimeofday(&tv, NULL);
    localtime_r(&secs, &t);                             /* the weekday, for the register */
    bool ok = rtc_write(&t);
    ESP_LOGW(TAG, "RX8130CE seeded from build time %s %s%s", __DATE__, __TIME__, ok ? "" : " - the write FAILED");
    return true;
}
