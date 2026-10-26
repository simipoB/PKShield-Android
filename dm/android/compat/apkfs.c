// Game files read straight out of the installed APKs (Android): a game too big for one APK (4 GB, the ZIP limit APK signing allows) ships as a base APK and data splits,
// every file stored uncompressed and 16 KB-aligned (a page on 4 KB and on 16 KB kernels) under assets/game/ (dm/android/app/build_app.sh). Paths under /nxapk/ resolve to those entries; the host's file calls
// (open, read, pread, fstat, stat, opendir, readdir, dirfd + fstatat, fopen, mmap, …) take them through the wrappers below, everything else goes to the real calls.
// The index is built once from the APKs' central directories (the APK folder is found through /proc/self/maps; tests can name it with DM_APK_DIR).
// Files under assets/gamez/ (same tree as assets/game/) are compressed: "NXZ1", u32 block size, u64 size, u32 block count, u32 block offsets [count + 1] from the end of
// the table, then the blocks, each a zlib stream or (when its length equals the block's size) raw. Reads decompress the blocks they touch; mmap fills anonymous memory.
#define APKFS_IMPL 1
#define _GNU_SOURCE
#include "apkfs.h"
// darwin.h (force-included) brought the wrappers in as macros: this file calls the real functions
#undef open
#undef close
#undef read
#undef pread
#undef lseek
#undef fstat
#undef stat
#undef lstat
#undef access
#undef lgetxattr
#undef opendir
#undef readdir
#undef closedir
#undef dirfd
#undef fstatat
#undef fopen
#undef mmap
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <zlib.h>

#define PREFIX "/nxapk"   // the folder itself, and everything under it
#define PLEN 6
typedef struct node { char *name; struct node *parent, *child, *next; int dir, apk; uint64_t off, size; uint32_t nchild;
                     uint8_t z, zready; uint32_t bs, nblk, *boff; uint64_t zdata; } node;   // z: compressed (zload fills size, bs, nblk, boff, zdata)
static node g_root = { .name = "", .dir = 1 };
static int g_apkfd[16]; static int g_napk; static pthread_once_t g_once = PTHREAD_ONCE_INIT; static int g_ready;
enum { MAXFD = 65536 };
static struct vfd { node *n; uint64_t pos; } *g_fd;   // virtual file / directory fds (real fds of an APK, so the numbers are unique)
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;

static node *child(node *d, const char *name, size_t len, int create) {
    for (node *c = d->child; c; c = c->next) if (strlen(c->name) == len && !memcmp(c->name, name, len)) return c;
    if (!create) return NULL;
    node *c = calloc(1, sizeof *c); c->name = strndup(name, len); c->parent = d; c->next = d->child; d->child = c; d->nchild++; return c;
}
static uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t le32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static void index_apk(int fd, int ai) {
    struct stat st; if (fstat(fd, &st) || st.st_size < 22) return;
    size_t tail = st.st_size < 66000 ? (size_t)st.st_size : 66000; uint8_t *t = malloc(tail);
    if (pread(fd, t, tail, st.st_size - (off_t)tail) != (ssize_t)tail) { free(t); return; }
    long e = -1; for (long i = (long)tail - 22; i >= 0; i--) if (le32(t + i) == 0x06054b50) { e = i; break; }
    if (e < 0) { free(t); return; }
    uint32_t n = le16(t + e + 10), cdsize = le32(t + e + 12), cdoff = le32(t + e + 16); free(t);
    uint8_t *cd = malloc(cdsize); if (pread(fd, cd, cdsize, cdoff) != (ssize_t)cdsize) { free(cd); return; }
    const char *want = "assets/game/"; size_t wl = strlen(want);   // and assets/gamez/ (compressed)
    for (uint32_t i = 0, p = 0; i < n && p + 46 <= cdsize; i++) {
        uint8_t *h = cd + p; if (le32(h) != 0x02014b50) break;
        uint16_t method = le16(h + 10), nl = le16(h + 28), xl = le16(h + 30), cl = le16(h + 32); uint32_t size = le32(h + 24), lho = le32(h + 42);
        const char *name = (const char *)h + 46; p += 46 + nl + xl + cl;
        int z = nl > wl + 1 && !memcmp(name, "assets/gamez/", wl + 1); size_t pl = wl + (size_t)z;
        if (nl <= pl || (!z && memcmp(name, want, wl)) || method != 0) continue;   // only stored entries are mapped
        uint8_t lh[30]; if (pread(fd, lh, 30, lho) != 30 || le32(lh) != 0x04034b50) continue;
        uint64_t data = (uint64_t)lho + 30 + le16(lh + 26) + le16(lh + 28);
        node *d = &g_root; const char *s = name + pl, *end = name + nl;
        while (s < end) { const char *sl = memchr(s, '/', (size_t)(end - s));
            if (!sl) { node *f = child(d, s, (size_t)(end - s), 1); f->apk = ai; f->off = data; f->size = size; f->z = (uint8_t)z; break; }
            d = child(d, s, (size_t)(sl - s), 1); d->dir = 1; s = sl + 1; }
    }
    free(cd);
}
static void build(void) {
    char dir[1024] = "";
    const char *env = getenv("DM_APK_DIR");
    if (env) snprintf(dir, sizeof dir, "%s", env);
    else { FILE *m = fopen("/proc/self/maps", "r"); char ln[1200];
        while (m && fgets(ln, sizeof ln, m)) { char *p = strstr(ln, "/base.apk"); if (p) { char *s = strchr(ln, '/'); *p = 0; snprintf(dir, sizeof dir, "%s", s); break; } }
        if (m) fclose(m); }
    if (!dir[0]) return;
    DIR *d = opendir(dir); struct dirent *e;
    while (d && (e = readdir(d)) && g_napk < 16) {
        size_t l = strlen(e->d_name); if (l < 4 || strcmp(e->d_name + l - 4, ".apk")) continue;
        char p[1400]; snprintf(p, sizeof p, "%s/%s", dir, e->d_name);
        int fd = open(p, O_RDONLY | O_CLOEXEC); if (fd < 0) continue;
        g_apkfd[g_napk] = fd; index_apk(fd, g_napk); g_napk++;
    }
    if (d) closedir(d);
    g_fd = calloc(MAXFD, sizeof *g_fd);
    g_ready = 1;
    fprintf(stderr, "[dm] apkfs: %d APKs in %s, %u top-level entries\n", g_napk, dir, g_root.nchild);
}
int apkfs_is(const char *path);
static node *lookup(const char *path) {
    if (!apkfs_is(path)) return NULL;
    pthread_once(&g_once, build); if (!g_ready) return NULL;
    node *n = &g_root; const char *s = path + PLEN;
    while (*s) { while (*s == '/') s++; if (!*s) break; const char *sl = strchr(s, '/'); size_t l = sl ? (size_t)(sl - s) : strlen(s);
        if (l == 1 && *s == '.') { s += l; continue; }
        n = child(n, s, l, 0); if (!n) return NULL; s += l; }
    return n;
}
int apkfs_is(const char *path) { return path && !strncmp(path, PREFIX, PLEN) && (path[PLEN] == '/' || !path[PLEN]); }
// A compressed entry's header and block table, read on first use
static int zload(node *n) {
    if (!n->z || __atomic_load_n(&n->zready, __ATOMIC_ACQUIRE)) return 0;
    pthread_mutex_lock(&g_mu);
    if (!n->zready) {
        uint8_t h[20]; int fd = g_apkfd[n->apk];
        if (pread(fd, h, 20, (off_t)n->off) != 20 || memcmp(h, "NXZ1", 4)) { pthread_mutex_unlock(&g_mu); fprintf(stderr, "[dm] [RED] apkfs: %s is not a compressed entry\n", n->name); errno = EIO; return -1; }
        uint32_t bs = le32(h + 4), nb = le32(h + 16); uint64_t raw = (uint64_t)le32(h + 8) | (uint64_t)le32(h + 12) << 32;
        uint32_t *t = malloc(4 * ((size_t)nb + 1));
        if (pread(fd, t, 4 * ((size_t)nb + 1), (off_t)n->off + 20) != (ssize_t)(4 * ((size_t)nb + 1))) { free(t); pthread_mutex_unlock(&g_mu); errno = EIO; return -1; }
        n->bs = bs; n->nblk = nb; n->boff = t; n->zdata = n->off + 20 + 4 * ((uint64_t)nb + 1); n->size = raw;
        __atomic_store_n(&n->zready, 1, __ATOMIC_RELEASE);
    }
    pthread_mutex_unlock(&g_mu);
    return 0;
}
// One decompressed block per thread (sequential small reads hit it)
static __thread struct { node *n; uint32_t b, len; uint8_t *buf, *cbuf; size_t cap, ccap; } t_blk;
static ssize_t zread(node *n, uint8_t *out, size_t len, uint64_t off) {
    if (zload(n)) return -1;
    if (off >= n->size) return 0;
    if (len > n->size - off) len = (size_t)(n->size - off);
    size_t got = 0;
    while (got < len) {
        uint64_t at = off + got; uint32_t b = (uint32_t)(at / n->bs), in = (uint32_t)(at % n->bs);
        uint32_t rawlen = (uint32_t)((uint64_t)(b + 1) * n->bs > n->size ? n->size - (uint64_t)b * n->bs : n->bs);
        if (t_blk.n != n || t_blk.b != b) {
            uint32_t clen = n->boff[b + 1] - n->boff[b];
            if (t_blk.cap < n->bs) { free(t_blk.buf); t_blk.buf = malloc(n->bs); t_blk.cap = n->bs; }
            if (clen == rawlen) { if (pread(g_apkfd[n->apk], t_blk.buf, rawlen, (off_t)(n->zdata + n->boff[b])) != (ssize_t)rawlen) { t_blk.n = NULL; errno = EIO; return got ? (ssize_t)got : -1; } }
            else {
                if (t_blk.ccap < clen) { free(t_blk.cbuf); t_blk.cbuf = malloc(clen); t_blk.ccap = clen; }
                uLongf dl = rawlen;
                if (pread(g_apkfd[n->apk], t_blk.cbuf, clen, (off_t)(n->zdata + n->boff[b])) != (ssize_t)clen || uncompress(t_blk.buf, &dl, t_blk.cbuf, clen) != Z_OK || dl != rawlen) {
                    t_blk.n = NULL; fprintf(stderr, "[dm] [RED] apkfs: block %u of %s does not decompress\n", b, n->name); errno = EIO; return got ? (ssize_t)got : -1; }
            }
            t_blk.n = n; t_blk.b = b; t_blk.len = rawlen;
        }
        size_t k = rawlen - in; if (k > len - got) k = len - got;
        memcpy(out + got, t_blk.buf + in, k); got += k;
    }
    return (ssize_t)got;
}
static void fill(node *n, struct stat *st) {
    zload(n);
    memset(st, 0, sizeof *st); st->st_mode = n->dir ? S_IFDIR | 0555 : S_IFREG | 0444; st->st_size = (off_t)n->size; st->st_nlink = 1; st->st_blksize = 4096; st->st_blocks = (blkcnt_t)((n->size + 511) / 512);
    st->st_ino = (ino_t)(uintptr_t)n;
}
static struct vfd *vf(int fd) { return g_fd && fd >= 0 && fd < MAXFD && g_fd[fd].n ? &g_fd[fd] : NULL; }
static int vopen(node *n) {
    int fd = dup(g_apkfd[n->dir ? 0 : n->apk]); if (fd < 0) return -1;
    if (fd >= MAXFD) { close(fd); errno = EMFILE; return -1; }
    pthread_mutex_lock(&g_mu); g_fd[fd].n = n; g_fd[fd].pos = 0; pthread_mutex_unlock(&g_mu);
    return fd;
}
int dm_open(const char *path, int flags, ...) {
    va_list ap; va_start(ap, flags); int mode = (flags & O_CREAT) ? va_arg(ap, int) : 0; va_end(ap);
    if (!apkfs_is(path)) return open(path, flags, mode);
    node *n = lookup(path); if (!n) { errno = ENOENT; return -1; }
    if ((flags & O_ACCMODE) != O_RDONLY || (flags & (O_CREAT | O_TRUNC))) { errno = EROFS; return -1; }
    return vopen(n);
}
int dm_close(int fd) { if (vf(fd)) { pthread_mutex_lock(&g_mu); g_fd[fd].n = NULL; pthread_mutex_unlock(&g_mu); } return close(fd); }
ssize_t dm_pread(int fd, void *buf, size_t n, off_t off) {
    struct vfd *v = vf(fd); if (!v) return pread(fd, buf, n, off);
    if (v->n->dir) { errno = EISDIR; return -1; }
    if (off < 0) { errno = EINVAL; return -1; }
    if (v->n->z) return zread(v->n, buf, n, (uint64_t)off);
    if ((uint64_t)off >= v->n->size) return 0;
    if (n > v->n->size - (uint64_t)off) n = (size_t)(v->n->size - (uint64_t)off);
    return pread(fd, buf, n, (off_t)(v->n->off + (uint64_t)off));
}
ssize_t dm_read(int fd, void *buf, size_t n) {
    struct vfd *v = vf(fd); if (!v) return read(fd, buf, n);
    ssize_t r = dm_pread(fd, buf, n, (off_t)v->pos); if (r > 0) v->pos += (uint64_t)r; return r;
}
off_t dm_lseek(int fd, off_t off, int whence) {
    struct vfd *v = vf(fd); if (!v) return lseek(fd, off, whence);
    if (whence == SEEK_END) zload(v->n);
    int64_t p = whence == SEEK_SET ? off : whence == SEEK_CUR ? (int64_t)v->pos + off : (int64_t)v->n->size + off;
    if (p < 0) { errno = EINVAL; return -1; } v->pos = (uint64_t)p; return (off_t)p;
}
int dm_fstat(int fd, struct stat *st) { struct vfd *v = vf(fd); if (!v) return fstat(fd, st); fill(v->n, st); return 0; }
int dm_stat(const char *path, struct stat *st) { if (!apkfs_is(path)) return stat(path, st); node *n = lookup(path); if (!n) { errno = ENOENT; return -1; } fill(n, st); return 0; }
int dm_lstat(const char *path, struct stat *st) { if (!apkfs_is(path)) return lstat(path, st); return dm_stat(path, st); }
int dm_access(const char *path, int mode) { if (!apkfs_is(path)) return access(path, mode); if (!lookup(path)) { errno = ENOENT; return -1; } if (mode & W_OK) { errno = EROFS; return -1; } return 0; }
ssize_t dm_lgetxattr(const char *path, const char *name, void *v, size_t n) { if (!apkfs_is(path)) return lgetxattr(path, name, v, n); errno = ENODATA; return -1; }
// directories: a DIR handle of our own, told apart from bionic's by its magic (bionic's DIR starts with the directory fd)
typedef struct { uint32_t magic; node *n, *cur; int fd, dot; struct dirent ent; } vdir;
#define VDIR_MAGIC 0x564b4446u
static int isv(DIR *d) { return d && *(uint32_t *)(void *)d == VDIR_MAGIC; }
DIR *dm_opendir(const char *path) {
    if (!apkfs_is(path)) return opendir(path);
    node *n = lookup(path); if (!n) { errno = ENOENT; return NULL; } if (!n->dir) { errno = ENOTDIR; return NULL; }
    vdir *d = calloc(1, sizeof *d); d->magic = VDIR_MAGIC; d->n = n; d->cur = n->child; d->fd = -1; return (DIR *)(void *)d;
}
struct dirent *dm_readdir(DIR *dd) {
    if (!isv(dd)) return readdir(dd);
    vdir *d = (vdir *)(void *)dd;
    if (d->dot < 2) { memset(&d->ent, 0, sizeof d->ent); strcpy(d->ent.d_name, d->dot ? ".." : "."); d->ent.d_type = DT_DIR; d->dot++; return &d->ent; }
    if (!d->cur) return NULL;
    node *c = d->cur; d->cur = c->next;
    memset(&d->ent, 0, sizeof d->ent); snprintf(d->ent.d_name, sizeof d->ent.d_name, "%s", c->name); d->ent.d_type = c->dir ? DT_DIR : DT_REG; d->ent.d_ino = (ino_t)(uintptr_t)c;
    return &d->ent;
}
int dm_closedir(DIR *dd) { if (!isv(dd)) return closedir(dd); vdir *d = (vdir *)(void *)dd; if (d->fd >= 0) dm_close(d->fd); free(d); return 0; }
int dm_dirfd(DIR *dd) { if (!isv(dd)) return dirfd(dd); vdir *d = (vdir *)(void *)dd; if (d->fd < 0) d->fd = vopen(d->n); return d->fd; }
int dm_fstatat(int dfd, const char *name, struct stat *st, int flags) {
    struct vfd *v = vf(dfd); if (!v || !v->n->dir) return fstatat(dfd, name, st, flags);
    node *n = v->n; const char *s = name;
    while (*s) { const char *sl = strchr(s, '/'); size_t l = sl ? (size_t)(sl - s) : strlen(s); n = child(n, s, l, 0); if (!n) { errno = ENOENT; return -1; } s += l; while (*s == '/') s++; }
    fill(n, st); return 0;
}
// fopen of an entry: a read-only stream over the APK through funopen
typedef struct { int fd; } vcookie;
static int fo_read(void *c, char *b, int n) { ssize_t r = dm_read(((vcookie *)c)->fd, b, (size_t)n); return (int)r; }
static fpos_t fo_seek(void *c, fpos_t o, int w) { return (fpos_t)dm_lseek(((vcookie *)c)->fd, (off_t)o, w); }
static int fo_close(void *c) { int r = dm_close(((vcookie *)c)->fd); free(c); return r; }
FILE *dm_fopen(const char *path, const char *mode) {
    if (!apkfs_is(path)) return fopen(path, mode);
    if (strchr(mode, 'w') || strchr(mode, 'a') || strchr(mode, '+')) { errno = EROFS; return NULL; }
    int fd = dm_open(path, O_RDONLY); if (fd < 0) return NULL;
    vcookie *c = malloc(sizeof *c); c->fd = fd;
    FILE *f = funopen(c, fo_read, NULL, fo_seek, fo_close); if (!f) { dm_close(fd); free(c); }
    return f;
}
void *dm_mmap(void *addr, size_t len, int prot, int flags, int fd, off_t off) {
    struct vfd *v = vf(fd); if (!v) return mmap(addr, len, prot, flags, fd, off);
    if ((prot & PROT_WRITE) && (flags & MAP_SHARED)) { errno = EROFS; return MAP_FAILED; }
    if (v->n->z) {   // compressed: anonymous memory filled by reading, then the asked protection
        if (prot & PROT_WRITE && flags & MAP_SHARED) { errno = EROFS; return MAP_FAILED; }
        void *m = mmap(addr, len, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | (flags & MAP_FIXED), -1, 0); if (m == MAP_FAILED) return m;
        ssize_t r = zread(v->n, m, len, (uint64_t)off); if (r < 0) { munmap(m, len); return MAP_FAILED; }
        if (prot != (PROT_READ | PROT_WRITE)) mprotect(m, len, prot);
        return m;
    }
    uint64_t at = v->n->off + (uint64_t)off;
    if (at & ((uint64_t)getpagesize() - 1)) { errno = EINVAL; return MAP_FAILED; }   // entries are stored 16 KB-aligned (zipalign 16384): whole pages on 4 KB and 16 KB kernels
    return mmap(addr, len, prot, flags, fd, (off_t)at);
}
