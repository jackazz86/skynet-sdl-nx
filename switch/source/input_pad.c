/* input_pad.c -- controls for the game on a native SDL3 build that has NO joystick
 * support (SDL_InitSubSystem(JOYSTICK) and SDL_AttachVirtualJoystick fail: "SDL not
 * built with joystick support"). Like the Android-wrapper ports do for their engines,
 * input comes straight from libnx's pad API; the engine here is closed, so the seam is
 * the dlsym table: the SDL entry points the game resolves are OURS (tools/sdl_override.txt
 * maps SDL_X -> skynet_SDL_X) and SDL_PollEvent is wrapped to inject events first.
 *
 *  - Gamepad: SDL_GetGamepads/OpenGamepad/GetGamepadButton/Axis/... + GAMEPAD_* events.
 *    Face buttons follow Nintendo LABELS (Switch A confirms = SDL SOUTH, B = EAST,
 *    Y = WEST, X = NORTH): the engine takes SOUTH as "A" (accept) and EAST as "B" (back).
 *  - Pointer (menus only): left stick moves a mouse pointer, A clicks, right stick Y
 *    scrolls. A software cursor is blitted on the swapchain before each submit.
 *    "Menu" = the cursor is shown and relative mouse mode is off; in play the stick
 *    goes to the game as an ordinary gamepad axis. L3+R3 together cycles the pointer
 *    mode auto -> off -> on (fallback if the detection is wrong). */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <switch.h>
#include <SDL3/SDL.h>
#include "util.h"

static PadState g_pad;
static Mutex g_mx;
static int g_ready;

/* ---------------------------------------------------------------- state */
static u64 g_prev_held;
static Sint16 g_ax[6], g_pax[6];
static int g_added_sent;

#define QN 256
static SDL_Event g_q[QN];
static int g_qh, g_qc;

/* mode detection */
static volatile int g_relative;         /* SDL_SetWindowRelativeMouseMode(true) seen */
static volatile int g_cursor_hidden;    /* SDL_HideCursor seen */
static int g_force;                     /* 0 auto, 1 off (gamepad only), 2 on (pointer) */
static int g_last_menu = -1;

/* pointer */
static float g_px = 640.0f, g_py = 360.0f;
static float g_win_w = 1280.0f, g_win_h = 720.0f;
static u64 g_active_until;              /* system ticks */
static int g_ptr_moved_once;
static int g_a_as_click;                /* the A press that became a mouse click */
static u64 g_last_tick;
static float g_wheel_acc;
static u64 g_last_wheel;

/* Nintendo labels (see header): Switch A -> SOUTH, B -> EAST, Y -> WEST, X -> NORTH */
static const struct { u64 mask; int button; } MAP[] = {
  { HidNpadButton_A, SDL_GAMEPAD_BUTTON_SOUTH },
  { HidNpadButton_B, SDL_GAMEPAD_BUTTON_EAST },
  { HidNpadButton_Y, SDL_GAMEPAD_BUTTON_WEST },
  { HidNpadButton_X, SDL_GAMEPAD_BUTTON_NORTH },
  { HidNpadButton_Minus, SDL_GAMEPAD_BUTTON_BACK },
  { HidNpadButton_Plus, SDL_GAMEPAD_BUTTON_START },
  { HidNpadButton_StickL, SDL_GAMEPAD_BUTTON_LEFT_STICK },
  { HidNpadButton_StickR, SDL_GAMEPAD_BUTTON_RIGHT_STICK },
  { HidNpadButton_L, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER },     /* previous weapon (engine: LB) */
  { HidNpadButton_R, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER },    /* next weapon (engine: RB) */
  { HidNpadButton_Up, SDL_GAMEPAD_BUTTON_DPAD_UP },
  { HidNpadButton_Down, SDL_GAMEPAD_BUTTON_DPAD_DOWN },
  { HidNpadButton_Left, SDL_GAMEPAD_BUTTON_DPAD_LEFT },
  { HidNpadButton_Right, SDL_GAMEPAD_BUTTON_DPAD_RIGHT },
};
#define NMAP ((int)(sizeof MAP / sizeof MAP[0]))
#define IDX_A 0

void input_pad_init(void) {
  padConfigureInput(1, HidNpadStyleSet_NpadStandard);
  padInitializeDefault(&g_pad);
  g_ready = 1;
}
void input_pad_stop(void) {}

static Sint16 clampv(int v) { return (Sint16)(v < -32767 ? -32767 : v > 32767 ? 32767 : v); }
static void push(const SDL_Event *e) {
  if (g_qc >= QN) return;
  g_q[(g_qh + g_qc++) % QN] = *e;
}
static int pressed(u64 held, int i) { return (held & MAP[i].mask) != 0; }
static SDL_WindowID window_id(void) {
  static SDL_WindowID cached;       /* SDL_GetWindows takes SDL's video lock: ask once */
  if (cached) return cached;
  int n = 0; SDL_Window **w = SDL_GetWindows(&n);
  if (w && n > 0) cached = SDL_GetWindowID(w[0]);
  SDL_free(w);
  return cached;
}
/* Menu (pointer) vs play (gamepad). The engine never touches relative mouse mode or the
 * cursor on this build, so the signal is the game's own log: "[Session] enter MAP.nnn" =
 * play. L3+R3 together flips it by hand (g_force). */
static volatile int g_gameplay;
void input_pad_on_log(const char *msg) {
  if (msg && !strncmp(msg, "[Session] enter", 15)) { g_gameplay = 1; g_force = 0; debugPrintf("[pad] session started -> gamepad mode\n"); }
}
static int menu_mode(void) {
  if (g_force == 1) return 0;
  if (g_force == 2) return 1;
  if (g_relative || g_cursor_hidden) return 0;
  return !g_gameplay;
}
static int pointer_active(void) {
  return menu_mode() && g_ptr_moved_once && armGetSystemTick() < g_active_until;
}

/* ---------------------------------------------------------------- gyro */
/* Recipe from the Godot/unreal ports (memory: switch-gyro-recipe-axes, full-rate sampling):
 * sensor Y = turn, sensor X = look up/down (minus), sensor Z = "steering wheel" added to the
 * turn; read all 17 LIFO samples, average only the fresh ones; pad order FullKey, Handheld,
 * JoyDual. Here the result goes to the right-stick axes instead of mouse motion. */
static struct { int enabled, debug; float yaw, pitch, roll, full_dps, deg_per_unit, engine_dz, deadzone; } gc =
  { 1, 0, 1.0f, 0.6f, 1.0f, 150.0f, 360.0f, 0.16f, 0.4f };
static int g_gcfg_loaded;
static HidSixAxisSensorHandle g_gh;
static u32 g_gstyle_sel;                 /* style bit the handle was taken for */
static int g_ghave;
static u64 g_gseq;
static float g_gx, g_gy;                 /* stick contribution source: dps (turn) and dps (pitch) */
static u64 g_gfresh_tick;
static unsigned g_g_fresh_total;
static float g_g_maxabs[3];

static void gyro_load_cfg(void) {
  g_gcfg_loaded = 1;
  FILE *f = fopen("/switch/skynet_sdl_nx/gyro.cfg", "r");
  if (!f) {
    f = fopen("/switch/skynet_sdl_nx/gyro.cfg", "w");
    if (f) {
      fputs("# gyro look for SkyNET (Switch). Sensor axes: Y = turn, X = look up/down, Z = steering wheel.\n"
            "# full_dps = angular speed (deg/s) that equals a fully pushed right stick.\n"
            "enabled 1\nyaw 1.0\npitch 0.6\nroll 1.0\nfull_dps 150\ndeg_per_unit 360\nengine_deadzone 0.16\ndeadzone 0.4\ndebug 0\n", f);
      fclose(f);
    }
    return;
  }
  char line[128], key[40]; float v;
  while (fgets(line, sizeof line, f)) {
    if (line[0] == '#' || sscanf(line, "%39s %f", key, &v) != 2) continue;
    if (!strcmp(key, "enabled")) gc.enabled = (int)v;
    else if (!strcmp(key, "debug")) gc.debug = (int)v;
    else if (!strcmp(key, "yaw")) gc.yaw = v;
    else if (!strcmp(key, "pitch")) gc.pitch = v;
    else if (!strcmp(key, "roll")) gc.roll = v;
    else if (!strcmp(key, "full_dps")) gc.full_dps = v;
    else if (!strcmp(key, "deg_per_unit")) gc.deg_per_unit = v;
    else if (!strcmp(key, "engine_deadzone")) gc.engine_dz = v;
    else if (!strcmp(key, "deadzone")) gc.deadzone = v;
  }
  fclose(f);
  debugPrintf("[gyro] cfg: enabled=%d yaw=%.2f pitch=%.2f roll=%.2f full_dps=%.0f deg_per_unit=%.0f engine_dz=%.2f dz=%.2f\n",
              gc.enabled, gc.yaw, gc.pitch, gc.roll, gc.full_dps, gc.deg_per_unit, gc.engine_dz, gc.deadzone);
}
/* dps -> stick fraction, stepping over the engine's deadzone */
static float gyro_stick(float dps) {
  float a = dps < 0 ? -dps : dps;
  if (a < gc.deadzone) return 0.0f;
  float d = a / gc.full_dps; if (d > 1.0f) d = 1.0f;
  float o = gc.engine_dz + (1.0f - gc.engine_dz) * d;
  return dps < 0 ? -o : o;
}
static void gyro_select(u32 style) {
  u32 want = (style & HidNpadStyleTag_NpadFullKey) ? HidNpadStyleTag_NpadFullKey
           : (style & HidNpadStyleTag_NpadHandheld) ? HidNpadStyleTag_NpadHandheld
           : (style & HidNpadStyleTag_NpadJoyDual) ? HidNpadStyleTag_NpadJoyDual : 0;
  if (want == g_gstyle_sel) return;
  g_gstyle_sel = want; g_ghave = 0; g_gseq = 0;
  if (!want) return;
  HidNpadIdType id = (want == HidNpadStyleTag_NpadHandheld) ? HidNpadIdType_Handheld : HidNpadIdType_No1;
  HidSixAxisSensorHandle h[2];
  Result rc = hidGetSixAxisSensorHandles(h, (want == HidNpadStyleTag_NpadJoyDual) ? 2 : 1, id, (HidNpadStyleTag)want);
  if (R_FAILED(rc)) { debugPrintf("[gyro] handles failed rc=%#x (style %#x)\n", rc, want); return; }
  g_gh = h[0];
  hidStartSixAxisSensor(g_gh);
  hidSetGyroscopeZeroDriftMode(g_gh, HidGyroscopeZeroDriftMode_Loose);
  g_ghave = 1;
  debugPrintf("[gyro] using %s sensor\n", want == HidNpadStyleTag_NpadFullKey ? "Pro Controller" : want == HidNpadStyleTag_NpadHandheld ? "handheld" : "Joy-Con pair");
}
/* mean of the FRESH samples only, kept for 30 ms so that polls between two sensor updates
 * do not read zero */
static void gyro_update(u64 now, u32 style) {
  if (!g_gcfg_loaded) gyro_load_cfg();
  if (!gc.enabled) { g_gx = g_gy = 0; return; }
  gyro_select(style);
  if (!g_ghave) { g_gx = g_gy = 0; return; }
  HidSixAxisSensorState st[17];
  size_t n = hidGetSixAxisSensorStates(g_gh, st, 17);
  float sx = 0, sy = 0, sz = 0; int fresh = 0; u64 maxseq = g_gseq;
  for (size_t i = 0; i < n; i++) {
    if (st[i].sampling_number <= g_gseq) continue;
    sx += st[i].angular_velocity.x; sy += st[i].angular_velocity.y; sz += st[i].angular_velocity.z; fresh++;
    if (st[i].sampling_number > maxseq) maxseq = st[i].sampling_number;
  }
  if (fresh) {
    g_gseq = maxseq; g_gfresh_tick = now; g_g_fresh_total += (unsigned)fresh;
    float ax = sx / fresh * gc.deg_per_unit, ay = sy / fresh * gc.deg_per_unit, az = sz / fresh * gc.deg_per_unit;
    if (ax < 0 ? -ax > g_g_maxabs[0] : ax > g_g_maxabs[0]) g_g_maxabs[0] = ax < 0 ? -ax : ax;
    if (ay < 0 ? -ay > g_g_maxabs[1] : ay > g_g_maxabs[1]) g_g_maxabs[1] = ay < 0 ? -ay : ay;
    if (az < 0 ? -az > g_g_maxabs[2] : az > g_g_maxabs[2]) g_g_maxabs[2] = az < 0 ? -az : az;
    g_gx = -(ay * gc.yaw + az * gc.roll);        /* turn: right stick X (right positive) */
    g_gy = -(ax * gc.pitch);                     /* look: right stick Y (down positive) */
  } else if (now - g_gfresh_tick > armGetSystemTickFreq() * 3 / 100) {
    g_gx = g_gy = 0;
  }
}

/* ------------------------------------------------------------- refresh */
/* caller holds g_mx. The game polls events hundreds of thousands of times a second while
 * a menu waits (measured: 180000/s); the pad is read at most once per millisecond. */
static u64 g_last_refresh;
static void refresh(void) {
  if (!g_ready) return;
  u64 now = armGetSystemTick();
  if (g_last_refresh && now - g_last_refresh < armGetSystemTickFreq() / 1000) return;
  g_last_refresh = now;
  float dt = g_last_tick ? (float)(now - g_last_tick) / (float)armGetSystemTickFreq() : 0.0f;
  g_last_tick = now;
  if (dt > 0.1f) dt = 0.1f;

  padUpdate(&g_pad);
  gyro_update(now, padGetStyleSet(&g_pad));
  u64 held = padGetButtons(&g_pad);
  HidAnalogStickState l = padGetStickPos(&g_pad, 0), r = padGetStickPos(&g_pad, 1);
  Sint16 lx = clampv(l.x), ly = clampv(-l.y), rx = clampv(r.x), ry = clampv(-r.y);
  int menu = menu_mode();

  if (menu != g_last_menu) {
    g_last_menu = menu;
    debugPrintf("[pad] mode: %s (relative=%d cursor_hidden=%d force=%d)\n", menu ? "MENU/pointer" : "GAMEPLAY/gamepad",
                g_relative, g_cursor_hidden, g_force);
  }
  /* L + R + ZL + ZR together: gyro on/off */
  {
    u64 four = HidNpadButton_L | HidNpadButton_R | HidNpadButton_ZL | HidNpadButton_ZR;
    if ((held & four) == four && (g_prev_held & four) != four) {
      gc.enabled = !gc.enabled;
      debugPrintf("[gyro] %s\n", gc.enabled ? "on" : "off");
    }
  }
  /* L3+R3 together: flip pointer <-> gamepad by hand */
  {
    u64 both = HidNpadButton_StickL | HidNpadButton_StickR;
    if ((held & both) == both && (g_prev_held & both) != both) {
      g_force = menu ? 1 : 2;
      menu = menu_mode();
      debugPrintf("[pad] L3+R3: now %s\n", menu ? "pointer" : "gamepad");
    }
  }

  SDL_Event e;
  SDL_WindowID wid = 0;
  if (!g_added_sent) {
    g_added_sent = 1;
    SDL_zero(e); e.type = SDL_EVENT_GAMEPAD_ADDED; e.gdevice.timestamp = SDL_GetTicksNS(); e.gdevice.which = 1;
    push(&e);
    debugPrintf("[pad] gamepad 1 announced (SDL_EVENT_GAMEPAD_ADDED)\n");
  }

  /* ---- pointer from the left stick (menus) ---- */
  if (menu) {
    float fx = lx / 32767.0f, fy = ly / 32767.0f, mag = sqrtf(fx * fx + fy * fy);
    if (mag > 0.22f) {
      float s = (mag - 0.22f) / 0.78f; if (s > 1) s = 1;
      float speed = 120.0f + 1100.0f * s * s;            /* px/s */
      float dx = fx / mag * speed * dt, dy = fy / mag * speed * dt;
      float ox = g_px, oy = g_py;
      g_px += dx; g_py += dy;
      if (g_px < 0) g_px = 0; if (g_py < 0) g_py = 0;
      if (g_px > g_win_w - 1) g_px = g_win_w - 1; if (g_py > g_win_h - 1) g_py = g_win_h - 1;
      g_ptr_moved_once = 1;
      g_active_until = now + 5 * armGetSystemTickFreq();
      if (!wid) wid = window_id();
      SDL_zero(e); e.type = SDL_EVENT_MOUSE_MOTION; e.motion.timestamp = SDL_GetTicksNS(); e.motion.windowID = wid;
      e.motion.which = 1; e.motion.x = g_px; e.motion.y = g_py; e.motion.xrel = g_px - ox; e.motion.yrel = g_py - oy;
      push(&e);
    }
    /* right stick Y: wheel */
    float wy = ry / 32767.0f;
    if (fabsf(wy) > 0.5f && now - g_last_wheel > armGetSystemTickFreq() / 8) {
      g_last_wheel = now;
      if (!wid) wid = window_id();
      SDL_zero(e); e.type = SDL_EVENT_MOUSE_WHEEL; e.wheel.timestamp = SDL_GetTicksNS(); e.wheel.windowID = wid;
      e.wheel.which = 1; e.wheel.y = wy > 0 ? -1.0f : 1.0f; e.wheel.mouse_x = g_px; e.wheel.mouse_y = g_py;
      push(&e);
    }
  }
  int ptr_on = pointer_active();

  /* ---- axes: the engine sees the sticks only in gameplay ---- */
  g_ax[0] = menu ? 0 : lx; g_ax[1] = menu ? 0 : ly; g_ax[2] = menu ? 0 : rx; g_ax[3] = menu ? 0 : ry;
  g_ax[4] = (held & HidNpadButton_ZL) ? 32767 : 0;
  g_ax[5] = (!menu && (held & HidNpadButton_ZR)) ? 32767 : 0;     /* ZR = fire (engine: RT); in menus it is the mouse click */
  if (!menu && gc.enabled) {
    /* gyro: added to the right stick (look); the engine's own deadzone is stepped over */
    float sx = gyro_stick(g_gx), sy = gyro_stick(g_gy);
    int vx = g_ax[2] + (int)(sx * 32767.0f), vy = g_ax[3] + (int)(sy * 32767.0f);
    g_ax[2] = clampv(vx); g_ax[3] = clampv(vy);
  }

  /* ---- buttons ---- */
  for (int i = 0; i < NMAP; i++) {
    int now_down = pressed(held, i), was = pressed(g_prev_held, i);
    if (now_down == was) continue;
    /* A stays the gamepad's A (menu Accept): the engine's screens are built for it (the name
     * dialog goes NAME -> CALLSIGN -> start on A; a mouse click only puts the cursor in a field).
     * Hovering with the pointer highlights an item, so A on it works as a click. */
    SDL_zero(e); e.type = now_down ? SDL_EVENT_GAMEPAD_BUTTON_DOWN : SDL_EVENT_GAMEPAD_BUTTON_UP;
    e.gbutton.timestamp = SDL_GetTicksNS(); e.gbutton.which = 1; e.gbutton.button = (Uint8)MAP[i].button; e.gbutton.down = now_down;
    push(&e);
  }
  /* ZR in a menu = left mouse button at the pointer (sliders, fields, anything that wants a real click) */
  if (menu) {
    int zr = (held & HidNpadButton_ZR) != 0, zrp = (g_prev_held & HidNpadButton_ZR) != 0;
    if (zr != zrp) {
      if (!wid) wid = window_id();
      g_a_as_click = zr;
      g_ptr_moved_once = 1; g_active_until = now + 5 * armGetSystemTickFreq();   /* a click also shows the pointer */
      SDL_zero(e); e.type = zr ? SDL_EVENT_MOUSE_BUTTON_DOWN : SDL_EVENT_MOUSE_BUTTON_UP;
      e.button.timestamp = SDL_GetTicksNS(); e.button.windowID = wid; e.button.which = 1;
      e.button.button = SDL_BUTTON_LEFT; e.button.down = zr; e.button.clicks = 1; e.button.x = g_px; e.button.y = g_py;
      push(&e);
    }
  } else g_a_as_click = 0;
  g_prev_held = held;
  for (int a = 0; a < 6; a++) {
    int d = g_ax[a] - g_pax[a]; if (d < 0) d = -d;
    if (d >= 256 || (g_ax[a] == 0 && g_pax[a] != 0)) {
      SDL_zero(e); e.type = SDL_EVENT_GAMEPAD_AXIS_MOTION;
      e.gaxis.timestamp = SDL_GetTicksNS(); e.gaxis.which = 1; e.gaxis.axis = (Uint8)a; e.gaxis.value = g_ax[a];
      push(&e); g_pax[a] = g_ax[a];
    }
  }
}

/* ------------------------------------------------- mode detection hooks */
bool skynet_SDL_SetWindowRelativeMouseMode(SDL_Window *w, bool en) {
  g_relative = en ? 1 : 0;
  return SDL_SetWindowRelativeMouseMode(w, en);
}
bool skynet_SDL_HideCursor(void) { g_cursor_hidden = 1; return SDL_HideCursor(); }
bool skynet_SDL_ShowCursor(void) { g_cursor_hidden = 0; return SDL_ShowCursor(); }
SDL_MouseButtonFlags skynet_SDL_GetMouseState(float *x, float *y) {
  SDL_MouseButtonFlags f = SDL_GetMouseState(x, y);
  mutexLock(&g_mx);
  int on = pointer_active();
  float px = g_px, py = g_py; int click = g_a_as_click;
  mutexUnlock(&g_mx);
  if (on) { if (x) *x = px; if (y) *y = py; if (click) f |= SDL_BUTTON_LMASK; }
  return f;
}

/* ----------------------------------------------------------- SDL events */
static int g_fake_gamepad;     /* its address is the "SDL_Gamepad *" */

/* Touch is dropped before the engine sees it: the game wants physical controls only, and
 * its on-screen touch UI (TouchControls) both shows up on the first finger and throws
 * KeyNotFoundException in StickAt() when fingers arrive on this build. */
static int is_touch_event(const SDL_Event *e) {
  switch (e->type) {
    case SDL_EVENT_FINGER_DOWN: case SDL_EVENT_FINGER_UP: case SDL_EVENT_FINGER_MOTION: case SDL_EVENT_FINGER_CANCELED:
      return 1;
    case SDL_EVENT_MOUSE_MOTION: return e->motion.which == SDL_TOUCH_MOUSEID;
    case SDL_EVENT_MOUSE_BUTTON_DOWN: case SDL_EVENT_MOUSE_BUTTON_UP: return e->button.which == SDL_TOUCH_MOUSEID;
    default: return 0;
  }
}
SDL_TouchID *skynet_SDL_GetTouchDevices(int *count) { if (count) *count = 0; return NULL; }

/* statistics, logged every 5 s: how often the game polls, and how often frames are submitted */
static u64 g_st_t0; static unsigned g_st_poll, g_st_submit, g_st_maxq;
static void stats_tick(void) {
  u64 now = armGetSystemTick(), f = armGetSystemTickFreq();
  if (!g_st_t0) g_st_t0 = now;
  if (now - g_st_t0 >= 5 * f) {
    debugPrintf("[pad] 5 s: %u polls, %u submits, max queue %u, mode %s\n", g_st_poll, g_st_submit, g_st_maxq, menu_mode() ? "pointer" : "gamepad");
    if (gc.enabled) {
      debugPrintf("[gyro] 5 s: %u fresh samples, peak |rate| x=%.0f y=%.0f z=%.0f deg/s (assuming %.0f deg per unit)\n",
                  g_g_fresh_total, g_g_maxabs[0], g_g_maxabs[1], g_g_maxabs[2], gc.deg_per_unit);
    }
    g_g_fresh_total = 0; g_g_maxabs[0] = g_g_maxabs[1] = g_g_maxabs[2] = 0;
    g_st_t0 = now; g_st_poll = g_st_submit = g_st_maxq = 0;
  }
}

bool skynet_SDL_PollEvent(SDL_Event *ev) {
  if (!ev) return SDL_PollEvent(ev);
  mutexLock(&g_mx);
  refresh();
  g_st_poll++; if ((unsigned)g_qc > g_st_maxq) g_st_maxq = (unsigned)g_qc;
  stats_tick();
  if (g_qc > 0) {
    *ev = g_q[g_qh]; g_qh = (g_qh + 1) % QN; g_qc--;
    mutexUnlock(&g_mx);
    return true;
  }
  mutexUnlock(&g_mx);
  while (SDL_PollEvent(ev)) {
    if (!is_touch_event(ev)) return true;
  }
  /* nothing to report: when the game spins on this call (menus: ~180000/s measured, which
   * starved the render thread and made the pointer judder), yield a little */
  {
    static u64 last_empty;
    u64 now = armGetSystemTick();
    if (last_empty && now - last_empty < armGetSystemTickFreq() / 2000) svcSleepThread(300000ULL);
    last_empty = armGetSystemTick();
  }
  return false;
}
void skynet_SDL_UpdateGamepads(void) { mutexLock(&g_mx); refresh(); mutexUnlock(&g_mx); }

SDL_JoystickID *skynet_SDL_GetGamepads(int *count) {
  SDL_JoystickID *a = SDL_malloc(2 * sizeof *a);
  if (a) { a[0] = 1; a[1] = 0; }
  if (count) *count = a ? 1 : 0;
  return a;
}
bool skynet_SDL_HasGamepad(void) { return true; }
bool skynet_SDL_IsGamepad(SDL_JoystickID id) { return id == 1; }
SDL_Gamepad *skynet_SDL_OpenGamepad(SDL_JoystickID id) { return id == 1 ? (SDL_Gamepad *)&g_fake_gamepad : NULL; }
void skynet_SDL_CloseGamepad(SDL_Gamepad *g) { (void)g; }
bool skynet_SDL_GamepadConnected(SDL_Gamepad *g) { return g == (SDL_Gamepad *)&g_fake_gamepad; }
SDL_JoystickID skynet_SDL_GetGamepadID(SDL_Gamepad *g) { (void)g; return 1; }
SDL_Gamepad *skynet_SDL_GetGamepadFromID(SDL_JoystickID id) { return id == 1 ? (SDL_Gamepad *)&g_fake_gamepad : NULL; }
const char *skynet_SDL_GetGamepadName(SDL_Gamepad *g) { (void)g; return "Nintendo Switch"; }
const char *skynet_SDL_GetGamepadNameForID(SDL_JoystickID id) { (void)id; return "Nintendo Switch"; }
/* reported as a STANDARD pad: the engine then uses Xbox-style names (A/B/X/Y), which the
 * label mapping above matches */
SDL_GamepadType skynet_SDL_GetGamepadType(SDL_Gamepad *g) { (void)g; return SDL_GAMEPAD_TYPE_STANDARD; }
SDL_GamepadType skynet_SDL_GetGamepadTypeForID(SDL_JoystickID id) { (void)id; return SDL_GAMEPAD_TYPE_STANDARD; }
int skynet_SDL_GetGamepadPlayerIndex(SDL_Gamepad *g) { (void)g; return 0; }
bool skynet_SDL_GamepadHasButton(SDL_Gamepad *g, SDL_GamepadButton b) { (void)g; return b >= 0 && b <= SDL_GAMEPAD_BUTTON_DPAD_RIGHT && b != SDL_GAMEPAD_BUTTON_GUIDE; }
bool skynet_SDL_GamepadHasAxis(SDL_Gamepad *g, SDL_GamepadAxis a) { (void)g; return a >= 0 && a <= SDL_GAMEPAD_AXIS_RIGHT_TRIGGER; }
bool skynet_SDL_GetGamepadButton(SDL_Gamepad *g, SDL_GamepadButton b) {
  (void)g;
  mutexLock(&g_mx); refresh();
  int down = 0;
  for (int i = 0; i < NMAP; i++) if (MAP[i].button == (int)b) down = pressed(g_prev_held, i);
  mutexUnlock(&g_mx);
  return down != 0;
}
Sint16 skynet_SDL_GetGamepadAxis(SDL_Gamepad *g, SDL_GamepadAxis a) {
  (void)g;
  if (a < 0 || a > 5) return 0;
  mutexLock(&g_mx); refresh(); Sint16 v = g_ax[a]; mutexUnlock(&g_mx);
  return v;
}
bool skynet_SDL_RumbleGamepad(SDL_Gamepad *g, Uint16 lo, Uint16 hi, Uint32 ms) { (void)g; (void)lo; (void)hi; (void)ms; return true; }
bool skynet_SDL_RumbleGamepadTriggers(SDL_Gamepad *g, Uint16 l, Uint16 r, Uint32 ms) { (void)g; (void)l; (void)r; (void)ms; return true; }
SDL_Joystick *skynet_SDL_GetGamepadJoystick(SDL_Gamepad *g) { (void)g; return NULL; }
bool skynet_SDL_GamepadHasSensor(SDL_Gamepad *g, SDL_SensorType t) { (void)g; (void)t; return false; }
bool skynet_SDL_GamepadSensorEnabled(SDL_Gamepad *g, SDL_SensorType t) { (void)g; (void)t; return false; }
bool skynet_SDL_SetGamepadSensorEnabled(SDL_Gamepad *g, SDL_SensorType t, bool en) { (void)g; (void)t; (void)en; return false; }
bool skynet_SDL_GamepadEventsEnabled(void) { return true; }
void skynet_SDL_SetGamepadEventsEnabled(bool en) { (void)en; }

/* The engine decides whether a gamepad can exist from SDL_Init/SDL_InitSubSystem/
 * SDL_WasInit. The native build fails JOYSTICK and GAMEPAD, so those two are claimed
 * here (the pad is ours) and removed from what the native SDL is asked to start. */
#define FAKE_SUBS (SDL_INIT_JOYSTICK | SDL_INIT_GAMEPAD)
static SDL_InitFlags g_claimed;
bool skynet_SDL_Init(SDL_InitFlags f) { g_claimed |= f & FAKE_SUBS; return SDL_Init(f & ~FAKE_SUBS); }
bool skynet_SDL_InitSubSystem(SDL_InitFlags f) { g_claimed |= f & FAKE_SUBS; return SDL_InitSubSystem(f & ~FAKE_SUBS); }
void skynet_SDL_QuitSubSystem(SDL_InitFlags f) { g_claimed &= ~(f & FAKE_SUBS); SDL_QuitSubSystem(f & ~FAKE_SUBS); }
SDL_InitFlags skynet_SDL_WasInit(SDL_InitFlags f) {
  SDL_InitFlags r = SDL_WasInit(f & ~FAKE_SUBS);
  if (f == 0) return r | g_claimed;
  return r | (g_claimed & f & FAKE_SUBS);
}

/* ------------------------------------------------- software mouse cursor */
/* No shaders needed: SDL_BlitGPUTexture copies rectangles of a tiny 2x1 texture
 * (black pixel, white pixel) onto the swapchain image just before the frame is
 * submitted. An arrow is a stack of such strips. */
static SDL_GPUDevice *g_dev;
static SDL_GPUTexture *g_cur_tex;
static SDL_GPUCommandBuffer *g_sw_cmd;
static SDL_GPUTexture *g_sw_tex;
static Uint32 g_sw_w, g_sw_h;
static int g_cur_failed, g_cur_logged;

SDL_GPUDevice *skynet_SDL_CreateGPUDeviceWithProperties(SDL_PropertiesID props) {
  SDL_GPUDevice *d = SDL_CreateGPUDeviceWithProperties(props);
  if (d) g_dev = d;
  return d;
}
SDL_GPUDevice *skynet_SDL_CreateGPUDevice(SDL_GPUShaderFormat f, bool dbg, const char *name) {
  SDL_GPUDevice *d = SDL_CreateGPUDevice(f, dbg, name);
  if (d) g_dev = d;
  return d;
}
static void note_swapchain(SDL_GPUCommandBuffer *cmd, SDL_GPUTexture **tex, Uint32 *w, Uint32 *h) {
  if (tex && *tex) {
    g_sw_cmd = cmd; g_sw_tex = *tex;
    if (w && h && *w && *h) { g_sw_w = *w; g_sw_h = *h; g_win_w = (float)*w; g_win_h = (float)*h; }
  }
}
bool skynet_SDL_WaitAndAcquireGPUSwapchainTexture(SDL_GPUCommandBuffer *cmd, SDL_Window *win, SDL_GPUTexture **tex, Uint32 *w, Uint32 *h) {
  Uint32 lw = 0, lh = 0;
  bool ok = SDL_WaitAndAcquireGPUSwapchainTexture(cmd, win, tex, w ? w : &lw, h ? h : &lh);
  note_swapchain(cmd, tex, w ? w : &lw, h ? h : &lh);
  return ok;
}
bool skynet_SDL_AcquireGPUSwapchainTexture(SDL_GPUCommandBuffer *cmd, SDL_Window *win, SDL_GPUTexture **tex, Uint32 *w, Uint32 *h) {
  Uint32 lw = 0, lh = 0;
  bool ok = SDL_AcquireGPUSwapchainTexture(cmd, win, tex, w ? w : &lw, h ? h : &lh);
  note_swapchain(cmd, tex, w ? w : &lw, h ? h : &lh);
  return ok;
}

static int make_cursor_texture(SDL_GPUCommandBuffer *cmd) {
  SDL_GPUTextureCreateInfo ti; SDL_zero(ti);
  ti.type = SDL_GPU_TEXTURETYPE_2D; ti.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
  ti.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER; ti.width = 2; ti.height = 1;
  ti.layer_count_or_depth = 1; ti.num_levels = 1; ti.sample_count = SDL_GPU_SAMPLECOUNT_1;
  g_cur_tex = SDL_CreateGPUTexture(g_dev, &ti);
  if (!g_cur_tex) return 0;
  SDL_GPUTransferBufferCreateInfo bi; SDL_zero(bi);
  bi.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD; bi.size = 8;
  SDL_GPUTransferBuffer *tb = SDL_CreateGPUTransferBuffer(g_dev, &bi);
  if (!tb) return 0;
  Uint8 *p = SDL_MapGPUTransferBuffer(g_dev, tb, false);
  if (!p) { SDL_ReleaseGPUTransferBuffer(g_dev, tb); return 0; }
  const Uint8 px[8] = { 0, 0, 0, 255, 255, 255, 255, 255 };   /* black, white */
  memcpy(p, px, 8);
  SDL_UnmapGPUTransferBuffer(g_dev, tb);
  SDL_GPUCopyPass *cp = SDL_BeginGPUCopyPass(cmd);
  SDL_GPUTextureTransferInfo src; SDL_zero(src); src.transfer_buffer = tb; src.pixels_per_row = 2; src.rows_per_layer = 1;
  SDL_GPUTextureRegion dst; SDL_zero(dst); dst.texture = g_cur_tex; dst.w = 2; dst.h = 1; dst.d = 1;
  SDL_UploadToGPUTexture(cp, &src, &dst, false);
  SDL_EndGPUCopyPass(cp);
  SDL_ReleaseGPUTransferBuffer(g_dev, tb);
  return 1;
}
/* one strip: white (src x=1) or black (src x=0) */
static void strip(SDL_GPUCommandBuffer *cmd, SDL_GPUTexture *dst, int white, int x, int y, int w, int h) {
  if (x < 0) { w += x; x = 0; }
  if (y < 0) { h += y; y = 0; }
  if (w <= 0 || h <= 0 || x >= (int)g_sw_w || y >= (int)g_sw_h) return;
  if (x + w > (int)g_sw_w) w = (int)g_sw_w - x;
  if (y + h > (int)g_sw_h) h = (int)g_sw_h - y;
  SDL_GPUBlitInfo bl; SDL_zero(bl);
  bl.source.texture = g_cur_tex; bl.source.x = white ? 1 : 0; bl.source.y = 0; bl.source.w = 1; bl.source.h = 1;
  bl.destination.texture = dst; bl.destination.x = (Uint32)x; bl.destination.y = (Uint32)y; bl.destination.w = (Uint32)w; bl.destination.h = (Uint32)h;
  bl.load_op = SDL_GPU_LOADOP_LOAD; bl.filter = SDL_GPU_FILTER_NEAREST;
  SDL_BlitGPUTexture(cmd, &bl);
}
static void draw_cursor(SDL_GPUCommandBuffer *cmd, SDL_GPUTexture *dst, int cx, int cy) {
  if (g_cur_failed || !g_dev) return;
  if (!g_cur_tex && !make_cursor_texture(cmd)) {
    g_cur_failed = 1; debugPrintf("[pad] cursor texture creation failed: %s\n", SDL_GetError()); return;
  }
  if (!g_cur_logged) { g_cur_logged = 1; debugPrintf("[pad] software cursor drawn (swapchain %ux%u)\n", g_sw_w, g_sw_h); }
  static const int RW[] = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 4, 4, 3, 3 };
  const int NR = (int)(sizeof RW / sizeof RW[0]);
  int s = (int)(g_sw_h / 360); if (s < 1) s = 1;
  /* black outline first, then the white fill on top */
  strip(cmd, dst, 0, cx - s, cy - s, 3 * s, s);
  for (int i = 0; i < NR; i++) {
    int w = RW[i] * s;
    int off = (i >= 10) ? (4 - RW[i]) * s / 2 + s : 0;     /* stem sits a little in */
    strip(cmd, dst, 0, cx - s + off, cy + i * s, w + 2 * s, s);
  }
  strip(cmd, dst, 0, cx - s + s, cy + NR * s, 4 * s, s);
  for (int i = 0; i < NR; i++) {
    int w = RW[i] * s;
    int off = (i >= 10) ? (4 - RW[i]) * s / 2 + s : 0;
    strip(cmd, dst, 1, cx + off, cy + i * s, w, s);
  }
}

bool skynet_SDL_SubmitGPUCommandBuffer(SDL_GPUCommandBuffer *cmd) {
  if (cmd == g_sw_cmd && g_sw_tex) {
    mutexLock(&g_mx);
    refresh();               /* the cursor is placed at present time, not at the last poll: smooth */
    g_st_submit++;
    int on = pointer_active();
    int cx = (int)g_px, cy = (int)g_py;
    mutexUnlock(&g_mx);
    if (on) draw_cursor(cmd, g_sw_tex, cx, cy);
    g_sw_cmd = NULL; g_sw_tex = NULL;
  }
  return SDL_SubmitGPUCommandBuffer(cmd);
}
SDL_GPUFence *skynet_SDL_SubmitGPUCommandBufferAndAcquireFence(SDL_GPUCommandBuffer *cmd) {
  if (cmd == g_sw_cmd && g_sw_tex) {
    mutexLock(&g_mx);
    refresh();               /* the cursor is placed at present time, not at the last poll: smooth */
    g_st_submit++;
    int on = pointer_active();
    int cx = (int)g_px, cy = (int)g_py;
    mutexUnlock(&g_mx);
    if (on) draw_cursor(cmd, g_sw_tex, cx, cy);
    g_sw_cmd = NULL; g_sw_tex = NULL;
  }
  return SDL_SubmitGPUCommandBufferAndAcquireFence(cmd);
}
