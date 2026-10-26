#!/usr/bin/env python3
# Checks bc_decode.c against Pillow's BCn decoder (an independent implementation) on random blocks of every kind: same texels, within rounding.
# Usage: bc_decode_check.py <bc_decode_cli> [blocks per kind]   (build the CLI: cc -O2 -o bc_decode_cli tests/bc_decode_cli.c bc_decode.c)
#   <bc_decode_cli> may be a command with arguments, e.g. "wsl ./bc_decode_cli" on Windows
import io, os, shlex, struct, subprocess, sys, tempfile
from PIL import Image

def dds(dxgi, data, w, h):   # a DDS file with the DX10 header around the blocks
    hdr = struct.pack('<4sIIIIIII44xII4sIIIII', b'DDS ', 124, 0x1 | 0x2 | 0x4 | 0x1000 | 0x80000, h, w, len(data), 0, 1, 32, 0x4, b'DX10', 0, 0, 0, 0, 0)
    hdr += struct.pack('<IIIII', 0x1000, 0, 0, 0, 0) + struct.pack('<IIIII', dxgi, 3, 0, 1, 0)
    return hdr + data

def half(h):   # IEEE half → float
    s, e, m = h >> 15, h >> 10 & 31, h & 1023
    v = (m / 1024) * 2 ** -14 if e == 0 else float('inf') if e == 31 else (1 + m / 1024) * 2 ** (e - 15)
    return -v if s else v

def bc6_mode(b):
    m = b[0] & 3
    return m + 1 if m < 2 else {2: 3, 6: 4, 10: 5, 14: 6, 18: 7, 22: 8, 26: 9, 30: 10, 3: 11, 7: 12, 11: 13, 15: 14}.get(b[0] & 31, 0)

def main():
    cli = shlex.split(sys.argv[1]); nblk = int(sys.argv[2]) if len(sys.argv) > 2 else 4096
    side = 4 * int(nblk ** 0.5); w = h = side
    kinds = [(1, 71, 8), (2, 74, 16), (3, 77, 16), (4, 80, 8), (5, 83, 16), (-5, 84, 16), (6, 95, 16), (-6, 96, 16), (7, 98, 16)]
    bad_total = 0
    with tempfile.TemporaryDirectory(dir='.') as td:
        for kind, dxgi, bb in kinds:
            data = os.urandom((w // 4) * (h // 4) * bb)
            if kind == 7:   # every BC7 mode equally: the mode is the lowest set bit of the first byte
                data = bytearray(data)
                for i in range(0, len(data), 16): data[i] = (data[i] & ~((2 << (i // 16 % 8)) - 1) | 1 << (i // 16 % 8)) & 255
                data = bytes(data)
            bi, bo = os.path.join(td, 'in.bin'), os.path.join(td, 'out.bin')
            open(bi, 'wb').write(data)
            subprocess.run(cli + [str(kind), str(w), str(h), os.path.relpath(bi).replace('\\', '/'), os.path.relpath(bo).replace('\\', '/')], check=True)
            ours = open(bo, 'rb').read()
            ref = Image.open(io.BytesIO(dds(dxgi, data, w, h))); ref.load()
            rb = ref.tobytes(); rch = len(ref.getbands())
            tb = {1: 4, 2: 4, 3: 4, 7: 4, 4: 1, 5: 2, -5: 2, 6: 8, -6: 8}[kind]
            worst = 0; bad = 0; n = w * h; skipped = 0
            for t in range(n):
                if kind == -6 and bc6_mode(data[((t // w) // 4 * (w // 4) + (t % w) // 4) * 16:][:1]) in (1, 2, 3, 4, 5, 6, 7, 8, 9, 12, 13):
                    skipped += 1; continue   # Pillow does not sign-extend signed endpoints after adding a delta (the format does): no reference there
                if kind in (6, -6):
                    hv = struct.unpack_from('<3H', ours, t * 8)
                    fv = [half(x) for x in hv]
                    if any(not (0 <= f <= 1) for f in fv): skipped += 1; continue   # Pillow clamps HDR values to 8 bits: compare only the [0, 1] ones
                    mine = [round(f * 255) for f in fv]
                elif kind == -5:
                    mine = [(struct.unpack('b', bytes([ours[t * 2 + c]]))[0] + 128) for c in range(2)]   # Pillow maps signed to v + 128
                else:
                    mine = list(ours[t * tb:t * tb + tb])
                theirs = list(rb[t * rch:t * rch + rch])
                cmp = min(len(mine), len(theirs))
                d = max(abs(mine[c] - theirs[c]) for c in range(cmp))
                worst = max(worst, d)
                if d > (3 if kind in (6, -6) else 2): bad += 1
            bad_total += bad
            print(f'kind {kind:>2}: {n} texels, worst difference {worst}, over tolerance {bad}' + (f', skipped {skipped} (HDR values outside [0, 1]' + (', signed delta modes' if kind == -6 else '') + ')' if skipped else ''))
    print('OK' if not bad_total else f'[RED] {bad_total} texels differ'); sys.exit(1 if bad_total else 0)

if __name__ == '__main__': main()
