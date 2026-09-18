/* rotate.h — the frame's orientation on the panel (2026-09-18): the IMU
 * picks it, the display port turns the frame, the touch port turns the
 * finger back. q is the number of quarter turns the IMAGE is turned
 * CLOCKWISE on the panel: 0 upright, 1 = 90, 2 = 180, 3 = 270. A square tank
 * turns in quarter turns - it fits the panel whichever edge is down; a
 * rectangular one (the 1.8's 448x368, the bordered 2.16 build) only in half
 * turns, so it never loses its aspect. The panel is TANK_W x TANK_H here: the
 * display port's own offsets (a border) are applied outside these.
 * Header-only and platform-free, so the sim's selftest checks the same math
 * the device runs. */
#ifndef POCKET_TANK_ROTATE_H
#define POCKET_TANK_ROTATE_H
#include <stdint.h>
#include "tank.h"

#define ROTATE_QUARTER_OK (TANK_W == TANK_H)       /* quarter turns: a square tank only */
static inline bool rotate_allowed(int q) { return ROTATE_QUARTER_OK || !(q & 1); }

/* the tank pixel shown at panel pixel (px, py). Turning the image clockwise
 * by one quarter puts its top row down the panel's right edge. */
static inline void rotate_panel_to_tank(int q, int px, int py, int *tx, int *ty) {
    switch (q & 3) {
    case 1:  *tx = py;              *ty = TANK_W - 1 - px; break;
    case 2:  *tx = TANK_W - 1 - px; *ty = TANK_H - 1 - py; break;
    case 3:  *tx = TANK_H - 1 - py; *ty = px;              break;
    default: *tx = px;              *ty = py;              break;
    }
}
/* the same for a finger (fractional panel coordinates) */
static inline void rotate_panel_to_tank_f(int q, float px, float py, float *tx, float *ty) {
    switch (q & 3) {
    case 1:  *tx = py;              *ty = TANK_W - 1 - px; break;
    case 2:  *tx = TANK_W - 1 - px; *ty = TANK_H - 1 - py; break;
    case 3:  *tx = TANK_H - 1 - py; *ty = px;              break;
    default: *tx = px;              *ty = py;              break;
    }
}

/* panel rows [y0, y0 + rows) of the turned frame into dst (TANK_W per row),
 * byte-swapped for the panel (big-endian RGB565 over SPI). The quarter turns
 * read the frame a row segment at a time - `rows` contiguous pixels, one
 * cache line from PSRAM at 32 - and scatter down the stripe's columns, the
 * way the 1.8's port turned every frame. */
static inline void rotate_stripe_be(const uint16_t *fb, uint16_t *dst, int y0, int rows, int q) {
    switch (q & 3) {
    case 0:
        for (int r = 0; r < rows; r++) {
            const uint16_t *s = fb + (y0 + r) * TANK_W; uint16_t *d = dst + r * TANK_W;
            for (int x = 0; x < TANK_W; x++) d[x] = __builtin_bswap16(s[x]);
        }
        break;
    case 2:
        for (int r = 0; r < rows; r++) {
            const uint16_t *s = fb + (TANK_H - 1 - y0 - r) * TANK_W + TANK_W - 1; uint16_t *d = dst + r * TANK_W;
            for (int x = 0; x < TANK_W; x++) d[x] = __builtin_bswap16(s[-x]);
        }
        break;
#if ROTATE_QUARTER_OK
    case 1:                                    /* panel (px, py) <- frame (py, N-1-px) */
        for (int px = 0; px < TANK_W; px++) {
            const uint16_t *s = fb + (TANK_W - 1 - px) * TANK_W + y0;
            for (int r = 0; r < rows; r++) dst[r * TANK_W + px] = __builtin_bswap16(s[r]);
        }
        break;
    case 3:                                    /* panel (px, py) <- frame (N-1-py, px) */
        for (int px = 0; px < TANK_W; px++) {
            const uint16_t *s = fb + px * TANK_W + (TANK_H - 1 - y0);
            for (int r = 0; r < rows; r++) dst[r * TANK_W + px] = __builtin_bswap16(s[-r]);
        }
        break;
#endif
    default: break;
    }
}
#endif
