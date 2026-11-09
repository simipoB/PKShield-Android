// The sorted pool index (nvn/pool_index.h) against the linear scan it replaces: a table changed the way nvn_vk.c changes g_pools (append; remove by moving the last entry
// into the gap), random pools that never overlap, and after every change the index's order and contents checked and lookups (inside, at the edges, past the end,
// outside every pool, ranges running past a pool's end) compared with the scan. Then the time per lookup at Sword's ~3,300 pools.
// cc -O2 -I../nvn -o /tmp/t_pool_index t_pool_index.c && /tmp/t_pool_index
#include <stdint.h>
#include <stdio.h>
#include <time.h>
static uint64_t T_base[8192], T_size[8192]; static int T_n;
#define PS_BASE(i) (T_base[i])
#define PS_SIZE(i) (T_size[i])
#include "pool_index.h"

static uint32_t rs = 12345u; static uint32_t rnd(void) { rs ^= rs << 13; rs ^= rs >> 17; rs ^= rs << 5; return rs; }
static int linear(uint64_t a, uint64_t len) { for (int i = 0; i < T_n; i++) if (a >= T_base[i] && a + len <= T_base[i] + T_size[i]) return i; return -1; }
static unsigned char used[200000];   // slot k (1 MB each) holds a pool: pools never overlap
static void add(void) { int k; do k = (int)(rnd() % 200000); while (used[k]); used[k] = 1; T_base[T_n] = 0x7000000000ull + (uint64_t)k * 0x100000; T_size[T_n] = 0x1000 * (1 + rnd() % 255); T_n++; ps_add(T_n - 1); }
static void del(int i) { used[(T_base[i] - 0x7000000000ull) / 0x100000] = 0; ps_remove(i); int last = --T_n; T_base[i] = T_base[last]; T_size[i] = T_size[last]; if (i != last) ps_moved(last, i); }
static int consistent(void) {
    if (g_psix_n != T_n) return 0;
    static unsigned char seen[8192]; memset(seen, 0, sizeof seen);
    for (int p = 0; p < g_psix_n; p++) { int i = g_psix[p]; if (i < 0 || i >= T_n || seen[i]++) return 0; if (p && T_base[g_psix[p - 1]] >= T_base[i]) return 0; }
    return 1;
}
static int lookups_agree(int n) {
    for (int k = 0; k < n; k++) {
        uint64_t a, len = 1 + rnd() % 64;
        switch (rnd() % 5) {
            case 0: a = 0x7000000000ull + (uint64_t)(rnd() % 200000) * 0x100000 + rnd() % 0x100000; break;   // anywhere: often outside every pool
            case 1: { int i = (int)(rnd() % (unsigned)(T_n ? T_n : 1)); a = T_n ? T_base[i] : 0x7000000000ull; break; }   // a pool's first byte
            case 2: { int i = (int)(rnd() % (unsigned)(T_n ? T_n : 1)); a = T_n ? T_base[i] + T_size[i] - len : 0; break; }   // ends at a pool's last byte
            case 3: { int i = (int)(rnd() % (unsigned)(T_n ? T_n : 1)); a = T_n ? T_base[i] + T_size[i] - len + 1 : 0; break; }   // runs one byte past a pool's end
            default: a = rnd() % 0x1000; break;   // below every pool
        }
        if (ps_find(a, len) != linear(a, len)) { printf("  differ at %#llx+%llu: index %d, scan %d\n", (unsigned long long)a, (unsigned long long)len, ps_find(a, len), linear(a, len)); return 0; }
    }
    return 1;
}
static int checks, fails;
#define CHECK(c, s) do { checks++; int ok_ = !!(c); fails += !ok_; printf("%s %s\n", ok_ ? "✓" : "✗", s); } while (0)
static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec / 1e9; }

int main(void) {
    CHECK(ps_find(0x7000000000ull, 1) == -1 && consistent(), "empty table: nothing found, index empty");
    int ok = 1, agree = 1;
    for (int step = 0; step < 20000; step++) {   // grow to ~3,300 pools with removals mixed in, then shrink
        if (step < 12000 ? rnd() % 4 != 0 : (T_n && rnd() % 3 != 0)) { if (T_n < 8000) add(); }
        else if (T_n) del((int)(rnd() % (unsigned)T_n));
        if (!consistent()) ok = 0;
        if (step % 50 == 0 && !lookups_agree(200)) agree = 0;
    }
    CHECK(ok, "20,000 adds and removals: the index stays sorted and holds each pool once");
    CHECK(agree, "80,000 lookups along the way: the same pool as the scan (inside, edges, past the end, outside, below)");
    while (T_n < 3300) add();
    while (T_n > 3300) del((int)(rnd() % (unsigned)T_n));
    CHECK(consistent() && lookups_agree(20000), "3,300 pools (Sword's count): 20,000 more lookups agree");
    // time per lookup: random addresses in random pools, as the last-pool shortcut misses
    uint64_t *as = malloc(sizeof *as * 200000); for (int k = 0; k < 200000; k++) { int i = (int)(rnd() % (unsigned)T_n); as[k] = T_base[i] + rnd() % T_size[i]; }
    volatile long sink = 0; double t0 = now(); for (int k = 0; k < 200000; k++) sink += linear(as[k], 1);
    double t1 = now(); for (int k = 0; k < 200000; k++) sink += ps_find(as[k], 1); double t2 = now();
    printf("  at %d pools: scan %.0f ns a lookup, index %.0f ns (%.0fx)\n", T_n, (t1 - t0) / 200000 * 1e9, (t2 - t1) / 200000 * 1e9, (t1 - t0) / (t2 - t1));
    CHECK((t2 - t1) * 20 < (t1 - t0), "the index is at least 20x faster than the scan at 3,300 pools");
    printf("%d checks, %d failed\n", checks, fails);
    return fails != 0;
}
