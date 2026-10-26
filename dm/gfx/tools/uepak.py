#!/usr/bin/env python3
# Unreal Engine game files for the shader tools: pak files (version 11, UE 4.26 and later; Octopath Traveler 0 is UE5), shader code archives (.ushaderbytecode version 2) and the job files the Mac host decodes Oodle with
# (DM_OODLE_JOB, dm/host/hle_oodle.c). Octopath Traveler 0 keeps everything in one pak, Octopath_Traveler0/Content/Paks/pakchunk0-Switch.pak (1.5 GB, 159,757 files): its index is AES-256 encrypted,
# its files are stored or Oodle-compressed in blocks. Its two shader code archives are stored, but every shader inside is Oodle-compressed (Mermaid, block header 8C 0A), which no decoder in this
# repository can undo, so the game's own OodleLZ_Decompress decodes them in the Mac host (profile key shader_ue_decode, the NSP script's decode_ue_shaders).
# The AES key is in main (Unreal embeds it): the 32-byte window of main's read-only or data segment that decrypts the index start to its mount point ("../../../"; Octopath Traveler 0: one window of
#   116,287 tried). It is only kept in memory, never printed or written.
# Pak footer (version 11, the last 0xdd bytes): encryption key GUID (16), encrypted index flag (u8), magic 0x5A6F12E1, version (u32), index offset (u64), index size (u64), index SHA-1 (20),
#   5 compression method names (32 bytes each).
# Index: mount point (FString), entry count (i32), path hash seed (u64), path hash index (i32 flag, then u64 offset, u64 size, SHA-1), full directory index (the same, encrypted like the index),
#   encoded entries (i32 size + bytes), the count of unencoded entries (0 here). Directory index: i32 count, then per directory its name (FString) and its files (i32 count, then name + i32 offset into
#   the encoded entries). FString: i32 length with the NUL, UTF-8; a negative length is UTF-16.
# Encoded entry: u32 bits (0-5 compression block size >> 11, 0x3f = a u32 follows; 6-21 block count; 22 encrypted; 23-28 compression method, 1-based; 29 stored size fits 32 bits; 30 raw size fits
#   32 bits; 31 offset fits 32 bits), then offset, raw size, stored size (compressed entries only) and, for several blocks or an encrypted one, each block's u32 stored size. The data starts after
#   the FPakEntry the pak repeats in front of it (53 bytes, + 4 + 16 per block when compressed); blocks follow each other, 16-aligned when encrypted.
# Shader code archive (version 2): shader map hashes (20 bytes each), shader hashes (20 each), shader map entries {u32 first index, u32 shader count, u32 first preload, u32 preload count}, shader
#   entries {u64 offset, u32 size, u32 raw size, u8 frequency} (17 bytes, packed), preload entries (16 each), shader indices (u32): each array is a u32 count and its elements; the code follows,
#   offsets relative to its start. A shader whose size differs from its raw size is compressed.
# Usage: uepak.py list <pak> <main NSO> <out tsv>                       every file in the pak: path, raw size, stored size, compression, encrypted, blocks
#        uepak.py job <RomFS> <main NSO> <work dir>                     the shader code archives of every pak, and an Oodle job of their compressed shaders
#        uepak.py finish <work dir> <out dir>                           after the host has run the job: the archives with every shader decoded
import ctypes, json, os, struct, sys

PAK_MAGIC, ARCHIVE_VERSION = 0x5A6F12E1, 2
JOB_MAGIC, OUT_MAGIC = 0x4a4f4d44, 0x524f4d44   # "DMOJ", "DMOR" (hle_oodle.c)
FREQ = {0: 'vs', 1: 'hs', 2: 'ds', 3: 'ps', 4: 'gs', 5: 'cs'}   # EShaderFrequency (the archive's frequency census: 0 vertex, 3 pixel, 5 compute)

_cc = None
def aes_decrypt(key, data):
    """AES-256 ECB decryption through CommonCrypto (every Mac has it; no Python package needed)"""
    global _cc
    if _cc is None:
        _cc = ctypes.CDLL('/usr/lib/libSystem.B.dylib').CCCrypt
        _cc.restype = ctypes.c_int32
        _cc.argtypes = [ctypes.c_uint32, ctypes.c_uint32, ctypes.c_uint32, ctypes.c_char_p, ctypes.c_size_t, ctypes.c_void_p, ctypes.c_char_p, ctypes.c_size_t, ctypes.c_void_p, ctypes.c_size_t, ctypes.POINTER(ctypes.c_size_t)]
    out = ctypes.create_string_buffer(len(data)); moved = ctypes.c_size_t()
    if _cc(1, 0, 2, key, len(key), None, data, len(data), out, len(data), ctypes.byref(moved)): raise ValueError('CCCrypt failed')   # decrypt, AES, ECB
    return out.raw[:moved.value]

def nso_image(path):
    """A decompressed NSO image and its (read-only start, data end)"""
    import lz4.block
    d = open(path, 'rb').read()
    flags = struct.unpack_from('<I', d, 0xc)[0]; segs = [struct.unpack_from('<III', d, 0x10 + i * 0x10) for i in range(3)]; csz = struct.unpack_from('<III', d, 0x60)
    img = bytearray(segs[2][1] + segs[2][2])
    for i, (fo, mo, sz) in enumerate(segs):
        raw = d[fo:fo + csz[i]]
        img[mo:mo + sz] = lz4.block.decompress(raw, uncompressed_size=sz) if flags >> i & 1 else raw
    return bytes(img), segs[1][1], segs[2][1] + segs[2][2]

class R:
    def __init__(self, b, o=0): self.b, self.o = b, o
    def take(self, fmt):
        v = struct.unpack_from(fmt, self.b, self.o); self.o += struct.calcsize(fmt); return v if len(v) > 1 else v[0]
    def raw(self, n): v = self.b[self.o:self.o + n]; self.o += n; return v
    def fstr(self):
        n = self.take('<i')
        if n < 0: return self.raw(-2 * n).decode('utf-16-le')[:-1]
        return self.raw(n)[:-1].decode('utf-8', 'replace') if n else ''

def footer(f):
    f.seek(-0xdd, 2); ft = f.read(0xdd)
    k = ft.find(struct.pack('<I', PAK_MAGIC))
    if k < 1: raise ValueError('no pak footer')
    ver, ioff, isz = struct.unpack_from('<IQQ', ft, k + 4)
    if ver != 11: raise ValueError(f'pak version {ver}, only 11 is read')
    methods = [ft[k + 44 + 32 * i:k + 76 + 32 * i].rstrip(b'\0').decode('latin1') for i in range(5)]
    return ft[k - 1], ioff, isz, methods

def find_key(img, lo, hi, pak):
    """The AES key among main's 32-byte windows (read-only and data segments, 4-aligned, at least 20 distinct bytes): the one that decrypts the index start to a short mount point"""
    with open(pak, 'rb') as f:
        _, ioff, _, _ = footer(f); f.seek(ioff); blk = f.read(16)
    for a in range(lo, hi - 32, 4):
        w = img[a:a + 32]
        if len(set(w)) < 20: continue
        d = aes_decrypt(w, blk)
        if 0 < struct.unpack_from('<i', d)[0] < 512 and d[4:7] == b'../': return w
    return None

class Pak:
    def __init__(self, path, key=None):
        self.path, self.key = path, key; self.f = open(path, 'rb')
        enc, ioff, isz, self.methods = footer(self.f)
        if enc and not key: raise ValueError(f'{path}: the index is encrypted and no key was given')
        r = R(self.read_at(ioff, isz, enc))
        self.mount = r.fstr(); self.count = r.take('<i'); r.take('<Q')
        if r.take('<i'): r.take('<QQ'); r.raw(20)   # path hash index (not needed with the full directory index)
        if not r.take('<i'): raise ValueError(f'{path}: no full directory index')
        doff, dsz = r.take('<QQ'); r.raw(20)
        self.enc = r.raw(r.take('<i'))
        if r.take('<i'): raise ValueError(f'{path}: unencoded entries are not read')
        d = R(self.read_at(doff, dsz, enc)); self.files = {}
        for _ in range(d.take('<i')):
            dn = d.fstr()
            for _ in range(d.take('<i')): fn = d.fstr(); self.files[(dn + fn).lstrip('/')] = d.take('<i')
    def read_at(self, off, n, enc):
        self.f.seek(off); b = self.f.read((n + 15) & ~15 if enc else n)
        return aes_decrypt(self.key, b)[:n] if enc else b
    def entry(self, path):
        r = R(self.enc, self.files[path]); v = r.take('<I')
        bsz = (v & 0x3f) << 11
        if v & 0x3f == 0x3f: bsz = r.take('<I')
        nblk, enc, meth = v >> 6 & 0xffff, v >> 22 & 1, v >> 23 & 0x3f
        off = r.take('<I' if v >> 31 & 1 else '<Q'); raw = r.take('<I' if v >> 30 & 1 else '<Q')
        size = r.take('<I' if v >> 29 & 1 else '<Q') if meth else raw
        cur, blocks = 53 + (4 + 16 * nblk if meth else 0), []
        if nblk == 1 and not enc: blocks = [(cur, size)]
        else:
            for _ in range(nblk): s = r.take('<I'); blocks.append((cur, s)); cur += (s + 15) & ~15 if enc else s
        return dict(off=off, raw=raw, size=size, method=self.methods[meth - 1] if meth else '', encrypted=enc, block_size=bsz, blocks=blocks)
    def read(self, path):
        """A stored file's bytes (compressed files need the Oodle job)"""
        e = self.entry(path)
        if e['method']: raise ValueError(f'{path} is {e["method"]}-compressed in the pak')
        return self.read_at(e['off'] + 53, e['size'], e['encrypted'])

def archive(b):
    """A shader code archive's tables: {'maps': [(first index, count)], 'shaders': [(offset, size, raw size, frequency)], 'indices': [...], 'code': start of the code}"""
    r = R(b)
    if r.take('<I') != ARCHIVE_VERSION: raise ValueError('not a version 2 shader code archive')
    nmap = r.take('<I'); r.raw(20 * nmap); nsh = r.take('<I'); r.raw(20 * nsh)
    maps = [r.take('<IIII')[:2] for _ in range(r.take('<I'))]
    shaders = [r.take('<QIIB') for _ in range(r.take('<I'))]
    r.raw(16 * r.take('<I'))
    n = r.take('<I'); indices = list(struct.unpack_from(f'<{n}I', b, r.o)); r.o += 4 * n
    return dict(maps=maps, shaders=shaders, indices=indices, code=r.o)

def decoded_archive(b, raws):
    """The archive with every shader replaced by its decoded bytes (raws, in entry order): the same tables, each entry's size = its raw size, the code laid out again
    (the preload entries keep their old file ranges; only the shader tools read these copies)"""
    a = archive(b); r = R(b); r.take('<I')
    for elem in (20, 20, 16): r.raw(elem * r.take('<I'))
    head, ents_at = b[:r.o + 4], r.o + 4; n = len(a['shaders']); tail_at = ents_at + 17 * n
    ents, off = bytearray(), 0
    for (_, _, raw, fr), d in zip(a['shaders'], raws):
        if len(d) != raw: raise ValueError(f'decoded shader of {len(d)} bytes, the archive says {raw}')
        ents += struct.pack('<QIIB', off, raw, raw, fr); off += raw
    return head + bytes(ents) + b[tail_at:a['code']] + b''.join(raws)

def paks(romfs):
    """Every pak under the RomFS"""
    return sorted(os.path.join(dp, f) for dp, _, fs in os.walk(romfs) for f in fs if f.endswith('.pak'))

def job(romfs, main_nso, work):
    """The shader code archives of every pak under the RomFS into <work>/archives, and one Oodle job (<work>/oodle.job) of every compressed shader in them; <work>/job.json lists them"""
    os.makedirs(os.path.join(work, 'archives'), exist_ok=True)
    img, lo, hi = nso_image(main_nso); key = None; listing, recs = [], []
    for pak in paks(romfs):
        with open(pak, 'rb') as f: enc = footer(f)[0]
        if enc and key is None:
            key = find_key(img, lo, hi, pak)
            if key is None: raise ValueError(f'{pak}: the index is encrypted and no window of main decrypts it')
        p = Pak(pak, key)
        for path in sorted(x for x in p.files if x.endswith('.ushaderbytecode')):
            b = p.read(path); a = archive(b); name = os.path.basename(path)
            open(os.path.join(work, 'archives', name), 'wb').write(b)
            comp = [i for i, (_, size, raw, _) in enumerate(a['shaders']) if size != raw]
            for i in comp:
                off, size, raw, _ = a['shaders'][i]; recs.append((name, i, b[a['code'] + off:a['code'] + off + size], raw))
            listing.append(dict(name=name, pak=os.path.relpath(pak, romfs), path=path, shaders=len(a['shaders']), compressed=len(comp)))
    with open(os.path.join(work, 'oodle.job'), 'wb') as f:
        f.write(struct.pack('<II', JOB_MAGIC, len(recs)))
        for _, _, c, raw in recs: f.write(struct.pack('<II', len(c), raw)); f.write(c)
    json.dump(dict(archives=listing, records=[(n, i) for n, i, _, _ in recs]), open(os.path.join(work, 'job.json'), 'w'), indent=1)
    return listing, len(recs)

def finish(work, out):
    """The decoded archives into out, from <work>/oodle.job.out (the host's results, in job order)"""
    meta = json.load(open(os.path.join(work, 'job.json')))
    b = open(os.path.join(work, 'oodle.job.out'), 'rb').read()
    magic, n = struct.unpack_from('<II', b)
    if magic != OUT_MAGIC or n != len(meta['records']): raise ValueError('the job output does not match the job')
    o, dec = 8, {}
    for name, i in meta['records']:
        l = struct.unpack_from('<I', b, o)[0]; o += 4
        if not l: raise ValueError(f'{name}: shader {i} was not decoded')
        dec[(name, i)] = b[o:o + l]; o += l
    os.makedirs(out, exist_ok=True)
    for x in meta['archives']:
        src = open(os.path.join(work, 'archives', x['name']), 'rb').read(); a = archive(src)
        raws = [dec.get((x['name'], i)) or src[a['code'] + off:a['code'] + off + size] for i, (off, size, _, _) in enumerate(a['shaders'])]
        tmp = os.path.join(out, x['name'] + '.partial'); open(tmp, 'wb').write(decoded_archive(src, raws)); os.replace(tmp, os.path.join(out, x['name']))
    return [x['name'] for x in meta['archives']]

def lslg(s):
    """A decoded Switch shader (LSLG): 'LSLG', a stage tag (SV vertex, SP pixel, SC compute, ...), a header with the shader's source name, then u32 code length, the code area,
    u32 control length and the control section (0x880 bytes), and a short tail (24 or 29 bytes). → (tag, control section, code area), or None if the layout differs"""
    if s[:4] != b'LSLG': return None
    k = s.find(b'\x34\x12\x76\x98')
    if k < 8: return None
    nctl, ncode = struct.unpack_from('<I', s, k - 4)[0], struct.unpack_from('<I', s, k + 0x704)[0]
    c = k - 4 - ncode
    if c < 4 or struct.unpack_from('<I', s, c - 4)[0] != ncode or k + nctl > len(s): return None
    return s[4:6].decode('latin1'), s[k:k + nctl], s[c:c + ncode]

if __name__ == '__main__':
    cmd = sys.argv[1] if len(sys.argv) > 1 else ''
    if cmd == 'list':
        img, lo, hi = nso_image(sys.argv[3])
        with open(sys.argv[2], 'rb') as f: enc = footer(f)[0]
        p = Pak(sys.argv[2], find_key(img, lo, hi, sys.argv[2]) if enc else None)
        with open(sys.argv[4], 'w') as f:
            f.write('path\traw\tstored\tmethod\tencrypted\tblocks\n')
            for path in sorted(p.files):
                e = p.entry(path); f.write(f"{path}\t{e['raw']}\t{e['size']}\t{e['method'] or 'none'}\t{e['encrypted']}\t{len(e['blocks'])}\n")
        print(f'pak version 11, mount {p.mount}, {len(p.files)} files, compression {[m for m in p.methods if m]}')
    elif cmd == 'job':
        listing, n = job(*sys.argv[2:5])
        for x in listing: print(f"{x['name']}: {x['shaders']} shaders, {x['compressed']} compressed")
        print(f'{n} records in {os.path.join(sys.argv[4], "oodle.job")}')
    elif cmd == 'finish':
        print('decoded:', ', '.join(finish(*sys.argv[2:4])))
    else: sys.exit(__doc__ or 'usage: uepak.py list | job | finish (see the header)')
