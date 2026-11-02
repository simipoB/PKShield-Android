#!/usr/bin/env python3
import os
import subprocess
import sys
import tempfile
import unittest

REPO = os.path.abspath(os.path.join(os.path.dirname(__file__), "../.."))
DM = os.path.join(REPO, "dm")

C_HARNESS = r"""
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "dm.h"
#include "nvn_ovl.h"
#include "nvn_obj.h"
#include "../gfx/tex/nvn_deswizzle.h"

struct dm_flags g_flag;
uint64_t now_ns(void) { return 0; }
uint64_t resolve_import(module_t *m, const char *name) { (void)m; (void)name; return 0; }
int hle_implements(const char *name) { (void)name; return 0; }
void gate_register(uint64_t r, const char *k, int a, uint64_t o) { (void)r; (void)k; (void)a; (void)o; }
void *hlecount_wrap(const char *k, const char *n, void *f) { (void)k; (void)n; return f; }
void hlecount_seal(void) {}
uint8_t *getsectiondata(const void *mh, const char *seg, const char *sect, unsigned long *sz) {
    (void)mh; (void)seg; (void)sect; if (sz) *sz = 0; return NULL;
}
int CC_SHA256_Init(void *c) { (void)c; return 1; }
int CC_SHA256_Update(void *c, const void *d, uint32_t l) { (void)c; (void)d; (void)l; return 1; }
int CC_SHA256_Final(unsigned char *md, void *c) { (void)md; (void)c; return 1; }

static int rt_same_ref(const p_set_rt *a, const p_set_rt *b) {
    if (a->n != b->n || !a->depth != !b->depth || (a->depth && a->depth->serial != b->depth->serial) || a->has_dview != b->has_dview) return 0;
    if (a->has_dview && memcmp(&a->dview, &b->dview, sizeof a->dview)) return 0;
    for (int i = 0; i < a->n && i < 8; i++) {
        if (!a->color[i] != !b->color[i] || (a->color[i] && a->color[i]->serial != b->color[i]->serial) || a->has_cview[i] != b->has_cview[i]) return 0;
        if (a->has_cview[i] && memcmp(&a->cview[i], &b->cview[i], sizeof a->cview[i])) return 0;
    }
    return 1;
}

int main(int argc, char **argv) {
    assert(argc == 3);
    int32_t s[4], d[4];

    // 1. 16:9 fit on a 2400x1080 (20:9) mobile screen: 240 px side bars
    dm_present_rect(DM_ASPECT_FIT, 1920, 1080, NULL, 2400, 1080, s, d);
    assert(s[0] == 0 && s[1] == 0 && s[2] == 1920 && s[3] == 1080);
    assert(d[0] == 240 && d[1] == 0 && d[2] == 1920 && d[3] == 1080);

    // 2. 20:9 mode on 2400x1080 (20:9), 2520x1080 (21:9), and 2340x1080 (19.5:9)
    dm_present_rect(DM_ASPECT_20_9, 1920, 1080, NULL, 2400, 1080, s, d);
    assert(s[0] == 0 && s[1] == 0 && s[2] == 1920 && s[3] == 1080);
    assert(d[0] == 0 && d[1] == 0 && d[2] == 2400 && d[3] == 1080);

    dm_present_rect(DM_ASPECT_20_9, 1920, 1080, NULL, 2520, 1080, s, d);
    assert(d[0] == 60 && d[1] == 0 && d[2] == 2400 && d[3] == 1080);

    dm_present_rect(DM_ASPECT_20_9, 1920, 1080, NULL, 2340, 1080, s, d);
    assert(d[0] == 0 && d[1] == 13 && d[2] == 2340 && d[3] == 1053);

    // 3. Crop (20:9 Zoom) on 2400x1080: exact 1:1 pixel ratio (1920/864 == 20/9), no stretching
    dm_present_rect(DM_ASPECT_CROP, 1920, 1080, NULL, 2400, 1080, s, d);
    assert(s[0] == 0 && s[1] == 108 && s[2] == 1920 && s[3] == 864);
    assert(d[0] == 0 && d[1] == 0 && d[2] == 2400 && d[3] == 1080);

    // 4. Touch menu tap mapping across fit, 20:9, and crop
    float lx = 0, ly = 0;
    assert(!dm_menu_tap_layout(DM_ASPECT_FIT, 100.0f, 540.0f, 2400.0f, 1080.0f, &lx, &ly));
    assert(dm_menu_tap_layout(DM_ASPECT_FIT, 1200.0f, 540.0f, 2400.0f, 1080.0f, &lx, &ly));
    assert(fabsf(lx) < 0.01f && fabsf(ly) < 0.01f);

    assert(dm_menu_tap_layout(DM_ASPECT_20_9, 600.0f, 540.0f, 2400.0f, 1080.0f, &lx, &ly));
    assert(fabsf(lx - (-480.0f)) < 0.01f && fabsf(ly) < 0.01f);

    assert(dm_menu_tap_layout(DM_ASPECT_CROP, 1200.0f, 0.0f, 2400.0f, 1080.0f, &lx, &ly));
    assert(fabsf(lx) < 0.01f && fabsf(ly - 432.0f) < 0.01f);

    // 5. IPSwitch .pchtxt loader: build ID check, offset_shift, @disabled, @stop, and rewritten-stub guard
    uint8_t img[0x2000];
    memset(img, 0xaa, sizeof img);
    // Place a rewritten unconditional branch at +0x200 targeting the patch area at +0x4000
    uint32_t branch_to_patch = 0x14000000u | (((0x4000 - 0x200) >> 2) & 0x3ffffffu);
    memcpy(img + 0x200, &branch_to_patch, 4);

    module_t m;
    memset(&m, 0, sizeof m);
    strcpy(m.name, "main");
    strcpy(m.build_id, "a16802625e7826bf83b6f9708e475b91");
    m.base = img;
    m.text_end = 0x1000;

    // Wrong build ID must not apply anything
    assert(dm_pchtxt_apply_file(&m, sizeof img, 0x4000, 0x8000, argv[2]) == 0);
    assert(img[0x100] == 0xaa);

    // Matching build ID applies 20:9 MOVZ/MOVK at +0x100/+0x104, skips rewritten branch at +0x200, skips @disabled and after @stop
    assert(dm_pchtxt_apply_file(&m, sizeof img, 0x4000, 0x8000, argv[1]) == 2);
    uint8_t expect[8] = { 0x89, 0x1c, 0x9c, 0x52, 0xc9, 0x01, 0xa8, 0x72 };
    assert(memcmp(img + 0x100, expect, 8) == 0);
    uint32_t kept = 0;
    memcpy(&kept, img + 0x200, 4);
    assert(kept == branch_to_patch);
    assert(img[0x300] == 0xaa && img[0x304] == 0xaa);

    // 6. Built-in 20:9 camera patch + Shield 1.3.2 60 FPS patch (dm_apply_builtin_patches)
    size_t big_sz = 0x02100000;
    uint8_t *big = calloc(1, big_sz);
    assert(big != NULL);
    const uint32_t movk_16_9[2] = { 0xf2d1c729u, 0xf2e7fc69u };
    const uint32_t movk_20_9[2] = { 0xf2c71c89u, 0xf2e801c9u };
    const uint32_t orig_itv = 0x52800041u, want_itv = 0x52800021u;
    const uint32_t orig_step = 0x01fca056u, want_step = 0x00fe502bu;
    memcpy(big + 0x50000, movk_16_9, 8);
    memcpy(big + 0x018a2d18, &orig_itv, 4);
    memcpy(big + 0x0203eeb8, &orig_step, 4);

    module_t bm;
    memset(&bm, 0, sizeof bm);
    strcpy(bm.name, "main");
    strcpy(bm.build_id, "a16802625e7826bf83b6f9708e475b9100000000");
    bm.base = big;
    bm.text_end = 0x01f00000;

    // 16:9 default + 30 FPS default: neither patch triggers
    setenv("DM_ASPECT", "fit", 1);
    unsetenv("DM_FPS_TARGET");
    unsetenv("DM_FPS");
    assert(dm_apply_builtin_patches(&bm, big_sz, 0x03000000, 0x03100000, 0) == 0);
    assert(memcmp(big + 0x50000, movk_16_9, 8) == 0);

    // 20:9 with pchtxt_applied=1: skips built-in camera patch so external .pchtxt wins
    setenv("DM_ASPECT", "20:9", 1);
    assert(dm_apply_builtin_patches(&bm, big_sz, 0x03000000, 0x03100000, 1) == 0);
    assert(memcmp(big + 0x50000, movk_16_9, 8) == 0);

    // Duplicate 16:9 MOVK pair: refused (ambiguous)
    memcpy(big + 0x60000, movk_16_9, 8);
    assert(dm_apply_builtin_patches(&bm, big_sz, 0x03000000, 0x03100000, 0) == 0);
    memset(big + 0x60000, 0, 8);

    // Unique 16:9 MOVK pair + DM_FPS_TARGET=60: applies both 20:9 camera (1 site) and 60 FPS (2 sites)
    setenv("DM_FPS_TARGET", "60", 1);
    assert(dm_apply_builtin_patches(&bm, big_sz, 0x03000000, 0x03100000, 0) == 3);
    assert(memcmp(big + 0x50000, movk_20_9, 8) == 0);
    uint32_t got_itv = 0, got_step = 0;
    memcpy(&got_itv, big + 0x018a2d18, 4);
    memcpy(&got_step, big + 0x0203eeb8, 4);
    assert(got_itv == want_itv && got_step == want_step);
    assert(dm_step_hz == 60 && dm_step_word == big + 0x0203eeb8);

    // 120 FPS: step = ceil(1e9 / 120) = 8,333,334 ns; out-of-range targets (30, 121) leave the code untouched
    memcpy(big + 0x018a2d18, &orig_itv, 4); memcpy(big + 0x0203eeb8, &orig_step, 4);
    setenv("DM_FPS_TARGET", "30", 1);
    assert(dm_apply_builtin_patches(&bm, big_sz, 0x03000000, 0x03100000, 0) == 0);
    setenv("DM_FPS_TARGET", "121", 1);
    assert(dm_apply_builtin_patches(&bm, big_sz, 0x03000000, 0x03100000, 0) == 0);
    memcpy(&got_step, big + 0x0203eeb8, 4); assert(got_step == orig_step);
    setenv("DM_FPS_TARGET", "120", 1);
    assert(dm_apply_builtin_patches(&bm, big_sz, 0x03000000, 0x03100000, 0) == 2);   // the 20:9 camera pair is already patched above
    memcpy(&got_itv, big + 0x018a2d18, 4); memcpy(&got_step, big + 0x0203eeb8, 4);
    assert(got_itv == want_itv && got_step == 8333334u && dm_step_hz == 120);
    free(big);

    // Sword 1.3.2 build ID (a3b75bcd3311385a) 60 FPS patch
    uint8_t *sword_big = calloc(1, big_sz);
    memcpy(sword_big + 0x018a2d18, &orig_itv, 4);
    memcpy(sword_big + 0x0203eeb8, &orig_step, 4);
    module_t swm; memset(&swm, 0, sizeof swm);
    strcpy(swm.name, "main");
    strcpy(swm.build_id, "a3b75bcd3311385a0000000000000000");
    swm.base = sword_big;
    swm.text_end = 0x01f00000;
    setenv("DM_FPS_TARGET", "60", 1);
    setenv("DM_ASPECT", "fit", 1);
    assert(dm_apply_builtin_patches(&swm, big_sz, 0x03000000, 0x03100000, 0) == 2);
    memcpy(&got_itv, sword_big + 0x018a2d18, 4); memcpy(&got_step, sword_big + 0x0203eeb8, 4);
    assert(got_itv == want_itv && got_step == want_step && dm_step_hz == 60);
    free(sword_big);

    // 7. rt_same serial comparison across distinct tex_t snapshots (hle_nvn copies tex_t per SetRenderTargets)
    tex_t ta1 = { .serial = 42 }, ta2 = { .serial = 42 }, tb = { .serial = 99 }, da1 = { .serial = 7 }, da2 = { .serial = 7 };
    p_set_rt r1 = { .n = 1, .color = { &ta1 }, .depth = &da1 };
    p_set_rt r2 = { .n = 1, .color = { &ta2 }, .depth = &da2 };
    assert(&ta1 != &ta2 && rt_same_ref(&r1, &r2) == 1);
    r2.color[0] = &tb;
    assert(rt_same_ref(&r1, &r2) == 0);

    // 8. Optimized GOB row deswizzle/swizzle exact match against nvn_bl_offset across unaligned tails (<64B, >64B)
    const int widths[] = { 3, 12, 16, 20, 37 }; // RGBA8 -> 12, 48, 64, 80, 148 bytes per row
    for (size_t wi = 0; wi < sizeof widths / sizeof *widths; wi++) {
        size_t pitch = (size_t)widths[wi] * 4;
        nvn_texdesc td = { .target = 2 /* 2D */, .format = 0x25 /* RGBA8 */, .width = (uint32_t)widths[wi], .height = 19, .depth = 1, .levels = 1 };
        nvn_texlayout lo;
        nvn_tex_layout(&td, &lo);
        uint8_t *swz = calloc(1, (size_t)lo.size), *lin = calloc(1, pitch * 19), *ref = calloc(1, pitch * 19);
        for (size_t k = 0; k < pitch * 19; k++) ref[k] = (uint8_t)((k * 131 + wi * 17) & 0xff);
        assert(nvn_swizzle(&td, 0, 0, ref, pitch, swz) == 0);
        assert(nvn_deswizzle(&td, 0, 0, swz, lin, pitch) == 0);
        assert(memcmp(lin, ref, pitch * 19) == 0);
        // Verify byte-by-byte against scalar nvn_bl_offset
        for (uint32_t y = 0; y < 19; y++)
            for (size_t x = 0; x < pitch; x++)
                assert(swz[nvn_bl_offset((uint32_t)x, y, 0, (uint32_t)pitch, 19, lo.tileH, lo.tileD)] == ref[y * pitch + x]);
        free(swz); free(lin); free(ref);
    }

    return 0;
}
"""


class AspectAndPchtxtTest(unittest.TestCase):
    def test_present_rect_menu_tap_and_pchtxt(self):
        with tempfile.TemporaryDirectory() as tmp:
            gh = os.path.join(tmp, "game_profile.h")
            subprocess.run([sys.executable, os.path.join(DM, "games/game.py"), "header", "01008DB008C2C000", gh], check=True)
            c_path = os.path.join(tmp, "harness.c")
            bin_path = os.path.join(tmp, "harness")
            with open(c_path, "w", encoding="utf-8") as f:
                f.write(C_HARNESS)
            subprocess.run([
                "cc", "-O2", "-Wall", "-Wno-format", "-Wno-misleading-indentation", "-Werror", "-D_GNU_SOURCE",
                f"-I{os.path.join(DM, 'android/compat')}",
                f"-I{os.path.join(DM, 'host')}",
                f"-I{os.path.join(DM, 'gfx/nvn')}",
                f"-I{os.path.join(DM, 'gfx/nvn/include')}",
                f"-I{tmp}",
                c_path,
                os.path.join(DM, "host/loader.c"),
                os.path.join(DM, "gfx/tex/nvn_deswizzle.c"),
                os.path.join(DM, "gfx/nvn/nvn_tex.c"),
                "-lm", "-lpthread", "-o", bin_path,
            ], check=True)

            pch_ok = os.path.join(tmp, "20_9.pchtxt")
            with open(pch_ok, "wb") as f:
                f.write(b"\xef\xbb\xbf" + (
                    "@nsobid-A16802625E7826BF83B6F9708E475B9100000000000000000000000000000000\n"
                    "@flag print_values\n"
                    "@flag offset_shift 0x100\n"
                    "Pokemon Shield 1.3.2 20:9 test\n"
                    "@enabled\n"
                    "00000100 891C9C52 // movz w9, #0xe0e4\n"
                    "00000104 C901A872 # movk w9, #0x400e, lsl #16\n"
                    "00000200 1F2003D5 // overlaps host patch branch -> skipped\n"
                    "@disabled\n"
                    "00000300 00000000\n"
                    "@stop\n"
                    "@enabled\n"
                    "00000304 11111111\n"
                ).encode("utf-8"))

            pch_other = os.path.join(tmp, "other.pchtxt")
            with open(pch_other, "w", encoding="utf-8") as f:
                f.write("@nsobid-DEADBEEFDEADBEEFDEADBEEFDEADBEEF\n@flag offset_shift 0x100\n@enabled\n00000100 00000000\n")

            subprocess.run([bin_path, pch_ok, pch_other], check=True)


if __name__ == "__main__":
    unittest.main()
