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
  { HidNpadButton_L, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER },
  { HidNpadButton_R, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER },
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
  int n = 0; SDL_Window **w = SDL_GetWindows(&n);
  SDL_WindowID id = (w && n > 0) ? SDL_GetWindowID(w[0]) : 0;
  SDL_free(w);
  return id;
}
static int menu_mode(void) {
  if (g_force == 1) return 0;
  if (g_force == 2) return 1;
  return !g_relative && !g_cursor_hidden;
}
static int pointer_active(void) {
  return menu_mode() && g_ptr_moved_once && armGetSystemTick() < g_active_until;
}

/* ------------------------------------------------------------- refresh */
/* caller holds g_mx */
static void refresh(void) {
  if (!g_ready) return;
  u64 now = armGetSystemTick();
  float dt = g_last_tick ? (float)(now - g_last_tick) / (float)armGetSystemTickFreq() : 0.0f;
  g_last_tick = now;
  if (dt > 0.1f) dt = 0.1f;

  padUpdate(&g_pad);
  u64 held = padGetButtons(&g_pad);
  HidAnalogStickState l = padGetStickPos(&g_pad, 0), r = padGetStickPos(&g_pad, 1);
  Sint16 lx = clampv(l.x), ly = clampv(-l.y), rx = clampv(r.x), ry = clampv(-r.y);
  int menu = menu_mode();

  if (menu != g_last_menu) {
    g_last_menu = menu;
    debugPrintf("[pad] mode: %s (relative=%d cursor_hidden=%d force=%d)\n", menu ? "MENU/pointer" : "GAMEPLAY/gamepad",
                g_relative, g_cursor_hidden, g_force);
  }
  /* L3+R3 together: cycle pointer mode auto -> off -> on */
  {
    u64 both = HidNpadButton_StickL | HidNpadButton_StickR;
    if ((held & both) == both && (g_prev_held & both) != both) {
      g_force = (g_force + 1) % 3;
      debugPrintf("[pad] pointer mode forced: %s\n", g_force == 0 ? "auto" : g_force == 1 ? "off" : "on");
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
  g_ax[5] = (held & HidNpadButton_ZR) ? 32767 : 0;

  /* ---- buttons ---- */
  for (int i = 0; i < NMAP; i++) {
    int now_down = pressed(held, i), was = pressed(g_prev_held, i);
    if (now_down == was) continue;
    if (i == IDX_A && (ptr_on || g_a_as_click)) {
      /* A with the pointer showing is a mouse click (not also a menu "accept") */
      if (now_down) g_a_as_click = 1;
      if (!now_down || g_a_as_click) {
        if (!wid) wid = window_id();
        SDL_zero(e); e.type = now_down ? SDL_EVENT_MOUSE_BUTTON_DOWN : SDL_EVENT_MOUSE_BUTTON_UP;
        e.button.timestamp = SDL_GetTicksNS(); e.button.windowID = wid; e.button.which = 1;
        e.button.button = SDL_BUTTON_LEFT; e.button.down = now_down; e.button.clicks = 1; e.button.x = g_px; e.button.y = g_py;
        push(&e);
        if (!now_down) g_a_as_click = 0;
      }
      continue;
    }
    SDL_zero(e); e.type = now_down ? SDL_EVENT_GAMEPAD_BUTTON_DOWN : SDL_EVENT_GAMEPAD_BUTTON_UP;
    e.gbutton.timestamp = SDL_GetTicksNS(); e.gbutton.which = 1; e.gbutton.button = (Uint8)MAP[i].button; e.gbutton.down = now_down;
    push(&e);
  }
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

bool skynet_SDL_PollEvent(SDL_Event *ev) {
  mutexLock(&g_mx);
  refresh();
  if (g_qc > 0 && ev) {
    *ev = g_q[g_qh]; g_qh = (g_qh + 1) % QN; g_qc--;
    mutexUnlock(&g_mx);
    return true;
  }
  mutexUnlock(&g_mx);
  return SDL_PollEvent(ev);
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
  for (int i = 0; i < NMAP; i++) if (MAP[i].button == (int)b) down = pressed(g_prev_held, i) && !(i == IDX_A && g_a_as_click);
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
    int on = pointer_active();
    int cx = (int)g_px, cy = (int)g_py;
    mutexUnlock(&g_mx);
    if (on) draw_cursor(cmd, g_sw_tex, cx, cy);
    g_sw_cmd = NULL; g_sw_tex = NULL;
  }
  return SDL_SubmitGPUCommandBufferAndAcquireFence(cmd);
}
