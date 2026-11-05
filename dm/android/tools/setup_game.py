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
# Environment: DM_GAME (the profile: the app's game, SetupActivity passes it; default 01008DB008C2C000, Pokémon Shield), J (jobs)
import argparse, glob, hashlib, os, re, shutil, signal, struct, subprocess, sys, threading, time, zlib

HERE = os.path.dirname(os.path.abspath(__file__)); DM = os.path.normpath(os.path.join(HERE, '../..'))
GAME = os.environ.setdefault('DM_GAME', '01008DB008C2C000')
sys.path.insert(0, os.path.join(DM, 'games')); import game as _game
PROFILE = _game.load()
NAME = PROFILE.get('name')

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

def watch_translation(found_map, work, built, stop):
    """'PROGRESS done/total' lines for the setup screen (SetupActivity.runSetup) while dmsh_build.py runs: it prints nothing until the end, and a phone takes hours.
    Counted from what it writes (left unchanged, since any edit to it changes the shader chain version): every shader's SPIR-V in its dmsh_* scratch dir,
    the vertex shaders' GL depth variants in gl_depth/ there, then every shader's reflection json in the cache"""
    n = vs = 0
    with open(found_map, encoding='utf-8') as f:
        for line in f:
            c = line.rstrip('\n').split('\t')
            if len(c) > 4 and not line.startswith('#') and c[0] != 'package': n += 1; vs += c[2] == 'vertex'
    total, shown = 2 * n + vs, -1
    def count(d, end):
        try: return sum(e.name.endswith(end) for e in os.scandir(d))
        except OSError: return 0
    while not stop.wait(5):
        done = sum(count(d, '.spv') + count(os.path.join(d, 'gl_depth'), '.spv') for d in glob.glob(os.path.join(work, 'dmsh_*'))) + count(built, '_b.json')
        done = min(done, total - 1)   # the map also lists shaders that wait for capture, so the count can fall short of it: 100% is the step's end
        if done != shown: shown = done; print(f'PROGRESS {done}/{total}', flush=True)

def explain_failure(log_path, files):
    """The unpacker's known failures in words a player can act on (from the end of setup.log), and whether setting up anyway (--allow-damaged) can get past
    it; ('', False) when it is none of them"""
    try:
        with open(log_path, encoding='utf-8', errors='replace') as f: tail = f.read()[-8000:]
    except OSError: return '', False
    run = tail[tail.rfind('$ '):] if '$ ' in tail else tail   # the last run only: an older failure higher up the log is not this one
    names = [os.path.basename(f) for f in files]
    m = re.search(r'Exception: (.+?) is incomplete: it has (\d+) of its (\d+) bytes \(([\d.]+)%\)', run)
    if m:
        return (f'{m.group(1)} is incomplete: it holds {int(m.group(2)) / (1 << 30):.2f} GB of its {int(m.group(3)) / (1 << 30):.2f} GB ({m.group(4)}%). '
                'It was cut short while being copied or downloaded: copy the whole file again, then set up again'), False
    m = re.search(r'\[RED\] damaged: (the update|the base game|your game file) fails its hash check', run)
    if m:
        who = {'the update': 'your update file', 'the base game': 'your base game file', 'your game file': 'your game file'}[m.group(1)]
        return (f'{who} is damaged, or the update does not belong to this base game: its data fails the checksum stored inside it (2002-4604). '
                'Copy it again in full or dump it again from your Switch. "Set up anyway" tries to go on regardless, but the game may then not start'), True
    m = re.search(r'\[RED\] the NSP is damaged: (\d+) files fail', run)
    if m:
        return (f'{m.group(1)} of the game\'s files are damaged in your dump: they fail the checksum stored inside it (2002-4604). Copy your files again in full or dump them '
                'again from your Switch. "Set up anyway" keeps the damaged files as they are: the game may crash where it uses them'), True
    if '2002-4604' in run or 'Hash error' in run or 'VerificationFailed' in run or 'LinkedListEmpty' in run:
        return ('one of your files is damaged: its data does not match the checksum stored inside it. A file cut short or corrupted while being copied or downloaded '
                'does this: copy it again in full, or dump it again from your Switch, then set up again'), True
    if 'MissingKeyException' in run or 'KeyNotFound' in run or 'Unable to decrypt' in run or 'key' in run.lower() and 'missing' in run.lower():
        return 'a key your dump needs is not in your prod.keys / title.keys: dump the keys again from the same Switch (newer system versions add keys)', False
    if 'No space left' in run or 'ENOSPC' in run:
        return 'the phone ran out of storage while unpacking: free some space and set up again', False
    if 'is not an NSP or XCI' in run or 'has a damaged header' in run:
        return 'one of your files is not a Switch game file (.nsp / .xci), or its header is damaged: choose your dump files again', False
    if 'no base game program' in run:
        return 'none of your files is the base game: choose the base game (.nsp / .xci) too, with the update', False
    return '', False

def main():
    global PROFILE, NAME   # (the dump can name another game of the app: --games)
    ap = argparse.ArgumentParser()
    ap.add_argument('--keys'); ap.add_argument('--base'); ap.add_argument('--captures'); ap.add_argument('--update'); ap.add_argument('--dlc', action='append', default=[])
    ap.add_argument('--out', required=True); ap.add_argument('--work', required=True); ap.add_argument('--bin', required=True)
    ap.add_argument('--ext-font'); ap.add_argument('--std-font'); ap.add_argument('--games'); ap.add_argument('--root')
    ap.add_argument('--allow-damaged', action='store_true', help='the unpacker writes files that fail their hash check as read (the setup screen: Set up anyway)')
    ap.add_argument('--live', help='with --captures, while the game runs: the translated shaders also go into this folder for the engine to load at once (nvn_vk.c live_check)')
    ap.add_argument('--mod-shaders', help="the RomFS folders of the mods that are on, ':'-separated: their new shaders translated into the pack")
    ap.add_argument('--resume', action='store_true', help='go on from the shader translation when an earlier run of this setup got that far (resume.json in --work)')
    a = ap.parse_args()
    if a.mod_shaders is not None: return mod_shaders(a)
    if a.games and not a.root: ap.error('--games needs --root')
    if a.captures:
        if a.live: live_yield()
        return captures(a)
    if not (a.keys and a.base): ap.error('--keys and --base are needed (or --captures)')
    os.makedirs(a.work, exist_ok=True); os.makedirs(a.out, exist_ok=True)
    log = open(os.path.join(a.work, 'setup.log'), 'a', buffering=1)
    py = sys.executable; env = dict(os.environ, PYTHONUNBUFFERED='1')
    env['PATH'] = a.bin + os.pathsep + env.get('PATH', '')
    tool = lambda n: os.path.join(a.bin, n)
    steps = ['unpack the game', 'rewrite the code', 'find the shaders', 'translate the shaders', 'pack the shaders', 'finish']
    def step(i, extra=''):
        print(f'STEP {i}/{len(steps)} {steps[i - 1]}{extra}', flush=True); log.write(f'== {time.strftime("%H:%M:%S")} {steps[i - 1]}{extra}\n')
    def run(cmd, retry_killed=0, **kw):
        # retry_killed: the shader translation, which Android's low-memory killer can stop as a whole (#51: both setups ended in it with no failure line): it is run
        #   again that many times, with half the translators each time; what it translated is kept in its cache, so a retry goes on where it stopped
        e = kw.pop('env', env)
        for attempt in range(retry_killed + 1):
            log.write('$ ' + ' '.join(map(str, cmd)) + '\n')
            r = subprocess.run(list(map(str, cmd)), stdout=log, stderr=subprocess.STDOUT, env=e, **kw)
            if r.returncode >= 0 or attempt == retry_killed: break
            slots = max(1, int(e.get('DM_GATE_SLOTS', e.get('J', '8'))) // 2); e = dict(e, J=str(slots), DM_GATE_SLOTS=str(slots))
            log.write(f'{os.path.basename(str(cmd[1]))} was stopped ({r.returncode}): again with {slots} at a time\n')
        if r.returncode:
            name = os.path.basename(str(cmd[1] if str(cmd[0]) == py else cmd[0]))
            if r.returncode < 0:
                try: reason = signal.Signals(-r.returncode).name
                except ValueError: reason = f'signal {-r.returncode}'
                raise RuntimeError(f'{name} terminated by {reason} ({r.returncode}); see {log.name}')
            hint, allow = explain_failure(log.name, files) if name == 'nx_extract' else ('', False)   # (files: the chosen dump files, set before the unpacker runs)
            if allow and not a.allow_damaged: print('OFFER allow-damaged', flush=True)   # the setup screen offers "Set up anyway"
            elif allow: hint = 'even set up anyway, your files could not be read: ' + hint.split(':')[0] + '. Copy them again in full or dump them again from your Switch'
            raise RuntimeError(f'{name} failed ({r.returncode}){": " + hint if hint else ""}; see {log.name}')
    keys = os.path.join(a.keys, 'prod.keys')
    if not os.path.isfile(keys): raise RuntimeError(f'no prod.keys in {a.keys}')
    ex = os.path.join(a.work, 'extract')

    # --resume (the setup screen, after Android stopped this setup, #51 / #52: exit 137 during the shader translation): resume.json, written once the shader scan is done,
    #   says what steps 1-3 found; the unpacked game, the rewritten code and the scanned shaders are still in place, and the translation goes on from what it had done
    state = os.path.join(a.work, 'resume.json'); resumed = False
    if a.resume and os.path.isfile(state) and os.path.isfile(os.path.join(ex, 'ExeFS', 'main')) and os.path.isdir(os.path.join(a.work, 'shader_pkg')):
        import json
        st = json.load(open(state, encoding='utf-8'))
        a.out = st['out']; games = st['games']; files = st['files']
        if st.get('game'):
            PROFILE = _game.load(st['game']); NAME = PROFILE.get('name'); env['DM_GAME'] = os.environ['DM_GAME'] = st['game']
            print(f'GAME {st["game"].upper()}', flush=True)
        exefs, romfs, pkg, pat = os.path.join(ex, 'ExeFS'), os.path.join(ex, 'RomFS'), os.path.join(a.work, 'shader_pkg'), os.path.join(a.out, 'patched')
        resumed = True; log.write(f'== {time.strftime("%H:%M:%S")} resumed after a stop: steps 1-3 kept, the translation goes on\n')
    if not resumed:
        step(1)
        if os.path.isdir(ex): shutil.rmtree(ex)
        # the DLC unpacks into the scratch folder first: which game the files are is only known once they are unpacked, and the chosen game's own DLC (another game's
        # when the files are the other one of the pair) must stay until this setup replaces it
        aoc = os.path.join(a.work, 'aoc'); shutil.rmtree(aoc, ignore_errors=True)
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
        except OSError as e: free = None; log.write(f'free-space check failed: {e}\n')
        # the unpacked game takes about as much as the files it comes from, plus the shaders: said now, not as a failure halfway through the unpacking
        try: need = sum(os.path.getsize(f) for f in files) + (1 << 30)
        except OSError: need = 0
        if free is not None and need and free < need:
            raise RuntimeError(f'not enough free storage: the setup needs about {need / (1 << 30):.1f} GB free and the phone has {free / (1 << 30):.1f} GB. '
                               'Free some space (or delete files you copied into the app), then set up again')
        run([tool('nx_extract'), keys, files[0], '-', ex, '--aoc-dir', aoc] + [x for f in files[1:] for x in ('--more', f)] + (['--allow-damaged'] if a.allow_damaged else []))
        exefs, romfs = os.path.join(ex, 'ExeFS'), os.path.join(ex, 'RomFS')
        if not os.path.isfile(os.path.join(exefs, 'main')): raise RuntimeError(f'the dump has no main module: is it {NAME}?')
        # another game (the other one of Sword and Shield most often, #46) said so before the version check, which would otherwise ask for an update the user already chose:
        # control.nacp holds the title's application ID at 0x3038 and its English name at 0
        try:
            nacp = open(os.path.join(ex, 'control.nacp'), 'rb').read()
            tid = struct.unpack_from('<Q', nacp, 0x3038)[0]
            name = nacp[:0x200].split(b'\0')[0].decode('utf-8', 'replace')
        except (OSError, struct.error): tid, name = 0, ''
        # --games (the app's games, its own first) and --root (the app's folder): the dump says which game it is, and it is set up in that game's place: the first game in
        # --root, another in --root/<title ID>, replacing what was there (one of each game). Without --games, the profile's game only
        games = [g for g in (a.games or '').split(',') if g]
        if tid and games:
            hit = next((i for i, g in enumerate(games) if int(g, 16) == tid), None)
            if hit is None:
                have = ' and '.join(_game.load(g).get('name') for g in games)
                raise RuntimeError(f'your files are {name or "another game"} ({tid:016X}): this app runs {have}')
            PROFILE = _game.load(games[hit]); NAME = PROFILE.get('name'); env['DM_GAME'] = os.environ['DM_GAME'] = games[hit]
            out = a.root if hit == 0 else os.path.join(a.root, games[hit].upper())
            if os.path.realpath(out) != os.path.realpath(a.out):
                os.makedirs(out, exist_ok=True); a.out = out
            log.write(f'the dump is {NAME} ({tid:016X}): set up in {a.out}\n'); print(f'GAME {games[hit].upper()}', flush=True)
        elif tid and tid != PROFILE.title_id:
            other = _game.by_title(tid)
            name = (other.get('name') if other else name) or 'another game'
            raise RuntimeError(f'your files are {name} ({tid:016X}), not {NAME} ({PROFILE.title_id:016X})')
        # the game's version, checked now rather than as a failure ten minutes later in the shader steps: the code (main's build ID) must be one the profile knows
        with open(os.path.join(exefs, 'main'), 'rb') as f: build_id = f.read(0x50)[0x40:0x50].hex()
        builds = PROFILE.builds(); want = ', '.join(sorted(b['display'] for b in builds.values()))
        if build_id not in {b.get('main') for b in builds.values()}:
            have = ''
            try: have = open(os.path.join(ex, 'control.nacp'), 'rb').read()[0x3060:0x3070].split(b'\0')[0].decode()
            except OSError: pass
            raise RuntimeError(f'your files hold {NAME} {have or "of an unknown version"}, and this app needs update {want}: '
                               f'choose the {want} update too (a separate update file, or a dump that includes it)')

        # from here the game's folder changes: it is not ready until this setup finishes (a setup stopped halfway left the old ready file, and Play then started a game
        # with its rewritten code deleted)
        try: os.remove(os.path.join(a.out, 'ready'))
        except FileNotFoundError: pass
        dst = os.path.join(a.out, 'aoc'); shutil.rmtree(dst, ignore_errors=True)
        if os.path.isdir(aoc): shutil.move(aoc, dst)
        aoc = dst

        step(2)
        pat = os.path.join(a.out, 'patched'); shutil.rmtree(pat, ignore_errors=True); os.makedirs(pat)
        for m in PROFILE.words('modules'):
            if os.path.isfile(os.path.join(exefs, m)):
                run([py, os.path.join(DM, 'tools/patch_nso.py'), os.path.join(exefs, m), pat, m], env=dict(env, DM_PATCH_TARGET='android'))

        step(3)
        pkg = os.path.join(a.work, 'shader_pkg'); shutil.rmtree(pkg, ignore_errors=True)
        # the shaders an earlier run of the game captured (nvn_capture/ in the game dir, android_main.c DM_NVN_CAPTURE) go into the new pack too: the game saves each
        # one only once, so a pack rebuilt without them would never get them back
        # Trinity games (Violet: every file of arc/data.trpfs is Oodle-compressed): the scan decodes them with the game's own decoder, dmoodle running the rewritten
        # modules of step 2, and only the files that hold shaders come back over the pipe (hle_oodle.c DM_OODLE_SERVE=BNSH)
        if PROFILE.get('shader_oodle', 'off') == 'on':
            if not os.path.isfile(tool('dmoodle')): raise RuntimeError(f'{NAME} needs the dmoodle tool, which this app build does not have')
            env.update(DM_OODLE_HOST=tool('dmoodle'), DM_OODLE_MODULES=pat, DM_OODLE_FILTER='BNSH')
        run([py, os.path.join(DM, 'gfx/tools/shader_split.py'), romfs, os.path.join(exefs, 'main'), pkg,
             os.path.join(a.work, 'shader_map.found.tsv'), os.path.join(a.work, 'shader_facts.found.tsv'), os.path.join(a.work, 'shader_pairs.found.tsv')],
            env=dict(env, DM_NVN_CAPTURE=os.path.join(a.out, 'nvn_capture')))

        import json
        json.dump({'out': a.out, 'game': env.get('DM_GAME'), 'games': games, 'files': files}, open(state, 'w', encoding='utf-8'))
    step(4)
    chain = subprocess.run([py, os.path.join(DM, 'gfx/tools/dmsh_chain.py')], capture_output=True, text=True, env=env).stdout.strip()
    # The other game of the pair already set up with the same translation chain (Scarlet and Violet share every one of their 35,937 shaders; Sword and Shield most of
    # theirs): its pack is linked (or copied) in, and only the shaders it lacks are translated and merged into it, instead of the hours of translating them all
    found_keys = {bytes.fromhex(c[3][:16]) for c in (l.rstrip('\n').split('\t') for l in open(os.path.join(a.work, 'shader_map.found.tsv'), encoding='utf-8'))
                  if len(c) > 3 and len(c[3]) >= 16 and not c[0].startswith('#') and c[0] != 'package'}
    donor = None
    for i, g in enumerate(games):
        d = a.root if i == 0 else os.path.join(a.root, g.upper())
        dp = os.path.join(d, 'shaders.pak')
        if os.path.realpath(d) == os.path.realpath(a.out) or not os.path.isfile(dp) or not os.path.isfile(os.path.join(d, 'ready')): continue
        try: have, pchain = pak_keys(dp)
        except (OSError, RuntimeError, struct.error): continue
        if pchain == chain and len(found_keys & have) > len(found_keys) // 2: donor = (dp, have); break
    if donor:
        dp, have = donor; missing = sorted(k.hex() for k in found_keys - have)
        log.write(f'shaders: {len(found_keys) - len(missing)} of {len(found_keys)} taken from {dp}, {len(missing)} to translate\n')
        print(f'STEP 4/6 translate the shaders (reusing {len(found_keys) - len(missing)} from your other game)', flush=True)
        target = os.path.join(a.out, 'shaders.pak'); tmp = target + '.part'
        for f in (tmp, target):
            if os.path.exists(f): os.remove(f)
        try: os.link(dp, tmp)   # (no extra space: a later merge into either game writes a new file and leaves the other's alone)
        except OSError: shutil.copyfile(dp, tmp)
        if missing:
            vk = os.path.join(a.work, 'vk', 'b-' + chain); cache = os.path.join(a.work, 'shader_cache')
            if not resumed: shutil.rmtree(os.path.dirname(vk), ignore_errors=True); shutil.rmtree(cache, ignore_errors=True)
            os.makedirs(vk, exist_ok=True)
            gate = os.path.isfile(tool('dmsh_gate')) and os.path.isfile('/proc/meminfo')
            xenv = dict(XLATE=tool('dmsh_gate'), DM_GATE_EXEC=tool('dmsh_xlate'), DM_GATE_SLOTS=env.get('J', '8'), DM_GATE_DIR=a.work) if gate else dict(XLATE=tool('dmsh_xlate'))
            run([py, os.path.join(DM, 'gfx/tools/dmsh_build.py'), pkg, cache, *missing], retry_killed=3,
                env=dict(env, DMSH_SPV_ONLY='1', VK_SPV_OUT=vk, **xenv, DMSH_CAS=os.path.join(a.work, 'cas'), WORK=a.work,
                         DMSH_SOURCE_MAP=os.path.join(a.work, 'shader_map.found.tsv'), DMSH_FACTS=os.path.join(a.work, 'shader_facts.found.tsv'),
                         DMSH_PAIRS=os.path.join(a.work, 'shader_pairs.found.tsv'), MAIN=os.path.join(exefs, 'main')))
            built = os.path.join(cache, 'b-' + chain)
            for f in os.listdir(built):
                if f.endswith('_b.json'): shutil.copyfile(os.path.join(built, f), os.path.join(vk, f))
            step(5)
            run([py, os.path.join(DM, 'android/tools/vk_meta.py'), vk, vk, built])
            small = os.path.join(a.work, 'shaders.new.pak'); run([py, os.path.join(DM, 'android/tools/spv_pak.py'), vk, small])
            pak_merge(tmp, small); os.remove(small)
        else: step(5)
        os.replace(tmp, target)
    else:
        vk = os.path.join(a.work, 'vk', 'b-' + chain); cache = os.path.join(a.work, 'shader_cache')
        if not resumed:
            shutil.rmtree(os.path.dirname(vk), ignore_errors=True); shutil.rmtree(cache, ignore_errors=True)
            for d in glob.glob(os.path.join(a.work, 'dmsh_*')): shutil.rmtree(d, ignore_errors=True)   # an interrupted run's scratch dir would count toward the progress
        os.makedirs(vk, exist_ok=True)
        stop = threading.Event(); found = os.path.join(a.work, 'shader_map.found.tsv')
        threading.Thread(target=watch_translation, args=(found, a.work, os.path.join(cache, 'b-' + chain), stop), daemon=True).start()
        # the translators go through dmsh_gate when the app has it: each beyond the first starts only while the phone has the memory for it (dmsh_gate.c)
        gate = os.path.isfile(tool('dmsh_gate')) and os.path.isfile('/proc/meminfo')
        xenv = dict(XLATE=tool('dmsh_gate'), DM_GATE_EXEC=tool('dmsh_xlate'), DM_GATE_SLOTS=env.get('J', '8'), DM_GATE_DIR=a.work) if gate else dict(XLATE=tool('dmsh_xlate'))
        log.write(f'translators: {env.get("J", "8")} at most, ' + ('started as memory allows (dmsh_gate)\n' if gate else 'all at once (no dmsh_gate)\n'))
        try: run([py, os.path.join(DM, 'gfx/tools/dmsh_build.py'), pkg, cache], retry_killed=3,
            env=dict(env, DMSH_SPV_ONLY='1', VK_SPV_OUT=vk, **xenv, DMSH_CAS=os.path.join(a.work, 'cas'), WORK=a.work,
                     DMSH_SOURCE_MAP=os.path.join(a.work, 'shader_map.found.tsv'),
                     DMSH_FACTS=os.path.join(a.work, 'shader_facts.found.tsv'),
                     DMSH_PAIRS=os.path.join(a.work, 'shader_pairs.found.tsv'),
                     MAIN=os.path.join(exefs, 'main')))
        finally: stop.set()
        built = os.path.join(cache, 'b-' + chain)
        for f in os.listdir(built):
            if f.endswith('_b.json'): shutil.copyfile(os.path.join(built, f), os.path.join(vk, f))

        step(5)
        run([py, os.path.join(DM, 'android/tools/vk_meta.py'), vk, vk, built])
        # the pack compressed (zlib per shader, the engine inflates each as it loads it: nvn_vk.c pak_blob): about 3.4 times smaller on the phone (Violet 2.9 GB → 0.85 GB, Sword 2.3 → 0.7)
        raw = os.path.join(a.work, 'shaders.raw.pak')
        run([py, os.path.join(DM, 'android/tools/spv_pak.py'), vk, raw])
        run([py, os.path.join(DM, 'android/tools/spv_pak.py'), '--compress', raw, os.path.join(a.out, 'shaders.pak')])
        os.remove(raw)

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
    for d in ('shader_pkg', 'cas', 'vk', 'shader_cache', 'extract', 'aoc'): shutil.rmtree(os.path.join(a.work, d), ignore_errors=True)   # only setup.log stays
    # the new pack holds every capture there is, so the launcher's list of captures already added is stale: the next start checks them against the pack again
    try: os.remove(os.path.join(a.out, 'captures_done'))
    except FileNotFoundError: pass
    open(os.path.join(a.out, 'ready'), 'w').write(chain + '\n')
    try: os.remove(state)
    except FileNotFoundError: pass
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

def trinity_index(a, roots, log):
    # Trinity games (Scarlet / Violet) read every file through arc/data.trpfd, the sorted list of its paths' FNV-1a 64 hashes: a mod's loose file is only read once its path
    #   is out of that list. <game>/mods/.trinity_index/romfs/arc/data.trpfd is the game's own index with every path the enabled mods replace moved up by one (the list stays
    #   sorted, every other file is still found); android_main.c lays it over the RomFS first. No mods: it goes, so no path is left pointing at a loose file that is gone
    src = os.path.join(a.out, 'RomFS', 'arc', 'data.trpfd'); idx = os.path.join(a.out, 'mods', '.trinity_index')
    if not os.path.isfile(src): return
    sys.path.insert(0, os.path.join(DM, 'gfx/tools')); import shader_split as ss, bisect
    def fnv(s):
        h = 0xcbf29ce484222645
        for c in s.encode(): h = ((h ^ c) * 0x100000001b3) & 0xffffffffffffffff
        return h
    rels = set()
    for r in roots:
        for d, _, fs in os.walk(r):
            for f in fs:
                rel = os.path.relpath(os.path.join(d, f), r).replace(os.sep, '/')
                if rel != 'arc/data.trpfd': rels.add(rel)
    shutil.rmtree(idx, ignore_errors=True)
    if not rels: log.write('trinity index: no mod files, none\n'); return
    fd = bytearray(open(src, 'rb').read()); fr = ss.fb_root(fd); st, n = ss.fb_vec(fd, fr, 0)
    hashes = list(struct.unpack_from(f'<{n}Q', fd, st)); moved = 0
    for rel in sorted(rels):
        h = fnv(rel); i = bisect.bisect_left(hashes, h)
        if i < n and hashes[i] == h and (i + 1 >= n or hashes[i + 1] != h + 1):
            struct.pack_into('<Q', fd, st + 8 * i, h + 1); moved += 1
    os.makedirs(os.path.join(idx, 'romfs', 'arc')); open(os.path.join(idx, 'romfs', 'arc', 'data.trpfd'), 'wb').write(fd)
    log.write(f'trinity index: {moved} of {len(rels)} mod file(s) taken out of the pack (the rest are new files)\n')

def mod_shaders(a):
    # Mods with shaders of their own (a changed .bfsha, a new effect): the game would hand the engine shaders the pack lacks, which then draw nothing until a capture
    # is translated on the next start, and captures miss the vertex / pixel pairing and compute facts a full scan has. So the mods' RomFS is scanned as setup scans the
    # game's (shader_split.py), and the shaders the pack does not have are translated with their pairs and facts and merged into it, before the game starts
    log = open(os.path.join(a.work, 'setup.log'), 'a', buffering=1); py = sys.executable
    env = dict(os.environ, PYTHONUNBUFFERED='1', PATH=a.bin + os.pathsep + os.environ.get('PATH', ''))
    tool = lambda n: os.path.join(a.bin, n)
    def run(cmd, **kw):
        log.write('$ ' + ' '.join(map(str, cmd)) + '\n')
        r = subprocess.run(list(map(str, cmd)), stdout=log, stderr=subprocess.STDOUT, env=kw.pop('env', env), **kw)
        if r.returncode: raise RuntimeError(f'{os.path.basename(str(cmd[1]))} failed ({r.returncode}): see {log.name}')
    roots = [r for r in a.mod_shaders.split(':') if r and os.path.isdir(r)]
    log.write(f'== {time.strftime("%H:%M:%S")} mod shaders: {", ".join(roots) or "no mod folders"}\n')
    trinity_index(a, roots, log)
    if not roots: print('DONE', flush=True); return
    chain = subprocess.run([py, os.path.join(DM, 'gfx/tools/dmsh_chain.py')], capture_output=True, text=True, env=env).stdout.strip()
    pak = os.path.join(a.out, 'shaders.pak'); have, pchain = pak_keys(pak)
    if pchain != chain: raise RuntimeError(f'the shader pack is version {pchain}, this app makes {chain}: set the game up again')
    mw = os.path.join(a.work, 'mods'); shutil.rmtree(mw, ignore_errors=True)
    pkg, cache = os.path.join(mw, 'pkg'), os.path.join(mw, 'shader_cache'); vk, built = os.path.join(mw, 'vk', 'b-' + chain), os.path.join(cache, 'b-' + chain)
    smap, facts, pairs = (os.path.join(mw, f) for f in ('map.tsv', 'facts.tsv', 'pairs.tsv'))
    os.makedirs(vk)
    print('STEP 1/3 find the mods\' shaders', flush=True)
    if PROFILE.get('shader_oodle', 'off') == 'on' and os.path.isfile(tool('dmoodle')):   # (Trinity games: a mod's own packs are Oodle-compressed too)
        env.update(DM_OODLE_HOST=tool('dmoodle'), DM_OODLE_MODULES=os.path.join(a.out, 'patched'), DM_OODLE_FILTER='BNSH')
    run([py, os.path.join(DM, 'gfx/tools/shader_split.py'), ','.join(roots), ','.join('-' for _ in roots), pkg, smap, facts, pairs])
    new = []
    for ln in open(smap, encoding='utf-8'):
        c = ln.rstrip('\n').split('\t')
        if len(c) > 3 and not ln.startswith(('#', 'package\t')) and bytes.fromhex(c[3][:16]) not in have: new.append(c)
    log.write(f'mod shaders: {len(new)} the pack does not have\n')
    if not new: shutil.rmtree(mw, ignore_errors=True); print('DONE', flush=True); return
    keys = [c[3] for c in new]
    print(f'STEP 2/3 translate {len(keys)} shaders', flush=True)
    gate = os.path.isfile(tool('dmsh_gate')) and os.path.isfile('/proc/meminfo')
    xenv = dict(XLATE=tool('dmsh_gate'), DM_GATE_EXEC=tool('dmsh_xlate'), DM_GATE_SLOTS=env.get('J', '8'), DM_GATE_DIR=mw) if gate else dict(XLATE=tool('dmsh_xlate'))
    benv = dict(env, DMSH_SPV_ONLY='1', VK_SPV_OUT=vk, **xenv, DMSH_CAS=os.path.join(mw, 'cas'), WORK=mw, DMSH_SOURCE_MAP=smap, DMSH_FACTS=facts, DMSH_PAIRS=pairs)
    try: run([py, os.path.join(DM, 'gfx/tools/dmsh_build.py'), pkg, cache, *keys], env=benv)
    except RuntimeError as e: log.write(f'mod shaders: {e}; the ones translated are kept\n')
    ok = [k for k in keys if os.path.exists(os.path.join(vk, k + '.spv')) and os.path.exists(os.path.join(built, k + '_b.json'))]
    for k in keys:
        if k not in ok:
            log.write(f'mod shader {k}: not translated\n')
            if os.path.exists(os.path.join(vk, k + '.spv')): os.unlink(os.path.join(vk, k + '.spv'))
    for k in ok: shutil.copyfile(os.path.join(built, k + '_b.json'), os.path.join(vk, k + '_b.json'))
    if not ok: shutil.rmtree(mw, ignore_errors=True); print('DONE', flush=True); return
    print('STEP 3/3 pack the shaders', flush=True)
    run([py, os.path.join(DM, 'android/tools/vk_meta.py'), vk, vk, built])
    small = os.path.join(mw, 'new.pak'); run([py, os.path.join(DM, 'android/tools/spv_pak.py'), vk, small])
    pak_merge(pak, small)
    gmap = os.path.join(a.out, 'shader_map.tsv'); got = set(ok)   # the engine reads the map at start: the keys and their stage, with where they came from
    with open(gmap, 'a', encoding='utf-8') as f: f.writelines('\t'.join(c[:4] + ['/mods' + c[4] if len(c) > 4 else '/mods']) + '\n' for c in new if c[3] in got)
    log.write(f'mod shaders: {len(ok)} of {len(keys)} translated and added to the pack\n')
    shutil.rmtree(mw, ignore_errors=True)
    print('DONE', flush=True)

def live_yield():
    # beside the running game (inherited by every tool started from here): the lowest priority, the phone's small cores only (the game keeps the fast ones), and
    # first in line for Android's low-memory killer (oom_score_adj 1000), so a phone short of memory ends this translation, not the game; the shader then waits for the next start
    try: os.nice(10)
    except OSError: pass
    try:
        with open('/proc/self/oom_score_adj', 'w') as f: f.write('1000')
    except OSError: pass
    top = {}
    for c in range(os.cpu_count() or 0):
        try: top[c] = int(open(f'/sys/devices/system/cpu/cpu{c}/cpufreq/cpuinfo_max_freq').read())
        except (OSError, ValueError): pass
    small = {c for c, f in top.items() if f == min(top.values())} if len(set(top.values())) > 1 else set()
    if small:
        try: os.sched_setaffinity(0, small)
        except (OSError, AttributeError): pass

def live_publish(live, vk, rows):
    # the game is running: its shaders go where the engine looks while it runs (nvn_vk.c live_check), files first, then their map rows, then the generation the engine
    # watches; the next start finds them in the pack (merged above) and clears this folder
    os.makedirs(live, exist_ok=True)
    for r in rows:
        for ext in ('.spv', '.vkr', '.gl.spv'):
            f = os.path.join(vk, r[3] + ext)
            if os.path.exists(f): shutil.copyfile(f, os.path.join(live, r[3] + ext + '.part')); os.replace(os.path.join(live, r[3] + ext + '.part'), os.path.join(live, r[3] + ext))
    with open(os.path.join(live, 'map.tsv'), 'a', encoding='utf-8') as f: f.writelines('\t'.join(map(str, r)) + '\n' for r in rows)
    gen = os.path.join(live, 'gen'); open(gen + '.part', 'w').write(f'{time.time_ns()}\n'); os.replace(gen + '.part', gen)

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
    if a.live: live_publish(a.live, vk, [r for r in rows if r[3] in got] + added)
    if added:   # the engine reads the map at start: the next one finds these keys and their stage
        with open(gmap, 'a', encoding='utf-8') as f: f.writelines('\t'.join(map(str, r)) + '\n' for r in added)
        log.write(f'{len(added)} captured shaders not in the shader map translated and added to it\n')
    shutil.rmtree(cw, ignore_errors=True)
    print('DONE', flush=True)

if __name__ == '__main__':
    try: main()
    except Exception as e: print(f'FAILED {e}', flush=True); sys.exit(1)
