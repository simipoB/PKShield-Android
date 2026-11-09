// The memory pools by base address: a sorted index of the pool table, for a binary search (nvn_vk.c pool_find; test: gfx/tests/t_pool_index.c). Sword / Shield keep
// about 3,300 pools and every draw looks up its vertex streams and the uniform / storage buffers of every stage; a linear scan walked the whole table whenever an address
// was not in the last pool found. The includer defines PS_BASE(i) / PS_SIZE(i), the base address and size of the table's entry i, and keeps the index in step with the
// table: ps_add after appending entry i, ps_remove(i) before entry i goes, ps_moved(from, to) after entry `from` was moved into slot `to`. Pools never overlap
#pragma once
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static int *g_psix, g_psix_n, g_psix_cap;   // table indices by base; g_psix_n entries (the table's count when in step)
static int ps_lower(uint64_t base) {   // the first position whose base is at or above base
    int lo = 0, hi = g_psix_n;
    while (lo < hi) { int m = (lo + hi) / 2; if ((uint64_t)PS_BASE(g_psix[m]) < base) lo = m + 1; else hi = m; }
    return lo;
}
static int ps_pos(int idx, uint64_t base) {   // where table entry idx sits in the index
    int p = ps_lower(base); while (p < g_psix_n && g_psix[p] != idx && (uint64_t)PS_BASE(g_psix[p]) == base) p++;
    if (p < g_psix_n && g_psix[p] == idx) return p;
    for (p = 0; p < g_psix_n; p++) if (g_psix[p] == idx) return p;   // (not where its base says: found the slow way)
    return -1;
}
static void ps_add(int idx) {
    if (g_psix_n == g_psix_cap) { int c = g_psix_cap ? g_psix_cap * 2 : 256; int *n = realloc(g_psix, (size_t)c * sizeof *g_psix); if (!n) return; g_psix = n; g_psix_cap = c; }
    int p = ps_lower((uint64_t)PS_BASE(idx)); memmove(g_psix + p + 1, g_psix + p, (size_t)(g_psix_n - p) * sizeof *g_psix); g_psix[p] = idx; g_psix_n++;
}
static void ps_remove(int idx) {
    int p = ps_pos(idx, (uint64_t)PS_BASE(idx)); if (p < 0) return;
    memmove(g_psix + p, g_psix + p + 1, (size_t)(g_psix_n - p - 1) * sizeof *g_psix); g_psix_n--;
}
static void ps_moved(int from, int to) { int p = ps_pos(from, (uint64_t)PS_BASE(to)); if (p >= 0) g_psix[p] = to; }   // (entry `to` is now what `from` was)
// The table entry holding [addr, addr + len): the last pool whose base is at or below addr, when it holds the whole range; -1 when none does
static int ps_find(uint64_t addr, uint64_t len) {
    int lo = 0, hi = g_psix_n;
    while (lo < hi) { int m = (lo + hi) / 2; if ((uint64_t)PS_BASE(g_psix[m]) <= addr) lo = m + 1; else hi = m; }
    if (!lo) return -1;
    int i = g_psix[lo - 1];
    return addr + len <= (uint64_t)PS_BASE(i) + (uint64_t)PS_SIZE(i) ? i : -1;
}
