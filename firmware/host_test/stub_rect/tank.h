/* imu_host.c's rectangular run: the tank of the 1.8 and the bordered 2.16
 * build (rotate.h only reads the size). Force-included (-include), with the
 * real tank.h's guard, so rotate.h's own #include "tank.h" is skipped. */
#ifndef POCKET_TANK_TANK_H
#define POCKET_TANK_TANK_H
#include <stdbool.h>
#define TANK_W 448
#define TANK_H 368
#endif
