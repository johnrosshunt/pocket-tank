/* board_tab5.c — M5Stack Tab5 board bring-up (board_tab5.h;
 * docs/boards/m5stack-tab5.md). Phase 1 of the port: the I2C bus, the two
 * IO expanders, and a logged bus scan plus the chip / memory facts, so the
 * first boot says on the serial console what this board really is. */
#include "board_tab5.h"
#include "esp_io_expander_pi4ioe5v6408.h"
#include "esp_log.h"
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_heap_caps.h"
#include "esp_psram.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "tab5";
static i2c_master_bus_handle_t s_bus;
static esp_io_expander_handle_t s_iox1, s_iox2;

i2c_master_bus_handle_t board_i2c_bus(void) { return s_bus; }
esp_io_expander_handle_t board_iox1(void) { return s_iox1; }
esp_io_expander_handle_t board_iox2(void) { return s_iox2; }

static const char *who(uint8_t a) {
    switch (a) {
    case TAB5_ADDR_ES8388:    return "ES8388 codec";
    case TAB5_ADDR_RX8130:    return "RX8130CE RTC";
    case TAB5_ADDR_ES7210:    return "ES7210 mic ADC";
    case TAB5_ADDR_INA226:    return "INA226 power monitor";
    case TAB5_IOX1_ADDR:      return "PI4IOE5V6408 expander 1";
    case TAB5_IOX2_ADDR:      return "PI4IOE5V6408 expander 2";
    case TAB5_ADDR_ST7123_TP: return "ST7123 touch";
    case TAB5_ADDR_BMI270:    return "BMI270 IMU";
    default:                  return "unexpected";
    }
}

static void log_chip(void) {
    esp_chip_info_t ci; esp_chip_info(&ci);
    uint32_t flash = 0; esp_flash_get_size(NULL, &flash);
    ESP_LOGI(TAG, "chip ESP32-P4 revision v%d.%d, %d cores | flash %lu MB | PSRAM %u MB (%u KB free) | internal %u KB free",
             ci.revision / 100, ci.revision % 100, ci.cores, (unsigned long)(flash >> 20),
             (unsigned)(esp_psram_get_size() >> 20), (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) >> 10),
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) >> 10));
}

static void scan(void) {
    static const uint8_t want[] = { TAB5_ADDR_ES8388, TAB5_ADDR_RX8130, TAB5_ADDR_ES7210, TAB5_ADDR_INA226,
                                    TAB5_IOX1_ADDR, TAB5_IOX2_ADDR, TAB5_ADDR_ST7123_TP, TAB5_ADDR_BMI270 };
    bool seen[128] = { false };
    int n = 0;
    for (uint8_t a = 0x08; a < 0x78; a++)
        if (i2c_master_probe(s_bus, a, 20) == ESP_OK) { seen[a] = true; n++; ESP_LOGI(TAG, "i2c 0x%02x %s", a, who(a)); }
    int missing = 0;
    for (size_t i = 0; i < sizeof want; i++)
        if (!seen[want[i]]) { missing++; ESP_LOGW(TAG, "i2c 0x%02x %s: NOT FOUND", want[i], who(want[i])); }
    ESP_LOGI(TAG, "i2c scan: %d devices, %d of %d expected missing", n, missing, (int)sizeof want);
}

/* an output pin at a level; a failure is logged, not fatal. The
 * PI4IOE5V6408 powers up with every output in HIGH-IMPEDANCE (register 0x07
 * = 0xFF) and a pull-down on every pin, and the driver's set_dir does not
 * touch that register: an "output" set high stays low until it is made
 * push-pull (the silent speaker amp, phase 6 bench 2026-09-19) */
static void iox_out(esp_io_expander_handle_t x, uint32_t pin, int level, const char *what) {
    if (!x) return;
    if (esp_io_expander_set_level(x, pin, level) != ESP_OK ||
        esp_io_expander_set_output_mode(x, pin, IO_EXPANDER_OUTPUT_MODE_PUSH_PULL) != ESP_OK ||
        esp_io_expander_set_dir(x, pin, IO_EXPANDER_OUTPUT) != ESP_OK)
        ESP_LOGW(TAG, "expander: %s failed", what);
}

bool board_init(void) {
    log_chip();
    i2c_master_bus_config_t bus = { .i2c_port = I2C_NUM_0, .sda_io_num = TAB5_I2C_SDA, .scl_io_num = TAB5_I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT, .glitch_ignore_cnt = 7, .flags.enable_internal_pullup = true };
    if (i2c_new_master_bus(&bus, &s_bus) != ESP_OK) { ESP_LOGE(TAG, "no I2C bus"); s_bus = NULL; return false; }
    if (esp_io_expander_new_i2c_pi4ioe5v6408(s_bus, TAB5_IOX1_ADDR, &s_iox1) != ESP_OK) { ESP_LOGW(TAG, "expander 1 (0x43) absent"); s_iox1 = NULL; }
    if (esp_io_expander_new_i2c_pi4ioe5v6408(s_bus, TAB5_IOX2_ADDR, &s_iox2) != ESP_OK) { ESP_LOGW(TAG, "expander 2 (0x44) absent"); s_iox2 = NULL; }
    iox_out(s_iox1, TAB5_IOX1_SPK_EN, 0, "amp off");
    iox_out(s_iox2, TAB5_IOX2_WLAN_PWR, 0, "Wi-Fi chip off");
    iox_out(s_iox1, TAB5_IOX1_TP_RST, 0, "touch reset");
    vTaskDelay(pdMS_TO_TICKS(20));
    iox_out(s_iox1, TAB5_IOX1_TP_RST, 1, "touch out of reset");
    vTaskDelay(pdMS_TO_TICKS(300));                 /* the touch chip's boot, before the scan asks for it */
    scan();
    return true;
}
