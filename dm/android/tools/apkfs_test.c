// Stress test of the in-APK game files (compat/apkfs.c): walks /nxapk/ (DM_APK_DIR names the installed APK folder) with opendir / readdir / dirfd + fstatat, then
// reads every file from several threads through each access path the host uses (read in random chunk sizes, pread at random offsets, mmap, fopen + fread) and
// checks that all of them agree, plus fd churn and the error cases. Prints "hash size path" lines; built for the Mac with -DPLAIN over the files folder itself,
// the two listings must match (tools/apkfs_stress.sh).
#define _GNU_SOURCE
#ifndef PLAIN
#include "apkfs.h"
#endif
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

static uint64_t hash(uint64_t h, const uint8_t *p, size_t n) {   // order-dependent, chunking-independent when fed in 8-byte multiples
    for (; n >= 8; p += 8, n -= 8) { uint64_t w; memcpy(&w, p, 8); h = (h ^ w) * 0x100000001b3ull; h ^= h >> 29; }
    for (; n; p++, n--) { h = (h ^ *p) * 0x100000001b3ull; }
    return h;
}
typedef struct { char *path; uint64_t size, h; int bad; } file;
static file *g_f; static int g_n, g_cap; static int g_next; static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static int g_err; static const char *g_root;
#define FAIL(...) do { fprintf(stderr, "FAIL " __VA_ARGS__); fprintf(stderr, "\n"); __atomic_add_fetch(&g_err, 1, __ATOMIC_RELAXED); } while (0)

static void walk(const char *dir) {
    DIR *d = opendir(dir); if (!d) { FAIL("opendir %s: %s", dir, strerror(errno)); return; }
    int dfd = dirfd(d); struct dirent *e; int dots = 0;
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) { dots++; continue; }
        if (e->d_name[0] == '.') continue;
        char p[2048]; snprintf(p, sizeof p, "%s/%s", dir, e->d_name);
        struct stat a, b; if (stat(p, &a)) { FAIL("stat %s", p); continue; }
        if (fstatat(dfd, e->d_name, &b, 0)) { FAIL("fstatat %s", p); continue; }
        if (a.st_size != b.st_size || S_ISDIR(a.st_mode) != S_ISDIR(b.st_mode)) FAIL("stat/fstatat disagree %s", p);
        if (S_ISDIR(a.st_mode) != (e->d_type == DT_DIR)) FAIL("d_type %s", p);
        if (S_ISDIR(a.st_mode)) walk(p);
        else { if (g_n == g_cap) g_f = realloc(g_f, sizeof *g_f * (g_cap = g_cap ? g_cap * 2 : 4096)); g_f[g_n].path = strdup(p); g_f[g_n].size = (uint64_t)a.st_size; g_f[g_n].bad = 0; g_n++; }
    }
    if (dots != 2) FAIL("%s: %d dot entries", dir, dots);
    closedir(d);
}
static uint32_t rnd(uint64_t *s) { *s ^= *s << 13; *s ^= *s >> 7; *s ^= *s << 17; return (uint32_t)(*s >> 11); }
static void check(file *f, uint64_t *rs, uint8_t *buf, uint8_t *buf2) {
    enum { CH = 1 << 20 };
    int fd = open(f->path, O_RDONLY); if (fd < 0) { FAIL("open %s: %s", f->path, strerror(errno)); f->bad = 1; return; }
    struct stat st; if (fstat(fd, &st) || (uint64_t)st.st_size != f->size) FAIL("fstat size %s", f->path);
    // 1. sequential read in random chunk sizes (multiples of 8 so the hash is chunk-independent, except the tail)
    uint64_t h = 0xcbf29ce484222325ull, got = 0; ssize_t r;
    for (;;) { size_t want = ((rnd(rs) % CH) & ~7u) + 8; r = read(fd, buf, want); if (r <= 0) break;
        if ((size_t)r != want && got + (uint64_t)r != f->size) { FAIL("short read %s", f->path); break; }
        h = hash(h, buf, (size_t)r); got += (uint64_t)r; }
    if (r < 0 || got != f->size) FAIL("read %s: got %llu of %llu", f->path, (unsigned long long)got, (unsigned long long)f->size);
    if (read(fd, buf, 16) != 0) FAIL("read past end %s", f->path);
    f->h = h;
    // 2. pread at random offsets against lseek + read of the same range
    for (int k = 0; k < 4 && f->size; k++) {
        uint64_t off = ((uint64_t)rnd(rs) << 20 ^ rnd(rs)) % f->size; size_t n = rnd(rs) % 65536 + 1;
        ssize_t a = pread(fd, buf, n, (off_t)off);
        if (lseek(fd, (off_t)off, SEEK_SET) != (off_t)off) { FAIL("lseek %s", f->path); break; }
        ssize_t b = read(fd, buf2, n); size_t exp = off + n > f->size ? (size_t)(f->size - off) : n;
        if (a != (ssize_t)exp || b != (ssize_t)exp || memcmp(buf, buf2, exp)) { FAIL("pread/read disagree %s @%llu", f->path, (unsigned long long)off); break; }
    }
    if (lseek(fd, 0, SEEK_END) != (off_t)f->size) FAIL("SEEK_END %s", f->path);
    // 3. mmap of the whole file
    if (f->size) {
        void *m = mmap(NULL, f->size, PROT_READ, MAP_PRIVATE, fd, 0);
        if (m == MAP_FAILED) FAIL("mmap %s: %s", f->path, strerror(errno));
        else { if (hash(0xcbf29ce484222325ull, m, f->size) != h) FAIL("mmap content %s", f->path); munmap(m, f->size); }
    }
    close(fd);
    // 4. stdio (fopen + fread + fseek), on a sample
    if (rnd(rs) % 8 == 0) {
        FILE *fp = fopen(f->path, "rb"); if (!fp) { FAIL("fopen %s", f->path); return; }
        uint64_t h2 = 0xcbf29ce484222325ull; size_t n;
        while ((n = fread(buf, 1, CH, fp)) > 0) h2 = hash(h2, buf, n);
        if (h2 != h) FAIL("fread content %s", f->path);
        if (fseek(fp, 0, SEEK_END) || ftell(fp) != (long)f->size) FAIL("fseek/ftell %s", f->path);
        fclose(fp);
    }
}
static void *worker(void *arg) {
    uint64_t rs = (uintptr_t)arg * 0x9e3779b97f4a7c15ull + 1; uint8_t *buf = malloc(1 << 20 | 4096), *buf2 = malloc(65536);
    for (;;) { pthread_mutex_lock(&g_mu); int i = g_next++; pthread_mutex_unlock(&g_mu); if (i >= g_n) break; check(&g_f[i], &rs, buf, buf2); }
    // fd churn: open / pread / close fast, racing the other threads' table updates
    for (int k = 0; k < 20000 && g_n; k++) {
        file *f = &g_f[rnd(&rs) % (uint32_t)g_n]; int fd = open(f->path, O_RDONLY); if (fd < 0) { FAIL("churn open %s", f->path); continue; }
        uint8_t b[8]; if (f->size && pread(fd, b, 1, 0) != 1) FAIL("churn pread %s", f->path);
        struct stat st; if (fstat(fd, &st) || (uint64_t)st.st_size != f->size) FAIL("churn fstat %s", f->path);
        close(fd);
    }
    free(buf); free(buf2); return NULL;
}
static int cmp(const void *a, const void *b) { return strcmp(((const file *)a)->path, ((const file *)b)->path); }
int main(int argc, char **argv) {
    g_root = argc > 1 ? argv[1] : "/nxapk"; int nt = argc > 2 ? atoi(argv[2]) : 8;
    walk(g_root);
    fprintf(stderr, "files: %d\n", g_n);
#ifndef PLAIN   // the error cases
    char p[2048]; snprintf(p, sizeof p, "%s/RomFS/no-such-file", g_root);
    if (open(p, O_RDONLY) >= 0 || errno != ENOENT) FAIL("missing file did not fail with ENOENT");
    if (g_n && (open(g_f[0].path, O_RDWR) >= 0 || errno != EROFS)) FAIL("write open did not fail with EROFS");
    if (g_n && (fopen(g_f[0].path, "w") || errno != EROFS)) FAIL("fopen w did not fail with EROFS");
    if (access(g_root, R_OK)) FAIL("access root");
    if (g_n && (opendir(g_f[0].path) || errno != ENOTDIR)) FAIL("opendir of a file did not fail with ENOTDIR");
    if (open("/proc/self/stat", O_RDONLY) < 0) FAIL("real open broken");
#endif
    pthread_t th[64]; if (nt > 64) nt = 64;
    for (int i = 0; i < nt; i++) pthread_create(&th[i], NULL, worker, (void *)(uintptr_t)(i + 1));
    for (int i = 0; i < nt; i++) pthread_join(th[i], NULL);
    qsort(g_f, (size_t)g_n, sizeof *g_f, cmp);
    size_t rl = strlen(g_root); uint64_t total = 0;
    for (int i = 0; i < g_n; i++) { printf("%016llx %llu %s\n", (unsigned long long)g_f[i].h, (unsigned long long)g_f[i].size, g_f[i].path + rl); total += g_f[i].size; }
    fprintf(stderr, "checked %d files, %.2f GB, %d failures\n", g_n, total / 1e9, g_err);
    return g_err ? 1 : 0;
}
