// Out-of-line parts of the Darwin stand-ins (dm/android/compat).
#define _GNU_SOURCE
#include <mach/mach.h>
#include <CommonCrypto/CommonDigest.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/uio.h>
#include <unistd.h>
#include <time.h>
#include <malloc.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <sys/stat.h>
#include <sys/clonefile.h>
#include <ftw.h>
#include <stdlib.h>
#include <sys/syscall.h>

uint64_t mach_absolute_time(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec; }
mach_port_t mach_thread_self(void) { return (mach_port_t)gettid(); }

// A read that fails instead of faulting on unmapped memory: process_vm_readv on this process
kern_return_t vm_read_overwrite(task_t t, vm_address_t a, vm_size_t n, vm_address_t to, vm_size_t *got) {
    (void)t; struct iovec l = { (void *)to, n }, r = { (void *)a, n };
    ssize_t k = process_vm_readv(getpid(), &l, 1, &r, 1, 0);
    if (k != (ssize_t)n) { if (got) *got = k > 0 ? (vm_size_t)k : 0; return KERN_INVALID_ADDRESS; }
    if (got) *got = n; return KERN_SUCCESS;
}
kern_return_t vm_allocate(task_t t, vm_address_t *a, vm_size_t n, int flags) {
    (void)t; (void)flags; void *p = mmap(0, n, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (p == MAP_FAILED) return KERN_FAILURE; *a = (vm_address_t)p; return KERN_SUCCESS;
}
kern_return_t vm_deallocate(task_t t, vm_address_t a, vm_size_t n) { (void)t; return munmap((void *)a, n) ? KERN_FAILURE : KERN_SUCCESS; }

// SHA-256 (FIPS 180-4)
static const uint32_t K[64] = {
    0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
    0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
    0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
    0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2 };
#define ROR(x, n) ((x) >> (n) | (x) << (32 - (n)))
static void block(CC_SHA256_CTX *c, const uint8_t *p) {
    uint32_t w[64], a, b, d, e, f, g, h, cc;
    for (int i = 0; i < 16; i++) w[i] = (uint32_t)p[4*i] << 24 | (uint32_t)p[4*i+1] << 16 | (uint32_t)p[4*i+2] << 8 | p[4*i+3];
    for (int i = 16; i < 64; i++) w[i] = w[i-16] + (ROR(w[i-15], 7) ^ ROR(w[i-15], 18) ^ w[i-15] >> 3) + w[i-7] + (ROR(w[i-2], 17) ^ ROR(w[i-2], 19) ^ w[i-2] >> 10);
    a = c->h[0]; b = c->h[1]; cc = c->h[2]; d = c->h[3]; e = c->h[4]; f = c->h[5]; g = c->h[6]; h = c->h[7];
    for (int i = 0; i < 64; i++) {
        uint32_t t1 = h + (ROR(e, 6) ^ ROR(e, 11) ^ ROR(e, 25)) + ((e & f) ^ (~e & g)) + K[i] + w[i];
        uint32_t t2 = (ROR(a, 2) ^ ROR(a, 13) ^ ROR(a, 22)) + ((a & b) ^ (a & cc) ^ (b & cc));
        h = g; g = f; f = e; e = d + t1; d = cc; cc = b; b = a; a = t1 + t2;
    }
    c->h[0] += a; c->h[1] += b; c->h[2] += cc; c->h[3] += d; c->h[4] += e; c->h[5] += f; c->h[6] += g; c->h[7] += h;
}
int CC_SHA256_Init(CC_SHA256_CTX *c) {
    static const uint32_t H[8] = {0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19};
    memcpy(c->h, H, sizeof H); c->n = 0; return 1;
}
int CC_SHA256_Update(CC_SHA256_CTX *c, const void *data, CC_LONG n) {
    const uint8_t *p = data;
    while (n) { size_t o = c->n & 63, k = 64 - o < n ? 64 - o : n; memcpy(c->buf + o, p, k); c->n += k; p += k; n -= (CC_LONG)k; if (!(c->n & 63)) block(c, c->buf); }
    return 1;
}
int CC_SHA256_Final(unsigned char *md, CC_SHA256_CTX *c) {
    uint64_t bits = c->n * 8; uint8_t pad = 0x80, z = 0;
    CC_SHA256_Update(c, &pad, 1); while ((c->n & 63) != 56) CC_SHA256_Update(c, &z, 1);
    for (int i = 7; i >= 0; i--) { uint8_t b = (uint8_t)(bits >> (8 * i)); CC_SHA256_Update(c, &b, 1); }
    for (int i = 0; i < 8; i++) { md[4*i] = (uint8_t)(c->h[i] >> 24); md[4*i+1] = (uint8_t)(c->h[i] >> 16); md[4*i+2] = (uint8_t)(c->h[i] >> 8); md[4*i+3] = (uint8_t)c->h[i]; }
    return 1;
}

// Heap pointer tagging: bionic's allocator puts a tag in the top byte of every heap pointer (TBI, Android 11+). Switch code is written for plain 39-bit addresses and gets host heap
// memory handed to it (thread blocks, file buffers, NVN objects), so tagging is turned off before anything is allocated. The app does the same with allowNativeHeapPointerTagging="false"
__attribute__((constructor(101))) static void dm_no_heap_tagging(void) { mallopt(M_BIONIC_SET_HEAP_TAGGING_LEVEL, M_HEAP_TAGGING_LEVEL_NONE); }

// clonefile: recursive copy. A failure part-way removes nothing (the caller treats any failure as "no clone" and keeps the committed directory)
static int copy_file(const char *s, const char *d, mode_t mode) {
    int in = open(s, O_RDONLY | O_CLOEXEC); if (in < 0) return -1;
    int out = open(d, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, mode & 07777); if (out < 0) { int e = errno; close(in); errno = e; return -1; }
    char buf[1 << 16]; ssize_t n; int rc = 0;
    while ((n = read(in, buf, sizeof buf)) != 0) {
        if (n < 0) { if (errno == EINTR) continue; rc = -1; break; }
        for (ssize_t o = 0; o < n; ) { ssize_t w = write(out, buf + o, (size_t)(n - o)); if (w < 0) { if (errno == EINTR) continue; rc = -1; break; } o += w; }
        if (rc) break;
    }
    int e = errno; close(in); if (close(out)) rc = -1; errno = e; return rc;
}
int clonefile(const char *src, const char *dst, unsigned flags) {
    (void)flags; struct stat st; if (lstat(src, &st)) return -1;
    if (S_ISREG(st.st_mode)) return copy_file(src, dst, st.st_mode);
    if (!S_ISDIR(st.st_mode)) { errno = ENOTSUP; return -1; }
    if (mkdir(dst, st.st_mode & 07777)) return -1;
    DIR *dir = opendir(src); if (!dir) return -1;
    struct dirent *e; int rc = 0;
    while (!rc && (e = readdir(dir))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        char a[PATH_MAX], b[PATH_MAX];
        if (snprintf(a, sizeof a, "%s/%s", src, e->d_name) >= (int)sizeof a || snprintf(b, sizeof b, "%s/%s", dst, e->d_name) >= (int)sizeof b) { errno = ENAMETOOLONG; rc = -1; break; }
        rc = clonefile(a, b, flags);
    }
    int er = errno; closedir(dir); errno = er; return rc;
}

// Darwin's private per-thread instruction / cycle counters (hle_nvn.c timing lines): not available, reported as zeros
int thread_selfcounts(int type, void *buf, size_t n) { (void)type; memset(buf, 0, n); return -1; }

// renamex_np (darwin.h). The fallback swap: b → b.swp, a → b, b.swp → a. Between the first two renames b is missing and b.swp holds the old committed version;
// between the last two, b is the new version and b.swp the old one: dm_swap_recover puts the first case back and drops the leftover in the second
static int rm_rf_cb(const char *p, const struct stat *st, int type, struct FTW *ftw) { (void)st; (void)ftw; return type == FTW_DP ? rmdir(p) : unlink(p); }
int renamex_np(const char *a, const char *b, unsigned f) {
    if (!(f & RENAME_SWAP)) return (int)syscall(SYS_renameat2, AT_FDCWD, a, AT_FDCWD, b, f & RENAME_EXCL ? 1u /* RENAME_NOREPLACE */ : 0u);
    static int no_exchange = -1; if (no_exchange < 0) no_exchange = getenv("DM_TEST_NO_EXCHANGE") != NULL;
    if (!no_exchange && !syscall(SYS_renameat2, AT_FDCWD, a, AT_FDCWD, b, 2u /* RENAME_EXCHANGE */)) return 0;
    if (!no_exchange && errno != EINVAL && errno != ENOSYS && errno != EOPNOTSUPP && errno != EPERM && errno != EXDEV) return -1;
    if (!no_exchange) { no_exchange = 1; fprintf(stderr, "[dm] renamex_np: the file system has no atomic exchange (errno=%d), swapping through .swp renames\n", errno); }
    char t[1200]; snprintf(t, sizeof t, "%s.swp", b);
    if (!access(t, F_OK)) nftw(t, rm_rf_cb, 16, FTW_DEPTH | FTW_PHYS);
    if (rename(b, t)) return -1;
    if (rename(a, b)) { int e = errno; rename(t, b); errno = e; return -1; }
    if (rename(t, a)) { nftw(t, rm_rf_cb, 16, FTW_DEPTH | FTW_PHYS); }   // a held a temporary copy the caller deletes anyway
    return 0;
}
void dm_swap_recover(const char *b) {
    char t[1200]; snprintf(t, sizeof t, "%s.swp", b);
    if (access(t, F_OK)) return;
    if (access(b, F_OK)) { fprintf(stderr, "[dm] save swap interrupted last time: putting the committed version back from %s\n", t); rename(t, b); }
    else { fprintf(stderr, "[dm] save swap interrupted last time after the new version was in place: removing %s\n", t); nftw(t, rm_rf_cb, 16, FTW_DEPTH | FTW_PHYS); }
}
