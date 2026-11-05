#!/usr/bin/env python3
# Trinity (Scarlet / Violet) RomFS mods as loose files: the game finds every file through arc/data.trpfd, the sorted list of its paths' FNV-1a 64 hashes, and reads it from
#   arc/data.trpfs; a path that is not in the list is read from RomFS as a loose file (what Trinity Mod Loader does by removing it). Here each modded path's hash in the list
#   is moved up by one: the list stays sorted, every other lookup still finds its file, and the modded paths fall through to the loose files of the mod folder.
# Usage: trinity_mod.py <the game's arc/data.trpfd> <mod folder: the RomFS layout, e.g. world/scene/...> <out: a mod folder with romfs/>
import os, shutil, struct, sys, bisect
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '../gfx/tools'))
os.environ.setdefault('DM_GAME', '01008F6008C5E000')
import shader_split as ss

def fnv(s):
    h = 0xcbf29ce484222645
    for c in s.encode(): h = ((h ^ c) * 0x100000001b3) & 0xffffffffffffffff
    return h

def main():
    trpfd, mod, out = sys.argv[1:4]
    fd = bytearray(open(trpfd, 'rb').read()); fr = ss.fb_root(fd)
    st, n = ss.fb_vec(fd, fr, 0); hashes = list(struct.unpack_from(f'<{n}Q', fd, st))
    files = []
    for root, _, fs in os.walk(mod):
        for f in fs: files.append(os.path.relpath(os.path.join(root, f), mod).replace(os.sep, '/'))
    rom = os.path.join(out, 'romfs'); shutil.rmtree(out, ignore_errors=True)
    for rel in files:
        if rel.startswith('romfs/'): rel_out = rel[6:]
        else: rel_out = rel
        h = fnv(rel_out); i = bisect.bisect_left(hashes, h)
        if i < n and hashes[i] == h:
            if i + 1 < n and hashes[i + 1] == h + 1: sys.exit(f'{rel_out}: no room to move its hash')
            struct.pack_into('<Q', fd, st + 8 * i, h + 1); print(f'unlisted from the pack: {rel_out}')
        else: print(f'not in the pack (a new file): {rel_out}')
        os.makedirs(os.path.dirname(os.path.join(rom, rel_out)), exist_ok=True); shutil.copyfile(os.path.join(mod, rel), os.path.join(rom, rel_out))
    os.makedirs(os.path.join(rom, 'arc'), exist_ok=True); open(os.path.join(rom, 'arc', 'data.trpfd'), 'wb').write(fd)
    print(f'{out}: {len(files)} file(s) and arc/data.trpfd')

if __name__ == '__main__': main()
