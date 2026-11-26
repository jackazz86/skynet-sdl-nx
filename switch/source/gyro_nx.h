#ifndef GYRO_NX_H
#define GYRO_NX_H
#include <stddef.h>
#include <switch.h>

typedef struct {
  int enabled, debug, drift;
  int mode;                          /* 0: added to the right stick, 1: relative mouse motion */
  float mouse_deg_per_px;            /* the engine's mouse look, docs/game-logic/player-movement.md: 0.176 */
  float yaw, pitch, roll;            /* multipliers, 1.0 = 1:1 */
  float yaw_full, pitch_full;        /* deg/s at a fully pushed right stick */
  float deadzone;                    /* deg/s */
  float smooth_ms;
  float stick_deadzone;              /* right stick dead zone applied by the port, 0..0.5 */
} gyro_cfg_t;
extern gyro_cfg_t gyro_cfg;

void gyro_nx_load_cfg(void);
void gyro_nx_save_cfg(void);
/* view-rotation rates in the engine's stick convention (turn: right positive, pitch: down positive), deg/s */
void gyro_nx_sample(u64 now, float *turn_dps, float *pitch_dps);
void gyro_nx_toggle(void);        /* cycle: stick -> mouse -> off -> stick */
void gyro_nx_stats(char *buf, size_t n);
#endif
