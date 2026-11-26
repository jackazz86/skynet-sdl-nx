/* util.c -- logging (stdout/nxlink + SD log file) and fatal_error. */
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <switch.h>
#include "config.h"
#include "util.h"
#include "error.h"

static FILE *g_log;

void util_log_open(void) { g_log = fopen(LOG_PATH, "w"); }
void debugFlush(void) { if (g_log) fflush(g_log); fflush(stdout); }

int debugPrintf(const char *fmt, ...) {
  char buf[1024];
  va_list va;
  va_start(va, fmt);
  int n = vsnprintf(buf, sizeof buf, fmt, va);
  va_end(va);
  fputs(buf, stdout);
  if (g_log) { fputs(buf, g_log); fflush(g_log); }
  return n;
}

void fatal_error(const char *fmt, ...) {
  char buf[512];
  va_list va;
  va_start(va, fmt);
  vsnprintf(buf, sizeof buf, fmt, va);
  va_end(va);
  debugPrintf("FATAL: %s\n", buf);
  debugFlush();
  consoleUpdate(NULL);
  svcSleepThread(10ULL * 1000000000ULL);
  exit(1);
}
