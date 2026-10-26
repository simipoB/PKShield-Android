// Darwin stand-in (dm/android/compat), included first in every Android host file (-include): Darwin-only libc and pthread extras on bionic
#pragma once
#define _GNU_SOURCE 1
#include <stdint.h>
#include <time.h>
#include <pthread.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdio.h>
#include <sys/syscall.h>
#include <sys/param.h>
static inline uint64_t clock_gettime_nsec_np(clockid_t c) { struct timespec t; clock_gettime(c, &t); return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec; }
#define CLOCK_UPTIME_RAW CLOCK_MONOTONIC
#define CLOCK_MONOTONIC_RAW_APPROX CLOCK_MONOTONIC
static inline void *pthread_get_stackaddr_np(pthread_t t) {   // Darwin: the stack's high end
    pthread_attr_t a; void *lo = 0; size_t n = 0;
    if (pthread_getattr_np(t, &a)) return 0; pthread_attr_getstack(&a, &lo, &n); pthread_attr_destroy(&a); return (char *)lo + n;
}
static inline size_t pthread_get_stacksize_np(pthread_t t) { pthread_attr_t a; void *lo = 0; size_t n = 0; if (pthread_getattr_np(t, &a)) return 0; pthread_attr_getstack(&a, &lo, &n); pthread_attr_destroy(&a); return n; }
static inline int pthread_threadid_np(pthread_t t, uint64_t *id) { *id = t == pthread_self() || !t ? (uint64_t)gettid() : (uint64_t)pthread_gettid_np(t); return 0; }
static inline int pthread_main_np(void) { return getpid() == gettid(); }
static inline int pthread_cond_timedwait_relative_np(pthread_cond_t *c, pthread_mutex_t *m, const struct timespec *rel) {
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); t.tv_sec += rel->tv_sec; t.tv_nsec += rel->tv_nsec; if (t.tv_nsec >= 1000000000) { t.tv_sec++; t.tv_nsec -= 1000000000; }
    return pthread_cond_timedwait_monotonic_np(c, m, &t);
}
static inline int getdtablesize(void) { return (int)sysconf(_SC_OPEN_MAX); }
// F_GETPATH: the path of an open descriptor, through /proc/self/fd
#define F_GETPATH 50
static inline int dm_fd_path(int fd, char *out) { char l[64]; snprintf(l, sizeof l, "/proc/self/fd/%d", fd); ssize_t n = readlink(l, out, MAXPATHLEN - 1); if (n < 0) return -1; out[n] = 0; return 0; }
#define fcntl(fd, cmd, ...) ((cmd) == F_GETPATH ? dm_fd_path((fd), __VA_ARGS__) : fcntl((fd), (cmd), ##__VA_ARGS__))
// renamex_np(RENAME_SWAP): renameat2 with RENAME_EXCHANGE; where the file system refuses it (shared storage through FUSE can), three renames through <b>.swp,
// which dm_swap_recover(<b>) settles if the process dies between them (compat.c; called before the save directory is mounted)
#define RENAME_SWAP 2
#define RENAME_EXCL 4
int renamex_np(const char *a, const char *b, unsigned f);
void dm_swap_recover(const char *b);
// strlcpy / strlcat are in bionic; arc4random too
// Signal context registers with Darwin's names (DM_UC_* in dm.h). Linux's sigcontext keeps x0..x30, sp, pc, pstate in Darwin's thread-state order, so the thread state is a view of regs[];
// ESR and the NEON registers are records in __reserved, found by their magic
#include <signal.h>
#include <sys/ucontext.h>
#include <asm/sigcontext.h>
struct __darwin_arm_thread_state64 { uint64_t __x[29], __fp, __lr, __sp, __pc; uint32_t __cpsr, __pad; };
#define __darwin_arm_thread_state64_get_pc(s) ((s).__pc)
#define __darwin_arm_thread_state64_get_lr(s) ((s).__lr)
#define __darwin_arm_thread_state64_get_sp(s) ((s).__sp)
#define __darwin_arm_thread_state64_get_fp(s) ((s).__fp)
static inline void *dm_uc_rec(ucontext_t *uc, uint32_t magic) {
    struct _aarch64_ctx *h = (struct _aarch64_ctx *)(void *)uc->uc_mcontext.__reserved;
    for (int i = 0; i < 64 && h->magic; i++) { if (h->magic == magic) return h; h = (struct _aarch64_ctx *)((char *)h + h->size); }
    return 0;
}
static inline uint64_t dm_uc_esr(ucontext_t *uc) { struct esr_context *e = dm_uc_rec(uc, ESR_MAGIC); return e ? e->esr : 0; }
static __uint128_t dm_uc_vzero[32];
#define DM_UC_SS(uc) (*(struct __darwin_arm_thread_state64 *)(void *)&(uc)->uc_mcontext.regs[0])
#define DM_UC_ESR(uc) dm_uc_esr(uc)
#define DM_UC_FP_(uc) ((struct fpsimd_context *)dm_uc_rec((uc), FPSIMD_MAGIC))
#define DM_UC_V(uc) (DM_UC_FP_(uc) ? DM_UC_FP_(uc)->vregs : dm_uc_vzero)
#define DM_UC_FPCR(uc) (DM_UC_FP_(uc) ? DM_UC_FP_(uc)->fpcr : 0)
#define DM_UC_FPSR(uc) (DM_UC_FP_(uc) ? DM_UC_FP_(uc)->fpsr : 0)
// pthread_setname_np: Darwin names the calling thread (one argument); Linux takes the thread and allows 15 characters
static inline int dm_setname(pthread_t t, const char *n) { char b[16]; size_t i = 0; for (; n[i] && i < 15; i++) b[i] = n[i]; b[i] = 0; return (pthread_setname_np)(t, b); }
#define DM_SN_PICK(_1, _2, N, ...) N
#define DM_SN1(n) dm_setname(pthread_self(), (n))
#define pthread_setname_np(...) DM_SN_PICK(__VA_ARGS__, dm_setname, DM_SN1)(__VA_ARGS__)

// Game files inside the installed APKs (paths under /nxapk/): the host's file calls go through apkfs.c
#include <apkfs.h>
