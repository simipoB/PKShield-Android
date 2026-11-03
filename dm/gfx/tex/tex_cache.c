// Converted textures kept on disk (tex_cache.h): one file a texture level / layer, named by a hash of its BC blocks, so a texture converted once loads as it is the next time
#include "tex_cache.h"
#include <dirent.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

enum { TC_VERSION = 1 };   // the encoders' version: a change to their output starts a new cache (the files of the old one stop matching)
static const char *g_dir; static long long g_used, g_cap; static int g_ready, g_full;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
typedef struct { char magic[4]; uint32_t version; uint64_t key, size; } tc_head;

static long long dir_size(const char *d) {
    long long n = 0; DIR *dp = opendir(d); if (!dp) return 0; struct dirent *e; char p[1200]; struct stat st;
    while ((e = readdir(dp))) { if (e->d_name[0] == '.') continue; snprintf(p, sizeof p, "%s/%s", d, e->d_name); if (!stat(p, &st)) n += S_ISDIR(st.st_mode) ? dir_size(p) : st.st_size; }
    closedir(dp); return n;
}
static int ready(void) {
    pthread_mutex_lock(&g_lock);
    if (!g_ready) {
        g_ready = 1; g_dir = getenv("DM_TEX_CACHE"); const char *mb = getenv("DM_TEX_CACHE_MB"); g_cap = (mb ? atoll(mb) : 1024) << 20;
        if (g_dir && (!*g_dir || g_cap <= 0)) g_dir = NULL;
        if (g_dir) { mkdir(g_dir, 0700); g_used = dir_size(g_dir); fprintf(stderr, "texture cache: %s, %lld of %lld MB used\n", g_dir, g_used >> 20, g_cap >> 20); }
    }
    pthread_mutex_unlock(&g_lock); return g_dir != NULL;
}
static void path_of(uint64_t key, char *p, size_t n, int mk) {   // <dir>/<first hex digit>/<key>.tex (16 folders, none too large to list)
    snprintf(p, n, "%s/%x", g_dir, (unsigned)(key >> 60)); if (mk) mkdir(p, 0700);
    snprintf(p, n, "%s/%x/%016llx.tex", g_dir, (unsigned)(key >> 60), (unsigned long long)key);
}

uint64_t tex_cache_key(const uint8_t *src, size_t n, int kind, int w, int h, int z) {
    uint64_t k = 0x9E3779B97F4A7C15ull ^ ((uint64_t)TC_VERSION << 56) ^ ((uint64_t)(kind & 0xff) << 48) ^ ((uint64_t)w << 32) ^ ((uint64_t)h << 16) ^ (uint64_t)z;
    size_t i = 0;
    for (; i + 8 <= n; i += 8) { uint64_t v; memcpy(&v, src + i, 8); k = (k ^ v) * 0xFF51AFD7ED558CCDull; k ^= k >> 29; }
    for (; i < n; i++) { k = (k ^ src[i]) * 0x100000001B3ull; }
    k ^= n; k *= 0xC4CEB9FE1A85EC53ull; k ^= k >> 33; return k;
}
int tex_cache_get(uint64_t key, uint8_t *out, size_t n) {
    if (!ready()) return 0;
    char p[1200]; path_of(key, p, sizeof p, 0); FILE *f = fopen(p, "rb"); if (!f) return 0;
    tc_head hd; int ok = fread(&hd, sizeof hd, 1, f) == 1 && !memcmp(hd.magic, "DMTC", 4) && hd.version == TC_VERSION && hd.key == key && hd.size == n && fread(out, 1, n, f) == n;
    fclose(f); if (!ok) remove(p);   // (a damaged or foreign file: converted again and written anew)
    return ok;
}
void tex_cache_put(uint64_t key, const uint8_t *data, size_t n) {
    if (!ready() || g_full) return;
    pthread_mutex_lock(&g_lock);
    int room = g_used + (long long)(n + sizeof(tc_head)) <= g_cap; if (room) g_used += (long long)(n + sizeof(tc_head)); else g_full = 1;
    pthread_mutex_unlock(&g_lock);
    if (!room) { fprintf(stderr, "texture cache: full (%lld MB, DM_TEX_CACHE_MB): new textures are converted each time\n", g_cap >> 20); return; }
    char p[1200], t[1210]; path_of(key, p, sizeof p, 1); snprintf(t, sizeof t, "%s.part", p);
    FILE *f = fopen(t, "wb"); if (!f) return;
    tc_head hd = { { 'D', 'M', 'T', 'C' }, TC_VERSION, key, n };
    int ok = fwrite(&hd, sizeof hd, 1, f) == 1 && fwrite(data, 1, n, f) == n; ok &= fclose(f) == 0;
    if (!ok || rename(t, p)) remove(t);   // (written whole, then renamed: a crash mid-write leaves no half file under the real name)
}
