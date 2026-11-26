/* shim.c -- bionic -> newlib/libnx translation layer for libSkyNet.so (NativeAOT).
 *
 * Every function named shim_<libc name> is wired into the import table by
 * tools/gen_imports.sh (list: tools/wrapped.txt). Design and the hard-won
 * facts about the NativeAOT runtime on Horizon (malloc-backed mmap, no signals,
 * stop-the-world GC, per-thread tpidr_el0 block) follow sts2_nx (MIT, see
 * LICENSE.sts2_nx). Written from scratch for this port.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <stdint.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <malloc.h>
#include <time.h>
#include <wchar.h>
#include <wctype.h>
#include <locale.h>
#include <dirent.h>
#include <pthread.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <switch.h>
#include "so_util.h"
#include "util.h"

#define SH(ret, name, args) ret shim_##name args

/* ------------------------------------------------------------------ TLS */
/* Bionic code reads its stack-protector canary at tpidr_el0+0x28. tpidr_el0 is
 * free on Horizon (libnx uses tpidrro_el0), so every thread gets a block. */
void shim_tls_install(void) {
  uint8_t *b = memalign(0x1000, 0x1000);
  if (!b) return;
  memset(b, 0, 0x1000);
  *(uint64_t *)(b + 0x28) = 0x0123456789ABCDEFull;
  __asm__ volatile("msr tpidr_el0, %0" : : "r"(b));
}

/* ------------------------------------------------------------ diagnostics */
SH(int, android_log_write, (int prio, const char *tag, const char *msg)) {
  debugPrintf("[log %d] %s: %s\n", prio, tag ? tag : "", msg ? msg : "");
  return 0;
}
SH(int, android_log_print, (int prio, const char *tag, const char *fmt, ...)) {
  char b[768]; va_list v; va_start(v, fmt); vsnprintf(b, sizeof b, fmt, v); va_end(v);
  return shim_android_log_write(prio, tag, b);
}
SH(int, android_log_vprint, (int prio, const char *tag, const char *fmt, va_list v)) {
  char b[768]; vsnprintf(b, sizeof b, fmt, v);
  return shim_android_log_write(prio, tag, b);
}
SH(void, android_set_abort_message, (const char *m)) { debugPrintf("[abort msg] %s\n", m ? m : "(null)"); }
SH(void, assert2, (const char *file, int line, const char *func, const char *msg)) {
  debugPrintf("[assert] %s:%d %s: %s\n", file, line, func, msg);
  debugFlush(); abort();
}
SH(void, stack_chk_fail, (void)) { debugPrintf("[FATAL] stack smashing detected\n"); debugFlush(); abort(); }
SH(void, abort, (void)) { debugPrintf("[abort] called (lr=%p)\n", __builtin_return_address(0)); debugFlush(); abort(); }
SH(void, exit, (int c)) { debugPrintf("[exit] %d\n", c); debugFlush(); exit(c); }
SH(void, _exit, (int c)) { debugPrintf("[_exit] %d\n", c); debugFlush(); _exit(c); }
SH(int, cxa_atexit, (void *f, void *a, void *d)) { (void)f; (void)a; (void)d; return 0; }
SH(void, cxa_finalize, (void *d)) { (void)d; }

/* ------------------------------------------------------------- fortify */
SH(void *, memcpy_chk, (void *d, const void *s, size_t n, size_t dl)) { (void)dl; return memcpy(d, s, n); }
SH(void *, memset_chk, (void *d, int c, size_t n, size_t dl)) { (void)dl; return memset(d, c, n); }
SH(char *, strcpy_chk, (char *d, const char *s, size_t dl)) { (void)dl; return strcpy(d, s); }
SH(char *, strncpy_chk2, (char *d, const char *s, size_t n, size_t dl, size_t sl)) { (void)dl; (void)sl; return strncpy(d, s, n); }
SH(size_t, strlen_chk, (const char *s, size_t sl)) { (void)sl; return strlen(s); }
SH(int, vsnprintf_chk, (char *s, size_t n, int f, size_t sl, const char *fmt, va_list v)) { (void)f; (void)sl; return vsnprintf(s, n, fmt, v); }
SH(ssize_t, read_chk, (int fd, void *b, size_t n, size_t bl)) { (void)bl; return read(fd, b, n); }
SH(mode_t, umask_chk, (mode_t m)) { return m; }

/* --------------------------------------------------------------- time */
static uint64_t mono_ns(void) { return armTicksToNs(armGetSystemTick()); }
SH(int, clock_gettime, (int clk, struct timespec *ts)) {
  if (!ts) { errno = EFAULT; return -1; }
  if (clk == 0 || clk == 8 /*REALTIME_ALARM*/ ) {
    struct timeval tv; gettimeofday(&tv, NULL);
    ts->tv_sec = tv.tv_sec; ts->tv_nsec = tv.tv_usec * 1000; return 0;
  }
  uint64_t n = mono_ns();
  ts->tv_sec = n / 1000000000ull; ts->tv_nsec = n % 1000000000ull; return 0;
}
SH(int, nanosleep, (const struct timespec *r, struct timespec *rem)) {
  if (rem) memset(rem, 0, sizeof *rem);
  if (!r) return 0;
  svcSleepThread((int64_t)r->tv_sec * 1000000000ll + r->tv_nsec); return 0;
}
SH(int, clock_nanosleep, (int clk, int flags, const struct timespec *r, struct timespec *rem)) {
  int64_t ns = (int64_t)r->tv_sec * 1000000000ll + r->tv_nsec;
  if (flags & 1) { struct timespec now; shim_clock_gettime(clk, &now);
    ns -= (int64_t)now.tv_sec * 1000000000ll + now.tv_nsec; }
  if (ns > 0) svcSleepThread(ns);
  if (rem) memset(rem, 0, sizeof *rem);
  return 0;
}

/* ------------------------------------------------------------ pthread */
/* bionic pthread_mutex_t is 40 B of zeroes when statically initialised. We
 * lazily create a newlib object and keep its pointer in the first 8 bytes. */
#define OBJ_MIN 0x10000ull
typedef struct { uint64_t p; uint32_t extra; } lazy_t;

/* bionic sync objects are only 4-byte aligned; LDAR/CAS need 8, so the pointer
 * slot is the first 8-aligned doubleword inside the object. */
static volatile uint64_t *slot_of(void *base) { return (volatile uint64_t *)(((uintptr_t)base + 7) & ~7ull); }
static void *lazy_get(void *base, size_t objsz, void (*init)(void *, uint32_t), uint32_t arg) {
  volatile uint64_t *slot = slot_of(base);
  uint64_t cur = __atomic_load_n(slot, __ATOMIC_ACQUIRE);
  if (cur >= OBJ_MIN) return (void *)cur;
  void *o = calloc(1, objsz);
  init(o, arg);
  uint64_t expect = cur;
  if (__atomic_compare_exchange_n(slot, &expect, (uint64_t)o, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) return o;
  free(o);                       /* lost the race */
  return (void *)expect;
}
static void mtx_init_obj(void *o, uint32_t type) {
  pthread_mutexattr_t a; pthread_mutexattr_init(&a);
  if (type == 1) pthread_mutexattr_settype(&a, PTHREAD_MUTEX_RECURSIVE);
  pthread_mutex_init(o, &a); pthread_mutexattr_destroy(&a);
}
static pthread_mutex_t *mtx(void *bm) {
  uint32_t type = 0;
  if (*slot_of(bm) < OBJ_MIN) { uint32_t fl = *(volatile uint32_t *)bm & 0xC000; type = fl == 0x4000 ? 1 : 0; }
  return lazy_get(bm, sizeof(pthread_mutex_t), mtx_init_obj, type);
}
SH(int, pthread_mutexattr_init, (int *a)) { *a = 0; return 0; }
SH(int, pthread_mutexattr_destroy, (int *a)) { (void)a; return 0; }
SH(int, pthread_mutexattr_settype, (int *a, int t)) { *a = t; return 0; }
SH(int, pthread_mutex_init, (void *m, const int *a)) {
  memset(m, 0, 40);
  lazy_get(m, sizeof(pthread_mutex_t), mtx_init_obj, a && *a == 1);
  return 0;
}
SH(int, pthread_mutex_destroy, (void *m)) { (void)m; return 0; } /* object leaked on purpose: late users must not crash */
SH(int, pthread_mutex_lock, (void *m)) { return pthread_mutex_lock(mtx(m)); }
SH(int, pthread_mutex_unlock, (void *m)) { return pthread_mutex_unlock(mtx(m)); }

/* condvars: [0..8) newlib object, [8..12) clock id (0 realtime, 1 monotonic) */
static void cnd_init_obj(void *o, uint32_t a) { (void)a; pthread_cond_init(o, NULL); }
static pthread_cond_t *cnd(void *bc) { return lazy_get(bc, sizeof(pthread_cond_t), cnd_init_obj, 0); }
SH(int, pthread_condattr_init, (int *a)) { *a = 0; return 0; }
SH(int, pthread_condattr_destroy, (int *a)) { (void)a; return 0; }
SH(int, pthread_condattr_setclock, (int *a, int clk)) { *a = clk; return 0; }
SH(int, pthread_cond_init, (void *c, const int *a)) {
  memset(c, 0, 48); cnd(c); *(uint32_t *)((char *)c + 40) = (a && *a) ? 1 : 0; return 0;
}
SH(int, pthread_cond_destroy, (void *c)) { (void)c; return 0; }
SH(int, pthread_cond_signal, (void *c)) { return pthread_cond_signal(cnd(c)); }
SH(int, pthread_cond_broadcast, (void *c)) { return pthread_cond_broadcast(cnd(c)); }
SH(int, pthread_cond_wait, (void *c, void *m)) { return pthread_cond_wait(cnd(c), mtx(m)); }
SH(int, pthread_cond_timedwait, (void *c, void *m, const struct timespec *abs)) {
  int clk = *(uint32_t *)((char *)c + 40) ? 1 : 0;
  struct timespec now; shim_clock_gettime(clk, &now);
  int64_t rel = ((int64_t)abs->tv_sec - now.tv_sec) * 1000000000ll + (abs->tv_nsec - now.tv_nsec);
  if (rel < 0) rel = 0;
  struct timespec rt; shim_clock_gettime(0, &rt);
  int64_t t = (int64_t)rt.tv_sec * 1000000000ll + rt.tv_nsec + rel;
  struct timespec ta = { t / 1000000000ll, t % 1000000000ll };
  int r = pthread_cond_timedwait(cnd(c), mtx(m), &ta);
  return r == ETIMEDOUT ? 110 : r;   /* bionic ETIMEDOUT */
}

/* rwlock: treated as a recursive mutex for now (bring-up). */
SH(int, pthread_rwlock_rdlock, (void *l)) { return pthread_mutex_lock(lazy_get(l, sizeof(pthread_mutex_t), mtx_init_obj, 1)); }
SH(int, pthread_rwlock_wrlock, (void *l)) { return pthread_mutex_lock(lazy_get(l, sizeof(pthread_mutex_t), mtx_init_obj, 1)); }
SH(int, pthread_rwlock_unlock, (void *l)) { return pthread_mutex_unlock(lazy_get(l, sizeof(pthread_mutex_t), mtx_init_obj, 1)); }

SH(int, pthread_once, (volatile int *once, void (*fn)(void))) {
  int exp = 0;
  if (__atomic_compare_exchange_n(once, &exp, 1, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
    fn(); __atomic_store_n(once, 2, __ATOMIC_RELEASE); return 0;
  }
  while (__atomic_load_n(once, __ATOMIC_ACQUIRE) != 2) svcSleepThread(100000);
  return 0;
}

/* bionic pthread_attr_t (56 B): u32 flags; void *stack_base; size_t stack_size; ... */
typedef struct { uint32_t flags; void *stack_base; size_t stack_size; size_t guard; int32_t pol, prio; char r[16]; } battr_t;
SH(int, pthread_attr_init, (battr_t *a)) { memset(a, 0, sizeof *a); a->stack_size = 0x100000; return 0; }
SH(int, pthread_attr_destroy, (battr_t *a)) { (void)a; return 0; }
SH(int, pthread_attr_setdetachstate, (battr_t *a, int s)) { a->flags = (a->flags & ~1u) | (s ? 1u : 0u); return 0; }
SH(int, pthread_attr_setstacksize, (battr_t *a, size_t s)) { a->stack_size = s; return 0; }
SH(int, pthread_attr_getstack, (const battr_t *a, void **b, size_t *s)) { *b = a->stack_base; *s = a->stack_size; return 0; }
SH(int, pthread_getattr_np, (unsigned long th, battr_t *a)) {
  (void)th;
  memset(a, 0, sizeof *a);
  MemoryInfo mi; u32 pi; char probe;
  if (R_SUCCEEDED(svcQueryMemory(&mi, &pi, (u64)(uintptr_t)&probe))) { a->stack_base = (void *)mi.addr; a->stack_size = mi.size; }
  else { a->stack_base = (void *)((uintptr_t)&probe & ~0xFFFFFull); a->stack_size = 0x100000; }
  return 0;
}

typedef struct { void *(*fn)(void *); void *arg; } tramp_t;
static void *tramp(void *p) {
  tramp_t t = *(tramp_t *)p; free(p);
  shim_tls_install();
  return t.fn(t.arg);
}
SH(int, pthread_create, (uint64_t *out, const battr_t *a, void *(*fn)(void *), void *arg)) {
  tramp_t *t = malloc(sizeof *t); t->fn = fn; t->arg = arg;
  pthread_attr_t na; pthread_attr_init(&na);
  size_t ss = a && a->stack_size >= 0x20000 ? a->stack_size : 0x100000;
  pthread_attr_setstacksize(&na, ss);
  if (a && (a->flags & 1)) pthread_attr_setdetachstate(&na, PTHREAD_CREATE_DETACHED);
  pthread_t th = 0;
  int r = pthread_create(&th, &na, tramp, t);
  pthread_attr_destroy(&na);
  if (r) { free(t); debugPrintf("[pthread_create] failed rc=%d stack=0x%zx\n", r, ss); return r; }
  *out = (uint64_t)th;
  return 0;
}
SH(int, pthread_detach, (unsigned long t)) { (void)t; return 0; } /* newlib/libnx returns ENOSYS; thread resources are reclaimed on exit anyway */
SH(unsigned long, pthread_self, (void)) { return (unsigned long)pthread_self(); }
SH(int, pthread_setname_np, (unsigned long t, const char *n)) { (void)t; (void)n; return 0; }
SH(int, pthread_kill, (unsigned long t, int s)) { (void)t; (void)s; return 0; }
SH(int, pthread_key_create, (int *k, void (*d)(void *))) {
  pthread_key_t kk; int r = pthread_key_create(&kk, d); *k = (int)kk; return r;
}
SH(int, pthread_key_delete, (int k)) { return pthread_key_delete((pthread_key_t)k); }
SH(void *, pthread_getspecific, (int k)) { return pthread_getspecific((pthread_key_t)k); }
SH(int, pthread_setspecific, (int k, const void *v)) { return pthread_setspecific((pthread_key_t)k, v); }

/* ------------------------------------------------------ signals (none) */
SH(int, sigaction, (int s, const void *a, void *o)) { (void)s; (void)a; if (o) memset(o, 0, 32); return 0; }
SH(void *, signal, (int s, void *h)) { (void)s; (void)h; return NULL; }
SH(int, sigemptyset, (uint64_t *s)) { *s = 0; return 0; }
SH(int, sigfillset, (uint64_t *s)) { *s = ~0ull; return 0; }
SH(int, sigaddset, (uint64_t *s, int sig)) { *s |= 1ull << (sig - 1); return 0; }
SH(int, pthread_sigmask, (int h, const void *s, void *o)) { (void)h; (void)s; if (o) memset(o, 0, 8); return 0; }
SH(int, libc_current_sigrtmin, (void)) { return 34; }
SH(int, libc_current_sigrtmax, (void)) { return 64; }

/* ------------------------------------------------------- process/system */
SH(long, sysconf, (int n)) {
  switch (n) {
    case 39: return 4096;              /* _SC_PAGESIZE */
    case 2: return 100;                /* _SC_CLK_TCK */
    case 4: return 1024;               /* _SC_OPEN_MAX */
    case 96: case 97: return 3;        /* _SC_NPROCESSORS_CONF/ONLN */
    case 98: { u64 t = 0; svcGetInfo(&t, InfoType_TotalMemorySize, CUR_PROCESS_HANDLE, 0); return (long)(t / 4096); }
    case 99: { u64 t = 0, u = 0; svcGetInfo(&t, InfoType_TotalMemorySize, CUR_PROCESS_HANDLE, 0);
               svcGetInfo(&u, InfoType_UsedMemorySize, CUR_PROCESS_HANDLE, 0); return (long)((t - u) / 4096); }
    default: return -1;
  }
}
SH(int, getpagesize, (void)) { return 4096; }
SH(unsigned long, getauxval, (unsigned long t)) { return t == 6 ? 4096 : 0; }
SH(int, gettid, (void)) { u64 id = 1; svcGetThreadId(&id, CUR_THREAD_HANDLE); return (int)(id & 0x7fffffff); }
SH(int, getpid, (void)) { return 1; }
SH(unsigned, geteuid, (void)) { return 1000; }
SH(unsigned, getegid, (void)) { return 1000; }
SH(int, sched_getaffinity, (int pid, size_t sz, void *mask)) { (void)pid; memset(mask, 0, sz); *(uint8_t *)mask = 7; return 0; }
SH(int, sched_getcpu, (void)) { return (int)svcGetCurrentProcessorNumber(); }
SH(int, sched_yield, (void)) { svcSleepThread(0); return 0; }
SH(int, sched_cpucount, (size_t sz, const uint8_t *s)) { int n = 0; for (size_t i = 0; i < sz; i++) n += __builtin_popcount(s[i]); return n; }
SH(void *, sched_cpualloc, (size_t n)) { return calloc(1, (n + 7) / 8 * 8); }
SH(void, sched_cpufree, (void *p)) { free(p); }
SH(int, getrlimit, (int r, uint64_t *l)) { l[0] = r == 3 ? 0x800000 : ~0ull; l[1] = ~0ull; return 0; }
SH(int, getrusage, (int w, void *u)) { (void)w; memset(u, 0, 144); return 0; }
SH(int, uname, (char *u)) { memset(u, 0, 65 * 6); strcpy(u, "Linux"); strcpy(u + 65, "switch"); strcpy(u + 130, "5.10"); strcpy(u + 195, "#1"); strcpy(u + 260, "aarch64"); return 0; }
SH(int, sysinfo, (void *s)) { memset(s, 0, 112); return 0; }
SH(int, prctl, (int o, ...)) { (void)o; return 0; }
SH(long, syscall, (long n, ...)) { if (n == 178) return shim_gettid(); errno = ENOSYS; return -1; }
SH(int, system_property_get, (const char *n, char *v)) { (void)n; if (v) v[0] = 0; return 0; }
SH(void, arc4random_buf, (void *b, size_t n)) { csrngGetRandomBytes(b, n); }

/* ------------------------------------------------------------- memory */
#define MAP_FIXED_B 0x10
#define MAP_ANON_B  0x20
SH(void *, mmap, (void *addr, size_t len, int prot, int flags, int fd, long off)) {
  (void)prot;
  if (!len) return (void *)-1;
  if (flags & MAP_FIXED_B) { if (!addr) return (void *)-1; memset(addr, 0, len); return addr; }
  void *p = memalign(0x1000, len);
  if (!p) { debugPrintf("[mmap] 0x%zx FAILED\n", len); return (void *)-1; }
  if (!(flags & MAP_ANON_B) && fd >= 0) {
    off_t cur = lseek(fd, 0, SEEK_CUR); lseek(fd, off, SEEK_SET); ssize_t g = read(fd, p, len); (void)g; lseek(fd, cur, SEEK_SET);
  } else memset(p, 0, len);
  if (len >= (16u << 20)) debugPrintf("[mmap] LARGE %zu MB -> %p (real allocation)\n", len >> 20, p);
  return p;
}
SH(int, munmap, (void *a, size_t l)) { (void)a; (void)l; return 0; } /* GC-style callers re-map; never free */
SH(int, mprotect, (void *addr, size_t len, int prot)) {
  uintptr_t pa = (uintptr_t)addr & ~0xFFFull;
  size_t pl = (((uintptr_t)addr + len) - pa + 0xFFF) & ~0xFFFull;
  u32 perm = (prot & 1 ? 1 : 0) | (prot & 2 ? 2 : 0);
  Result rc = svcSetMemoryPermission((void *)pa, pl, perm);
  if (R_SUCCEEDED(rc)) return 0;
  MemoryInfo mi; u32 pi;
  if (R_SUCCEEDED(svcQueryMemory(&mi, &pi, pa)) && (mi.perm & perm) == perm) return 0;
  debugPrintf("[mprotect] %p 0x%zx prot=%d failed rc=0x%x\n", addr, len, prot, rc);
  errno = EACCES; return -1;
}
SH(int, mlock, (const void *a, size_t l)) { (void)a; (void)l; return 0; }
SH(int, munlock, (const void *a, size_t l)) { (void)a; (void)l; return 0; }
SH(int, madvise, (void *a, size_t l, int v)) { (void)a; (void)l; (void)v; return 0; }
SH(int, posix_memalign, (void **o, size_t al, size_t sz)) { void *p = memalign(al, sz); if (!p) return ENOMEM; *o = p; return 0; }

/* ----------------------------------------------------------- locale/wide */
SH(void *, newlocale, (int m, const char *l, void *b)) { (void)m; (void)l; (void)b; return (void *)1; }
SH(void, freelocale, (void *l)) { (void)l; }
SH(void *, uselocale, (void *l)) { (void)l; return (void *)1; }
SH(long double, strtold_l, (const char *s, char **e, void *l)) { (void)l; return strtold(s, e); }
SH(long long, strtoll_l, (const char *s, char **e, int b, void *l)) { (void)l; return strtoll(s, e, b); }
SH(unsigned long long, strtoull_l, (const char *s, char **e, int b, void *l)) { (void)l; return strtoull(s, e, b); }
SH(size_t, strftime_l, (char *s, size_t m, const char *f, const struct tm *t, void *l)) { (void)l; return strftime(s, m, f, t); }
SH(int, iswlower_l, (unsigned w, void *l)) { (void)l; return iswlower(w); }
SH(size_t, ctype_get_mb_cur_max, (void)) { return 1; }
/* single-byte multibyte layer (invariant globalization) */
SH(size_t, mbrlen, (const char *s, size_t n, void *st)) { (void)st; return (!s || !n) ? 0 : (*s ? 1 : 0); }
SH(size_t, mbrtowc, (wchar_t *w, const char *s, size_t n, void *st)) { (void)st; if (!s) return 0; if (!n) return (size_t)-2; if (w) *w = (unsigned char)*s; return *s ? 1 : 0; }
SH(int, mbtowc, (wchar_t *w, const char *s, size_t n)) { if (!s) return 0; if (!n) return -1; if (w) *w = (unsigned char)*s; return *s ? 1 : 0; }
SH(size_t, wcrtomb, (char *s, wchar_t w, void *st)) { (void)st; if (!s) return 1; *s = w < 256 ? (char)w : '?'; return 1; }
SH(size_t, mbsrtowcs, (wchar_t *d, const char **s, size_t n, void *st)) { (void)st; size_t i = 0; for (; (!d || i < n) && **s; i++, (*s)++) if (d) d[i] = (unsigned char)**s; if (d && i < n) { d[i] = 0; *s = NULL; } return i; }
SH(size_t, mbsnrtowcs, (wchar_t *d, const char **s, size_t nms, size_t n, void *st)) { (void)st; size_t i = 0; for (; (!d || i < n) && i < nms && **s; i++, (*s)++) if (d) d[i] = (unsigned char)**s; if (d && i < n && i < nms) { d[i] = 0; *s = NULL; } return i; }
SH(size_t, wcsnrtombs, (char *d, const wchar_t **s, size_t nwc, size_t n, void *st)) { (void)st; size_t i = 0; for (; (!d || i < n) && i < nwc && **s; i++, (*s)++) if (d) d[i] = **s < 256 ? (char)**s : '?'; if (d && i < n && i < nwc) { d[i] = 0; *s = NULL; } return i; }
SH(int, strerror_r, (int e, char *b, size_t n)) { snprintf(b, n, "error %d", e); return 0; }

/* ----------------------------------------------------------- stdio / io */
char shim_sF[3][152];
static int std_index(const void *f) {
  uintptr_t p = (uintptr_t)f, b = (uintptr_t)shim_sF;
  return (p >= b && p < b + sizeof shim_sF) ? (int)((p - b) / 152) : -1;
}
static FILE *mapf(FILE *f) { int i = std_index(f); return i < 0 ? f : (i == 0 ? stdin : (i == 1 ? stdout : stderr)); }
SH(int, vfprintf, (FILE *f, const char *fmt, va_list v)) {
  int i = std_index(f);
  if (i > 0) { char b[1024]; int n = vsnprintf(b, sizeof b, fmt, v); debugPrintf("%s", b); return n; }
  return vfprintf(mapf(f), fmt, v);
}
SH(int, fprintf, (FILE *f, const char *fmt, ...)) { va_list v; va_start(v, fmt); int n = shim_vfprintf(f, fmt, v); va_end(v); return n; }
SH(int, fputs, (const char *s, FILE *f)) { if (std_index(f) > 0) { debugPrintf("%s", s); return 1; } return fputs(s, mapf(f)); }
SH(int, fputc, (int c, FILE *f)) { if (std_index(f) > 0) { debugPrintf("%c", c); return c; } return fputc(c, mapf(f)); }
SH(size_t, fwrite, (const void *p, size_t s, size_t n, FILE *f)) {
  if (std_index(f) > 0) { debugPrintf("%.*s", (int)(s * n), (const char *)p); return n; }
  return fwrite(p, s, n, mapf(f));
}
SH(int, fflush, (FILE *f)) { return f && std_index(f) > 0 ? 0 : fflush(mapf(f)); }
SH(int, fclose, (FILE *f)) { return std_index(f) >= 0 ? 0 : fclose(f); }
SH(int, fgetc, (FILE *f)) { return fgetc(mapf(f)); }
SH(int, fscanf, (FILE *f, const char *fmt, ...)) { va_list v; va_start(v, fmt); int r = vfscanf(mapf(f), fmt, v); va_end(v); return r; }

static int oflags(int f) {                      /* bionic(aarch64) -> newlib */
  int o = f & 3;
  if (f & 0100) o |= O_CREAT;
  if (f & 0200) o |= O_EXCL;
  if (f & 01000) o |= O_TRUNC;
  if (f & 02000) o |= O_APPEND;
  if (f & 04000) o |= O_NONBLOCK;
  return o;
}
SH(int, open, (const char *p, int fl, ...)) {
  va_list v; va_start(v, fl); mode_t m = va_arg(v, mode_t); va_end(v);
  return open(p, oflags(fl), m);
}
SH(int, open_2, (const char *p, int fl)) { return open(p, oflags(fl), 0); }
SH(int, fcntl, (int fd, int cmd, ...)) { (void)fd; (void)cmd; return 0; }
SH(int, ioctl, (int fd, unsigned long r, ...)) { (void)fd; (void)r; errno = ENOTTY; return -1; }

typedef struct { uint64_t dev, ino; uint32_t mode, nlink, uid, gid; uint64_t rdev, pad1; int64_t size; int32_t blksize, pad2; int64_t blocks;
  int64_t as, an, ms, mn, cs, cn; uint32_t un[2]; } bstat_t;
static void conv_stat(const struct stat *s, bstat_t *b) {
  memset(b, 0, sizeof *b);
  b->dev = s->st_dev; b->ino = s->st_ino; b->mode = s->st_mode; b->nlink = s->st_nlink; b->size = s->st_size;
  b->blksize = 4096; b->blocks = (s->st_size + 511) / 512;
  b->as = s->st_atime; b->ms = s->st_mtime; b->cs = s->st_ctime;
}
SH(int, stat64, (const char *p, bstat_t *b)) { struct stat s; int r = stat(p, &s); if (!r) conv_stat(&s, b); return r; }
SH(int, lstat64, (const char *p, bstat_t *b)) { return shim_stat64(p, b); }
SH(int, fstat64, (int fd, bstat_t *b)) { struct stat s; int r = fstat(fd, &s); if (!r) conv_stat(&s, b); return r; }
SH(long long, lseek64, (int fd, long long o, int w)) { return lseek(fd, o, w); }
SH(int, ftruncate64, (int fd, long long l)) { return ftruncate(fd, l); }
SH(int, statfs, (const char *p, void *b)) { (void)p; memset(b, 0, 120); return 0; }
SH(int, fstatfs, (int fd, void *b)) { (void)fd; memset(b, 0, 120); return 0; }

typedef struct { uint64_t ino; int64_t off; uint16_t reclen; uint8_t type; char name[256]; } bdirent_t;
SH(void *, readdir, (DIR *d)) {
  static __thread bdirent_t be;
  struct dirent *e = readdir(d); if (!e) return NULL;
  memset(&be, 0, sizeof be); be.ino = e->d_ino; be.type = e->d_type == DT_DIR ? 4 : 8;
  strncpy(be.name, e->d_name, 255); be.reclen = sizeof be; return &be;
}

/* --------------------------------------------------------- dl*, EH */
extern int so_dl_iterate_phdr(int (*cb)(void *, size_t, void *), void *d);
SH(int, dl_iterate_phdr, (int (*cb)(void *, size_t, void *), void *d)) { return so_dl_iterate_phdr(cb, d); }
/* Handles: 0x5D1 = libSDL3 (mapped later), 0x5A1 = "system" libs (libc/libm/libdl/liblog/libz ...,
 * resolved from our import table), 0x5E1 = the game library itself. */
extern so_module *g_skynet_mod;
extern DynLibFunction skynet_libc_imports[];
extern const int skynet_libc_imports_count;
SH(void *, dlopen, (const char *n, int f)) {
  (void)f;
  void *h = NULL;
  if (!n) h = (void *)0x5E1;
  else if (strstr(n, "SDL3")) h = (void *)0x5D1;
  else if (strstr(n, "SkyNet")) h = (void *)0x5E1;
  else h = (void *)0x5A1;   /* c, m, dl, log, z, android, ... and anything else: resolve from our table */
  debugPrintf("[dlopen] %s -> %p\n", n ? n : "(null)", h);
  return h;
}
SH(void *, dlsym, (void *h, const char *n)) {
  void *r = NULL;
  if (h == (void *)0x5D1) { /* SDL: not mapped yet */ }
  else {
    if (h == (void *)0x5E1 || h == NULL || h == (void *)-1 || h == (void *)0x5A1) {
      for (int i = 0; i < skynet_libc_imports_count && !r; i++)
        if (!strcmp(skynet_libc_imports[i].symbol, n)) r = (void *)skynet_libc_imports[i].func;
    }
    if (!r && g_skynet_mod && (h == (void *)0x5E1 || h == NULL)) r = (void *)so_try_find_addr_rx(g_skynet_mod, n);
  }
  debugPrintf("[dlsym] h=%p %s -> %p\n", h, n, r);
  return r;
}
SH(int, dlclose, (void *h)) { (void)h; return 0; }
SH(char *, dlerror, (void)) { return NULL; }
SH(int, dladdr, (const void *a, void *i)) { (void)a; (void)i; return 0; }
SH(const char *, gai_strerror, (int e)) { (void)e; return "gai error"; }

/* networking, processes, users: not available -> fail cleanly */
#define FAIL(name, args) SH(int, name, args) { errno = ENOSYS; return -1; }
FAIL(fork, (void)) FAIL(execv, (const char *p, char *const a[])) FAIL(execve, (const char *p, char *const a[], char *const e[]))
FAIL(waitpid, (int p, int *s, int o)) FAIL(waitid, (int a, int b, void *c, int d)) FAIL(kill, (int p, int s))
FAIL(pipe, (int f[2])) FAIL(pipe2, (int f[2], int fl)) FAIL(socket, (int a, int b, int c)) FAIL(bind, (int a, const void *b, int c))
FAIL(connect, (int a, const void *b, int c)) FAIL(shutdown, (int a, int b)) FAIL(getpeername, (int a, void *b, void *c))
FAIL(getsockname, (int a, void *b, void *c)) FAIL(getsockopt, (int a, int b, int c, void *d, void *e)) FAIL(setsockopt, (int a, int b, int c, const void *d, int e))
FAIL(recvfrom, (int a, void *b, size_t c, int d, void *e, void *f)) FAIL(recvmsg, (int a, void *b, int c)) FAIL(sendmsg, (int a, const void *b, int c))
FAIL(sendto, (int a, const void *b, size_t c, int d, const void *e, int f)) FAIL(getaddrinfo, (const char *a, const char *b, const void *c, void **d))
FAIL(getnameinfo, (const void *a, int b, char *c, int d, char *e, int f, int g)) FAIL(poll, (void *a, unsigned long b, int c))
FAIL(epoll_create1, (int f)) FAIL(epoll_ctl, (int a, int b, int c, void *d)) FAIL(epoll_wait, (int a, void *b, int c, int d))
FAIL(getpwnam_r, (const char *a, void *b, char *c, size_t d, void **e)) FAIL(getpwuid_r, (unsigned a, void *b, char *c, size_t d, void **e))
FAIL(getgrouplist, (const char *a, int b, int *c, int *d)) FAIL(getgroups, (int a, int *b)) FAIL(setgroups, (size_t a, const int *b))
FAIL(setuid, (unsigned a)) FAIL(setgid, (unsigned a)) FAIL(fallocate, (int a, int b, long c, long d)) FAIL(sendfile, (int a, int b, long *c, size_t d))
FAIL(if_nametoindex, (const char *a)) FAIL(link, (const char *a, const char *b)) FAIL(readlink, (const char *a, char *b, size_t c))
FAIL(dup2, (int a, int b))
SH(void, freeaddrinfo, (void *p)) { (void)p; }
SH(void, openlog, (const char *a, int b, int c)) { (void)a; (void)b; (void)c; }
SH(void, closelog, (void)) {}
SH(void, syslog, (int p, const char *f, ...)) { (void)p; (void)f; }
SH(int, flock, (int a, int b)) { (void)a; (void)b; return 0; }
SH(int, fchmod, (int a, int b)) { (void)a; (void)b; return 0; }
SH(int, futimens, (int a, const void *b)) { (void)a; (void)b; return 0; }
SH(int, utimensat, (int a, const char *b, const void *c, int d)) { (void)a; (void)b; (void)c; (void)d; return 0; }
SH(int, posix_fadvise64, (int a, long b, long c, int d)) { (void)a; (void)b; (void)c; (void)d; return 0; }

SH(void *, mmap64, (void *a, size_t l, int p, int f, int fd, long o)) { return shim_mmap(a, l, p, f, fd, o); }

SH(ssize_t, getline, (char **l, size_t *n, FILE *f)) {
  f = mapf(f);
  if (!*l || !*n) { *n = 128; *l = malloc(*n); if (!*l) return -1; }
  size_t i = 0; int c;
  while ((c = fgetc(f)) != EOF) {
    if (i + 2 > *n) { *n *= 2; char *t = realloc(*l, *n); if (!t) return -1; *l = t; }
    (*l)[i++] = (char)c;
    if (c == '\n') break;
  }
  if (!i) return -1;
  (*l)[i] = 0; return (ssize_t)i;
}
SH(ssize_t, pread, (int fd, void *b, size_t n, long o)) {
  off_t cur = lseek(fd, 0, SEEK_CUR); if (lseek(fd, o, SEEK_SET) < 0) return -1;
  ssize_t r = read(fd, b, n); lseek(fd, cur, SEEK_SET); return r;
}
SH(ssize_t, pwrite, (int fd, const void *b, size_t n, long o)) {
  off_t cur = lseek(fd, 0, SEEK_CUR); if (lseek(fd, o, SEEK_SET) < 0) return -1;
  ssize_t r = write(fd, b, n); lseek(fd, cur, SEEK_SET); return r;
}
