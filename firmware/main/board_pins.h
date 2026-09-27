/* board_pins.h — Waveshare ESP32-S3-Touch-AMOLED-2.16 (CO5300 480x480 AMOLED
 * over QSPI + CST9220 touch, a CST9217-family part). Source: Waveshare's
 * esp-idf BSP (waveshare/esp32_s3_touch_amoled_2_16 2.0.1) and the pin_config.h
 * of their Arduino examples. No IO expander on this board: the panel and the
 * touch chip have their own reset GPIOs. */
#ifndef BOARD_PINS_H
#define BOARD_PINS_H
#define PIN_LCD_CS        12
#define PIN_LCD_PCLK      38
#define PIN_LCD_DATA0     4
#define PIN_LCD_DATA1     5
#define PIN_LCD_DATA2     6
#define PIN_LCD_DATA3     7
#define PIN_LCD_RST       39
#define PIN_I2C_SDA       15
#define PIN_I2C_SCL       14
#define PIN_TP_INT        11
#define PIN_TP_RST        40
#define PIN_I2S_MCLK      42       /* the ES8311's MCLK (the 1.8 board: 16 - on this board GPIO16 is SYS_OUT) */
/* the AXP2101 (2026-09-26): the rails the firmware may switch off at boot,
 * and the codec's supply the audio port switches with the sound. The 1.8's
 * lists come from ITS schematic; this board's rail wiring has not been
 * checked, and Waveshare's own firmware (esp32_s3_touch_amoled_2_16 2.0.1)
 * never touches the PMIC - every rail stays at its power-on default. With
 * the 1.8's list a warm reset now and then left the I2C bus timing out
 * from the moment the rails were trimmed (bench, 2026-09-25). So: nothing
 * trimmed, no rail switched for the codec, until the schematic says which
 * rails are really unused (battery_port_axp2101.c, audio_port_es8311.c). */
#define BOARD_PMIC_TRIM   NULL     /* none */
#define BOARD_CODEC_RAIL  NULL     /* none: the codec stays powered */
#define I2C_ADDR_CST9217  0x5A
#define PANEL_W           480      /* square */
#define PANEL_H           480
/* the 448x368 landscape tank sits centered on the square panel; the border
 * stays AMOLED black. Even offsets: the CO5300 wants even window starts. */
#define PANEL_TANK_X0     16
#define PANEL_TANK_Y0     56
#endif
