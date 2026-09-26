/* director.h — serial "director" console: scenario setup on cue (filming,
 * bench). Lines typed on the USB serial port (the same port the log comes
 * out of) become tank commands: `hungry 3`, `feed`, `algae 40`, ... Type
 * `help` for the list. The tank owns nothing new: the console only sets
 * state the tank already has and the advisor still decides what fish do. */
#pragma once
#include "tank.h"
void director_init(void);
void director_poll(tank_t *t);   /* once per frame, from the tank task */
void device_sleep(int wake_after_s);   /* main.c: the keeper's sleep (0: grace then power-off) or, N > 0, a 5 s grace then deep sleep with an N s timer wake */
void device_fake_battery(int pct, int state);   /* main.c: the gauge reads pct% with the cable in state BAT_* (battery.h) for the pill, the battery page and the low-battery rule (b-roll); pct < 0 = the real gauge again. Not saved */
void device_battery_log(void);          /* main.c: the battery page's numbers and the learned rates, to the log */
void device_poweroff(void);            /* main.c: save + PMIC cut now */
