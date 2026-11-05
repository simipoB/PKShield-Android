// A frame capture's files into one ZIP (framecap_zip.h): RGBA8 pictures as PNG (zlib), text as is, the archive stored (the PNGs are compressed already)
#include "framecap_zip.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

typedef struct { char name[64]; uint32_t crc, size, off; } zent;
struct fczip { FILE *f; zent *e; int n, cap; uint32_t pos; };

fczip *fczip_open(const char *path) {
    FILE *f = fopen(path, "wb"); if (!f) return NULL;
    fczip *z = calloc(1, sizeof *z); z->f = f; return z;
}
static void put16(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void put32(uint8_t *p, uint32_t v) { put16(p, v); put16(p + 2, v >> 16); }
int fczip_add(fczip *z, const char *name, const void *data, size_t n) {
    if (!z || n > 0xFFFFFFF0u) return 0;
    if (z->n == z->cap) { z->cap = z->cap ? z->cap * 2 : 64; z->e = realloc(z->e, z->cap * sizeof *z->e); }
    zent *e = &z->e[z->n++]; snprintf(e->name, sizeof e->name, "%s", name);
    e->crc = (uint32_t)crc32(0, data, (uInt)n); e->size = (uint32_t)n; e->off = z->pos;
    uint8_t h[30] = { 0x50, 0x4b, 3, 4, 20, 0 }; size_t nl = strlen(e->name);
    put32(h + 14, e->crc); put32(h + 18, e->size); put32(h + 22, e->size); put16(h + 26, (uint32_t)nl);
    fwrite(h, 1, 30, z->f); fwrite(e->name, 1, nl, z->f); fwrite(data, 1, n, z->f);
    z->pos += (uint32_t)(30 + nl + n); return 1;
}
int fczip_add_png(fczip *z, const char *name, const uint8_t *rgba, int w, int h) {
    size_t raw = (size_t)h * (1 + (size_t)w * 4); uint8_t *r = malloc(raw); if (!r) return 0;
    for (int y = 0; y < h; y++) { r[y * (1 + (size_t)w * 4)] = 0; memcpy(r + y * (1 + (size_t)w * 4) + 1, rgba + (size_t)y * w * 4, (size_t)w * 4); }
    uLongf cn = compressBound((uLong)raw); uint8_t *c = malloc(cn); if (!c || compress2(c, &cn, r, (uLong)raw, 6) != Z_OK) { free(r); free(c); return 0; }
    free(r);
    size_t total = 8 + 25 + 12 + cn + 12; uint8_t *png = malloc(total), *p = png; if (!png) { free(c); return 0; }
    static const uint8_t sig[8] = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n' }; memcpy(p, sig, 8); p += 8;
    // chunk: length (big-endian), type, data, CRC of type + data
    #define CHUNK(type, data, len) do { uint32_t L_ = (uint32_t)(len); p[0] = L_ >> 24; p[1] = L_ >> 16; p[2] = L_ >> 8; p[3] = L_; memcpy(p + 4, type, 4); if (L_) memcpy(p + 8, data, L_); \
        uint32_t C_ = (uint32_t)crc32(crc32(0, p + 4, 4), p + 8, L_); p[8 + L_] = C_ >> 24; p[9 + L_] = C_ >> 16; p[10 + L_] = C_ >> 8; p[11 + L_] = C_; p += 12 + L_; } while (0)
    uint8_t ih[13] = { (uint8_t)(w >> 24), (uint8_t)(w >> 16), (uint8_t)(w >> 8), (uint8_t)w, (uint8_t)(h >> 24), (uint8_t)(h >> 16), (uint8_t)(h >> 8), (uint8_t)h, 8, 6, 0, 0, 0 };
    CHUNK("IHDR", ih, 13); CHUNK("IDAT", c, cn); CHUNK("IEND", "", 0);
    #undef CHUNK
    int ok = fczip_add(z, name, png, (size_t)(p - png)); free(png); free(c); return ok;
}
int fczip_close(fczip *z) {
    if (!z) return 0;
    uint32_t cd = z->pos, cdn = 0;
    for (int i = 0; i < z->n; i++) {
        zent *e = &z->e[i]; uint8_t h[46] = { 0x50, 0x4b, 1, 2, 20, 0, 20, 0 }; size_t nl = strlen(e->name);
        put32(h + 16, e->crc); put32(h + 20, e->size); put32(h + 24, e->size); put16(h + 28, (uint32_t)nl); put32(h + 42, e->off);
        fwrite(h, 1, 46, z->f); fwrite(e->name, 1, nl, z->f); cdn += (uint32_t)(46 + nl);
    }
    uint8_t end[22] = { 0x50, 0x4b, 5, 6 }; put16(end + 8, (uint32_t)z->n); put16(end + 10, (uint32_t)z->n); put32(end + 12, cdn); put32(end + 16, cd);
    fwrite(end, 1, 22, z->f);
    int ok = !ferror(z->f); fclose(z->f); free(z->e); free(z); return ok;
}
