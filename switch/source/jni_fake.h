#ifndef JNI_FAKE_H
#define JNI_FAKE_H
void *jni_fake_vm(void);
void *jni_fake_env(void);
#include <stdint.h>
intptr_t jni_fake_handle(const char *kind, const char *name);
#endif
