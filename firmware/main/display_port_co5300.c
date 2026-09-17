/* display_port_co5300.c — Waveshare 2.16" AMOLED (CO5300, 480x480) over QSPI
 * via esp_lcd. The tank renders 480x480 (common/render.c), the whole panel,
 * and is sent unrotated in 32-row DMA stripes. */
#include "display_port.h"
#include "board_pins.h"
#include "tank.h"
#include "driver/spi_master.h"
#include "driver/i2c_master.h"
#include "driver/gpio.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_co5300.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <string.h>

static const char *TAG = "co5300";
#define LCD_HOST SPI2_HOST
#define STRIPE_ROWS 32                         /* panel rows per DMA transfer */

_Static_assert(TANK_W == PANEL_W && TANK_H == PANEL_H && PANEL_H % STRIPE_ROWS == 0,
               "the tank is the whole panel, in whole stripes");

static esp_lcd_panel_handle_t s_panel;
static esp_lcd_panel_io_handle_t s_io;          /* kept for DCS writes after init (brightness) */
static uint8_t s_brightness = 0xFF;             /* what init_cmds' 0x51 sets */
static uint16_t *s_stripe[2];                   /* PANEL_W x STRIPE_ROWS, DMA-capable; ping-pong */
static SemaphoreHandle_t s_stripe_free;         /* counts stripe buffers not in DMA flight */
static i2c_master_bus_handle_t s_i2c;
static bool s_inverted;                         /* 180-degree flip, done in the copy */

void display_port_set_inverted(bool inverted) { s_inverted = inverted; }

static bool on_trans_done(esp_lcd_panel_io_handle_t io, esp_lcd_panel_io_event_data_t *ev, void *ctx) {
    (void)io; (void)ev; (void)ctx;
    BaseType_t hp = pdFALSE;
    xSemaphoreGiveFromISR(s_stripe_free, &hp);
    return hp == pdTRUE;
}

i2c_master_bus_handle_t board_i2c_bus(void) { return s_i2c; }

/* Waveshare's BSP sequence (esp32_s3_touch_amoled_2_16 2.0.1), with its 600 ms
 * sleep-out wait cut to the usual 120 ms and display-on left out: that comes
 * after the black clear, so the first thing lit is not stale panel RAM.
 * MADCTL 0xA0 is the BSP's upright orientation. */
static const co5300_lcd_init_cmd_t init_cmds[] = {
    {0x11, NULL, 0, 120},
    {0xFE, (uint8_t[]){0x20}, 1, 0},
    {0x19, (uint8_t[]){0x10}, 1, 0},
    {0x1C, (uint8_t[]){0xA0}, 1, 0},
    {0xFE, (uint8_t[]){0x00}, 1, 0},
    {0xC4, (uint8_t[]){0x80}, 1, 0},
    {0x3A, (uint8_t[]){0x55}, 1, 0},
    {0x35, (uint8_t[]){0x00}, 1, 0},
    {0x53, (uint8_t[]){0x20}, 1, 0},
    {0x51, (uint8_t[]){0xFF}, 1, 0},
    {0x63, (uint8_t[]){0xFF}, 1, 0},
    {0x2A, (uint8_t[]){0x00, 0x00, 0x01, 0xDF}, 4, 0},
    {0x2B, (uint8_t[]){0x00, 0x00, 0x01, 0xDF}, 4, 0},
    {0x36, (uint8_t[]){0xA0}, 1, 0},
};

/* the whole panel to black before display-on: the first frame comes later,
 * and panel RAM holds noise until then */
static void clear_panel(void) {
    memset(s_stripe[0], 0, PANEL_W * STRIPE_ROWS * 2);
    for (int y = 0; y < PANEL_H; y += STRIPE_ROWS) {
        xSemaphoreTake(s_stripe_free, portMAX_DELAY);
        if (esp_lcd_panel_draw_bitmap(s_panel, 0, y, PANEL_W, y + STRIPE_ROWS, s_stripe[0]) != ESP_OK)
            xSemaphoreGive(s_stripe_free);
    }
}

bool display_port_init(void) {
    i2c_master_bus_config_t bus = { .i2c_port = I2C_NUM_0, .sda_io_num = PIN_I2C_SDA, .scl_io_num = PIN_I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT, .glitch_ignore_cnt = 7, .flags.enable_internal_pullup = true };
    ESP_ERROR_CHECK(i2c_new_master_bus(&bus, &s_i2c));

    for (int i = 0; i < 2; i++) {
        s_stripe[i] = heap_caps_malloc(PANEL_W * STRIPE_ROWS * 2, MALLOC_CAP_DMA);
        if (!s_stripe[i]) return false;
    }
    s_stripe_free = xSemaphoreCreateCounting(2, 2);
    const spi_bus_config_t spi = CO5300_PANEL_BUS_QSPI_CONFIG(PIN_LCD_PCLK, PIN_LCD_DATA0, PIN_LCD_DATA1,
                                                              PIN_LCD_DATA2, PIN_LCD_DATA3, PANEL_W * STRIPE_ROWS * 2);
    ESP_ERROR_CHECK(spi_bus_initialize(LCD_HOST, &spi, SPI_DMA_CH_AUTO));
    esp_lcd_panel_io_handle_t io;
    esp_lcd_panel_io_spi_config_t io_cfg = CO5300_PANEL_IO_QSPI_CONFIG(PIN_LCD_CS, on_trans_done, NULL);
    io_cfg.pclk_hz = 80 * 1000 * 1000;              /* the BSP runs 40 MHz; drop to that if the picture tears or speckles */
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_HOST, &io_cfg, &io));
    s_io = io;
    const co5300_vendor_config_t vendor = { .init_cmds = init_cmds, .init_cmds_size = sizeof init_cmds / sizeof init_cmds[0],
                                            .flags.use_qspi_interface = 1 };
    const esp_lcd_panel_dev_config_t pcfg = { .reset_gpio_num = PIN_LCD_RST, .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
                                              .bits_per_pixel = 16, .vendor_config = (void *)&vendor };
    ESP_ERROR_CHECK(esp_lcd_new_panel_co5300(io, &pcfg, &s_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_reset(s_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(s_panel));
    clear_panel();
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(s_panel, true));
    ESP_LOGI(TAG, "panel up: %dx%d, the tank's frame", PANEL_W, PANEL_H);
    return true;
}

/* sleep-mode power-down: display off, sleep-in + deep standby (the driver
 * sends DSTBON because the panel has a reset line), touch chip held in reset.
 * The panel and touch run off VCC3V3 on this board - there is no rail to cut.
 * display_port_wake (a hardware reset + full init) brings both back. */
void display_port_sleep(void) {
    if (!s_panel) return;
    esp_lcd_panel_disp_on_off(s_panel, false);
    esp_lcd_panel_disp_sleep(s_panel, true);
    gpio_set_level(PIN_TP_RST, 0);
}

/* drowse wake: touch out of reset, then the full panel init over the still-
 * open QSPI/I2C buses - no reboot needed. init_cmds is file-static, so the
 * driver's retained pointer stays valid for this re-init. */
void display_port_wake(void) {
    if (!s_panel) return;
    gpio_set_level(PIN_TP_RST, 1);
    esp_lcd_panel_reset(s_panel);                   /* ends deep standby; 150 ms covers the touch chip's boot too */
    esp_lcd_panel_init(s_panel);
    display_port_set_brightness(s_brightness);      /* init_cmds put it back at 255 */
    clear_panel();
    esp_lcd_panel_disp_on_off(s_panel, true);
}

/* DCS 0x51 WRDISBV over the QSPI link: the co5300 driver frames a command as
 * <write opcode 0x02><cmd><00> in a 32-bit word, so we do the same here */
void display_port_set_brightness(uint8_t level) {
    s_brightness = level;
    if (!s_io) return;
    int cmd = (0x02 << 24) | (0x51 << 8);
    esp_lcd_panel_io_tx_param(s_io, cmd, &level, 1);
}
uint8_t display_port_brightness(void) { return s_brightness; }

/* fb[y][x] (TANK_W x TANK_H) -> the panel, row for row; flipped, panel
 * (x,y) = fb[TANK_H-1-y][TANK_W-1-x]. Colors are byte-swapped for the panel
 * (big-endian RGB565 over SPI). Two stripe buffers ping-pong so the copy of
 * stripe N+1 overlaps the DMA of stripe N. */
void display_port_flush(const uint16_t *fb) {
    int cur = 0;
    for (int y0 = 0; y0 < TANK_H; y0 += STRIPE_ROWS) {
        xSemaphoreTake(s_stripe_free, portMAX_DELAY);
        uint16_t *stripe = s_stripe[cur];
        for (int r = 0; r < STRIPE_ROWS; r++) {
            uint16_t *dst = stripe + r * TANK_W;
            if (!s_inverted) {
                const uint16_t *src = fb + (y0 + r) * TANK_W;
                for (int x = 0; x < TANK_W; x++) dst[x] = __builtin_bswap16(src[x]);
            } else {
                const uint16_t *src = fb + (TANK_H - 1 - y0 - r) * TANK_W + TANK_W - 1;
                for (int x = 0; x < TANK_W; x++) dst[x] = __builtin_bswap16(src[-x]);
            }
        }
        esp_err_t err = esp_lcd_panel_draw_bitmap(s_panel, 0, y0, TANK_W, y0 + STRIPE_ROWS, stripe);
        if (err != ESP_OK) {
            static int logged;
            if (logged++ < 3) ESP_LOGE(TAG, "draw_bitmap y0=%d: %s", y0, esp_err_to_name(err));
            xSemaphoreGive(s_stripe_free);          /* nothing went into flight: no callback will return it */
        }
        cur ^= 1;
    }
}
