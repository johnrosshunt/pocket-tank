/* persist_port_sim.c — sim implementation of the progression ports:
 * ~/.cache/pocket-tank/tank480.sav + wall clock. The square tank's save has
 * its own file (2026-09-17): main's 448 x 368 sim keeps tank.sav, and it
 * takes a longer save for a newer build's and starts a fresh tank over it.
 * With no tank480.sav yet, the load reads tank.sav once (progression.c moves
 * a 448 x 368 tank into the square one) and never writes it. */
#include "progression.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <errno.h>

/* POCKET_TANK_SAVE overrides the save path (selftests use a scratch file) */
static const char *path(void) {
    static char p[512];
    if (getenv("POCKET_TANK_SAVE")) return getenv("POCKET_TANK_SAVE");
    snprintf(p, sizeof p, "%s/.cache/pocket-tank/tank480.sav", getenv("HOME") ? getenv("HOME") : ".");
    return p;
}
static const char *legacy_path(void) {
    static char p[512];
    snprintf(p, sizeof p, "%s/.cache/pocket-tank/tank.sav", getenv("HOME") ? getenv("HOME") : ".");
    return p;
}
bool persist_port_load(void *buf, size_t max, size_t *got) {
    FILE *f = fopen(path(), "rb");
    if (!f && errno == ENOENT && !getenv("POCKET_TANK_SAVE")) f = fopen(legacy_path(), "rb");   /* main's tank, read once */
    if (!f) return false;
    size_t n = fread(buf, 1, max, f); bool longer = fgetc(f) != EOF; fclose(f);
    if (longer || n == 0) return false;          /* a newer build's save, or empty */
    *got = n; return true;
}
bool persist_port_save(const void *buf, size_t len) {
    char dir[512]; snprintf(dir, sizeof dir, "%s/.cache/pocket-tank", getenv("HOME") ? getenv("HOME") : ".");
    char cmd[600]; snprintf(cmd, sizeof cmd, "mkdir -p '%s'", dir); (void)system(cmd);
    FILE *f = fopen(path(), "wb"); if (!f) return false;
    size_t n = fwrite(buf, 1, len, f); fclose(f); return n == len;
}
bool persist_port_erase(void) { return remove(path()) == 0 || errno == ENOENT; }
#ifndef PT_VERSION
#define PT_VERSION "sim"
#endif
const char *version_port_string(void) { return PT_VERSION; }   /* the Makefile's git describe */
int64_t clock_port_now_unix(void) { return (int64_t)time(NULL); }
