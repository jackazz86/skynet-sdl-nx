/* sdl_compat.c -- SDL 3.3/3.4 entry points the game's bindings (SDL3-CS for a newer SDL
 * than the native Switch build, 3.2.27) call, implemented on top of the native library.
 * Each name here must also be listed in tools/sdl_compat.txt so gen_sdl.sh puts it in the
 * dlsym table. Missing-in-native names not listed there make the game die with
 * EntryPointNotFoundException the first time it calls them (see skynet_debug.log). */
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <time.h>
#include <stdio.h>

typedef uint32_t SDL_PropertiesID;
extern SDL_PropertiesID SDL_CreateProperties(void);
extern bool SDL_SetStringProperty(SDL_PropertiesID props, const char *name, const char *value);
extern const char *SDL_GetGPUDeviceDriver(void *device);
extern const char *SDL_GetError(void);

/* SDL_GetGPUDeviceProperties: SDL.gpu.device.{name,driver_name,driver_version,driver_info} */
SDL_PropertiesID SDL_GetGPUDeviceProperties(void *device) {
  static void *cached_dev;
  static SDL_PropertiesID cached;
  if (cached && cached_dev == device) return cached;
  SDL_PropertiesID p = SDL_CreateProperties();
  if (!p) return 0;
  const char *drv = SDL_GetGPUDeviceDriver(device);
  SDL_SetStringProperty(p, "SDL.gpu.device.name", "NVIDIA Tegra X1 (NVK, Nintendo Switch)");
  SDL_SetStringProperty(p, "SDL.gpu.device.driver_name", drv ? drv : "vulkan");
  SDL_SetStringProperty(p, "SDL.gpu.device.driver_version", "Mesa NVK");
  SDL_SetStringProperty(p, "SDL.gpu.device.driver_info", "Mesa NVK on Horizon");
  cached_dev = device; cached = p;
  return p;
}

bool SDL_SetWindowProgressState(void *window, int state) { (void)window; (void)state; return true; }
bool SDL_SetWindowProgressValue(void *window, float value) { (void)window; (void)value; return true; }
int SDL_GetWindowProgressState(void *window) { (void)window; return 0; }
float SDL_GetWindowProgressValue(void *window) { (void)window; return 0.0f; }
bool SDL_SetWindowFillDocument(void *window, bool fill) { (void)window; (void)fill; return true; }
bool SDL_HasSVE2(void) { return false; }
int SDL_GetSystemPageSize(void) { return 4096; }
bool SDL_TryLockJoysticks(void) { extern void SDL_LockJoysticks(void); SDL_LockJoysticks(); return true; }
bool SDL_SetRelativeMouseTransform(void *cb, void *ud) { (void)cb; (void)ud; return true; }
bool SDL_GetCurrentTime(int64_t *ticks) {
  if (!ticks) return false;
  *ticks = (int64_t)time(NULL) * 1000000000ll; return true;
}
