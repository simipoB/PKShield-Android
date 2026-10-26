#!/usr/bin/env python3
# Packs the Vulkan shader set (<key>.spv + <key>.vkr, dm/android/tools/vk_shaders.sh) into one file the replay maps (nvn_vk.c, DM_SPV_PAK): 77,000 small files are slow to copy to a phone.
# Layout (little endian): "NXSP" u32 version 1, u32 count, char chain[16], then count entries {u8 key[8], u64 spv_off, u32 spv_len, u64 vkr_off, u32 vkr_len} sorted by key, then the data.
# Version 2 (--compress, from a version 1 pack): every blob is u32 size + a zlib stream (Android ships it inside the APK; nvn_vk.c pak_blob inflates on load).
# Usage: spv_pak.py <dir b-<chain>> <out.pak>  |  spv_pak.py --compress <in.pak (version 1)> <out.pak>
import os, struct, sys, zlib
if sys.argv[1] == '--compress':
    src, out = sys.argv[2], sys.argv[3]; f = open(src, 'rb'); h = f.read(28)
    assert h[:4] == b'NXSP' and struct.unpack('<I', h[4:8])[0] == 1, 'not a version 1 pack'
    n = struct.unpack('<I', h[8:12])[0]; idx = f.read(32 * n); off = 28 + 32 * n; ents = []; blobs = []
    for i in range(n):
        e = idx[32 * i:32 * i + 32]; so, sl, ro, rl = struct.unpack('<QIQI', e[8:]); row = [e[:8]]
        for o, l in ((so, sl), (ro, rl)):
            f.seek(o); b = f.read(l); z = struct.pack('<I', l) + zlib.compress(b, 9); row += [off, len(z)]; blobs.append(z); off += len(z)
        ents.append(row)
    with open(out, 'wb') as g:
        g.write(b'NXSP' + struct.pack('<II', 2, n) + h[12:28])
        for k, so, sl, ro, rl in ents: g.write(k + struct.pack('<QIQI', so, sl, ro, rl))
        for b in blobs: g.write(b)
    print(f'{out}: {n} shaders, {off / 1e6:.0f} MB compressed (from {os.path.getsize(src) / 1e6:.0f} MB)'); sys.exit(0)
d, out = sys.argv[1], sys.argv[2]; chain = os.path.basename(d.rstrip('/'))[2:]
keys = sorted(f[:-4] for f in os.listdir(d) if f.endswith('.spv') and os.path.exists(os.path.join(d, f[:-4] + '.vkr')))
ent = 32; head = 4 + 4 + 4 + 16; off = head + ent * len(keys); idx = []; blobs = []
for k in keys:
    s = open(os.path.join(d, k + '.spv'), 'rb').read(); r = open(os.path.join(d, k + '.vkr'), 'rb').read()
    idx.append((bytes.fromhex(k), off, len(s), off + len(s), len(r))); blobs += [s, r]; off += len(s) + len(r)
with open(out, 'wb') as f:
    f.write(b'NXSP' + struct.pack('<II', 1, len(keys)) + chain.encode().ljust(16, b'\0'))
    for k, so, sl, ro, rl in idx: f.write(k + struct.pack('<QIQI', so, sl, ro, rl))
    for b in blobs: f.write(b)
print(f'{out}: {len(keys)} shaders, {off / 1e6:.0f} MB, chain {chain}')
