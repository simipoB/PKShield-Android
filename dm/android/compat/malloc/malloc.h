// Darwin stand-in (dm/android/compat)
#pragma once
#include_next <malloc.h>
#include <stddef.h>
static inline size_t malloc_size(const void *p) { return malloc_usable_size(p); }
static inline size_t malloc_good_size(size_t n) { return n; }
typedef struct malloc_zone malloc_zone_t;
static inline malloc_zone_t *malloc_create_zone(size_t a, unsigned f) { (void)a; (void)f; return (malloc_zone_t *)1; }
static inline void malloc_set_zone_name(malloc_zone_t *z, const char *n) { (void)z; (void)n; }
static inline void *malloc_zone_malloc(malloc_zone_t *z, size_t n) { (void)z; return malloc(n); }
static inline void *malloc_zone_realloc(malloc_zone_t *z, void *p, size_t n) { (void)z; return realloc(p, n); }
static inline void *malloc_zone_memalign(malloc_zone_t *z, size_t a, size_t n) { (void)z; return memalign(a, n); }
static inline void malloc_zone_free(malloc_zone_t *z, void *p) { (void)z; free(p); }
