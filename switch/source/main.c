/* main.c -- Phase 1b bring-up: load libSkyNet.so, relocate, resolve, finalize,
 * run its init_array. JNI_OnLoad / SDL_main are NOT called yet. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <malloc.h>
#include <sys/stat.h>
#include <switch.h>
#include "config.h"
#include "so_util.h"
#include "util.h"
#include "imports.h"
#include "jni_fake.h"

#include <pthread.h>

extern void shim_tls_install(void);

/* SDL_main (the game's managed entry point) runs on its own thread, like the
 * SDLThread on Android, with a big stack. */
static uintptr_t g_sdl_main;
static volatile int g_sdl_done;
static void *sdl_main_thread(void *arg) {
  (void)arg;
  shim_tls_install();
  char *argv[] = { "skynet", "/switch/skynet_sdl_nx", "/switch/skynet_sdl_nx/GAMEDATA", NULL };
  debugPrintf("== SDL_main thread: calling SDL_main ==\n");
  int rc = ((int (*)(int, char **))g_sdl_main)(3, argv);
  debugPrintf("== SDL_main returned %d ==\n", rc);
  g_sdl_done = 1;
  return NULL;
}
static so_module skynet_mod;
so_module *g_skynet_mod = &skynet_mod;

/* Take (almost) all the RAM the process was given: the GC heap and the 42 MB
 * LOAD zone both live in the newlib heap. */
void __libnx_initheap(void) {
  extern char *fake_heap_start, *fake_heap_end;
  void *addr; size_t size = 0;
  if (envHasHeapOverride()) { addr = envGetHeapOverrideAddr(); size = envGetHeapOverrideSize(); }
  else {
    u64 total = 0, used = 0;
    svcGetInfo(&total, InfoType_TotalMemorySize, CUR_PROCESS_HANDLE, 0);
    svcGetInfo(&used, InfoType_UsedMemorySize, CUR_PROCESS_HANDLE, 0);
    if (total > used + 0x200000) size = (total - used - 0x200000) & ~0x1FFFFFull;
    if (!size) size = 0x20000000;
    if (R_FAILED(svcSetHeapSize(&addr, size))) diagAbortWithResult(MAKERESULT(Module_Libnx, LibnxError_HeapAllocFailed));
  }
  fake_heap_start = addr; fake_heap_end = (char *)addr + size;
}

static void set_runtime_env(void) {
  mkdir(APP_DIR "/tmp", 0777); mkdir(APP_DIR "/home", 0777);
  setenv("TMPDIR", "/switch/skynet_sdl_nx/tmp", 1);
  setenv("HOME", "/switch/skynet_sdl_nx/home", 1);
  setenv("DOTNET_GCHeapHardLimit", "10000000", 1);   /* hex: 256 MB */
  setenv("DOTNET_GCRegionRange", "14000000", 1);     /* hex: 320 MB, bounds the upfront reserve */
  setenv("DOTNET_GCgen0size", "400000", 1);
  setenv("DOTNET_PROCESSOR_COUNT", "3", 1);
  setenv("DOTNET_gcConcurrent", "0", 1);             /* no pthread_kill-based suspension */
  setenv("DOTNET_gcServer", "0", 1);
  setenv("DOTNET_EnableWriteXorExecute", "0", 1);
  setenv("DOTNET_SYSTEM_TIMEZONE_INVARIANT", "1", 1);
  setenv("DOTNET_SYSTEM_GLOBALIZATION_INVARIANT", "1", 1);
}

int main(int argc, char **argv) {
  consoleInit(NULL);
  socketInitializeDefault();
  nxlinkStdio();
  mkdir(APP_DIR, 0777);
  util_log_open();
  debugPrintf("== skynet_sdl_nx phase 1c ==\n");
  {
    u64 total = 0, used = 0;
    svcGetInfo(&total, InfoType_TotalMemorySize, CUR_PROCESS_HANDLE, 0);
    svcGetInfo(&used, InfoType_UsedMemorySize, CUR_PROCESS_HANDLE, 0);
    struct mallinfo mi = mallinfo();
    debugPrintf("[mem] total=%llu MB used=%llu MB newlib arena=%u MB\n",
                (unsigned long long)(total >> 20), (unsigned long long)(used >> 20), (unsigned)(mi.arena >> 20));
  }

  FILE *f = fopen(SKYNET_SO, "rb");
  if (!f) { debugPrintf("!! %s not found\n", SKYNET_SO); goto done; }
  fclose(f);

  shim_tls_install();
  set_runtime_env();

  size_t slice = 0x3000000;
  void *base = memalign(0x1000, slice);
  debugPrintf("heap base %p\n", base);
  int res = so_load(&skynet_mod, SKYNET_SO, base, slice);
  if (res < 0) { debugPrintf("!! so_load failed (%d)\n", res); goto done; }
  debugPrintf("so_load ok: load_size=%u KB\n", (unsigned)(skynet_mod.load_size >> 10));
  so_relocate(&skynet_mod);
  so_resolve(&skynet_mod, skynet_libc_imports, skynet_libc_imports_count, 1);

  uintptr_t e_main = so_try_find_addr_rx(&skynet_mod, "SDL_main");
  uintptr_t e_onload = so_try_find_addr_rx(&skynet_mod, "JNI_OnLoad");
  uintptr_t e_init = so_try_find_addr_rx(&skynet_mod, "Java_net_dot_jni_nativeaot_JavaInteropRuntime_init");
  debugPrintf("export SDL_main=%p JNI_OnLoad=%p JavaInteropRuntime_init=%p\n", (void *)e_main, (void *)e_onload, (void *)e_init);

  debugPrintf("== so_finalize ==\n");
  so_finalize(&skynet_mod);
  so_flush_caches(&skynet_mod);
  debugPrintf("== running init_array ==\n");
  so_execute_init_array(&skynet_mod);
  debugPrintf("== init_array returned OK ==\n");
  if (e_onload) {
    debugPrintf("== calling JNI_OnLoad ==\n");
    int (*onload)(void *, void *) = (int (*)(void *, void *))e_onload;
    int ver = onload(jni_fake_vm(), NULL);
    debugPrintf("== JNI_OnLoad returned %#x ==\n", ver);
  }
  if (e_init) {
    /* Java_net_dot_jni_nativeaot_JavaInteropRuntime_init(env, cls, x2..x5): the
     * extra args are unknown (loader/context-like); pass distinct fake handles. */
    debugPrintf("== calling JavaInteropRuntime_init ==\n");
    void (*init)(void *, intptr_t, intptr_t, intptr_t, intptr_t, intptr_t) = (void *)e_init;
    init(jni_fake_env(), jni_fake_handle("class", "net/dot/jni/nativeaot/JavaInteropRuntime"),
         jni_fake_handle("obj", "ClassLoader"), (intptr_t)"/switch/skynet_sdl_nx/tmp",
         (intptr_t)"/switch/skynet_sdl_nx/home", (intptr_t)"/switch/skynet_sdl_nx");
    debugPrintf("== JavaInteropRuntime_init returned ==\n");
  }
  if (e_main) {
    g_sdl_main = e_main;
    setenv("SKYNET_DATA", "/switch/skynet_sdl_nx/GAMEDATA", 1);
    /* libnx's console owns the default NWindow; SDL needs it for its own window
     * (nwindowSetDimensions fails with 0xf59 otherwise). Log goes on to the SD file. */
    debugPrintf("== releasing the text console for SDL ==\n");
    debugFlush();
    util_console_active(0);
    consoleExit(NULL);
    consoleDebugInit(debugDevice_NULL);
    pthread_attr_t at; pthread_attr_init(&at);
    pthread_attr_setstacksize(&at, 16u << 20);
    pthread_t th;
    int prc = pthread_create(&th, &at, sdl_main_thread, NULL);
    debugPrintf("== SDL_main thread started rc=%d ==\n", prc);
    while (appletMainLoop() && !g_sdl_done) svcSleepThread(50000000ULL);
    /* the game released the window (or died): bring the text console back */
    consoleInit(NULL);
    util_console_active(1);
    debugPrintf("== game ended; log is on the SD; press + to quit ==\n");
  }

done:
  debugFlush();
  PadState pad; padConfigureInput(1, HidNpadStyleSet_NpadStandard); padInitializeDefault(&pad);
  while (appletMainLoop()) {
    padUpdate(&pad);
    if (padGetButtonsDown(&pad) & HidNpadButton_Plus) break;
    consoleUpdate(NULL);
  }
  consoleExit(NULL);
  return 0;
}
