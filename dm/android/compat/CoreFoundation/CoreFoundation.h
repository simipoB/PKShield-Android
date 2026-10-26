// Darwin stand-in (dm/android/compat): CFMutableDictionary with pointer keys and values and no callbacks (the only use: hle_nvn.c's program key table), as an open-addressing hash map
#pragma once
#include <stdint.h>
#include <stdlib.h>
typedef struct dm_cfdict { const void **k, **v; size_t cap, n; } *CFMutableDictionaryRef, *CFDictionaryRef;
typedef const void *CFAllocatorRef;
typedef long CFIndex;
static inline size_t dm_cf_h(const void *p, size_t cap) { uint64_t x = (uint64_t)(uintptr_t)p; x ^= x >> 33; x *= 0xff51afd7ed558ccdull; x ^= x >> 33; return (size_t)x & (cap - 1); }
static inline CFMutableDictionaryRef CFDictionaryCreateMutable(CFAllocatorRef a, CFIndex n, const void *kc, const void *vc) {
    (void)a; (void)n; (void)kc; (void)vc; CFMutableDictionaryRef d = calloc(1, sizeof *d); d->cap = 64; d->k = calloc(64, sizeof *d->k); d->v = calloc(64, sizeof *d->v); return d;
}
static inline const void *CFDictionaryGetValue(CFDictionaryRef d, const void *k) {
    for (size_t i = dm_cf_h(k, d->cap);; i = (i + 1) & (d->cap - 1)) { if (d->k[i] == k) return d->v[i]; if (!d->k[i]) return 0; }
}
static inline void dm_cf_put(CFMutableDictionaryRef d, const void *k, const void *v) {
    size_t i = dm_cf_h(k, d->cap); while (d->k[i] && d->k[i] != k) i = (i + 1) & (d->cap - 1);
    if (!d->k[i]) d->n++; d->k[i] = k; d->v[i] = v;
}
static inline void CFDictionarySetValue(CFMutableDictionaryRef d, const void *k, const void *v) {
    if ((d->n + 1) * 2 > d->cap) {
        const void **ok = d->k, **ov = d->v; size_t oc = d->cap; d->cap *= 2; d->n = 0; d->k = calloc(d->cap, sizeof *d->k); d->v = calloc(d->cap, sizeof *d->v);
        for (size_t i = 0; i < oc; i++) if (ok[i]) dm_cf_put(d, ok[i], ov[i]);
        free(ok); free(ov);
    }
    dm_cf_put(d, k, v);
}
static inline void CFDictionaryRemoveValue(CFMutableDictionaryRef d, const void *k) {   // backward-shift deletion keeps probe chains intact
    size_t i = dm_cf_h(k, d->cap); while (d->k[i] != k) { if (!d->k[i]) return; i = (i + 1) & (d->cap - 1); }
    d->k[i] = 0; d->n--;
    for (size_t j = (i + 1) & (d->cap - 1); d->k[j]; j = (j + 1) & (d->cap - 1)) {
        const void *kk = d->k[j], *vv = d->v[j]; d->k[j] = 0; d->n--; dm_cf_put(d, kk, vv);
    }
}
static inline CFIndex CFDictionaryGetCount(CFDictionaryRef d) { return (CFIndex)d->n; }
