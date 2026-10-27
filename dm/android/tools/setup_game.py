#!/usr/bin/env python3
# The game's setup from the player's own dump, on the phone (the app's setup screen runs it with the Python it ships) or on Linux (the reference run):
#   1. unpack: nx_extract (LibHac, built for the platform) merges the update over the base game → ExeFS / RomFS; DLC → aoc/<n>
#   2. rewrite: patch_nso.py, Android target, for every module the profile loads → patched/
#   3. shaders: shader_split.py (the shaders inside RomFS) → dmsh_build.py in SPIR-V-only mode (the translator and spirv-cross built for the platform)
#      → vk_meta.py (the replay's reflection) → spv_pak.py → shaders.pak
#   4. the rest: shader_map.tsv from the profile, fonts (the phone's own font as the standard font, the button-icon font shipped with the app)
# The game ends up in <out>: RomFS/, patched/, shaders.pak, shader_map.tsv, font/, aoc/ (what android_main.c reads). Progress goes to stdout as lines
#   "STEP <n>/<total> <what>" and "DONE" / "FAILED <why>"; details go to <work>/setup.log.
#        setup_game.py --captures <dir> --out <game dir> --work <scratch dir> --bin <dir>: translate the shaders the game built in memory and a run saved
#          (nvn_vk.c mtl_shader_seen: <key>.ctl / <key>.code), into the same shader set, and pack it again; the game picks them up at its next start
# Usage: setup_game.py --keys <dir with prod.keys [title.keys]> --base <.nsp|.xci> [--update <.nsp>] [--dlc <.nsp> ...] --out <game dir> --work <scratch dir>
#                      --bin <dir with nx_extract, dmsh_xlate, spirv-cross> [--ext-font <nintendo_ext.ttf>] [--std-font <ttf>]
# Environment: DM_GAME (the profile; default 01008DB008C2C000), J (jobs)
import argparse, hashlib, os, shutil, struct, subprocess, sys, time, zlib

HERE = os.path.dirname(os.path.abspath(__file__)); DM = os.path.normpath(os.path.join(HERE, '../..'))
GAME = os.environ.setdefault('DM_GAME', '01008DB008C2C000')
sys.path.insert(0, os.path.join(DM, 'games')); import game as _game
PROFILE = _game.load()

def write_runtime_shader_map(found_map, profile_map, output):
    """Use shader locations found in this dump, retaining profile slots for runtime-captured shaders."""
    with open(found_map, encoding='utf-8') as f: rows = f.readlines()
    keys = set()
    for line in rows:
        c = line.rstrip('\n').split('\t')
        if len(c) > 4 and not line.startswith('#'): keys.add(c[3])
    with open(profile_map, encoding='utf-8') as f:
        for line in f:
            c = line.rstrip('\n').split('\t')
            if len(c) > 4 and c[3] not in keys and c[4].startswith('/nvn_capture/'):
                rows.append(line)
    with open(output, 'w', encoding='utf-8') as f: f.writelines(rows)

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--keys'); ap.add_argument('--base'); ap.add_argument('--captures'); ap.add_argument('--update'); ap.add_argument('--dlc', action='append', default=[])
    ap.add_argument('--out', required=True); ap.add_argument('--work', required=True); ap.add_argument('--bin', required=True)
    ap.add_argument('--ext-font'); ap.add_argument('--std-font')
    a = ap.parse_args()
    if a.captures: return captures(a)
    if not (a.keys and a.base): ap.error('--keys and --base are needed (or --captures)')
    os.makedirs(a.work, exist_ok=True); os.makedirs(a.out, exist_ok=True)
    log = open(os.path.join(a.work, 'setup.log'), 'a', buffering=1)
    py = sys.executable; env = dict(os.environ, PYTHONUNBUFFERED='1')
    env['PATH'] = a.bin + os.pathsep + env.get('PATH', '')
    tool = lambda n: os.path.join(a.bin, n)
    steps = ['unpack the game', 'rewrite the code', 'find the shaders', 'translate the shaders', 'pack the shaders', 'finish']
    def step(i, extra=''):
        print(f'STEP {i}/{len(steps)} {steps[i - 1]}{extra}', flush=True); log.write(f'== {time.strftime("%H:%M:%S")} {steps[i - 1]}{extra}\n')
    def run(cmd, **kw):
        log.write('$ ' + ' '.join(map(str, cmd)) + '\n')
        r = subprocess.run(list(map(str, cmd)), stdout=log, stderr=subprocess.STDOUT, env=kw.pop('env', env), **kw)
        if r.returncode: raise RuntimeError(f'{os.path.basename(str(cmd[1] if str(cmd[0]) == py else cmd[0]))} failed ({r.returncode}): see {log.name}')
    keys = os.path.join(a.keys, 'prod.keys')
    if not os.path.isfile(keys): raise RuntimeError(f'no prod.keys in {a.keys}')
    ex = os.path.join(a.work, 'extract')

    step(1)
    if os.path.isdir(ex): shutil.rmtree(ex)
    aoc = os.path.join(a.out, 'aoc'); shutil.rmtree(aoc, ignore_errors=True)
    # every chosen file in one run: the unpacker tells base game, update and DLC apart by their contents, not their names or the order they were chosen in
    # (an update named without [UPD], a merged XCI, DLC chosen first all work)
    files = [f for f in [a.base, a.update] + a.dlc if f]
    run([tool('nx_extract'), keys, files[0], '-', ex, '--aoc-dir', aoc] + [x for f in files[1:] for x in ('--more', f)])
    exefs, romfs = os.path.join(ex, 'ExeFS'), os.path.join(ex, 'RomFS')
    if not os.path.isfile(os.path.join(exefs, 'main')): raise RuntimeError('the dump has no main module: is it Pokémon Shield?')

    step(2)
    pat = os.path.join(a.out, 'patched'); shutil.rmtree(pat, ignore_errors=True); os.makedirs(pat)
    for m in PROFILE.words('modules'):
        if os.path.isfile(os.path.join(exefs, m)):
            run([py, os.path.join(DM, 'tools/patch_nso.py'), os.path.join(exefs, m), pat, m], env=dict(env, DM_PATCH_TARGET='android'))

    step(3)
    pkg = os.path.join(a.work, 'shader_pkg'); shutil.rmtree(pkg, ignore_errors=True)
    run([py, os.path.join(DM, 'gfx/tools/shader_split.py'), romfs, os.path.join(exefs, 'main'), pkg,
         os.path.join(a.work, 'shader_map.found.tsv'), os.path.join(a.work, 'shader_facts.found.tsv'), os.path.join(a.work, 'shader_pairs.found.tsv')])

    step(4)
    chain = subprocess.run([py, os.path.join(DM, 'gfx/tools/dmsh_chain.py')], capture_output=True, text=True, env=env).stdout.strip()
    vk = os.path.join(a.work, 'vk', 'b-' + chain); shutil.rmtree(os.path.dirname(vk), ignore_errors=True); os.makedirs(vk)
    cache = os.path.join(a.work, 'shader_cache'); shutil.rmtree(cache, ignore_errors=True)
    run([py, os.path.join(DM, 'gfx/tools/dmsh_build.py'), pkg, cache],
        env=dict(env, DMSH_SPV_ONLY='1', VK_SPV_OUT=vk, XLATE=tool('dmsh_xlate'), DMSH_CAS=os.path.join(a.work, 'cas'), WORK=a.work,
                 DMSH_SOURCE_MAP=os.path.join(a.work, 'shader_map.found.tsv'),
                 DMSH_FACTS=os.path.join(a.work, 'shader_facts.found.tsv'),
                 DMSH_PAIRS=os.path.join(a.work, 'shader_pairs.found.tsv'),
                 MAIN=os.path.join(exefs, 'main')))
    built = os.path.join(cache, 'b-' + chain)
    for f in os.listdir(built):
        if f.endswith('_b.json'): shutil.copyfile(os.path.join(built, f), os.path.join(vk, f))

    step(5)
    run([py, os.path.join(DM, 'android/tools/vk_meta.py'), vk, vk, built])
    run([py, os.path.join(DM, 'android/tools/spv_pak.py'), vk, os.path.join(a.out, 'shaders.pak')])

    step(6)
    write_runtime_shader_map(os.path.join(a.work, 'shader_map.found.tsv'),
                             str(PROFILE.table('shader_map.tsv')),
                             os.path.join(a.out, 'shader_map.tsv'))
    font = os.path.join(a.out, 'font'); os.makedirs(font, exist_ok=True)
    std = a.std_font or next((f for f in ('/system/fonts/Roboto-Regular.ttf', '/system/fonts/NotoSans-Regular.ttf') if os.path.isfile(f)), None)
    if std: shutil.copyfile(std, os.path.join(font, 'standard.ttf'))
    if a.ext_font: shutil.copyfile(a.ext_font, os.path.join(font, 'nintendo_ext.ttf'))
    if os.path.isfile(os.path.join(ex, 'icon.jpg')): shutil.copyfile(os.path.join(ex, 'icon.jpg'), os.path.join(a.out, 'icon.jpg'))   # the game's icon from the player's dump: the setup screen puts it on the home screen shortcut
    rom = os.path.join(a.out, 'RomFS')
    if os.path.realpath(rom) != os.path.realpath(romfs):
        shutil.rmtree(rom, ignore_errors=True); os.replace(romfs, rom)   # same storage: a rename, nothing copied
    for d in ('shader_pkg', 'cas', 'vk', 'shader_cache', 'extract'): shutil.rmtree(os.path.join(a.work, d), ignore_errors=True)   # only setup.log stays
    open(os.path.join(a.out, 'ready'), 'w').write(chain + '\n')
    print('DONE', flush=True)

def pak_keys(path):   # the keys an NXSP shader pack holds, and its chain version
    with open(path, 'rb') as f:
        h = f.read(28)
        if h[:4] != b'NXSP': raise RuntimeError(f'{path} is not a shader pack')
        n = struct.unpack_from('<I', h, 8)[0]; idx = f.read(32 * n)
    return {idx[32 * i:32 * i + 8] for i in range(n)}, h[12:28].rstrip(b'\0').decode()

def pak_merge(path, add):   # add the shaders of pack `add` (version 1) to pack `path` (version 1 or 2), rewriting it beside and swapping it in
    with open(add, 'rb') as f: b = f.read()
    nb = struct.unpack_from('<I', b, 8)[0]
    src = open(path, 'rb'); h = src.read(28); ver, na = struct.unpack_from('<II', h, 4); idx = src.read(32 * na)
    shift = 32 * nb; end = os.fstat(src.fileno()).st_size + shift; rows = []; blobs = []
    for i in range(na):
        e = idx[32 * i:32 * i + 32]; so, sl, ro, rl = struct.unpack_from('<QIQI', e, 8); rows.append((e[:8], so + shift, sl, ro + shift, rl))
    for i in range(nb):
        e = b[28 + 32 * i:60 + 32 * i]; so, sl, ro, rl = struct.unpack_from('<QIQI', e, 8); row = [e[:8]]
        for o, l in ((so, sl), (ro, rl)):
            blob = b[o:o + l]
            if ver == 2: blob = struct.pack('<I', l) + zlib.compress(blob, 9)
            row += [end, len(blob)]; blobs.append(blob); end += len(blob)
        rows.append(tuple(row))
    rows.sort(key=lambda r: r[0]); tmp = path + '.new'
    with open(tmp, 'wb') as out:
        out.write(h[:8] + struct.pack('<I', na + nb) + h[12:28])
        for k, so, sl, ro, rl in rows: out.write(k + struct.pack('<QIQI', so, sl, ro, rl))
        src.seek(28 + 32 * na)
        while True:
            chunk = src.read(1 << 24)
            if not chunk: break
            out.write(chunk)
        for blob in blobs: out.write(blob)
    src.close(); os.replace(tmp, path)

def captures(a):
    # The profile's map places each in-memory shader at a fixed index of <stage>_other.bin; the package is written with the captured ones there and an empty record
    # in each other slot (never read: only the captured keys are translated). They are translated on their own and merged into the shader pack, so nothing
    # from the first setup has to be kept
    log = open(os.path.join(a.work, 'setup.log'), 'a', buffering=1); py = sys.executable
    env = dict(os.environ, PYTHONUNBUFFERED='1', PATH=a.bin + os.pathsep + os.environ.get('PATH', ''))
    def run(cmd, **kw):
        log.write('$ ' + ' '.join(map(str, cmd)) + '\n')
        r = subprocess.run(list(map(str, cmd)), stdout=log, stderr=subprocess.STDOUT, env=kw.pop('env', env), **kw)
        if r.returncode: raise RuntimeError(f'{os.path.basename(str(cmd[1]))} failed ({r.returncode}): see {log.name}')
    chain = subprocess.run([py, os.path.join(DM, 'gfx/tools/dmsh_chain.py')], capture_output=True, text=True, env=env).stdout.strip()
    pak = os.path.join(a.out, 'shaders.pak'); have, pchain = pak_keys(pak)
    if pchain != chain: raise RuntimeError(f'the shader pack is version {pchain}, this app makes {chain}: set the game up again')
    slots = {}
    for ln in open(PROFILE.table('shader_map.tsv'), encoding='utf-8'):
        c = ln.rstrip('\n').split('\t')
        if len(c) > 4 and c[4].startswith('/nvn_capture/'): slots[c[3]] = (c[0], int(c[1]))
    print(f'STEP 1/3 read the captured shaders', flush=True)
    new = []
    for f in sorted(os.listdir(a.captures)) if os.path.isdir(a.captures) else []:
        k = f[:-5]
        if not f.endswith('.code') or k not in slots or bytes.fromhex(k) in have: continue
        ctl, code = open(os.path.join(a.captures, k + '.ctl'), 'rb').read(), open(os.path.join(a.captures, f), 'rb').read()
        own = struct.unpack_from('<I', ctl, 0x20)[0]; n = own if 0x760 <= own < 0x880 else 0x880   # the key convention of shader_split.py / hle_nvn.c keys_hash
        if hashlib.sha256(ctl[:n] + code).hexdigest()[:16] != k: log.write(f'capture {k}: the saved files do not hash to their key, skipped\n'); continue
        new.append((k, ctl, code))
    if not new: print('DONE', flush=True); return
    cw = os.path.join(a.work, 'capture'); shutil.rmtree(cw, ignore_errors=True)
    pkg, cache = os.path.join(cw, 'pkg'), os.path.join(cw, 'shader_cache'); vk, built = os.path.join(cw, 'vk', 'b-' + chain), os.path.join(cache, 'b-' + chain)
    os.makedirs(pkg); os.makedirs(vk)
    got = {k: (ctl, code) for k, ctl, code in new}
    for name in sorted({p for p, _ in slots.values()}):
        rows = sorted((i, k) for k, (p, i) in slots.items() if p == name); recs = []
        for i, k in rows:
            assert i == len(recs), f'{name}: index {i} out of order'
            ctl, code = got.get(k, (b'', b''))
            rec = bytearray(b'BTCDBTCD' + bytes(0x30)); struct.pack_into('<2I', rec, 0x28, len(ctl), len(code)); recs.append(bytes(rec) + code + ctl)
        open(os.path.join(pkg, name), 'wb').write(b''.join(recs))
    print(f'STEP 2/3 translate {len(new)} shaders', flush=True)
    run([py, os.path.join(DM, 'gfx/tools/dmsh_build.py'), pkg, cache, *got],
        env=dict(env, DMSH_SPV_ONLY='1', VK_SPV_OUT=vk, XLATE=os.path.join(a.bin, 'dmsh_xlate'), DMSH_CAS=os.path.join(cw, 'cas'), WORK=cw))
    for k in got: shutil.copyfile(os.path.join(built, k + '_b.json'), os.path.join(vk, k + '_b.json'))
    print(f'STEP 3/3 pack the shaders', flush=True)
    run([py, os.path.join(DM, 'android/tools/vk_meta.py'), vk, vk, built])
    small = os.path.join(cw, 'new.pak'); run([py, os.path.join(DM, 'android/tools/spv_pak.py'), vk, small])
    pak_merge(pak, small)
    shutil.rmtree(cw, ignore_errors=True)
    print('DONE', flush=True)

if __name__ == '__main__':
    try: main()
    except Exception as e: print(f'FAILED {e}', flush=True); sys.exit(1)
