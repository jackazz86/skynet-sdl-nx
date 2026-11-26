/* input_pad.c -- gamepad for the game on a native SDL3 build that has NO joystick
 * support at all (SDL_InitSubSystem(JOYSTICK) and SDL_AttachVirtualJoystick both fail:
 * "SDL not built with joystick support"). Like the Android-wrapper ports do for their
 * engines, input comes straight from libnx's pad API; here the engine is closed, so the
 * seam is the dlsym table: the SDL gamepad entry points the game resolves are OURS
 * (tools/sdl_override.txt maps SDL_X -> skynet_SDL_X), and SDL_PollEvent is wrapped to
 * inject SDL_EVENT_GAMEPAD_* events before delegating to the native one.
 *
 * Layout is SDL's positional "standard" one: Switch B = SOUTH, A = EAST, Y = WEST,
 * X = NORTH. */
#include <stdint.h>
#include <string.h>
#include <switch.h>
#include <SDL3/SDL.h>
#include "util.h"

static PadState g_pad;
static Mutex g_mx;
static int g_ready;

/* state */
static u64 g_prev_held;
static Sint16 g_ax[6], g_pax[6];
static int g_added_sent;

/* synthetic event queue */
#define QN 128
static SDL_Event g_q[QN];
static int g_qh, g_qc;

static const struct { u64 mask; int button; } MAP[] = {
  { HidNpadButton_B, SDL_GAMEPAD_BUTTON_SOUTH },
  { HidNpadButton_A, SDL_GAMEPAD_BUTTON_EAST },
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
static int g_pressed(u64 held, int i) { return (held & MAP[i].mask) != 0; }

/* caller holds g_mx */
static void refresh(void) {
  if (!g_ready) return;
  padUpdate(&g_pad);
  u64 held = padGetButtons(&g_pad);
  HidAnalogStickState l = padGetStickPos(&g_pad, 0), r = padGetStickPos(&g_pad, 1);
  g_ax[0] = clampv(l.x); g_ax[1] = clampv(-l.y); g_ax[2] = clampv(r.x); g_ax[3] = clampv(-r.y);
  g_ax[4] = (held & HidNpadButton_ZL) ? 32767 : 0;
  g_ax[5] = (held & HidNpadButton_ZR) ? 32767 : 0;

  SDL_Event e;
  if (!g_added_sent) {
    g_added_sent = 1;
    SDL_zero(e); e.type = SDL_EVENT_GAMEPAD_ADDED; e.gdevice.timestamp = SDL_GetTicksNS(); e.gdevice.which = 1;
    push(&e);
    debugPrintf("[pad] gamepad 1 announced (SDL_EVENT_GAMEPAD_ADDED)\n");
  }
  for (int i = 0; i < NMAP; i++) {
    int now = g_pressed(held, i), was = g_pressed(g_prev_held, i);
    if (now != was) {
      SDL_zero(e); e.type = now ? SDL_EVENT_GAMEPAD_BUTTON_DOWN : SDL_EVENT_GAMEPAD_BUTTON_UP;
      e.gbutton.timestamp = SDL_GetTicksNS(); e.gbutton.which = 1; e.gbutton.button = (Uint8)MAP[i].button; e.gbutton.down = now;
      push(&e);
    }
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

/* ---- the SDL entry points the game resolves through dlsym ---- */
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
SDL_GamepadType skynet_SDL_GetGamepadType(SDL_Gamepad *g) { (void)g; return SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_PRO; }
SDL_GamepadType skynet_SDL_GetGamepadTypeForID(SDL_JoystickID id) { (void)id; return SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_PRO; }
int skynet_SDL_GetGamepadPlayerIndex(SDL_Gamepad *g) { (void)g; return 0; }
bool skynet_SDL_GamepadHasButton(SDL_Gamepad *g, SDL_GamepadButton b) { (void)g; return b >= 0 && b <= SDL_GAMEPAD_BUTTON_DPAD_RIGHT && b != SDL_GAMEPAD_BUTTON_GUIDE; }
bool skynet_SDL_GamepadHasAxis(SDL_Gamepad *g, SDL_GamepadAxis a) { (void)g; return a >= 0 && a <= SDL_GAMEPAD_AXIS_RIGHT_TRIGGER; }
bool skynet_SDL_GetGamepadButton(SDL_Gamepad *g, SDL_GamepadButton b) {
  (void)g;
  mutexLock(&g_mx); refresh();
  int down = 0;
  for (int i = 0; i < NMAP; i++) if (MAP[i].button == (int)b) down = g_pressed(g_prev_held, i);
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
