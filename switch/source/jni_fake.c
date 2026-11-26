/* jni_fake.c -- a fake JavaVM/JNIEnv for libSkyNet.so (NativeAOT .NET-for-Android).
 * Every JNI call is logged; only the handful the runtime needs return real values.
 * Unhandled functions return 0 and say so, so the log is the to-do list. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "util.h"
#include "jni_fake.h"

#define LOG_CAP 4000
static int g_logged;
static intptr_t jni_unhandled(int idx);
#include "jni_gen.inc"

static intptr_t jni_unhandled(int idx) {
  if (g_logged++ < LOG_CAP) debugPrintf("[jni] UNHANDLED %s\n", jni_names[idx]);
  return 0;
}

/* handles: unique non-null integers, with a name table for the log */
#define MAX_H 4096
static char *h_name[MAX_H];
static int h_count = 1;
static intptr_t new_handle(const char *kind, const char *name) {
  int h = h_count++ % MAX_H;
  if (h == 0) h = h_count++ % MAX_H;
  free(h_name[h]);
  char b[256];
  snprintf(b, sizeof b, "%s:%s", kind, name ? name : "");
  h_name[h] = strdup(b);
  return (intptr_t)(0x10000 + h * 8);
}
static const char *hn(intptr_t h) {
  int i = (int)((h - 0x10000) / 8);
  return (i > 0 && i < MAX_H && h_name[i]) ? h_name[i] : "?";
}

static intptr_t f_GetVersion(void *env) { (void)env; return 0x00010006; }
static intptr_t f_FindClass(void *env, const char *n) {
  (void)env; intptr_t h = new_handle("class", n);
  debugPrintf("[jni] FindClass(%s) -> %#lx\n", n, (long)h); return h;
}
static intptr_t f_GetMethodID(void *env, intptr_t c, const char *n, const char *s) {
  (void)env; intptr_t h = new_handle("method", n);
  debugPrintf("[jni] GetMethodID(%s, %s %s) -> %#lx\n", hn(c), n, s, (long)h); return h;
}
static intptr_t f_GetStaticMethodID(void *env, intptr_t c, const char *n, const char *s) {
  (void)env; intptr_t h = new_handle("smethod", n);
  debugPrintf("[jni] GetStaticMethodID(%s, %s %s) -> %#lx\n", hn(c), n, s, (long)h); return h;
}
static intptr_t f_GetFieldID(void *env, intptr_t c, const char *n, const char *s) {
  (void)env; intptr_t h = new_handle("field", n);
  debugPrintf("[jni] GetFieldID(%s, %s %s) -> %#lx\n", hn(c), n, s, (long)h); return h;
}
static intptr_t f_GetStaticFieldID(void *env, intptr_t c, const char *n, const char *s) {
  (void)env; intptr_t h = new_handle("sfield", n);
  debugPrintf("[jni] GetStaticFieldID(%s, %s %s) -> %#lx\n", hn(c), n, s, (long)h); return h;
}
static intptr_t f_Ref(void *env, intptr_t o) { (void)env; return o; }
static intptr_t f_Nop(void *env, intptr_t o) { (void)env; (void)o; return 0; }
static intptr_t f_Zero(void) { return 0; }
static intptr_t f_GetObjectClass(void *env, intptr_t o) { (void)env; return new_handle("class-of", hn(o)); }
static intptr_t f_NewStringUTF(void *env, const char *s) { (void)env; return (intptr_t)strdup(s ? s : ""); }
static int is_handle(intptr_t h) { return h >= 0x10000 && h < 0x10000 + MAX_H * 8; }
static intptr_t f_GetStringUTFChars(void *env, const char *s, uint8_t *copy) {
  (void)env; if (copy) *copy = 0;
  return (intptr_t)(is_handle((intptr_t)s) ? "" : s);
}
static intptr_t f_GetStringUTFLength(void *env, const char *s) { (void)env; return is_handle((intptr_t)s) ? 0 : (intptr_t)strlen(s); }

/* Method calls: log, and return a fresh non-null object for Object-returning
 * methods (the runtime mostly just needs "not null"), 0 for everything else. */
static intptr_t call_obj(void *env, intptr_t o, intptr_t mid) {
  (void)env; (void)o; intptr_t h = new_handle("ret", hn(mid));
  debugPrintf("[jni] Call*Object %s on %s -> %#lx\n", hn(mid), hn(o), (long)h); return h;
}
static intptr_t call_zero(void *env, intptr_t o, intptr_t mid) {
  (void)env; debugPrintf("[jni] Call* %s on %s -> 0\n", hn(mid), hn(o)); return 0;
}
static intptr_t callnv_obj(void *env, intptr_t o, intptr_t cls, intptr_t mid) { (void)cls; return call_obj(env, o, mid); }
static intptr_t callnv_zero(void *env, intptr_t o, intptr_t cls, intptr_t mid) { (void)cls; return call_zero(env, o, mid); }
static intptr_t f_NewObject(void *env, intptr_t cls, intptr_t mid) {
  (void)env; intptr_t h = new_handle("obj", hn(cls));
  debugPrintf("[jni] NewObject(%s, %s) -> %#lx\n", hn(cls), hn(mid), (long)h); return h;
}
static intptr_t f_GetObjectField(void *env, intptr_t o, intptr_t fid) {
  (void)env; intptr_t h = new_handle("fld", hn(fid));
  debugPrintf("[jni] GetObjectField(%s, %s) -> %#lx\n", hn(o), hn(fid), (long)h); return h;
}
static intptr_t f_GetPrimField(void *env, intptr_t o, intptr_t fid) {
  (void)env; debugPrintf("[jni] Get<prim>Field(%s, %s) -> 0\n", hn(o), hn(fid)); return 0;
}
static intptr_t f_GetObjectRefType(void *env, intptr_t o) { (void)env; (void)o; return 1; /* JNILocalRefType */ }
static intptr_t f_IsSameObject(void *env, intptr_t a, intptr_t b) { (void)env; return a == b; }
static intptr_t f_NewObjectArray(void *env, int n, intptr_t cls, intptr_t init) {
  (void)env; (void)init; intptr_t h = new_handle("array", hn(cls));
  debugPrintf("[jni] NewObjectArray(%d, %s) -> %#lx\n", n, hn(cls), (long)h); return h;
}
static intptr_t f_RegisterNatives(void *env, intptr_t c, void **m, int n) {
  (void)env;
  debugPrintf("[jni] RegisterNatives(%s, %d)\n", hn(c), n);
  for (int i = 0; i < n; i++)
    debugPrintf("[jni]   %s %s -> %p\n", (const char *)m[i * 3], (const char *)m[i * 3 + 1], m[i * 3 + 2]);
  return 0;
}
static intptr_t f_ThrowNew(void *env, intptr_t c, const char *m) {
  (void)env; debugPrintf("[jni] ThrowNew(%s, %s)\n", hn(c), m); return 0;
}
static intptr_t f_FatalError(void *env, const char *m) {
  (void)env; debugPrintf("[jni] FatalError: %s\n", m); debugFlush(); abort();
}

static void *g_vm_fns[8], *g_env_fns[JNI_SLOTS];
static void *g_vm_ptr = g_vm_fns, *g_env_ptr = g_env_fns;   /* JavaVM = &g_vm_ptr, JNIEnv = &g_env_ptr */

static int vm_GetEnv(void *vm, void **penv, int ver) {
  (void)vm; debugPrintf("[jni] GetEnv(ver=%#x)\n", ver); *penv = &g_env_ptr; return 0;
}
static int vm_Attach(void *vm, void **penv, void *args) {
  (void)vm; (void)args; debugPrintf("[jni] AttachCurrentThread\n"); *penv = &g_env_ptr; return 0;
}
static int vm_Detach(void *vm) { (void)vm; return 0; }
static intptr_t f_GetJavaVM(void *env, void **vm) { (void)env; *vm = &g_vm_ptr; return 0; }

static void set(const char *name, void *fn) {
  for (int i = 0; i < JNI_SLOTS; i++)
    if (jni_names[i][0] && !strcmp(jni_names[i], name)) { jni_table[i] = fn; return; }
  debugPrintf("[jni] internal: no slot %s\n", name);
}

void *jni_fake_vm(void) {
  set("GetVersion", f_GetVersion); set("FindClass", f_FindClass);
  set("GetMethodID", f_GetMethodID); set("GetStaticMethodID", f_GetStaticMethodID);
  set("GetFieldID", f_GetFieldID); set("GetStaticFieldID", f_GetStaticFieldID);
  set("NewGlobalRef", f_Ref); set("NewLocalRef", f_Ref); set("NewWeakGlobalRef", f_Ref);
  set("DeleteGlobalRef", f_Nop); set("DeleteLocalRef", f_Nop); set("DeleteWeakGlobalRef", f_Nop);
  set("ExceptionCheck", f_Zero); set("ExceptionOccurred", f_Zero); set("ExceptionClear", f_Zero);
  set("EnsureLocalCapacity", f_Zero); set("PushLocalFrame", f_Zero); set("PopLocalFrame", f_Ref);
  set("GetObjectClass", f_GetObjectClass); set("NewStringUTF", f_NewStringUTF);
  set("GetStringUTFChars", f_GetStringUTFChars); set("ReleaseStringUTFChars", f_Nop);
  set("GetStringUTFLength", f_GetStringUTFLength); set("RegisterNatives", f_RegisterNatives);
  {
    static const char *const T[] = { "Object", "Boolean", "Byte", "Char", "Short", "Int", "Long", "Float", "Double", "Void" };
    static const char *const S[] = { "", "V", "A" };
    char nm[64];
    for (int t = 0; t < 10; t++) for (int s = 0; s < 3; s++) {
      int obj = (t == 0);
      snprintf(nm, sizeof nm, "Call%sMethod%s", T[t], S[s]); set(nm, obj ? (void *)call_obj : (void *)call_zero);
      snprintf(nm, sizeof nm, "CallStatic%sMethod%s", T[t], S[s]); set(nm, obj ? (void *)call_obj : (void *)call_zero);
      snprintf(nm, sizeof nm, "CallNonvirtual%sMethod%s", T[t], S[s]); set(nm, obj ? (void *)callnv_obj : (void *)callnv_zero);
    }
    for (int t = 1; t < 9; t++) {
      snprintf(nm, sizeof nm, "Get%sField", T[t]); set(nm, f_GetPrimField);
      snprintf(nm, sizeof nm, "GetStatic%sField", T[t]); set(nm, f_GetPrimField);
    }
    set("GetObjectField", f_GetObjectField); set("GetStaticObjectField", f_GetObjectField);
    set("NewObject", f_NewObject); set("NewObjectV", f_NewObject); set("NewObjectA", f_NewObject);
    set("GetObjectRefType", f_GetObjectRefType);
    set("IsSameObject", f_IsSameObject); set("NewObjectArray", f_NewObjectArray);
  }
  set("ThrowNew", f_ThrowNew); set("FatalError", f_FatalError); set("GetJavaVM", f_GetJavaVM);
  memcpy(g_env_fns, jni_table, sizeof g_env_fns);
  g_vm_fns[4] = vm_Attach; g_vm_fns[5] = vm_Detach; g_vm_fns[6] = vm_GetEnv; g_vm_fns[7] = vm_Attach;
  return &g_vm_ptr;
}
void *jni_fake_env(void) { return &g_env_ptr; }
intptr_t jni_fake_handle(const char *kind, const char *name) { return new_handle(kind, name); }
