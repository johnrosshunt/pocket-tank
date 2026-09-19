/* display_port_tab5.c — the M5Stack Tab5's ST7123 panel over MIPI-DSI
 * (board_tab5.h; docs/boards/m5stack-tab5.md). The tank's 640 x 360 frame
 * goes up 2x onto the native 720 x 1280 portrait panel, turned a quarter so
 * the SD-card side is the floor: the P4's pixel-processing accelerator (PPA)
 * scales and rotates it in one pass straight into the panel's back frame
 * buffer, and the DPI driver swaps to it at the end of the frame being sent
 * (two frame buffers: no tearing, no copy). Director `disp test` shows the
 * phase-2 bench pattern.
 * Bring-up order and the ST7123 init table follow Espressif's board-support
 * package espressif/m5stack_tab5 1.3.1 (Apache-2.0): LCD reset released on
 * the expander, backlight PWM, DSI PHY power, touch firmware version, DSI
 * bus, DBI command IO, DPI panel. */
#include "display_port.h"
#include "board_tab5.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_st7123.h"
#include "esp_ldo_regulator.h"
#include "esp_cache.h"
#include "driver/ppa.h"
#include "freertos/semphr.h"
#include "tank.h"
#include "driver/ledc.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#include <stdlib.h>

static const char *TAG = "display";
#define BL_CH LEDC_CHANNEL_1
static esp_lcd_panel_handle_t s_panel;
static esp_lcd_panel_io_handle_t s_io;
static esp_lcd_dsi_bus_handle_t s_bus;
static esp_ldo_channel_handle_t s_ldo;
static uint16_t *s_fbs[2];                 /* the DPI panel's own frame buffers (PSRAM), native portrait */
static uint16_t *s_fb;                     /* the one on the glass (the bench pattern draws here) */
static int s_front;                        /* s_fbs[s_front] is on the glass, or will be by the next frame */
static ppa_client_handle_t s_ppa;
static SemaphoreHandle_t s_swapped;        /* given once the DMA has moved to the last drawn buffer */
static volatile bool s_armed;              /* a swap is asked for: the next frame-complete answers it */
static bool s_pending;                     /* a swap was asked for and not yet waited on */
static volatile bool s_hold;               /* `disp test` owns the glass: flushes are dropped */
static uint32_t s_flushes, s_swap_waits_ms, s_ppa_us;
/* the flash hunt (phase 3 bench, 2026-09-18): every frame the DPI DMA
 * finishes is timed; one far off the 15.1 ms period is logged with its
 * time, to line up with a flash seen on the glass */
static volatile int64_t s_done_us, s_odd_at_us;
static volatile uint32_t s_frames_done, s_odd, s_odd_us, s_gap_max_us;
static uint32_t s_odd_seen;
#define PANEL_FB_BYTES (TAB5_PANEL_W * TAB5_PANEL_H * 2)
/* the tank's frame, 2x on the panel: a quarter turn fills the portrait glass
 * exactly (docs/boards/m5stack-tab5.md: view (u, v) = native (v, 1279 - u)) */
_Static_assert(TANK_W * 2 == TAB5_PANEL_H && TANK_H * 2 == TAB5_PANEL_W, "the tank must be the panel's half, turned");
/* the pages (UI_X0 .. UI_X0 + UI_W, the full height) inside the corners' safe area */
#include "render.h"
_Static_assert(UI_X0 >= TANK_SAFE_INSET && UI_X0 + UI_W <= TANK_W - TANK_SAFE_INSET,
               "a page reaches into a rounded corner: lay the pages out from TANK_SAFE_INSET (board_tab5.h)");
_Static_assert(TANK_SAFE_INSET == 0, "the pages use the full height: a rounded panel needs them moved in from TANK_SAFE_INSET");
static uint8_t s_brightness = 255;
static bool s_bl_ready, s_inverted;

/* ST7123 init sequence: M5Stack's own, from M5GFX's Panel_ST7123.hpp (MIT,
 * (c) M5Stack). Espressif's BSP table lacks nine of its panel-tuning commands
 * (0xAE, 0xB2, 0xE8, 0x75, 0xE7, 0xEA, 0xB0, 0xB7, 0xBF - voltage / timing
 * registers by their place) and with the BSP's the panel flickered on the
 * bench (2026-09-18). No MADCTL (0x36): M5GFX leaves the panel's own
 * orientation, which the phase-2 orientation frame reports. */
static const st7123_lcd_init_cmd_t st7123_init[] = {
    {0x60, (uint8_t[]){0x71, 0x23, 0xa2}, 3, 0},
    {0x60, (uint8_t[]){0x71, 0x23, 0xa3}, 3, 0},
    {0x60, (uint8_t[]){0x71, 0x23, 0xa4}, 3, 0},
    {0xA4, (uint8_t[]){0x31}, 1, 0},
    {0xD7, (uint8_t[]){0x10, 0x0A, 0x10, 0x2A, 0x80, 0x80}, 6, 0},
    {0x90, (uint8_t[]){0x71, 0x23, 0x5A, 0x20, 0x24, 0x09, 0x09}, 7, 0},
    {0xA3, (uint8_t[]){0x80, 0x01, 0x88, 0x30, 0x05, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46, 0x00, 0x00, 0x1E, 0x5C, 0x1E, 0x80, 0x00, 0x4F, 0x05, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46, 0x00, 0x00, 0x1E, 0x5C, 0x1E, 0x80, 0x00, 0x6F, 0x58, 0x00, 0x00, 0x00, 0xFF}, 39, 0},
    {0xA6, (uint8_t[]){0x03, 0x00, 0x24, 0x55, 0x36, 0x00, 0x39, 0x00, 0x6E, 0x6E, 0x91, 0xFF, 0x00, 0x24, 0x55, 0x38, 0x00, 0x37, 0x00, 0x6E, 0x6E, 0x91, 0xFF, 0x00, 0x24, 0x11, 0x00, 0x00, 0x00, 0x00, 0x6E, 0x6E, 0x91, 0xFF, 0x00, 0xEC, 0x11, 0x00, 0x03, 0x00, 0x03, 0x6E, 0x6E, 0xFF, 0xFF, 0x00, 0x08, 0x80, 0x08, 0x80, 0x06, 0x00, 0x00, 0x00, 0x00}, 55, 0},
    {0xA7, (uint8_t[]){0x19, 0x19, 0x80, 0x64, 0x40, 0x07, 0x16, 0x40, 0x00, 0x44, 0x03, 0x6E, 0x6E, 0x91, 0xFF, 0x08, 0x80, 0x64, 0x40, 0x25, 0x34, 0x40, 0x00, 0x02, 0x01, 0x6E, 0x6E, 0x91, 0xFF, 0x08, 0x80, 0x64, 0x40, 0x00, 0x00, 0x40, 0x00, 0x00, 0x00, 0x6E, 0x6E, 0x91, 0xFF, 0x08, 0x80, 0x64, 0x40, 0x00, 0x00, 0x00, 0x00, 0x20, 0x00, 0x6E, 0x6E, 0x84, 0xFF, 0x08, 0x80, 0x44}, 60, 0},
    {0xAC, (uint8_t[]){0x03, 0x19, 0x19, 0x18, 0x18, 0x06, 0x13, 0x13, 0x11, 0x11, 0x08, 0x08, 0x0A, 0x0A, 0x1C, 0x1C, 0x07, 0x07, 0x00, 0x00, 0x02, 0x02, 0x01, 0x19, 0x19, 0x18, 0x18, 0x06, 0x12, 0x12, 0x10, 0x10, 0x09, 0x09, 0x0B, 0x0B, 0x1C, 0x1C, 0x07, 0x07, 0x03, 0x03, 0x01, 0x01}, 44, 0},
    {0xAD, (uint8_t[]){0xF0, 0x00, 0x46, 0x00, 0x03, 0x50, 0x50, 0xFF, 0xFF, 0xF0, 0x40, 0x06, 0x01, 0x07, 0x42, 0x42, 0xFF, 0xFF, 0x01, 0x00, 0x00, 0xFF, 0xFF, 0xFF, 0xFF}, 25, 0},
    {0xAE, (uint8_t[]){0xFE, 0x3F, 0x3F, 0xFE, 0x3F, 0x3F, 0x00}, 7, 0},
    {0xB2, (uint8_t[]){0x15, 0x19, 0x05, 0x23, 0x49, 0xAF, 0x03, 0x2E, 0x5C, 0xD2, 0xFF, 0x10, 0x20, 0xFD, 0x20, 0xC0, 0x00}, 17, 0},
    {0xE8, (uint8_t[]){0x20, 0x6F, 0x04, 0x97, 0x97, 0x3E, 0x04, 0xDC, 0xDC, 0x3E, 0x06, 0xFA, 0x26, 0x3E}, 14, 0},
    {0x75, (uint8_t[]){0x03, 0x04}, 2, 0},
    {0xE7, (uint8_t[]){0x3B, 0x00, 0x00, 0x7C, 0xA1, 0x8C, 0x20, 0x1A, 0xF0, 0xB1, 0x50, 0x00, 0x50, 0xB1, 0x50, 0xB1, 0x50, 0xD8, 0x00, 0x55, 0x00, 0xB1, 0x00, 0x45, 0xC9, 0x6A, 0xFF, 0x5A, 0xD8, 0x18, 0x88, 0x15, 0xB1, 0x01, 0x01, 0x77}, 36, 0},
    {0xEA, (uint8_t[]){0x13, 0x00, 0x04, 0x00, 0x00, 0x00, 0x00, 0x2C}, 8, 0},
    {0xB0, (uint8_t[]){0x22, 0x43, 0x11, 0x61, 0x25, 0x43, 0x43}, 7, 0},
    {0xB7, (uint8_t[]){0x00, 0x00, 0x73, 0x73}, 4, 0},
    {0xBF, (uint8_t[]){0xA6, 0xAA}, 2, 0},
    {0xA9, (uint8_t[]){0x00, 0x00, 0x73, 0xFF, 0x00, 0x00, 0x03, 0x00, 0x00, 0x03}, 10, 0},
    {0xC8, (uint8_t[]){0x00, 0x00, 0x10, 0x1F, 0x36, 0x00, 0x5D, 0x04, 0x9D, 0x05, 0x10, 0xF2, 0x06, 0x60, 0x03, 0x11, 0xAD, 0x00, 0xEF, 0x01, 0x22, 0x2E, 0x0E, 0x74, 0x08, 0x32, 0xDC, 0x09, 0x33, 0x0F, 0xF3, 0x77, 0x0D, 0xB0, 0xDC, 0x03, 0xFF}, 37, 0},
    {0xC9, (uint8_t[]){0x00, 0x00, 0x10, 0x1F, 0x36, 0x00, 0x5D, 0x04, 0x9D, 0x05, 0x10, 0xF2, 0x06, 0x60, 0x03, 0x11, 0xAD, 0x00, 0xEF, 0x01, 0x22, 0x2E, 0x0E, 0x74, 0x08, 0x32, 0xDC, 0x09, 0x33, 0x0F, 0xF3, 0x77, 0x0D, 0xB0, 0xDC, 0x03, 0xFF}, 37, 0},
    {0x11, (uint8_t[]){0x00}, 0, 120},                /* sleep out */
    {0x29, (uint8_t[]){0x00}, 0, 50},                 /* display on */
    {0x35, (uint8_t[]){0x00}, 1, 0},                  /* tearing effect line on */
};

/* 8-bit duty, the level itself; 255 is written as 256 = high the whole
 * period (a 10-bit 1023 left a 1-tick low pulse on the boost's EN every cycle) */
static uint32_t s_bl_hz = TAB5_LCD_BL_HZ;
static void backlight(uint8_t level) {
    if (!s_bl_ready) return;
    ledc_set_duty(LEDC_LOW_SPEED_MODE, BL_CH, level == 255 ? 256 : level);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, BL_CH);
}
static bool backlight_init(void) {
    const ledc_timer_config_t t = { .speed_mode = LEDC_LOW_SPEED_MODE, .duty_resolution = LEDC_TIMER_8_BIT,
                                    .timer_num = LEDC_TIMER_0, .freq_hz = TAB5_LCD_BL_HZ, .clk_cfg = LEDC_AUTO_CLK };
    const ledc_channel_config_t c = { .gpio_num = TAB5_LCD_BL_GPIO, .speed_mode = LEDC_LOW_SPEED_MODE, .channel = BL_CH,
                                      .intr_type = LEDC_INTR_DISABLE, .timer_sel = LEDC_TIMER_0, .duty = 0, .hpoint = 0 };
    s_bl_ready = ledc_timer_config(&t) == ESP_OK && ledc_channel_config(&c) == ESP_OK;
    if (!s_bl_ready) ESP_LOGE(TAG, "backlight PWM on GPIO %d failed", TAB5_LCD_BL_GPIO);
    return s_bl_ready;
}

/* the touch half of the ST7123 names the panel: firmware 3 = ST7123, 1 = ST7121
 * (the BSP's test). Register 0x0000, 16-bit address. -1 = no answer. */
static int touch_fw_version(void) {
    i2c_master_dev_handle_t dev;
    i2c_device_config_t cfg = { .dev_addr_length = I2C_ADDR_BIT_LEN_7, .device_address = TAB5_ADDR_ST7123_TP, .scl_speed_hz = 400000 };
    if (i2c_master_probe(board_i2c_bus(), TAB5_ADDR_ST7123_TP, 100) != ESP_OK) return -1;
    if (i2c_master_bus_add_device(board_i2c_bus(), &cfg, &dev) != ESP_OK) return -1;
    uint8_t reg[2] = { 0x00, 0x00 }, fw = 0;
    esp_err_t e = i2c_master_transmit_receive(dev, reg, 2, &fw, 1, 100);
    i2c_master_bus_rm_device(dev);
    return e == ESP_OK ? fw : -1;
}

/* ---- the phase-2 bench pattern ---- */
static void fb_sync(void) { esp_cache_msync(s_fb, TAB5_PANEL_W * TAB5_PANEL_H * 2, ESP_CACHE_MSYNC_FLAG_DIR_C2M); }
static void fill(uint16_t c) {
    for (int i = 0; i < TAB5_PANEL_W * TAB5_PANEL_H; i++) s_fb[i] = c;
    fb_sync();
}
static void rect(int x, int y, int w, int h, uint16_t c) {
    for (int yy = y; yy < y + h; yy++) for (int xx = x; xx < x + w; xx++) s_fb[yy * TAB5_PANEL_W + xx] = c;
}
#define RGB565_RED    0xF800
#define RGB565_GREEN  0x07E0
#define RGB565_BLUE   0x001F
#define RGB565_WHITE  0xFFFF
#define RGB565_YELLOW 0xFFE0
static void test_pattern(void) {
    static const struct { uint16_t c; const char *name; } solid[] = {
        { RGB565_RED, "RED" }, { RGB565_GREEN, "GREEN" }, { RGB565_BLUE, "BLUE" }, { RGB565_WHITE, "WHITE" }, { 0, "BLACK" } };
    for (size_t i = 0; i < sizeof solid / sizeof solid[0]; i++) {
        ESP_LOGI(TAG, "pattern %d/5: whole screen %s", (int)i + 1, solid[i].name);
        fill(solid[i].c);
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
    /* the orientation frame, in the panel's NATIVE coordinates: a 1-px yellow
       border on the outermost pixels, and a 96-px square in each native corner */
    const int W = TAB5_PANEL_W, H = TAB5_PANEL_H, S = 96, M = 8;
    memset(s_fb, 0, W * H * 2);
    rect(0, 0, W, 1, RGB565_YELLOW); rect(0, H - 1, W, 1, RGB565_YELLOW);
    rect(0, 0, 1, H, RGB565_YELLOW); rect(W - 1, 0, 1, H, RGB565_YELLOW);
    rect(M, M, S, S, RGB565_RED);                     /* native (0, 0) */
    rect(W - M - S, M, S, S, RGB565_GREEN);           /* native (719, 0) */
    rect(M, H - M - S, S, S, RGB565_BLUE);            /* native (0, 1279) */
    rect(W - M - S, H - M - S, S, S, RGB565_WHITE);   /* native (719, 1279) */
    fb_sync();
    ESP_LOGI(TAG, "pattern: orientation frame - yellow 1-px border on the outermost pixels; corner squares "
                  "RED = native (0,0), GREEN = native (719,0), BLUE = native (0,1279), WHITE = native (719,1279)");
    static const uint8_t steps[] = { 255, 128, 26, 255 };
    for (size_t i = 0; i < sizeof steps; i++) {
        backlight(steps[i]);
        ESP_LOGI(TAG, "pattern: backlight %d%%", steps[i] * 100 / 255);
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
    backlight(s_brightness);
    ESP_LOGI(TAG, "pattern done: the tank's frames resume");
}

/* the DPI DMA has finished a frame and restarted on the current buffer: if a
 * swap was asked for, that restart used the new one - the old one is free */
static bool IRAM_ATTR on_frame_done(esp_lcd_panel_handle_t panel, esp_lcd_dpi_panel_event_data_t *edata, void *ctx) {
    (void)panel; (void)edata; (void)ctx;
    int64_t now = esp_timer_get_time();
    if (s_done_us) {
        uint32_t d = (uint32_t)(now - s_done_us);
        if (d > s_gap_max_us) s_gap_max_us = d;
        if (d > 20000 || d < 10000) { s_odd++; s_odd_us = d; s_odd_at_us = now; }
    }
    s_done_us = now; s_frames_done++;
    if (!s_armed) return false;
    s_armed = false;
    BaseType_t woken = pdFALSE;
    xSemaphoreGiveFromISR(s_swapped, &woken);
    return woken == pdTRUE;
}
/* the last swap has happened (or 100 ms passed: a stalled panel must not
 * stall the tank) - the buffer off the glass may be drawn */
static void wait_swap(void) {
    if (!s_pending) return;
    int64_t t0 = esp_timer_get_time();
    if (xSemaphoreTake(s_swapped, pdMS_TO_TICKS(100)) != pdTRUE) {
        static int logged;
        if (logged++ < 3) ESP_LOGW(TAG, "frame swap not seen in 100 ms");
    }
    s_swap_waits_ms += (uint32_t)((esp_timer_get_time() - t0) / 1000);
    s_pending = false;
}

bool display_port_init(void) {
    esp_io_expander_handle_t x = board_iox1();
    if (!board_i2c_bus() || !x) { ESP_LOGE(TAG, "no I2C bus / expander 1: no panel"); return false; }
    /* LCD reset released: the docs' rule - input with its pull-up, never
       driven high (the expander driver's power-on pull-downs held it low) */
    if (esp_io_expander_set_pullupdown(x, TAB5_IOX1_LCD_RST, IO_EXPANDER_PULL_UP) != ESP_OK ||
        esp_io_expander_set_dir(x, TAB5_IOX1_LCD_RST, IO_EXPANDER_INPUT) != ESP_OK)
        ESP_LOGW(TAG, "LCD reset release on expander 1 P4 failed");
    backlight_init();
    esp_ldo_channel_config_t ldo_cfg = { .chan_id = TAB5_DSI_PHY_LDO, .voltage_mv = TAB5_DSI_PHY_MV };
    if (esp_ldo_acquire_channel(&ldo_cfg, &s_ldo) != ESP_OK) { ESP_LOGE(TAG, "DSI PHY LDO %d failed", TAB5_DSI_PHY_LDO); s_ldo = NULL; return false; }
    vTaskDelay(pdMS_TO_TICKS(500));                   /* the BSP's settle before it asks the touch half */
    int fw = touch_fw_version();
    if (fw == 3)       ESP_LOGI(TAG, "touch 0x55 answers, firmware 3: ST7123 panel");
    else if (fw == 1)  ESP_LOGW(TAG, "touch 0x55 answers, firmware 1: an ST7121 panel - not this port's board version");
    else if (fw < 0)   ESP_LOGW(TAG, "touch 0x55 does not answer: panel version unknown, initialising as ST7123");
    else               ESP_LOGW(TAG, "touch 0x55 firmware %d: unknown panel version, initialising as ST7123", fw);

    esp_lcd_dsi_bus_config_t bus_cfg = { .bus_id = 0, .num_data_lanes = TAB5_DSI_LANES, .lane_bit_rate_mbps = TAB5_DSI_LANE_MBPS };
    if (esp_lcd_new_dsi_bus(&bus_cfg, &s_bus) != ESP_OK) { ESP_LOGE(TAG, "DSI bus failed"); return false; }
    esp_lcd_dbi_io_config_t dbi = { .virtual_channel = 0, .lcd_cmd_bits = 8, .lcd_param_bits = 8 };
    if (esp_lcd_new_panel_io_dbi(s_bus, &dbi, &s_io) != ESP_OK) { ESP_LOGE(TAG, "DBI IO failed"); return false; }
    const esp_lcd_dpi_panel_config_t dpi = {
        .virtual_channel = 0, .dpi_clk_src = MIPI_DSI_DPI_CLK_SRC_DEFAULT, .dpi_clock_freq_mhz = TAB5_DPI_CLK_MHZ,
        .in_color_format = LCD_COLOR_FMT_RGB565, .num_fbs = 2,
        .video_timing = { .h_size = TAB5_PANEL_W, .v_size = TAB5_PANEL_H,
                          .hsync_back_porch = 40, .hsync_pulse_width = 2, .hsync_front_porch = 40,
                          .vsync_back_porch = 8, .vsync_pulse_width = 2, .vsync_front_porch = 220 },
    };
    const st7123_vendor_config_t vendor = { .init_cmds = st7123_init, .init_cmds_size = sizeof st7123_init / sizeof st7123_init[0],
                                            .mipi_config = { .dsi_bus = s_bus, .dpi_config = &dpi } };
    const esp_lcd_panel_dev_config_t pcfg = { .reset_gpio_num = -1, .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
                                              .bits_per_pixel = 16, .vendor_config = (void *)&vendor };
    if (esp_lcd_new_panel_st7123(s_io, &pcfg, &s_panel) != ESP_OK) { ESP_LOGE(TAG, "ST7123 panel failed"); return false; }
    esp_lcd_panel_reset(s_panel);
    esp_lcd_panel_init(s_panel);
    esp_lcd_panel_disp_on_off(s_panel, true);
    if (esp_lcd_dpi_panel_get_frame_buffer(s_panel, 2, (void **)&s_fbs[0], (void **)&s_fbs[1]) != ESP_OK || !s_fbs[0] || !s_fbs[1]) {
        ESP_LOGE(TAG, "no frame buffers"); return false; }
    s_fb = s_fbs[0]; s_front = 0;
    if (!s_swapped) s_swapped = xSemaphoreCreateBinary();
    s_armed = false; s_pending = false; s_done_us = 0;
    const esp_lcd_dpi_panel_event_callbacks_t cbs = { .on_frame_buf_complete = on_frame_done };
    if (!s_swapped || esp_lcd_dpi_panel_register_event_callbacks(s_panel, &cbs, NULL) != ESP_OK) { ESP_LOGE(TAG, "frame-done callback failed"); return false; }
    const ppa_client_config_t ppa_cfg = { .oper_type = PPA_OPERATION_SRM, .max_pending_trans_num = 1 };
    if (!s_ppa && ppa_register_client(&ppa_cfg, &s_ppa) != ESP_OK) { ESP_LOGE(TAG, "PPA client failed: no frames"); s_ppa = NULL; }
    const int ht = TAB5_PANEL_W + 40 + 2 + 40, vt = TAB5_PANEL_H + 8 + 2 + 220;
    ESP_LOGI(TAG, "panel up: %d x %d native, %d lanes x %d Mbps, %d MHz pixel clock (%.1f Hz refresh), backlight PWM %lu Hz",
             TAB5_PANEL_W, TAB5_PANEL_H, TAB5_DSI_LANES, TAB5_DSI_LANE_MBPS, TAB5_DPI_CLK_MHZ,
             TAB5_DPI_CLK_MHZ * 1e6 / ((double)ht * vt), (unsigned long)s_bl_hz);
    for (int i = 0; i < 2; i++) { memset(s_fbs[i], 0, PANEL_FB_BYTES); esp_cache_msync(s_fbs[i], PANEL_FB_BYTES, ESP_CACHE_MSYNC_FLAG_DIR_C2M); }
    backlight(s_brightness);
    ESP_LOGI(TAG, "frames: %d x %d tank -> PPA 2x + quarter turn -> %d x %d panel, two frame buffers", TANK_W, TANK_H, TAB5_PANEL_W, TAB5_PANEL_H);
    return true;
}

/* the tank's frame (TANK_W x TANK_H RGB565, little-endian as rendered) onto
 * the back buffer, then swapped onto the glass at the next frame boundary.
 * The PPA writes the input back from the cache and invalidates its output
 * itself (esp_driver_ppa ppa_srm.c). Flipped: the other quarter turn. */
void display_port_flush(const uint16_t *fb) {
    if (s_odd != s_odd_seen) {                         /* the flash hunt: a frame off its period */
        s_odd_seen = s_odd;
        ESP_LOGW(TAG, "frame timing: a frame took %.1f ms (period 15.1) at %.3f s - %lu so far",
                 s_odd_us / 1000.0, s_odd_at_us / 1e6, (unsigned long)s_odd);
    }
    if (!s_panel || !s_ppa || !fb || s_hold) return;
    wait_swap();
    int back = s_front ^ 1;
    const ppa_srm_oper_config_t op = {
        .in = { .buffer = fb, .pic_w = TANK_W, .pic_h = TANK_H, .block_w = TANK_W, .block_h = TANK_H,
                .block_offset_x = 0, .block_offset_y = 0, .srm_cm = PPA_SRM_COLOR_MODE_RGB565 },
        .out = { .buffer = s_fbs[back], .buffer_size = PANEL_FB_BYTES, .pic_w = TAB5_PANEL_W, .pic_h = TAB5_PANEL_H,
                 .block_offset_x = 0, .block_offset_y = 0, .srm_cm = PPA_SRM_COLOR_MODE_RGB565 },
        /* counter-clockwise: 90 takes view (u, v) to native (v, 1279 - u) */
        .rotation_angle = s_inverted ? PPA_SRM_ROTATION_ANGLE_270 : PPA_SRM_ROTATION_ANGLE_90,
        .scale_x = 2.0f, .scale_y = 2.0f,
        .mode = PPA_TRANS_MODE_BLOCKING,
    };
    int64_t t0 = esp_timer_get_time();
    esp_err_t err = ppa_do_scale_rotate_mirror(s_ppa, &op);
    s_ppa_us += (uint32_t)(esp_timer_get_time() - t0);
    if (err != ESP_OK) {
        static int logged;
        if (logged++ < 3) ESP_LOGE(TAG, "PPA: %s", esp_err_to_name(err));
        return;
    }
    xSemaphoreTake(s_swapped, 0);                  /* nothing stale */
    /* one of the panel's own buffers: the driver only points the DMA at it */
    err = esp_lcd_panel_draw_bitmap(s_panel, 0, 0, TAB5_PANEL_W, TAB5_PANEL_H, s_fbs[back]);
    if (err != ESP_OK) {
        static int logged;
        if (logged++ < 3) ESP_LOGE(TAG, "draw_bitmap: %s", esp_err_to_name(err));
        return;
    }
    s_armed = true; s_pending = true;
    s_front = back; s_fb = s_fbs[back];
    s_flushes++;
}
/* sleep: the whole pipeline down, not just the picture. With the panel only
 * blanked, its DPI DMA kept streaming the frame buffer out of PSRAM through
 * the light-sleep grace (`lcd.dsi: ... underrun`) and the deep-sleep entry
 * then hung until a watchdog reset (phase 7 bench, 2026-09-19). So: panel
 * off, the DPI panel deleted (its DMA stopped, its frame buffers freed), the
 * DBI IO and the DSI bus deleted, the PHY's LDO released, and the panel held
 * in reset on expander 1 P4. Wake builds it all again (display_port_init). */
void display_port_sleep(void) {
    backlight(0);
    if (!s_panel) return;
    s_hold = true;                                   /* the tank task is ours now; no flush lands */
    wait_swap();
    esp_lcd_panel_disp_on_off(s_panel, false);
    esp_lcd_panel_del(s_panel); s_panel = NULL;
    s_fbs[0] = s_fbs[1] = s_fb = NULL;
    if (s_io) { esp_lcd_panel_io_del(s_io); s_io = NULL; }
    if (s_bus) { esp_lcd_del_dsi_bus(s_bus); s_bus = NULL; }
    if (s_ldo) { esp_ldo_release_channel(s_ldo); s_ldo = NULL; }
    esp_io_expander_handle_t x = board_iox1();
    if (!board_iox_out(x, TAB5_IOX1_LCD_RST, 0)) ESP_LOGW(TAG, "LCD reset: could not assert it");
    ESP_LOGI(TAG, "panel down: DPI stopped, DSI bus and PHY power off, LCD held in reset");
}
void display_port_wake(void) {
    if (s_panel) { backlight(s_brightness); return; }
    bool ok = display_port_init();                   /* releases LCD reset, rebuilds bus, IO and panel */
    s_hold = false;
    ESP_LOGI(TAG, "panel %s", ok ? "back up" : "FAILED to come back");
}
void display_port_set_inverted(bool inverted) { s_inverted = inverted; }
void display_port_set_brightness(uint8_t level) {
    if (level != s_brightness) ESP_LOGI(TAG, "backlight %d -> %d /255", s_brightness, level);   /* the flash hunt */
    s_brightness = level; backlight(level);
}
uint8_t display_port_brightness(void) { return s_brightness; }

/* director `disp` (the phase-2 flicker hunt): `disp` the settings,
 * `disp bl <hz>` the backlight PWM frequency live, `disp bl steady` the EN
 * held high (no PWM at all), `disp bl pwm` PWM again at the last frequency */
void display_port_director(int argc, char **argv) {
    if (argc >= 3 && !strcmp(argv[1], "hold")) {       /* the flash hunt: no new frames, the panel keeps refreshing the last */
        s_hold = strcmp(argv[2], "off") != 0;
        ESP_LOGI(TAG, "hold %s", s_hold ? "on: the last frame stays on the glass, no PPA, no swaps" : "off: frames flow");
        return;
    }
    if (argc >= 2 && !strcmp(argv[1], "test")) {       /* the phase-2 bench pattern, over the tank for ~20 s */
        s_hold = true;
        vTaskDelay(pdMS_TO_TICKS(50));                 /* a flush in flight finishes */
        wait_swap();
        test_pattern();
        s_hold = false;
        return;
    }
    if (argc >= 3 && !strcmp(argv[1], "bl")) {
        if (!strcmp(argv[2], "steady")) {
            ledc_stop(LEDC_LOW_SPEED_MODE, BL_CH, 1); s_bl_ready = false;
            ESP_LOGI(TAG, "backlight: EN held high, no PWM (brightness commands do nothing until `disp bl pwm`)");
        } else {
            uint32_t hz = strcmp(argv[2], "pwm") ? (uint32_t)atoi(argv[2]) : s_bl_hz;
            if (hz < 100 || hz > 100000) { ESP_LOGW(TAG, "backlight: %lu Hz is out of range (100..100000)", (unsigned long)hz); return; }
            s_bl_ready = true;
            if (ledc_set_freq(LEDC_LOW_SPEED_MODE, LEDC_TIMER_0, hz) != ESP_OK) { ESP_LOGW(TAG, "backlight: %lu Hz not reachable", (unsigned long)hz); return; }
            s_bl_hz = hz; backlight(s_brightness);
            ESP_LOGI(TAG, "backlight: PWM %lu Hz at %d/255", (unsigned long)hz, s_brightness);
        }
        return;
    }
    ESP_LOGI(TAG, "disp: ST7123, %d lanes x %d Mbps, %d MHz pixel clock | backlight %s %lu Hz, level %d/255 | `disp bl <hz>|steady|pwm`, `disp test`",
             TAB5_DSI_LANES, TAB5_DSI_LANE_MBPS, TAB5_DPI_CLK_MHZ, s_bl_ready ? "PWM" : "steady (no PWM)", (unsigned long)s_bl_hz, s_brightness);
    ESP_LOGI(TAG, "disp: %lu frames flushed; PPA %.2f ms each; waits for the swap %.2f ms each%s%s",
             (unsigned long)s_flushes, s_flushes ? s_ppa_us / 1000.0 / s_flushes : 0.0,
             s_flushes ? (double)s_swap_waits_ms / s_flushes : 0.0, s_inverted ? " | flipped" : "", s_hold ? " | HOLD" : "");
    ESP_LOGI(TAG, "disp: panel frames %lu, longest %.1f ms, %lu off the 15.1 ms period | `disp hold on|off`",
             (unsigned long)s_frames_done, s_gap_max_us / 1000.0, (unsigned long)s_odd);
}
