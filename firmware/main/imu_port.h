/* imu_port.h — QMI8658 accelerometer -> screen orientation: quarter turns
 * for a square tank, half turns only for a rectangular one, so it keeps its
 * aspect ratio whichever way up (rotate.h). */
#ifndef IMU_PORT_H
#define IMU_PORT_H
#include <stdbool.h>
#include <stdint.h>
#include "driver/i2c_master.h"

bool imu_port_init(i2c_master_bus_handle_t bus);  /* false = no IMU, never turned */
void imu_port_poll(int64_t now_us);               /* call every frame; rate-limited inside */
int  imu_port_rotation(void);                     /* quarter turns clockwise the frame should take: 0..3 */
/* handling detector (2026-09-15, for the audio port): true while the
 * device has moved within the last IMU_MOTION_HOLD_US - picked up, in a
 * hand, carried. Lying on a table it goes false. */
bool imu_port_moving(void);
/* handling for the tank light (2026-09-15): the same, but the motion must
 * show on two consecutive polls (500 ms) - a pick-up does, a knock on the
 * desk or a mug set down beside it is one spike. */
bool imu_port_handled(void);
int  imu_port_motion(void);                       /* last poll's movement, counts (director / tuning) */
void imu_port_last(int16_t out[3], int *motion);  /* the last poll's raw sample + its movement (director `imu`) */
/* drowse bracket: quiesce the accel before the panel/touch rails cut (a
 * powered chip beside rail transitions is the latch-up recipe that railed
 * X/Z on 2026-08-31 - a full power-off revived them), then soft-reset +
 * reconfigure on wake so it never resumes on trust. */
void imu_port_sleep(void);
void imu_port_wake(void);

#endif
