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
    rs = t2d.decode_eacr_signed(open(f'{d}/img{n}.r11s', 'rb').read(), W, H)   # BGRA: red mapped from -1..1 to 0..255
    rs_d = [rs[i * 4 + 2] for i in range(W * H)]; rs_s = [max(1, src[i * 4]) for i in range(W * H)]   # (the source's -128 reads as -127)
    st = t2d.decode_astc(open(f'{d}/img{n}.astc', 'rb').read(), W, H, 4, 4)   # BGRA
    st_s = [src[i * 4 + c] for i in range(W * H) for c in range(4)]; st_d = [st[i * 4 + (2 - c if c < 3 else 3)] for i in range(W * H) for c in range(4)]
    pc, pa, pr, ps, pt = psnr(rgb_s, rgb_d), psnr(a_s, a_d), psnr(r_s, r_d), psnr(rs_s, rs_d), psnr(st_s, st_d)
    need = 30; cneed = 10 if n == 2 else 30   # (random colour per texel is ETC's / ASTC's worst case: one colour line per block; alpha / R11 stay exact enough)
    good = pc >= cneed and pa >= need and pr >= need and ps >= need and pt >= cneed; ok &= good
    print(f'img{n}: colour {pc:5.1f} dB, alpha {pa:5.1f} dB, R11 {pr:5.1f} dB, R11 signed {ps:5.1f} dB, ASTC {pt:5.1f} dB  {"ok" if good else "FAIL"}')
print('ETC2 / EAC / ASTC encoder:', 'matches the decoder' if ok else 'DOES NOT match'); sys.exit(0 if ok else 1)
