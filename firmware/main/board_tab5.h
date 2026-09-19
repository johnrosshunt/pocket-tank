/* board_tab5.h — M5Stack Tab5, October 2025 version (ST7123 display + touch).
 * Every pin, address and assumption here is tabled, with its status
 * (CONFIRMED / ASSUMED) and source, in docs/boards/m5stack-tab5.md - keep the
 * two in step. */
#ifndef BOARD_TAB5_H
#define BOARD_TAB5_H
#include <stdbool.h>
#include "driver/i2c_master.h"
#include "esp_io_expander.h"

/* system I2C: expanders, touch, IMU, codec, mic ADC, power monitor, RTC */
#define TAB5_I2C_SDA        31
#define TAB5_I2C_SCL        32
/* PI4IOE5V6408 IO expanders */
#define TAB5_IOX1_ADDR      0x43
#define TAB5_IOX2_ADDR      0x44
#define TAB5_IOX1_SPK_EN    IO_EXPANDER_PIN_NUM_1   /* NS4150B amp */
#define TAB5_IOX1_LCD_RST   IO_EXPANDER_PIN_NUM_4
#define TAB5_IOX1_TP_RST    IO_EXPANDER_PIN_NUM_5
#define TAB5_IOX2_WLAN_PWR  IO_EXPANDER_PIN_NUM_0   /* ESP32-C6 power: kept off */
#define TAB5_IOX2_PWROFF    IO_EXPANDER_PIN_NUM_4   /* pulse = power off (power phase) */
#define TAB5_IOX2_CHG_EN    IO_EXPANDER_PIN_NUM_7
/* display: ST7123 over MIPI-DSI, native 720 x 1280 portrait */
#define TAB5_PANEL_W        720
#define TAB5_PANEL_H        1280
/* the glass's corners (docs/boards/m5stack-tab5.md: square, every pixel
 * visible - CONFIRMED on the bench 2026-09-18, phase 2's orientation frame).
 * The safe-area inset is r - r/sqrt(2) rounded up, in panel px; the tank is
 * drawn 2x, so in tank px it is half that, rounded up. 0 here: the whole
 * glass is safe. */
#define PANEL_CORNER_RADIUS 0
#define PANEL_SAFE_INSET    ((PANEL_CORNER_RADIUS * 29290 + 99999) / 100000)   /* ceil(r * (1 - 1/sqrt 2)) */
#define TANK_SAFE_INSET     ((PANEL_SAFE_INSET + 1) / 2)
#define TAB5_DSI_LANES      2
#define TAB5_DSI_LANE_MBPS  1040     /* M5Stack's M5GFX for the ST7123 (Espressif's BSP: 1000) */
#define TAB5_DPI_CLK_MHZ    80       /* what the ST7123 gets either way: 240 MHz PLL / 3 (asking 70 rounds to it) */
#define TAB5_DSI_PHY_LDO    3        /* on-chip LDO_VO3 feeds VDD_MIPI_DPHY ... */
#define TAB5_DSI_PHY_MV     2500     /* ... at 2.5 V */
#define TAB5_LCD_BL_GPIO    22       /* PWM to the backlight boost converter's EN */
#define TAB5_LCD_BL_HZ      44100    /* M5GFX's; the BSP's 5 kHz is a flicker suspect (phase 2 bench) */
#define TAB5_TP_INT_GPIO    23
/* audio: I2S to the ES8388 (the schematic and Espressif's BSP agree); the
 * NS4150B speaker amp's CTRL is TAB5_IOX1_SPK_EN */
#define TAB5_I2S_MCLK       30
#define TAB5_I2S_BCLK       27
#define TAB5_I2S_WS         29
#define TAB5_I2S_DOUT       26       /* P4 -> ES8388 DACDAT */
#define TAB5_I2S_DIN        28       /* ES7210 mic ADC -> P4 (unused) */
/* IMU (BMI270, docs/boards/m5stack-tab5.md "IMU mounting"): the accel axis
 * (0 X, 1 Y, 2 Z) and sign that read +1 g held upright - landscape, USB-C
 * left, SD slot at the bottom. -1 = NOT MEASURED: the screen never flips.
 * Set only from a bench trace, never from a photo or datasheet. Measured
 * 2026-09-19 (director `imu`): upright X +16426, upside down X -16170,
 * face up Z -16516, portrait USB-C down Y +16120 - X toward the top of the
 * view, Y toward its right, Z into the glass. */
#define TAB5_IMU_UP_AXIS    0
#define TAB5_IMU_UP_SIGN    (+1)
/* the accel axis out of the glass (lying face up it carries the 1 g); the
 * flip needs "up" to dominate the remaining in-screen axis. Measured with it. */
#define TAB5_IMU_FACE_AXIS  2
#if TAB5_IMU_UP_AXIS >= 0 && (TAB5_IMU_FACE_AXIS < 0 || TAB5_IMU_FACE_AXIS == TAB5_IMU_UP_AXIS)
#error "TAB5_IMU_FACE_AXIS: measure it with TAB5_IMU_UP_AXIS (a different axis)"
#endif
#define TAB5_BOOT_GPIO      35
/* the I2C addresses the phase-1 scan expects, and what each one is */
#define TAB5_ADDR_ES8388    0x10
#define TAB5_ADDR_RX8130    0x32
#define TAB5_ADDR_ES7210    0x40
#define TAB5_ADDR_INA226    0x41
#define TAB5_ADDR_ST7123_TP 0x55
#define TAB5_ADDR_BMI270    0x68

/* bring up the I2C bus and both expanders: amp off, Wi-Fi chip off, touch
 * out of reset (LCD left alone until the display phase); then scan the bus
 * and log what answered against what the board should have. False = no bus. */
bool board_init(void);
i2c_master_bus_handle_t board_i2c_bus(void);
esp_io_expander_handle_t board_iox1(void);   /* 0x43, NULL if absent */
esp_io_expander_handle_t board_iox2(void);
/* drive an expander pin: push-pull, output, then the level (board_tab5.c) */
bool board_iox_out(esp_io_expander_handle_t x, uint32_t pin, int level);
/* just before esp_deep_sleep_start: the next boot logs how long it was gone */
void board_note_deep_sleep(int seconds);
/* this boot is that sleep's wake (the P4 v1.3 reports it as a watchdog reset) */
bool board_woke_from_deep_sleep(void);   /* 0x44, NULL if absent */
#endif
