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
#include <string.h>
#include "dm.h"
#include "nvn_ovl.h"

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
                f"-I{tmp}",
                c_path,
                os.path.join(DM, "host/loader.c"),
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
