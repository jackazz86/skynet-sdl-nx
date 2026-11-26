#ifndef JNI_OBJ_H
#define JNI_OBJ_H
#include <stdint.h>

/* Native-backed Java objects for the fake JNI: primitive arrays, MessageDigest,
 * Mac, SecretKeySpec, SecureRandom. Handles of these are pointers to a struct
 * with a magic number (distinct from the small integer handles of jni_fake.c). */

/* Instance/static method call. cls_or_recv: receiver (instance) or class handle (static).
 * mname: method name only. Returns the result; *handled = 0 if the method is not ours. */
intptr_t jobj_call(int is_static, const char *cname, intptr_t recv, const char *mname,
                   intptr_t a0, intptr_t a1, intptr_t a2, int *handled);
/* Constructor. */
intptr_t jobj_new(const char *cname, intptr_t a0, intptr_t a1, intptr_t a2, int *handled);
/* Registers New<T>Array, Get/Set/Release array functions through set(name, fn). */
void jobj_register(void (*set)(const char *name, void *fn));
#endif
