/* codec_port_tab5.c - the M5Stack Tab5's ES8388 codec (I2C 0x10; board_tab5.h,
 * docs/boards/m5stack-tab5.md), DAC side only: the speaker path for the audio
 * port. Down at boot and whenever the port idles; `codec` on the director
 * prints its power registers.
 *
 * Register values: Espressif's esp_codec_dev ES8388 driver (1.5.11,
 * Apache-2.0) as the Tab5 board-support package opens it - slave I2S,
 * 16-bit, MCLK = 256 fs, DAC to the output mixers, LOUT1/ROUT1 at 0 dB and
 * LOUT2/ROUT2 at -30 dB - with the ADC kept down (the tank never records). */
#include "codec_port.h"
#include "board_tab5.h"
#include "esp_log.h"

static const char *TAG = "codec";
static i2c_master_dev_handle_t s_dev;

static bool wr(uint8_t reg, uint8_t val) { uint8_t b[2] = { reg, val }; return i2c_master_transmit(s_dev, b, 2, 100) == ESP_OK; }
static bool rd(uint8_t reg, uint8_t *val) { return i2c_master_transmit_receive(s_dev, &reg, 1, val, 1, 100) == ESP_OK; }
static bool seq(const uint8_t (*s)[2], size_t n) { bool ok = true; for (size_t i = 0; i < n; i++) ok &= wr(s[i][0], s[i][1]); return ok; }

void codec_port_dump(void) {
    if (!s_dev) { ESP_LOGI(TAG, "no ES8388 found"); return; }
    uint8_t c1 = 0, cp = 0, dp = 0, ap = 0, d3 = 0;
    bool ok = rd(0x00, &c1) && rd(0x02, &cp) && rd(0x04, &dp) && rd(0x03, &ap) && rd(0x19, &d3);
    ESP_LOGI(TAG, "ES8388 @0x%02x %s: CONTROL1 %02x (vmid %s) CHIPPOWER %02x DACPOWER %02x ADCPOWER %02x DACCONTROL3 %02x (%s)",
             TAB5_ADDR_ES8388, ok ? "" : "(read failed)", c1, (c1 & 3) ? "on" : "off", cp, dp, ap, d3, (d3 & 0x04) ? "muted" : "unmuted");
}

bool codec_port_present(void) { return s_dev != NULL; }

/* down: DAC muted, DAC and every output off, ADC off, the mclk gate shut,
   vmid and the references off, every block powered down */
static const uint8_t DOWN[][2] = {
    { 0x19, 0x04 },             /* DACCONTROL3: mute */
    { 0x04, 0xC0 },             /* DACPOWER: DAC L/R down, LOUT1/ROUT1/LOUT2/ROUT2 off */
    { 0x03, 0xFF },             /* ADCPOWER: all down */
    { 0x2B, 0x9C },             /* DACCONTROL21: mclk off */
    { 0x00, 0x10 },             /* CONTROL1: vmid off */
    { 0x02, 0xFF },             /* CHIPPOWER: every block down, both references off */
};
void codec_port_down(void) {
    if (!s_dev) return;
    bool ok = seq(DOWN, sizeof DOWN / sizeof *DOWN);
    uint8_t cp = 0; rd(0x02, &cp);
    ESP_LOGI(TAG, "ES8388 down%s (CHIPPOWER %02x)", ok ? "" : " - a write FAILED", cp);
}
/* up, muted: the I2S clocks must already run. codec_port_settled unmutes
   once the port's settle (vmid and the references) has passed */
bool codec_port_up(void) {
    if (!s_dev) return false;
    static const uint8_t UP[][2] = {
        { 0x19, 0x04 },         /* DACCONTROL3: muted while it comes up */
        { 0x01, 0x50 },         /* CONTROL2 */
        { 0x02, 0x00 },         /* CHIPPOWER: everything powered, references on */
        { 0x35, 0xA0 }, { 0x37, 0xD0 }, { 0x39, 0xD0 },   /* internal DLL off (the driver's low-rate fix; we run 16 kHz) */
        { 0x08, 0x00 },         /* MASTERMODE: slave */
        { 0x04, 0xC0 },         /* DACPOWER: down while it is set */
        { 0x00, 0x12 },         /* CONTROL1: same fs for ADC and DAC, vmid 500 k */
        { 0x17, 0x18 },         /* DACCONTROL1: I2S, 16-bit */
        { 0x18, 0x02 },         /* DACCONTROL2: single speed, MCLK/LRCK 256 */
        { 0x26, 0x00 },         /* DACCONTROL16: mixer inputs LIN1/RIN1 (not used: bypass off) */
        { 0x27, 0x90 },         /* DACCONTROL17: left DAC to the left mixer, 0 dB */
        { 0x2A, 0x90 },         /* DACCONTROL20: right DAC to the right mixer, 0 dB */
        { 0x2B, 0x80 },         /* DACCONTROL21: ADC and DAC share LRCK, mclk on */
        { 0x2D, 0x00 },         /* DACCONTROL23: vroi 0 */
        { 0x1A, 0x00 }, { 0x1B, 0x00 },                   /* DACCONTROL4/5: DAC volume 0 dB (the mixer sets levels) */
        { 0x2E, 0x1E }, { 0x2F, 0x1E },                   /* DACCONTROL24/25: LOUT1/ROUT1 0 dB */
        { 0x30, 0x00 }, { 0x31, 0x00 },                   /* DACCONTROL26/27: LOUT2/ROUT2 -30 dB (the BSP's) */
        { 0x03, 0xFF },         /* ADCPOWER: the ADC stays down */
        { 0x02, 0xF0 }, { 0x02, 0x00 },                   /* restart the state machines */
        { 0x04, 0x3C },         /* DACPOWER: DAC L/R up, LOUT1/ROUT1/LOUT2/ROUT2 on */
    };
    bool ok = seq(UP, sizeof UP / sizeof *UP);
    uint8_t cp = 0, dp = 0; rd(0x02, &cp); rd(0x04, &dp);
    ESP_LOGI(TAG, "ES8388 up%s (CHIPPOWER %02x DACPOWER %02x), muted until settled", ok ? "" : " - a write FAILED", cp, dp);
    return ok;
}
void codec_port_settled(void) { if (s_dev) wr(0x19, 0x00); }   /* unmute */

bool codec_port_init(i2c_master_bus_handle_t bus) {
    if (!bus) return false;
    if (i2c_master_probe(bus, TAB5_ADDR_ES8388, 50) != ESP_OK) { ESP_LOGW(TAG, "no ES8388 at 0x%02x", TAB5_ADDR_ES8388); return false; }
    i2c_device_config_t cfg = { .dev_addr_length = I2C_ADDR_BIT_LEN_7, .device_address = TAB5_ADDR_ES8388, .scl_speed_hz = 400000 };
    if (i2c_master_bus_add_device(bus, &cfg, &s_dev) != ESP_OK) { s_dev = NULL; return false; }
    bool ok = seq(DOWN, sizeof DOWN / sizeof *DOWN);
    ESP_LOGI(TAG, "ES8388 @0x%02x powered down%s", TAB5_ADDR_ES8388, ok ? "" : " - a write FAILED");
    codec_port_dump();
    return ok;
}
