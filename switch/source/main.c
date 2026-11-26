/* main.c -- Phase 1 bring-up: load libSkyNet.so, relocate, resolve, report.
 * Does NOT run init_array / JNI_OnLoad yet. */
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

static so_module skynet_mod;

int main(int argc, char **argv) {
  consoleInit(NULL);
  socketInitializeDefault();
  nxlinkStdio();
  mkdir(APP_DIR, 0777);
  util_log_open();
  debugPrintf("== skynet_nx phase1 ==\n");

  FILE *f = fopen(SKYNET_SO, "rb");
  if (!f) { debugPrintf("!! %s not found\n", SKYNET_SO); goto done; }
  fclose(f);

  size_t slice = 0x3000000; // 48 MB; LOAD zone is ~42 MB
  void *base = memalign(0x1000, slice);
  debugPrintf("heap base %p\n", base);
  int res = so_load(&skynet_mod, SKYNET_SO, base, slice);
  if (res < 0) { debugPrintf("!! so_load failed (%d)\n", res); goto done; }
  debugPrintf("so_load ok: load_size=%u KB\n", (unsigned)(skynet_mod.load_size >> 10));
  so_relocate(&skynet_mod);
  so_resolve(&skynet_mod, skynet_libc_imports, skynet_libc_imports_count, 0);
  const char *exp[] = { "SDL_main", "JNI_OnLoad", "Java_net_dot_jni_nativeaot_JavaInteropRuntime_init" };
  for (int i = 0; i < 3; i++)
    debugPrintf("export %s = %p\n", exp[i], (void *)so_try_find_addr_rx(&skynet_mod, exp[i]));
  debugPrintf("== phase1 done (finalize/init not run) ==\n");

done:
  debugFlush();
  while (appletMainLoop()) {
    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    PadState pad; padInitializeDefault(&pad); padUpdate(&pad);
    if (padGetButtonsDown(&pad) & HidNpadButton_Plus) break;
    consoleUpdate(NULL);
  }
  consoleExit(NULL);
  return 0;
}
