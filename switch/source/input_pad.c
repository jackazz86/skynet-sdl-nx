/* input_pad.c -- controller input for the native SDL3 build, which has keyboard, mouse
 * and touch drivers for Horizon but NO gamepad driver. A virtual SDL gamepad (standard
 * layout) is attached and fed from libnx's HID pad API, so the game sees an ordinary
 * SDL_Gamepad: SDL_GetGamepads, GAMEPAD_* events, button/axis queries.
 *
 * Layout is positional (SDL's "standard" mapping), so Switch B is SOUTH, A is EAST,
 * Y is WEST, X is NORTH. */
#include <stdint.h>
#include <string.h>
#include <switch.h>
#include <SDL3/SDL.h>
#include "util.h"

static volatile int g_stop;
void input_pad_stop(void) { g_stop = 1; }

static Sint16 stick(int v) { return (Sint16)(v < -32767 ? -32767 : v > 32767 ? 32767 : v); }

void *input_pad_thread(void *arg) {
  (void)arg;
  padConfigureInput(1, HidNpadStyleSet_NpadStandard);
  PadState pad;
  padInitializeDefault(&pad);

  /* the game initialises SDL's joystick subsystem itself: wait for it */
  SDL_VirtualJoystickDesc desc;
  SDL_INIT_INTERFACE(&desc);
  desc.type = SDL_JOYSTICK_TYPE_GAMEPAD;
  desc.naxes = 6;        /* lx ly rx ry lt rt */
  desc.nbuttons = 15;    /* SDL_GamepadButton order up to DPAD_RIGHT */
  desc.nhats = 0;
  desc.vendor_id = 0x057E;   /* Nintendo */
  desc.product_id = 0x2009;  /* Switch Pro Controller */
  desc.name = "Nintendo Switch";
  SDL_JoystickID id = 0;
  while (!g_stop && !(id = SDL_AttachVirtualJoystick(&desc))) svcSleepThread(500000000ULL);
  if (!id) return NULL;
  SDL_Joystick *joy = NULL;
  while (!g_stop && !(joy = SDL_OpenJoystick(id))) svcSleepThread(100000000ULL);
  debugPrintf("[pad] virtual gamepad attached id=%u joy=%p\n", (unsigned)id, (void *)joy);
  if (!joy) return NULL;

  static const struct { u64 mask; int button; } MAP[] = {
    { HidNpadButton_B, 0 },      /* SOUTH */
    { HidNpadButton_A, 1 },      /* EAST  */
    { HidNpadButton_Y, 2 },      /* WEST  */
    { HidNpadButton_X, 3 },      /* NORTH */
    { HidNpadButton_Minus, 4 },  /* BACK  */
    { HidNpadButton_Plus, 6 },   /* START */
    { HidNpadButton_StickL, 7 },
    { HidNpadButton_StickR, 8 },
    { HidNpadButton_L, 9 },
    { HidNpadButton_R, 10 },
    { HidNpadButton_Up, 11 },
    { HidNpadButton_Down, 12 },
    { HidNpadButton_Left, 13 },
    { HidNpadButton_Right, 14 },
  };
  u64 prev = ~0ull;
  Sint16 pax[6] = { 1, 1, 1, 1, 1, 1 };

  while (!g_stop) {
    padUpdate(&pad);
    u64 held = padGetButtons(&pad);
    for (size_t i = 0; i < sizeof MAP / sizeof MAP[0]; i++) {
      u64 m = MAP[i].mask;
      if ((held & m) != (prev & m)) SDL_SetJoystickVirtualButton(joy, MAP[i].button, (held & m) != 0);
    }
    prev = held;
    HidAnalogStickState l = padGetStickPos(&pad, 0), r = padGetStickPos(&pad, 1);
    Sint16 ax[6] = { stick(l.x), stick(-l.y), stick(r.x), stick(-r.y),
                     (held & HidNpadButton_ZL) ? 32767 : 0, (held & HidNpadButton_ZR) ? 32767 : 0 };
    for (int i = 0; i < 6; i++)
      if (ax[i] != pax[i]) { SDL_SetJoystickVirtualAxis(joy, i, ax[i]); pax[i] = ax[i]; }
    svcSleepThread(4000000ULL);   /* ~250 Hz */
  }
  SDL_CloseJoystick(joy);
  SDL_DetachVirtualJoystick(id);
  return NULL;
}
