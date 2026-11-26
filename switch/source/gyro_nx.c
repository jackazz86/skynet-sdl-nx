/* gyro_nx.c -- Switch motion sensors as a view-rotation rate, the way the Godot port
 * (skynet_nx/source/gyro.c) and the iortcw / Serious Sam ports settled it on hardware:
 *  - read the whole 17-sample FIFO and average only the samples not seen yet;
 *  - handheld, Pro Controller and Joy-Con sensors are different handles; an explicitly
 *    paired controller wins (FullKey, then Handheld, then JoyDual);
 *  - axes: sensor X = nod (look up/down), Y = wrists in/out (turn), Z = steering wheel
 *    (turn, summed with Y); angular_velocity is in rotations per second.
 * The result is a rotation RATE, 1:1 with the hands: the caller turns it into right-stick
 * deflection using the engine's own full-stick rates (docs/game-logic/player-movement.md:
 * keyboard/pad turn 315 deg/s, look up/down 180 deg/s). Config: /switch/skynet_sdl_nx/gyro.cfg. */
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <switch.h>
#include "util.h"
#include "gyro_nx.h"

#define FIFO 17
#define CFG_PATH "/switch/skynet_sdl_nx/gyro.cfg"
#define CFG_MARK "# gyro v2"

gyro_cfg_t gyro_cfg = {
  .enabled = 1, .debug = 0, .drift = 0,
  .yaw = 1.0f, .pitch = 1.0f, .roll = 1.0f,
  .yaw_full = 315.0f, .pitch_full = 180.0f,
  .deadzone = 0.0f, .smooth_ms = 10.0f, .stick_deadzone = 0.15f,
};

static HidSixAxisSensorHandle s_h[4];   /* 0/1 joy-con pair (left/right), 2 pro, 3 handheld */
static int s_ready, s_cur = -1, s_cfg_loaded;
static u64 s_last[4];
static float s_ema_turn, s_ema_pitch;    /* rad/s */
static u64 s_last_t, s_last_fresh;
static unsigned s_fresh_total;
static float s_peak[3];

void gyro_nx_save_cfg(void) {
  FILE *f = fopen(CFG_PATH, "w");
  if (!f) return;
  fprintf(f, "%s\n"
    "# 1:1 gyro look. yaw/pitch/roll are multipliers (1.0 = 100%% = the view turns as much as your hands).\n"
    "# Sensor axes: Y = turn, X = look up/down, Z = steering wheel (turn). Negative flips a direction.\n"
    "# yaw_full / pitch_full: the game's turn and look rates at a fully pushed right stick (deg/s).\n"
    "# deadzone: deg/s below which the sensor is ignored; smooth_ms: averaging time of the readings.\n"
    "# stick_deadzone: the right stick's dead zone, now applied by the port (the game's own is set to 0).\n"
    "enabled %d\nyaw %.2f\npitch %.2f\nroll %.2f\nyaw_full %.0f\npitch_full %.0f\ndeadzone %.2f\nsmooth_ms %.0f\ndrift %d\ndebug %d\nstick_deadzone %.2f\n",
    CFG_MARK, gyro_cfg.enabled, gyro_cfg.yaw, gyro_cfg.pitch, gyro_cfg.roll, gyro_cfg.yaw_full, gyro_cfg.pitch_full,
    gyro_cfg.deadzone, gyro_cfg.smooth_ms, gyro_cfg.drift, gyro_cfg.debug, gyro_cfg.stick_deadzone);
  fclose(f);
}

void gyro_nx_load_cfg(void) {
  if (s_cfg_loaded) return;
  s_cfg_loaded = 1;
  FILE *f = fopen(CFG_PATH, "r");
  char line[160];
  if (!f || !fgets(line, sizeof line, f) || strncmp(line, CFG_MARK, strlen(CFG_MARK)) != 0) {
    if (f) fclose(f);
    gyro_nx_save_cfg();       /* missing or an older layout: write the tuned defaults */
    debugPrintf("[gyro] wrote default %s\n", CFG_PATH);
    return;
  }
  char key[40]; float v;
  while (fgets(line, sizeof line, f)) {
    if (line[0] == '#' || sscanf(line, "%39s %f", key, &v) != 2) continue;
    if (!strcmp(key, "enabled")) gyro_cfg.enabled = (int)v;
    else if (!strcmp(key, "debug")) gyro_cfg.debug = (int)v;
    else if (!strcmp(key, "drift")) gyro_cfg.drift = (int)v;
    else if (!strcmp(key, "yaw")) gyro_cfg.yaw = v;
    else if (!strcmp(key, "pitch")) gyro_cfg.pitch = v;
    else if (!strcmp(key, "roll")) gyro_cfg.roll = v;
    else if (!strcmp(key, "yaw_full")) gyro_cfg.yaw_full = v;
    else if (!strcmp(key, "pitch_full")) gyro_cfg.pitch_full = v;
    else if (!strcmp(key, "deadzone")) gyro_cfg.deadzone = v;
    else if (!strcmp(key, "smooth_ms")) gyro_cfg.smooth_ms = v;
    else if (!strcmp(key, "stick_deadzone")) gyro_cfg.stick_deadzone = v;
  }
  fclose(f);
  debugPrintf("[gyro] cfg: enabled=%d yaw=%.2f pitch=%.2f roll=%.2f yaw_full=%.0f pitch_full=%.0f deadzone=%.2f smooth=%.0fms drift=%d stick_dz=%.2f\n",
              gyro_cfg.enabled, gyro_cfg.yaw, gyro_cfg.pitch, gyro_cfg.roll, gyro_cfg.yaw_full, gyro_cfg.pitch_full,
              gyro_cfg.deadzone, gyro_cfg.smooth_ms, gyro_cfg.drift, gyro_cfg.stick_deadzone);
}

static void init_sensors(void) {
  if (s_ready) return;
  hidGetSixAxisSensorHandles(&s_h[0], 2, HidNpadIdType_No1, HidNpadStyleTag_NpadJoyDual);
  hidGetSixAxisSensorHandles(&s_h[2], 1, HidNpadIdType_No1, HidNpadStyleTag_NpadFullKey);
  hidGetSixAxisSensorHandles(&s_h[3], 1, HidNpadIdType_Handheld, HidNpadStyleTag_NpadHandheld);
  for (int i = 0; i < 4; i++) hidStartSixAxisSensor(s_h[i]);
  int want = gyro_cfg.drift < 0 ? 0 : gyro_cfg.drift > 2 ? 2 : gyro_cfg.drift;
  for (int i = 0; i < 4; i++) hidSetGyroscopeZeroDriftMode(s_h[i], (HidGyroscopeZeroDriftMode)want);
  s_ready = 1;
  debugPrintf("[gyro] motion sensors started (zero-drift mode %d)\n", want);
}

/* mean of the fresh samples (rotations per second); 0 when none */
static int read_fresh(int idx, float avg[3]) {
  HidSixAxisSensorState st[FIFO];
  size_t n = hidGetSixAxisSensorStates(s_h[idx], st, FIFO);
  double sum[3] = { 0, 0, 0 };
  int fresh = 0;
  u64 newest = s_last[idx];
  if (n > 0 && s_last[idx] == 0) {          /* first read of this source: skip its backlog */
    for (size_t i = 0; i < n; i++) if (st[i].sampling_number > newest) newest = st[i].sampling_number;
    s_last[idx] = newest;
    return 0;
  }
  for (size_t i = 0; i < n; i++) {
    if (st[i].sampling_number <= s_last[idx]) continue;
    sum[0] += st[i].angular_velocity.x; sum[1] += st[i].angular_velocity.y; sum[2] += st[i].angular_velocity.z;
    fresh++;
    if (st[i].sampling_number > newest) newest = st[i].sampling_number;
  }
  s_last[idx] = newest;
  if (!fresh) return 0;
  for (int k = 0; k < 3; k++) avg[k] = (float)(sum[k] / fresh);
  return fresh;
}

static float dead(float v, float dz) { return fabsf(v) < dz ? 0.0f : v; }

void gyro_nx_sample(u64 now, float *turn_dps, float *pitch_dps) {
  *turn_dps = *pitch_dps = 0.0f;
  gyro_nx_load_cfg();
  if (!gyro_cfg.enabled) return;
  init_sensors();

  const u64 stylesPad = hidGetNpadStyleSet(HidNpadIdType_No1);
  const u64 stylesHandheld = hidGetNpadStyleSet(HidNpadIdType_Handheld);
  int idx = -1; const char *which = "none";
  if (stylesPad & HidNpadStyleTag_NpadFullKey) { idx = 2; which = "Pro Controller"; }
  else if (stylesHandheld & HidNpadStyleTag_NpadHandheld) { idx = 3; which = "handheld"; }
  else if (stylesPad & HidNpadStyleTag_NpadJoyDual) { idx = 1; which = "Joy-Con pair (right)"; }
  if (idx < 0) return;

  float a[3] = { 0, 0, 0 };
  int fresh = read_fresh(idx, a);
  if (!fresh && idx == 1) { idx = 0; which = "Joy-Con pair (left)"; fresh = read_fresh(idx, a); }
  if (idx != s_cur) { debugPrintf("[gyro] using %s (styles %#x/%#x)\n", which, (unsigned)stylesPad, (unsigned)stylesHandheld); s_cur = idx; }

  const float freq = (float)armGetSystemTickFreq();
  if (fresh) {
    const float dz = gyro_cfg.deadzone / 360.0f;          /* deg/s -> rev/s */
    const float ax = dead(a[0], dz), ay = dead(a[1], dz), az = dead(a[2], dz);
    for (int k = 0; k < 3; k++) { float m = fabsf(a[k]) * 360.0f; if (m > s_peak[k]) s_peak[k] = m; }
    s_fresh_total += (unsigned)fresh;
    const float pitch_up = ax * 6.28318530718f * gyro_cfg.pitch;                    /* rad/s */
    const float turn_left = (ay * gyro_cfg.yaw + az * gyro_cfg.roll) * 6.28318530718f;
    float dt = s_last_t ? (float)(now - s_last_t) / freq : 0.005f;
    float alpha = gyro_cfg.smooth_ms > 0.5f ? 1.0f - expf(-dt * 1000.0f / gyro_cfg.smooth_ms) : 1.0f;
    s_ema_turn += alpha * (turn_left - s_ema_turn);
    s_ema_pitch += alpha * (pitch_up - s_ema_pitch);
    s_last_t = now; s_last_fresh = now;
    if (gyro_cfg.debug) {
      static u64 last_print;
      if (now - last_print > freq / 6) {
        last_print = now;
        debugPrintf("[gyro] raw rev/s X=%+6.3f Y=%+6.3f Z=%+6.3f (%d samples) -> turn %+6.1f pitch %+6.1f deg/s\n",
                    a[0], a[1], a[2], fresh, s_ema_turn * 57.29578f, s_ema_pitch * 57.29578f);
      }
    }
  } else if (now - s_last_fresh > (u64)(freq * 0.04f)) {
    s_ema_turn = s_ema_pitch = 0.0f;                      /* sensor silent: do not hold a stale rate */
  }
  /* the engine's convention: right stick X positive = turn right, Y positive = look down */
  *turn_dps = -s_ema_turn * 57.29578f;
  *pitch_dps = -s_ema_pitch * 57.29578f;
}

void gyro_nx_toggle(void) {
  gyro_cfg.enabled = !gyro_cfg.enabled;
  s_ema_turn = s_ema_pitch = 0.0f;
  debugPrintf("[gyro] %s\n", gyro_cfg.enabled ? "on" : "off");
}

void gyro_nx_stats(char *buf, size_t n) {
  snprintf(buf, n, "%u fresh samples, peak |rate| X=%.0f Y=%.0f Z=%.0f deg/s", s_fresh_total, s_peak[0], s_peak[1], s_peak[2]);
  s_fresh_total = 0; s_peak[0] = s_peak[1] = s_peak[2] = 0;
}
