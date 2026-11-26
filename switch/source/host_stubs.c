/* host_stubs.c -- libc symbols the statically linked Mesa/SDL pull in that
 * Horizon's newlib does not provide. They are for the HOST side (linked into
 * the NRO), not for libSkyNet.so (see shim.c). All fail cleanly. */
#include <errno.h>
#include <stdarg.h>
#include <stddef.h>
#include <signal.h>
#include <unistd.h>
#include <sys/types.h>

int posix_spawn(pid_t *p, const char *path, const void *fa, const void *attr, char *const argv[], char *const envp[]) {
  (void)p; (void)path; (void)fa; (void)attr; (void)argv; (void)envp; return ENOSYS;
}
int posix_spawnp(pid_t *p, const char *path, const void *fa, const void *attr, char *const argv[], char *const envp[]) {
  (void)p; (void)path; (void)fa; (void)attr; (void)argv; (void)envp; return ENOSYS;
}
int posix_spawn_file_actions_init(void *fa) { (void)fa; return 0; }
int posix_spawn_file_actions_destroy(void *fa) { (void)fa; return 0; }
int posix_spawn_file_actions_adddup2(void *fa, int a, int b) { (void)fa; (void)a; (void)b; return 0; }
int posix_spawn_file_actions_addclose(void *fa, int a) { (void)fa; (void)a; return 0; }
int posix_spawn_file_actions_addopen(void *fa, int a, const char *p, int f, mode_t m) { (void)fa; (void)a; (void)p; (void)f; (void)m; return 0; }
int posix_spawnattr_init(void *a) { (void)a; return 0; }
int posix_spawnattr_destroy(void *a) { (void)a; return 0; }
pid_t waitpid(pid_t p, int *s, int o) { (void)p; (void)s; (void)o; errno = ECHILD; return -1; }
pid_t vfork(void) { errno = ENOSYS; return -1; }
pid_t setsid(void) { errno = ENOSYS; return -1; }
int pipe(int fd[2]) { (void)fd; errno = ENOSYS; return -1; }
int sigaction(int s, const struct sigaction *a, struct sigaction *o) { (void)s; (void)a; (void)o; return 0; }
long sysconf(int name) {
  switch (name) {
#ifdef _SC_NPROCESSORS_ONLN
    case _SC_NPROCESSORS_ONLN: return 3;
#endif
#ifdef _SC_NPROCESSORS_CONF
    case _SC_NPROCESSORS_CONF: return 3;
#endif
#ifdef _SC_PAGESIZE
    case _SC_PAGESIZE: return 4096;
#endif
    default: errno = EINVAL; return -1;
  }
}

/* SDL3 build for Switch references this internal symbol but does not define it. */
_Bool SDL_GetSystemTimeLocalePreferences(int *date_format, int *time_format) { (void)date_format; (void)time_format; return 0; }
