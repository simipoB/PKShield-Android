// Disk cache for textures converted on upload (etc_encode.h): the conversion costs CPU time, a hitch the first time a texture loads; the cache keeps the result,
// so later loads read it back instead. DM_TEX_CACHE: the folder (none: no cache), DM_TEX_CACHE_MB: its size limit (1024); once full, new textures are not kept
#pragma once
#include <stddef.h>
#include <stdint.h>
uint64_t tex_cache_key(const uint8_t *src, size_t n, int kind, int w, int h, int z);   // the source blocks and their shape
int tex_cache_get(uint64_t key, uint8_t *out, size_t n);       // 1: out filled from the cache
void tex_cache_put(uint64_t key, const uint8_t *data, size_t n);
