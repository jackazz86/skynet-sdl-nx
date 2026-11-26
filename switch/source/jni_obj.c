/* jni_obj.c -- native implementations of the Java classes .NET-for-Android's crypto
 * shim (System.Security.Cryptography.Native.Android) calls through JNI. mbedtls 2.28
 * provides the digests and HMAC; csrng the randomness. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <switch.h>
#include "mbedtls/md.h"
#include "util.h"
#include "jni_obj.h"

#define JOBJ_MAGIC 0x4A4F424Au
enum { K_ARRAY = 1, K_MD, K_MAC, K_KEY, K_RNG };

typedef struct jobj {
  uint32_t magic; int kind;
  int esz, len; uint8_t *data;               /* arrays, key bytes */
  mbedtls_md_context_t md; const mbedtls_md_info_t *info; int hmac;
} jobj_t;

static int is_obj(intptr_t h) {
  return h > 0x1000000 && !(h & 7) && ((jobj_t *)h)->magic == JOBJ_MAGIC;
}
static jobj_t *mk(int kind) { jobj_t *o = calloc(1, sizeof *o); o->magic = JOBJ_MAGIC; o->kind = kind; return o; }
static jobj_t *as(intptr_t h, int kind) { return (is_obj(h) && ((jobj_t *)h)->kind == kind) ? (jobj_t *)h : NULL; }

/* ---- arrays ---- */
static intptr_t new_array(int esz, int len) {
  jobj_t *a = mk(K_ARRAY); a->esz = esz; a->len = len; a->data = calloc((size_t)(len > 0 ? len : 1), (size_t)esz);
  return (intptr_t)a;
}
#define NEWARR(T, SZ) static intptr_t na_##T(void *env, int len) { (void)env; return new_array(SZ, len); }
NEWARR(Boolean, 1) NEWARR(Byte, 1) NEWARR(Char, 2) NEWARR(Short, 2) NEWARR(Int, 4) NEWARR(Long, 8) NEWARR(Float, 4) NEWARR(Double, 8)
static intptr_t arr_len(void *env, intptr_t a) { (void)env; jobj_t *o = as(a, K_ARRAY); return o ? o->len : 0; }
static intptr_t arr_elems(void *env, intptr_t a, uint8_t *copy) { (void)env; if (copy) *copy = 0; jobj_t *o = as(a, K_ARRAY); return o ? (intptr_t)o->data : 0; }
static intptr_t arr_release(void *env, intptr_t a, void *p, int mode) { (void)env; (void)a; (void)p; (void)mode; return 0; }
static intptr_t arr_get_region(void *env, intptr_t a, int start, int len, void *buf) {
  (void)env; jobj_t *o = as(a, K_ARRAY); if (!o || start < 0 || len < 0 || start + len > o->len) return 0;
  memcpy(buf, o->data + (size_t)start * o->esz, (size_t)len * o->esz); return 0;
}
static intptr_t arr_set_region(void *env, intptr_t a, int start, int len, const void *buf) {
  (void)env; jobj_t *o = as(a, K_ARRAY); if (!o || start < 0 || len < 0 || start + len > o->len) return 0;
  memcpy(o->data + (size_t)start * o->esz, buf, (size_t)len * o->esz); return 0;
}

void jobj_register(void (*set)(const char *, void *)) {
  static const char *const T[] = { "Boolean", "Byte", "Char", "Short", "Int", "Long", "Float", "Double" };
  static void *const NEWF[] = { na_Boolean, na_Byte, na_Char, na_Short, na_Int, na_Long, na_Float, na_Double };
  char nm[64];
  for (int i = 0; i < 8; i++) {
    snprintf(nm, sizeof nm, "New%sArray", T[i]); set(nm, NEWF[i]);
    snprintf(nm, sizeof nm, "Get%sArrayElements", T[i]); set(nm, arr_elems);
    snprintf(nm, sizeof nm, "Release%sArrayElements", T[i]); set(nm, arr_release);
    snprintf(nm, sizeof nm, "Get%sArrayRegion", T[i]); set(nm, arr_get_region);
    snprintf(nm, sizeof nm, "Set%sArrayRegion", T[i]); set(nm, arr_set_region);
  }
  set("GetArrayLength", arr_len);
  set("GetPrimitiveArrayCritical", arr_elems);
  set("ReleasePrimitiveArrayCritical", arr_release);
}

/* ---- digests / HMAC ---- */
static const mbedtls_md_info_t *info_for(const char *java_name, int *is_hmac) {
  char n[32]; size_t j = 0; *is_hmac = 0;
  if (!java_name) return NULL;
  if (!strncasecmp(java_name, "Hmac", 4)) { *is_hmac = 1; java_name += 4; }
  for (; *java_name && j < sizeof n - 1; java_name++) if (*java_name != '-') n[j++] = *java_name;
  n[j] = 0;
  return mbedtls_md_info_from_string(n);
}
static jobj_t *new_md(int kind, const mbedtls_md_info_t *info, int hmac) {
  jobj_t *o = mk(kind); o->info = info; o->hmac = hmac;
  mbedtls_md_init(&o->md);
  mbedtls_md_setup(&o->md, info, hmac);
  if (!hmac) mbedtls_md_starts(&o->md);
  return o;
}
static intptr_t bytes_to_array(const uint8_t *p, int n) {
  intptr_t a = new_array(1, n); memcpy(((jobj_t *)a)->data, p, (size_t)n); return a;
}
static const char *jstr(intptr_t s) { return (s > 0x1000000) ? (const char *)s : ""; }

static intptr_t md_call(jobj_t *o, const char *m, intptr_t a0, int *ok) {
  *ok = 1;
  if (!strcmp(m, "update")) {
    jobj_t *arr = as(a0, K_ARRAY);
    if (arr) { if (o->kind == K_MD) mbedtls_md_update(&o->md, arr->data, (size_t)arr->len);
               else mbedtls_md_hmac_update(&o->md, arr->data, (size_t)arr->len); }
    return 0;
  }
  if (!strcmp(m, "digest") || !strcmp(m, "doFinal")) {
    jobj_t *arr = as(a0, K_ARRAY);
    if (arr) mbedtls_md_update(&o->md, arr->data, (size_t)arr->len);
    uint8_t out[64]; int n = mbedtls_md_get_size(o->info);
    if (o->kind == K_MD) { mbedtls_md_finish(&o->md, out); mbedtls_md_starts(&o->md); }
    else { mbedtls_md_hmac_finish(&o->md, out); mbedtls_md_hmac_reset(&o->md); }
    return bytes_to_array(out, n);
  }
  if (!strcmp(m, "reset")) { if (o->kind == K_MD) mbedtls_md_starts(&o->md); else mbedtls_md_hmac_reset(&o->md); return 0; }
  if (!strcmp(m, "clone")) {
    jobj_t *n = new_md(o->kind, o->info, o->hmac);
    mbedtls_md_clone(&n->md, &o->md); return (intptr_t)n;
  }
  if (!strcmp(m, "init")) {           /* Mac.init(Key) */
    jobj_t *k = as(a0, K_KEY);
    if (k) mbedtls_md_hmac_starts(&o->md, k->data, (size_t)k->len);
    return 0;
  }
  if (!strcmp(m, "getDigestLength") || !strcmp(m, "getMacLength")) return mbedtls_md_get_size(o->info);
  *ok = 0; return 0;
}

intptr_t jobj_call(int is_static, const char *cname, intptr_t recv, const char *mname,
                   intptr_t a0, intptr_t a1, intptr_t a2, int *handled) {
  (void)a1; (void)a2;
  *handled = 1;
  if (is_static) {
    if (!strcmp(mname, "getInstance") && cname) {
      int hm; const mbedtls_md_info_t *inf;
      if (strstr(cname, "java/security/MessageDigest")) {
        inf = info_for(jstr(a0), &hm);
        if (!inf) { debugPrintf("[jni] MessageDigest.getInstance(%s): unsupported\n", jstr(a0)); return 0; }
        debugPrintf("[jni] MessageDigest.getInstance(%s)\n", jstr(a0));
        return (intptr_t)new_md(K_MD, inf, 0);
      }
      if (strstr(cname, "javax/crypto/Mac")) {
        inf = info_for(jstr(a0), &hm);
        if (!inf) { debugPrintf("[jni] Mac.getInstance(%s): unsupported\n", jstr(a0)); return 0; }
        debugPrintf("[jni] Mac.getInstance(%s)\n", jstr(a0));
        return (intptr_t)new_md(K_MAC, inf, 1);
      }
    }
    *handled = 0; return 0;
  }
  jobj_t *o = is_obj(recv) ? (jobj_t *)recv : NULL;
  if (o && (o->kind == K_MD || o->kind == K_MAC)) {
    int ok; intptr_t r = md_call(o, mname, a0, &ok);
    if (ok) return r;
  }
  if (o && o->kind == K_RNG && (!strcmp(mname, "nextBytes") || !strcmp(mname, "generateSeed"))) {
    jobj_t *arr = as(a0, K_ARRAY);
    if (arr) csrngGetRandomBytes(arr->data, (size_t)arr->len);
    return 0;
  }
  *handled = 0; return 0;
}

intptr_t jobj_new(const char *cname, intptr_t a0, intptr_t a1, intptr_t a2, int *handled) {
  (void)a1; (void)a2;
  *handled = 1;
  if (cname && strstr(cname, "javax/crypto/spec/SecretKeySpec")) {
    jobj_t *k = mk(K_KEY), *arr = as(a0, K_ARRAY);
    if (arr) { k->len = arr->len; k->data = malloc((size_t)(arr->len ? arr->len : 1)); memcpy(k->data, arr->data, (size_t)arr->len); }
    return (intptr_t)k;
  }
  if (cname && strstr(cname, "java/security/SecureRandom")) return (intptr_t)mk(K_RNG);
  *handled = 0; return 0;
}
