#ifndef __UTIL_H__
#define __UTIL_H__
#include <stdint.h>
int debugPrintf(const char *text, ...);
void debugFlush(void);
void util_log_open(void);
void util_console_active(int on);
#endif
