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
void device_fake_battery(int pct);      /* main.c: the gauge reads pct% on battery for the pill and the low-battery rule (b-roll); < 0 = the real gauge again. Not saved */
bool main_set_llm(bool on);           /* main.c: the model advisor (true) or the rules; returns whether the model now decides. Not saved */
void device_poweroff(void);            /* main.c: save + PMIC cut now */
