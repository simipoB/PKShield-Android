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
import argparse, hashlib, os, shutil, signal, struct, subprocess, sys, time, zlib

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
        if r.returncode:
            name = os.path.basename(str(cmd[1] if str(cmd[0]) == py else cmd[0]))
            if r.returncode < 0:
                try: reason = signal.Signals(-r.returncode).name
                except ValueError: reason = f'signal {-r.returncode}'
                raise RuntimeError(f'{name} terminated by {reason} ({r.returncode}); see {log.name}')
            raise RuntimeError(f'{name} failed ({r.returncode}); see {log.name}')
    keys = os.path.join(a.keys, 'prod.keys')
    if not os.path.isfile(keys): raise RuntimeError(f'no prod.keys in {a.keys}')
    ex = os.path.join(a.work, 'extract')

    step(1)
    if os.path.isdir(ex): shutil.rmtree(ex)
    aoc = os.path.join(a.out, 'aoc'); shutil.rmtree(aoc, ignore_errors=True)
    # every chosen file in one run: the unpacker tells base game, update and DLC apart by their contents, not their names or the order they were chosen in
    # (an update named without [UPD], a merged XCI, DLC chosen first all work)
    files = [f for f in [a.base, a.update] + a.dlc if f]
    log.write('selected files:\n')
    for f in files:
        try: log.write(f'  {f} ({os.path.getsize(f)} bytes)\n')
        except OSError as e: log.write(f'  {f} (stat failed: {e})\n')
    try:
        free = shutil.disk_usage(a.work).free
        log.write(f'free space at extraction start: {free} bytes\n')
    except OSError as e: log.write(f'free-space check failed: {e}\n')
    run([tool('nx_extract'), keys, files[0], '-', ex, '--aoc-dir', aoc] + [x for f in files[1:] for x in ('--more', f)])
    exefs, romfs = os.path.join(ex, 'ExeFS'), os.path.join(ex, 'RomFS')
    if not os.path.isfile(os.path.join(exefs, 'main')): raise RuntimeError('the dump has no main module: is it Pokémon Shield?')
    # the game's version, checked now rather than as a failure ten minutes later in the shader steps: the code (main's build ID) must be one the profile knows
    with open(os.path.join(exefs, 'main'), 'rb') as f: build_id = f.read(0x50)[0x40:0x50].hex()
    builds = PROFILE.builds(); want = ', '.join(sorted(b['display'] for b in builds.values()))
    if build_id not in {b.get('main') for b in builds.values()}:
        have = ''
        try: have = open(os.path.join(ex, 'control.nacp'), 'rb').read()[0x3060:0x3070].split(b'\0')[0].decode()
        except OSError: pass
        raise RuntimeError(f'your files hold Pokémon Shield {have or "of an unknown version"}, and this app needs update {want}: '
                           f'choose the {want} update too (a separate update file, or a dump that includes it)')

    step(2)
    pat = os.path.join(a.out, 'patched'); shutil.rmtree(pat, ignore_errors=True); os.makedirs(pat)
    for m in PROFILE.words('modules'):
        if os.path.isfile(os.path.join(exefs, m)):
            run([py, os.path.join(DM, 'tools/patch_nso.py'), os.path.join(exefs, m), pat, m], env=dict(env, DM_PATCH_TARGET='android'))

    step(3)
    pkg = os.path.join(a.work, 'shader_pkg'); shutil.rmtree(pkg, ignore_errors=True)
    # the shaders an earlier run of the game captured (nvn_capture/ in the game dir, android_main.c DM_NVN_CAPTURE) go into the new pack too: the game saves each
    # one only once, so a pack rebuilt without them would never get them back
    run([py, os.path.join(DM, 'gfx/tools/shader_split.py'), romfs, os.path.join(exefs, 'main'), pkg,
         os.path.join(a.work, 'shader_map.found.tsv'), os.path.join(a.work, 'shader_facts.found.tsv'), os.path.join(a.work, 'shader_pairs.found.tsv')],
        env=dict(env, DM_NVN_CAPTURE=os.path.join(a.out, 'nvn_capture')))

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
    # the new pack holds every capture there is, so the launcher's list of captures already added is stale: the next start checks them against the pack again
    try: os.remove(os.path.join(a.out, 'captures_done'))
    except FileNotFoundError: pass
    open(os.path.join(a.out, 'ready'), 'w').write(chain + '\n')
    print('DONE', flush=True)

def pak_keys(path):   # the keys an NXSP shader pack holds, and its chain version
    with open(path, 'rb') as f:
        h = f.read(28)
        if h[:4] != b'NXSP': raise RuntimeError(f'{path} is not a shader pack')
        ver, n = struct.unpack_from('<II', h, 4)
        if ver not in (1, 2, 3, 4): raise RuntimeError(f'{path} has unsupported shader pack version {ver}')
        stride = 44 if ver >= 3 else 32; idx = f.read(stride * n)
    if len(idx) != stride * n: raise RuntimeError(f'{path} has a truncated shader pack index')
    return {idx[stride * i:stride * i + 8] for i in range(n)}, h[12:28].rstrip(b'\0').decode()

def pak_merge(path, add):   # merge a new uncompressed shader pack into the existing pack, preserving any GL depth variants
    with open(add, 'rb') as f: b = f.read()
    add_ver, nb = struct.unpack_from('<II', b, 4)
    if b[:4] != b'NXSP' or add_ver not in (1, 3): raise RuntimeError(f'{add} is not an uncompressed shader pack')
    add_stride = 44 if add_ver == 3 else 32
    src = open(path, 'rb'); h = src.read(28); ver, na = struct.unpack_from('<II', h, 4)
    if h[:4] != b'NXSP' or ver not in (1, 2, 3, 4): raise RuntimeError(f'{path} has an unsupported shader pack')
    old_stride = 44 if ver >= 3 else 32; compressed = ver in (2, 4)
    old_index = src.read(old_stride * na)
    has_gl = ver >= 3 or add_ver >= 3; new_stride = 44 if has_gl else 32; new_ver = (4 if compressed else 3) if has_gl else (2 if compressed else 1)
    index_delta = (new_stride - old_stride) * na + new_stride * nb
    end = os.fstat(src.fileno()).st_size + index_delta; rows = []; blobs = []
    for i in range(na):
        e = old_index[old_stride * i:old_stride * (i + 1)]
        so, sl, ro, rl = struct.unpack_from('<QIQI', e, 8)
        row = [e[:8], so + index_delta, sl, ro + index_delta, rl]
        if has_gl:
            go, gl = struct.unpack_from('<QI', e, 32) if old_stride == 44 else (0, 0)
            row += [go + index_delta if gl else 0, gl]
        rows.append(tuple(row))
    for i in range(nb):
        e = b[28 + add_stride * i:28 + add_stride * (i + 1)]
        so, sl, ro, rl = struct.unpack_from('<QIQI', e, 8); row = [e[:8]]
        pairs = [(so, sl), (ro, rl)]
        if add_stride == 44: pairs.append(struct.unpack_from('<QI', e, 32))
        elif has_gl: pairs.append((0, 0))
        for o, l in pairs:
            if not l: row += [0, 0]; continue
            blob = b[o:o + l]
            if len(blob) != l: raise RuntimeError(f'{add} has a truncated shader blob')
            if compressed: blob = struct.pack('<I', l) + zlib.compress(blob, 9)
            row += [end, len(blob)]; blobs.append(blob); end += len(blob)
        rows.append(tuple(row))
    rows.sort(key=lambda r: r[0]); tmp = path + '.new'
    with open(tmp, 'wb') as out:
        out.write(h[:4] + struct.pack('<II', new_ver, na + nb) + h[12:28])
        for row in rows:
            out.write(row[0] + (struct.pack('<QIQIQI', *row[1:]) if has_gl else struct.pack('<QIQI', *row[1:])))
        src.seek(28 + old_stride * na)
        while True:
            chunk = src.read(1 << 24)
            if not chunk: break
            out.write(chunk)
        for blob in blobs: out.write(blob)
    src.close(); os.replace(tmp, path)

def captures(a):
    # The profile's map places each in-memory shader at a fixed index of <stage>_other.bin; the package is written with the captured ones there and an empty record
    # in each other slot (never read: only the captured keys are translated). They are translated on their own and merged into the shader pack, so nothing
    # from the first setup has to be kept.
    # Captured shaders the map does not know (the profile's slots only list those its recording reached) go after the slots in <stage>_other.bin, the stage from the
    # shader program header (shader_split.py nvn_captured); they are translated in a run of their own that may fail without costing the others, and the ones translated
    # are added to the game's shader_map.tsv. Compute ones wait for the next full setup (their thread group comes from shader_split's facts)
    sys.path.insert(0, os.path.join(DM, 'gfx/tools')); import shader_split
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
    gmap = os.path.join(a.out, 'shader_map.tsv'); mapped = set()
    if os.path.exists(gmap):
        for ln in open(gmap, encoding='utf-8'):
            c = ln.rstrip('\n').split('\t')
            if len(c) > 3 and not ln.startswith('#'): mapped.add(c[3])
    print(f'STEP 1/3 read the captured shaders', flush=True)
    new, extra = [], []
    for f in sorted(os.listdir(a.captures)) if os.path.isdir(a.captures) else []:
        k = f[:-5]
        if not f.endswith('.code') or bytes.fromhex(k) in have or (k not in slots and k in mapped): continue
        if not os.path.exists(os.path.join(a.captures, k + '.ctl')): continue
        ctl, code = open(os.path.join(a.captures, k + '.ctl'), 'rb').read(), open(os.path.join(a.captures, f), 'rb').read()
        if shader_split.skey(ctl, code) != k: log.write(f'capture {k}: the saved files do not hash to their key, skipped\n'); continue
        if k in slots: new.append((k, ctl, code)); continue
        st = 'cs' if code[:4] == shader_split.CS_MAGIC else shader_split.SPH_STAGE.get((struct.unpack_from('<I', code, 0x30)[0] >> 10) & 0xf) if len(code) >= 0x34 else None
        why = 'no stage in the shader program header' if st is None else 'compute: waits for the next full setup' if st == 'cs' else shader_split.check(f'nvn_capture/{k}', st, ctl, code)
        if why: log.write(f'capture {k} (not in the shader map): {why}, skipped\n'); continue
        extra.append((k, st, ctl, code))
    if not new and not extra: print('DONE', flush=True); return
    cw = os.path.join(a.work, 'capture'); shutil.rmtree(cw, ignore_errors=True)
    pkg, cache = os.path.join(cw, 'pkg'), os.path.join(cw, 'shader_cache'); vk, built = os.path.join(cw, 'vk', 'b-' + chain), os.path.join(cache, 'b-' + chain)
    os.makedirs(pkg); os.makedirs(vk)
    got = {k: (ctl, code) for k, ctl, code in new}
    recs, rows = {}, []   # package name → records; the source map rows dmsh_build finds them by
    for name in sorted({p for p, _ in slots.values()}):
        recs[name] = []
        for i, k in sorted((i, k) for k, (p, i) in slots.items() if p == name):
            assert i == len(recs[name]), f'{name}: index {i} out of order'
            ctl, code = got.get(k, (b'', b''))
            rows.append((name, i, shader_split.STAGE_NAME[name[:2]], k, f'/nvn_capture/{k}'))
            rec = bytearray(b'BTCDBTCD' + bytes(0x30)); struct.pack_into('<2I', rec, 0x28, len(ctl), len(code)); recs[name].append(bytes(rec) + code + ctl)
    added = []
    for k, st, ctl, code in extra:
        name = f'{st}_other.bin'; r = recs.setdefault(name, [])
        added.append((name, len(r), shader_split.STAGE_NAME[st], k, f'/nvn_capture/{k}')); rows.append(added[-1])
        rec = bytearray(b'BTCDBTCD' + bytes(0x30)); struct.pack_into('<2I', rec, 0x28, len(ctl), len(code)); r.append(bytes(rec) + code + ctl)
    for name, r in recs.items(): open(os.path.join(pkg, name), 'wb').write(b''.join(r))
    smap = os.path.join(cw, 'capture_map.tsv')
    with open(smap, 'w', encoding='utf-8') as f: f.write('package\tindex\tstage\tdm_ctl_sha16\tsource\n'); f.writelines('\t'.join(map(str, r)) + '\n' for r in rows)
    print(f'STEP 2/3 translate {len(new) + len(extra)} shaders', flush=True)
    benv = dict(env, DMSH_SPV_ONLY='1', VK_SPV_OUT=vk, XLATE=os.path.join(a.bin, 'dmsh_xlate'), DMSH_CAS=os.path.join(cw, 'cas'), WORK=cw, DMSH_SOURCE_MAP=smap)
    if got: run([py, os.path.join(DM, 'gfx/tools/dmsh_build.py'), pkg, cache, *got], env=benv)
    if added:
        try: run([py, os.path.join(DM, 'gfx/tools/dmsh_build.py'), pkg, cache, *(r[3] for r in added)], env=benv)
        except RuntimeError as e: log.write(f'the captured shaders not in the shader map: {e}; the ones translated are kept\n')
        ok = [r for r in added if os.path.exists(os.path.join(vk, r[3] + '.spv')) and os.path.exists(os.path.join(built, r[3] + '_b.json'))]
        for r in added:
            if r not in ok:
                log.write(f'capture {r[3]} (not in the shader map): not translated\n')
                if os.path.exists(os.path.join(vk, r[3] + '.spv')): os.unlink(os.path.join(vk, r[3] + '.spv'))
        added = ok
    for k in [*got, *(r[3] for r in added)]: shutil.copyfile(os.path.join(built, k + '_b.json'), os.path.join(vk, k + '_b.json'))
    if not got and not added: shutil.rmtree(cw, ignore_errors=True); print('DONE', flush=True); return
    print(f'STEP 3/3 pack the shaders', flush=True)
    run([py, os.path.join(DM, 'android/tools/vk_meta.py'), vk, vk, built])
    small = os.path.join(cw, 'new.pak'); run([py, os.path.join(DM, 'android/tools/spv_pak.py'), vk, small])
    pak_merge(pak, small)
    if added:   # the engine reads the map at start: the next one finds these keys and their stage
        with open(gmap, 'a', encoding='utf-8') as f: f.writelines('\t'.join(map(str, r)) + '\n' for r in added)
        log.write(f'{len(added)} captured shaders not in the shader map translated and added to it\n')
    shutil.rmtree(cw, ignore_errors=True)
    print('DONE', flush=True)

if __name__ == '__main__':
    try: main()
    except Exception as e: print(f'FAILED {e}', flush=True); sys.exit(1)
