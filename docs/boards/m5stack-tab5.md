# M5Stack Tab5 (ST7123 version)

The board this branch (`M5Stack-Tab5`) targets: the M5Stack Tab5 made from
14 October 2025, with the ST7123 integrated display-touch driver. The
earlier ILI9881C + GT911 version and the later ST7121 version are not
supported (they cannot be tested here).

**CONFIRMED** = told by the owner or seen on the bench. **ASSUMED** = taken
from the sources below and not yet seen on the bench. Code that depends on
a row cites this file.

Sources: [M5Stack Tab5 docs](https://docs.m5stack.com/en/core/Tab5),
[Tab5 schematic](https://m5stack-doc.oss-cn-shenzhen.aliyuncs.com/1132/Tab5_Schematics_PDF.pdf),
Espressif's board-support package
[`espressif/m5stack_tab5` 1.3.1](https://components.espressif.com/components/espressif/m5stack_tab5).
Where the docs page and the board-support package disagreed (the IO
expanders' pins), the schematic sided with the board-support package.

## Physical

| Fact | Value | Status |
| --- | --- | --- |
| Board version | Tab5, October 2025, ST7123 display + touch | CONFIRMED |
| Held | Landscape, USB-C port on the LEFT, microSD slot along the BOTTOM | CONFIRMED |
| Panel | 5" IPS TFT, 720 x 1280 native (portrait), shown as 1280 x 720 landscape | CONFIRMED (phase 2: the 1-px border on all four edges) |
| Panel corners | Square: corner radius 0 px, every pixel visible. `PANEL_CORNER_RADIUS 0` in `board_tab5.h`; safe-area inset 0 | CONFIRMED (owner; phase 2 border) |
| Panel orientation | Held as above, native (0,0) is the TOP-RIGHT corner. Native corners seen: (0,0) top right, (719,0) bottom right, (0,1279) top left, (719,1279) bottom left. So the landscape view (u across 0..1279, v down 0..719) is native (x = v, y = 1279 - u): a quarter turn, no mirror | CONFIRMED (phase 2 orientation frame) |
| Tank frame | 640 x 360, drawn 2x: the PPA scales it and turns it 90 degrees counter-clockwise (270 when flipped) into the panel's back frame buffer, swapped in at a frame boundary: 33 fps, no tearing. Flipped (upside down): the 270-degree turn, touch mirrored with it | CONFIRMED (phase 3 bench; flipped, phase 4) |
| Panel frame timing | The ST7123 flashes white for an instant when a frame arrives late (a stretched frame = an overlong porch; M5GFX warns the porches must not move). Every flash was one 20-28 ms frame, and they came from saves: a flash write turns the cache off, and the DSI/DMA interrupt that restarts each frame waited it out. Fixed with `CONFIG_LCD_DSI_ISR_CACHE_SAFE=y` (`sdkconfig.defaults`): 14 saves, no flash. A late frame still logs `frame timing: ...` | CONFIRMED (phase 3 bench, 2026-09-19) |
| IMU mounting | BMI270 at 0x68, accel +-2 g. Held upright (landscape, USB-C left, SD slot at the bottom) +X reads +1 g; upside down -X; face up Z reads -1 g; portrait with USB-C down +Y. So X points to the top of the view, Y to its right, Z into the glass (right-handed). `board_tab5.h`: `TAB5_IMU_UP_AXIS 0`, `TAB5_IMU_UP_SIGN +1`, `TAB5_IMU_FACE_AXIS 2`. At rest the motion reading is 11-93 counts (threshold 220); picked up, 3,000-54,000 | CONFIRMED (phase 4 bench trace, 2026-09-19) |

## Chip and memory

| Fact | Value | Status |
| --- | --- | --- |
| SoC | ESP32-P4NRW32, 360 MHz, 2 cores | CONFIRMED (phase 1 boot log) |
| Chip revision | v1.3 (pre-v3 silicon): the build sets `CONFIG_ESP32P4_SELECTS_REV_LESS_V3`, minimum v1.0 | CONFIRMED (phase 1 boot log) |
| Flash / PSRAM | 16 MB QIO / 32 MB AP hex-mode PSRAM at 200 MHz | CONFIRMED (phase 1 boot log) |
| Serial console | USB-C to the P4's USB Serial/JTAG (GPIO 24/25 PHY), input and output; the CH9102 USB-UART is not fitted | CONFIRMED (phase 1: logs + director `help`) |

## Buses and pins

The PI4IOE5V6408 expanders power up with every output HIGH-IMPEDANCE
(register 0x07 = 0xFF) and a pull-down on every pin; the driver's `set_dir`
leaves that register alone, so an output set high reads low until it is
made push-pull (`board_tab5.c` `iox_out`). Found when the speaker amp's
enable never rose (phase 6 bench, 2026-09-19).
Reading an output back does not work either: the driver's `get_level`
reads the input-status register, which read LOW on the amp enable while
the speaker played.

| Function | Pin / address | Status |
| --- | --- | --- |
| System I2C | SDA GPIO 31, SCL GPIO 32, 2.2 kOhm pull-ups; answering: 0x10, 0x32, 0x40, 0x41, 0x43, 0x44, 0x68 | CONFIRMED (phase 1 scan) |
| IO expander 1 | PI4IOE5V6408 at 0x43: P0 antenna select, P1 speaker amp enable, P4 LCD reset, P5 touch reset, P6 camera reset, P7 headphone detect | ASSUMED (schematic U6, BSP) |
| IO expander 2 | PI4IOE5V6408 at 0x44: P0 Wi-Fi (ESP32-C6) power, P3 USB-A 5 V, P4 power-off pulse, P5 fast-charge disable, P6 charge status, P7 charge enable | ASSUMED (schematic U7, BSP) |
| Display | MIPI-DSI, 2 lanes; RGB565; HBP 40, HPW 2, HFP 40, VBP 8, VPW 2, VFP 220 (M5GFX: the vertical porches must stay so, or the image shifts and touch stops); DSI PHY powered by on-chip LDO 3 at 2.5 V. Brought up and showing images (phase 2). Now on M5Stack's M5GFX settings: 1040 Mbps/lane, 80 MHz pixel clock (240 MHz / 3; the BSP's 70 MHz request rounds to the same 80), about 66 Hz refresh, and M5GFX's init table - Espressif's lacks nine panel-tuning commands, and with it the panel flickered | CONFIRMED (phase 2: no flicker on M5GFX's settings) |
| Display colour order | RGB, no byte swap | CONFIRMED (phase 2: red, green, blue, white, black in order) |
| LCD reset | Expander 0x43 P4: drive low to reset, release as input pull-up | CONFIRMED (phase 2: the panel and the touch come up after the release) |
| Backlight | GPIO 22 PWM to the ME2212 boost converter's EN; 44.1 kHz, 8-bit (M5GFX's; the BSP's 5 kHz is a flicker suspect); director `disp bl <hz>\|steady` switches it live | CONFIRMED (phase 2: levels visible, no flicker at 44.1 kHz) |
| Touch | ST7123 at I2C 0x55, INT GPIO 23, reset expander 0x43 P5; panel-native coordinates; firmware version 3 = ST7123 (1 = ST7121). Answers only once LCD reset is released too (phase 1 had touch out of reset but LCD reset held: not found); phase 2 read firmware 3 = ST7123. Read after an INT edge (GPIO 23, falling) and while a finger is down - an idle tank is never read. Mapped with the panel: view u = 1279 - y, v = x, tank = view / 2; corners read (52, 46), (592, 46), (52, 319), (600, 315) about 9 mm in | CONFIRMED (phase 3 bench) |
| IMU | BMI270 at 0x68 (SDO grounded); INT1 to the wake circuit, not a GPIO | ASSUMED (schematic p2, docs) |
| Audio | ES8388 codec 0x10, ES7210 mic ADC 0x40 (unused); I2S MCLK 30, BCLK 27, LRCK 29, DOUT 26, DIN 28; NS4150B speaker amp CTRL on expander 0x43 P1, high = on. The ES8388 as Espressif's BSP opens it (slave, 16-bit, MCLK 256 fs, LOUT1/ROUT1 0 dB, LOUT2/ROUT2 -30 dB), DAC only; the mono stream in both I2S slots; down whenever idle. Headphone jack (detect on expander 0x43 P7) not handled | CONFIRMED (phase 6 bench, 2026-09-19: cues clean at a comfortable level, volume steps, no pops, no hiss at rest) |
| Battery | 2S NP-F550 (7.4 V); INA226 monitor at 0x41; IP2326 charger, charges only while the firmware enables it | ASSUMED (schematic p5, docs) |
| Power button | S1 to a custom-programmed PMS150G, which holds the power on; power-off = pulse on expander 0x44 P4; the PMS150G also drives GPIO 35 (behaviour undocumented) | ASSUMED (schematic p5) |
| BOOT button | GPIO 35 (strapping pin, shared with the PMS150G); not a deep-sleep wake pin on the P4 | ASSUMED (schematic p1, p5) |
| RTC | RX8130CE at 0x32 | ASSUMED (schematic p5) |
| Unused | ESP32-C6 Wi-Fi (kept off), camera, microSD, RS-485, USB-A | — |

## Port phases

1. Boot + serial: P4 build, I2C bus and expanders up, bus scan logged, the
   tank running headless. PASSED 2026-09-18: boots, 10+ minutes stable,
   director console both ways, 7 of 8 chips answering (not the touch, see
   above). The model runs on the plain-C path: 16.8 s per decision, 2.6
   tok/s - about 4.5x the S3's time; a P4 speed-up is a later phase.
2. Display init + solid colour fill. First bench (2026-09-18): the panel
   came up (ST7123 identified), colours, patterns and backlight steps all
   visible, stable - but the screen flickered badly on Espressif's settings.
   Second build: M5Stack's M5GFX init table, link rate and backlight PWM.
   PASSED 2026-09-18: no flicker, colours in order, every edge pixel visible,
   backlight steps visible, orientation read (the table above). The pattern
   is now director `disp test`.
3. Layout + safe area (+ touch). The tank world is 640 x 360 (the 1.8's
   448 x 368 before): drawn 2x it fills the 1280 x 720 glass exactly. The
   1.8's 448-wide pages sit centred (`UI_X0` = 96, `common/tank.h`); saves
   from the 448 x 368 world load with their spots moved with the glass
   (`common/progression.c`, sim `--selftest-shop`). Frames go up through
   the PPA into two panel frame buffers, swapped at a frame boundary.
   PASSED 2026-09-19: upright, full glass, crisp 2x, touch mapped at all
   four corners, pages centred, 33 fps, no tearing, no panics. Found on the
   way: the scene prefetch ran on the P4's AHB DMA at 6.6 MB/s from PSRAM (a
   68 ms wait, 10 fps) - now the AXI DMA; and the white flashes (the table
   above). The sand looks cut short at the bottom left by design: the reef
   rock (x ~58-134) and the porthole vignette, which darkens every corner.
4. IMU orientation: the BMI270 read (Espressif's `espressif/bmi270`,
   which uploads Bosch's config file); its axes measured on the bench, then
   the 180-degree flip. PASSED 2026-09-19: the axes traced (the table
   above); turned over the tank flips in about a second and back again,
   touch lands on the same corners either way up, flat or in portrait it
   holds, 33 fps flipped, no flashes, no panics.
5. Docs + commit. 2026-09-19.
6. Audio (ES8388 + NS4150B speaker amp). First bench: silent - the amp's
   enable never rose (the expander's high-impedance default, above). PASSED
   2026-09-19 on the second: cues clean, quiet / off / normal and the
   settings page's volume, the tank's own cues, no pops, no hiss.
7. Battery, charging and power (INA226, IP2326, expander 2, the power
   button's PMS150G), and sleep.
8. RTC (RX8130CE).
9. The model on the P4 (its SIMD path is S3-only: 16.8 s per decision).

## Build and flash

`firmware/sdkconfig.defaults` targets this board (ESP32-P4, pre-v3 silicon,
the Tab5 display, touch and IMU ports). From `firmware/`, with ESP-IDF 5.5:

    idf.py set-target esp32p4
    idf.py build
    idf.py -p <port> flash monitor

A build folder made before a `sdkconfig.defaults` change keeps its old
`sdkconfig`: delete it (or `idf.py fullclean`) so the defaults apply.
Director (serial console) extras on this board: `disp` (panel and frame
stats), `disp test` (the phase-2 pattern), `disp bl <hz>|steady|pwm`,
`disp hold on|off`, `touch log on|off`, `touch poll int|always|off`,
`llm on|off`, `imu [n]` (raw accel trace, 16384 = 1 g).
