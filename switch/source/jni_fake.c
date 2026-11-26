/* jni_fake.c -- a fake JavaVM/JNIEnv for libSkyNet.so (NativeAOT .NET-for-Android).
 * Every JNI call is logged; only the handful the runtime needs return real values.
 * Unhandled functions return 0 and say so, so the log is the to-do list. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "util.h"
#include <stdarg.h>
#include "jni_fake.h"
#include "jni_obj.h"

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
static const char *after_colon(const char *s) { const char *c = strchr(s, ':'); return c ? c + 1 : s; }

/* core of every Call*Method: native-backed classes first (jni_obj.c), else log + dummy */
static intptr_t core_call(int is_static, int ret_obj, intptr_t recv, intptr_t mid, intptr_t a0, intptr_t a1, intptr_t a2) {
  const char *mname = after_colon(hn(mid));
  int handled = 0;
  intptr_t r = jobj_call(is_static, is_static ? after_colon(hn(recv)) : NULL, recv, mname, a0, a1, a2, &handled);
  if (handled) return r;
  if (ret_obj) {
    intptr_t h = new_handle("ret", hn(mid));
    debugPrintf("[jni] Call*Object %s on %s -> %#lx\n", hn(mid), hn(recv), (long)h); return h;
  }
  debugPrintf("[jni] Call* %s on %s -> 0\n", hn(mid), hn(recv)); return 0;
}
#define MK_CALL(PFX, STATIC, RETOBJ) \
  static intptr_t PFX##_p(void *env, intptr_t o, intptr_t mid, intptr_t a0, intptr_t a1, intptr_t a2) { (void)env; return core_call(STATIC, RETOBJ, o, mid, a0, a1, a2); } \
  static intptr_t PFX##_v(void *env, intptr_t o, intptr_t mid, va_list ap) { (void)env; intptr_t a0 = va_arg(ap, intptr_t), a1 = va_arg(ap, intptr_t), a2 = va_arg(ap, intptr_t); return core_call(STATIC, RETOBJ, o, mid, a0, a1, a2); } \
  static intptr_t PFX##_a(void *env, intptr_t o, intptr_t mid, const intptr_t *a) { static const intptr_t z[3] = {0, 0, 0}; if (!a) a = z; (void)env; return core_call(STATIC, RETOBJ, o, mid, a[0], a[1], a[2]); }
MK_CALL(ci_o, 0, 1) MK_CALL(ci_z, 0, 0) MK_CALL(cs_o, 1, 1) MK_CALL(cs_z, 1, 0)
#define MK_CALLNV(PFX, RETOBJ) \
  static intptr_t PFX##_p(void *env, intptr_t o, intptr_t cls, intptr_t mid, intptr_t a0, intptr_t a1, intptr_t a2) { (void)env; (void)cls; return core_call(0, RETOBJ, o, mid, a0, a1, a2); } \
  static intptr_t PFX##_v(void *env, intptr_t o, intptr_t cls, intptr_t mid, va_list ap) { (void)env; (void)cls; intptr_t a0 = va_arg(ap, intptr_t), a1 = va_arg(ap, intptr_t), a2 = va_arg(ap, intptr_t); return core_call(0, RETOBJ, o, mid, a0, a1, a2); } \
  static intptr_t PFX##_a(void *env, intptr_t o, intptr_t cls, intptr_t mid, const intptr_t *a) { static const intptr_t z[3] = {0, 0, 0}; if (!a) a = z; (void)env; (void)cls; return core_call(0, RETOBJ, o, mid, a[0], a[1], a[2]); }
MK_CALLNV(cn_o, 1) MK_CALLNV(cn_z, 0)

static intptr_t core_new(intptr_t cls, intptr_t mid, intptr_t a0, intptr_t a1, intptr_t a2) {
  int handled = 0;
  intptr_t r = jobj_new(after_colon(hn(cls)), a0, a1, a2, &handled);
  if (handled) return r;
  intptr_t h = new_handle("obj", hn(cls));
  debugPrintf("[jni] NewObject(%s, %s) -> %#lx\n", hn(cls), hn(mid), (long)h); return h;
}
static intptr_t f_NewObject(void *env, intptr_t cls, intptr_t mid, intptr_t a0, intptr_t a1, intptr_t a2) { (void)env; return core_new(cls, mid, a0, a1, a2); }
static intptr_t f_NewObjectV(void *env, intptr_t cls, intptr_t mid, va_list ap) {
  (void)env; intptr_t a0 = va_arg(ap, intptr_t), a1 = va_arg(ap, intptr_t), a2 = va_arg(ap, intptr_t); return core_new(cls, mid, a0, a1, a2);
}
static intptr_t f_NewObjectA(void *env, intptr_t cls, intptr_t mid, const intptr_t *a) { static const intptr_t z[3] = {0, 0, 0}; if (!a) a = z; (void)env; return core_new(cls, mid, a[0], a[1], a[2]); }
static intptr_t f_GetObjectField(void *env, intptr_t o, intptr_t fid) {
  (void)env; intptr_t h = new_handle("fld", hn(fid));
  debugPrintf("[jni] GetObjectField(%s, %s) -> %#lx\n", hn(o), hn(fid), (long)h); return h;
}
static intptr_t f_GetPrimField(void *env, intptr_t o, intptr_t fid) {
  (void)env; debugPrintf("[jni] Get<prim>Field(%s, %s) -> 0\n", hn(o), hn(fid)); return 0;
}
static intptr_t f_AllocObject(void *env, intptr_t cls) {
  (void)env; intptr_t h = new_handle("obj", hn(cls));
  debugPrintf("[jni] AllocObject(%s) -> %#lx\n", hn(cls), (long)h); return h;
}
/* jstring == strdup'd UTF-8 char* in this fake; UTF-16 <-> UTF-8 on the way in/out */
static intptr_t f_NewString(void *env, const uint16_t *c, int n) {
  (void)env; char *b = malloc((size_t)n * 3 + 1); int j = 0;
  for (int i = 0; i < n; i++) {
    uint32_t u = c[i];
    if (u >= 0xD800 && u < 0xDC00 && i + 1 < n) { u = 0x10000 + ((u - 0xD800) << 10) + (c[++i] - 0xDC00); }
    if (u < 0x80) b[j++] = (char)u;
    else if (u < 0x800) { b[j++] = (char)(0xC0 | (u >> 6)); b[j++] = (char)(0x80 | (u & 63)); }
    else if (u < 0x10000) { b[j++] = (char)(0xE0 | (u >> 12)); b[j++] = (char)(0x80 | ((u >> 6) & 63)); b[j++] = (char)(0x80 | (u & 63)); }
    else { b = realloc(b, (size_t)n * 4 + 1); b[j++] = (char)(0xF0 | (u >> 18)); b[j++] = (char)(0x80 | ((u >> 12) & 63)); b[j++] = (char)(0x80 | ((u >> 6) & 63)); b[j++] = (char)(0x80 | (u & 63)); }
  }
  b[j] = 0; return (intptr_t)b;
}
static uint16_t *utf8_to_16(const char *s, int *len) {
  size_t n = strlen(s); uint16_t *o = malloc((n + 1) * 2); int j = 0;
  for (size_t i = 0; i < n;) {
    uint32_t u = (unsigned char)s[i];
    int extra = u >= 0xF0 ? 3 : u >= 0xE0 ? 2 : u >= 0xC0 ? 1 : 0;
    u = extra == 3 ? u & 7 : extra == 2 ? u & 15 : extra == 1 ? u & 31 : u; i++;
    while (extra-- > 0 && i < n) u = (u << 6) | ((unsigned char)s[i++] & 63);
    if (u >= 0x10000) { u -= 0x10000; o[j++] = (uint16_t)(0xD800 + (u >> 10)); o[j++] = (uint16_t)(0xDC00 + (u & 1023)); }
    else o[j++] = (uint16_t)u;
  }
  o[j] = 0; *len = j; return o;
}
static intptr_t f_GetStringChars(void *env, const char *s, uint8_t *copy) {
  (void)env; if (copy) *copy = 1; int l; return (intptr_t)utf8_to_16(is_handle((intptr_t)s) ? "" : s, &l);
}
static intptr_t f_GetStringLength(void *env, const char *s) {
  (void)env; if (is_handle((intptr_t)s)) return 0; int l; free(utf8_to_16(s, &l)); return l;
}
static intptr_t f_ReleaseStringChars(void *env, intptr_t s, void *chars) { (void)env; (void)s; free(chars); return 0; }
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
      void *ci[3] = { obj ? (void *)ci_o_p : (void *)ci_z_p, obj ? (void *)ci_o_v : (void *)ci_z_v, obj ? (void *)ci_o_a : (void *)ci_z_a };
      void *cs[3] = { obj ? (void *)cs_o_p : (void *)cs_z_p, obj ? (void *)cs_o_v : (void *)cs_z_v, obj ? (void *)cs_o_a : (void *)cs_z_a };
      void *cn[3] = { obj ? (void *)cn_o_p : (void *)cn_z_p, obj ? (void *)cn_o_v : (void *)cn_z_v, obj ? (void *)cn_o_a : (void *)cn_z_a };
      snprintf(nm, sizeof nm, "Call%sMethod%s", T[t], S[s]); set(nm, ci[s]);
      snprintf(nm, sizeof nm, "CallStatic%sMethod%s", T[t], S[s]); set(nm, cs[s]);
      snprintf(nm, sizeof nm, "CallNonvirtual%sMethod%s", T[t], S[s]); set(nm, cn[s]);
    }
    jobj_register(set);
    for (int t = 1; t < 9; t++) {
      snprintf(nm, sizeof nm, "Get%sField", T[t]); set(nm, f_GetPrimField);
      snprintf(nm, sizeof nm, "GetStatic%sField", T[t]); set(nm, f_GetPrimField);
    }
    set("GetObjectField", f_GetObjectField); set("GetStaticObjectField", f_GetObjectField);
    set("NewObject", f_NewObject); set("NewObjectV", f_NewObjectV); set("NewObjectA", f_NewObjectA);
    set("GetObjectRefType", f_GetObjectRefType); set("AllocObject", f_AllocObject);
    set("NewString", f_NewString); set("GetStringChars", f_GetStringChars);
    set("GetStringLength", f_GetStringLength); set("ReleaseStringChars", f_ReleaseStringChars);
    set("IsSameObject", f_IsSameObject); set("NewObjectArray", f_NewObjectArray);
  }
  set("ThrowNew", f_ThrowNew); set("FatalError", f_FatalError); set("GetJavaVM", f_GetJavaVM);
  memcpy(g_env_fns, jni_table, sizeof g_env_fns);
  g_vm_fns[4] = vm_Attach; g_vm_fns[5] = vm_Detach; g_vm_fns[6] = vm_GetEnv; g_vm_fns[7] = vm_Attach;
  return &g_vm_ptr;
}
void *jni_fake_env(void) { return &g_env_ptr; }
intptr_t jni_fake_handle(const char *kind, const char *name) { return new_handle(kind, name); }
