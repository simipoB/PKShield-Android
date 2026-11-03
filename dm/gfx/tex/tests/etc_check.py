#!/usr/bin/env python3
# Checks etc_encode.c's blocks with an independent decoder (texture2ddecoder, MIT): etc_check.py <dir written by etc_test>
# A wrong table, bit position or pixel order decodes to noise (PSNR far under 20 dB); a correct encoder comes back close to the source (ETC2's own loss only)
import math, sys
import texture2ddecoder as t2d
W = H = 64; d = sys.argv[1]; ok = True
def psnr(a, b):
    se = sum((x - y) ** 2 for x, y in zip(a, b)) / max(1, len(a)); return 99.0 if se == 0 else 10 * math.log10(255 * 255 / se)
for n in range(4):
    src = open(f'{d}/img{n}.rgba', 'rb').read()
    dec = t2d.decode_etc2a8(open(f'{d}/img{n}.etc2', 'rb').read(), W, H)   # BGRA
    rgb_s = [src[i * 4 + c] for i in range(W * H) for c in range(3)]; rgb_d = [dec[i * 4 + (2 - c)] for i in range(W * H) for c in range(3)]
    a_s = [src[i * 4 + 3] for i in range(W * H)]; a_d = [dec[i * 4 + 3] for i in range(W * H)]
    r = t2d.decode_eacr(open(f'{d}/img{n}.r11', 'rb').read(), W, H)        # BGRA, red in R
    r_s = [src[i * 4] for i in range(W * H)]; r_d = [r[i * 4 + 2] for i in range(W * H)]
    pc, pa, pr = psnr(rgb_s, rgb_d), psnr(a_s, a_d), psnr(r_s, r_d)
    need = 30; cneed = 10 if n == 2 else 30   # (random colour per texel is ETC's worst case: one base colour per half block; alpha / R11 stay exact enough)
    good = pc >= cneed and pa >= need and pr >= need; ok &= good
    print(f'img{n}: colour {pc:5.1f} dB, alpha {pa:5.1f} dB, R11 {pr:5.1f} dB  {"ok" if good else "FAIL"}')
print('ETC2 / EAC encoder:', 'matches the decoder' if ok else 'DOES NOT match'); sys.exit(0 if ok else 1)
