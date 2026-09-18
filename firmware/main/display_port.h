/* display_port.h — the ONLY platform-specific seam for the renderer.
 * The tank renders into an RGB565 buffer (common/render.c); this port ships
 * it to the panel. QEMU/bring-up: stub. Track 4: CO5300 over QSPI, the
 * landscape 448x368 tank centered on the 480x480 panel. */
#ifndef DISPLAY_PORT_H
#define DISPLAY_PORT_H
#include <stdint.h>
#include <stdbool.h>

bool display_port_init(void);
/* push a full TANK_W x TANK_H RGB565 frame; may return before DMA completes */
void display_port_flush(const uint16_t *fb);
/* power the panel down for device sleep; display_port_wake (or a boot's
 * display_port_init) re-sequences it */
void display_port_sleep(void);
/* re-power and re-init the panel after display_port_sleep, without reboot */
void display_port_wake(void);
/* present the frame turned q quarter turns clockwise (rotate.h): 2 = upside
 * down; 1 and 3 only for a square tank - an odd turn is ignored otherwise */
void display_port_set_rotation(int q);
/* panel brightness 0..255 (DCS 0x51; the init sequence starts at 255). Kept
 * across display_port_wake, which re-inits the panel. */
void    display_port_set_brightness(uint8_t level);
uint8_t display_port_brightness(void);
#endif
