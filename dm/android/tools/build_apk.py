#!/usr/bin/env python3
# Builds the installable app (no game content): the engine library, the setup screen and the tools the phone runs to set the game up from the player's dump.
# Runs on Windows, Linux and macOS (no Xcode, no sips): Android build-tools (aapt2, d8, zipalign, apksigner), a JDK (javac), Python 3.
#   lib/arm64-v8a: libnxnative.so (the engine, Makefile.host lib / win_build_lib.py), libnx_extract.so and libdmsh_xlate.so (.NET NativeAOT: extract_aot/, xlate_aot/),
#                  libspirv_cross.so (spirv_cross/), libpython3.14.so + libpython_exe.so (CPython for Android and its launcher), libcapstone.so, liblz4.so
#   assets: python.zip (CPython's standard library, trimmed), dm.zip (the setup scripts, the game profile, the Python stand-ins for lz4 and capstone), shader_chain (the engine's shader translation chain)
# Usage: build_apk.py --libs <dir with the .so files> --pylib <CPython for Android's lib/python3.14> --capstone-py <the capstone Python package> --out <app.apk>
#        [--ks <keystore> (default dm/android/probe/out/debug.keystore; made when missing)] [--bt <build-tools dir>] [--jar <android.jar>]
#        [--game <title ID> (default 01008DB008C2C000, Pokémon Shield; 0100ABF008968000 Pokémon Sword: libnxnative.so built for that game)] [--name <the profile's name>] [--package <com.nxnative.pokemon<game>>]
import argparse, glob, hashlib, io, os, shutil, struct, subprocess, sys, tempfile, zipfile, zlib
from xml.sax.saxutils import escape as xml_escape   # (the label: "Pokémon Sword & Shield")

HERE = os.path.dirname(os.path.abspath(__file__)); ANDROID = os.path.dirname(HERE); DM = os.path.dirname(ANDROID)
LIBS = ['libnxnative.so', 'libnx_extract.so', 'libdmsh_xlate.so', 'libspirv_cross.so', 'libpython3.14.so', 'libpython_exe.so', 'libcapstone.so', 'liblz4.so', 'libssl_python.so', 'libcrypto_python.so']
OPTIONAL = ['libdmsh_gate.so', 'libdmoodle.so', 'libadrenotools.so', 'libhook_impl.so', 'libmain_hook.so', 'libfile_redirect_hook.so', 'libgsl_alloc_hook.so']   # libadrenotools: custom Adreno drivers (Turnip); without them the phone's own driver only
PY_SKIP = ('test', 'idlelib', 'tkinter', 'turtledemo', 'ensurepip', 'pydoc_data', 'lib2to3', 'venv', 'sqlite3', '__pycache__', 'unittest', 'wsgiref', 'xmlrpc', 'curses', 'dbm')
DM_PARTS = ['games/game.py', 'tools/patch_nso.py', 'gfx/tools', 'ios/tools/ios_metal.py', 'android/tools']   # and every game profile (setup names the other game when given its dump)
sys.path.insert(0, os.path.join(DM, 'games')); import game as _game

def png(size, top=(220, 40, 50), bg=(24, 32, 64)):   # the app's icon, drawn here (no game artwork ships): a ball, its top half coloured (the profile's app_icon), on a dark ground
    px = []
    for y in range(size):
        row = bytearray([0])
        for x in range(size):
            dx, dy = (x + 0.5) / size - 0.5, (y + 0.5) / size - 0.5; d = (dx * dx + dy * dy) ** 0.5
            c = bg
            if d < 0.40: c = top if dy < -0.03 else (245, 245, 245) if dy > 0.03 else (30, 30, 36)
            if d < 0.11: c = (30, 30, 36)
            if d < 0.075: c = (250, 250, 250)
            row += bytes(c)
        px.append(bytes(row))
    raw = zlib.compress(b''.join(px), 9)
    chunk = lambda t, d: struct.pack('>I', len(d)) + t + d + struct.pack('>I', zlib.crc32(t + d) & 0xffffffff)
    return b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', size, size, 8, 2, 0, 0, 0)) + chunk(b'IDAT', raw) + chunk(b'IEND', b'')

def zipdir(z, src, arc, skip=()):
    if not os.path.isdir(src): sys.exit('[RED] missing folder: ' + src)
    for d, ds, fs in os.walk(src):
        ds[:] = [x for x in ds if x not in skip and x not in ('out', 'obj', 'bin')]
        for f in fs:
            if f.endswith(('.pyc', '.dll', '.dylib')): continue
            p = os.path.join(d, f); z.write(p, os.path.join(arc, os.path.relpath(p, src)).replace(os.sep, '/'))

def shader_chain(g):   # the translation chain the engine is compiled for (DMSH_CHAIN in the game's dmsh_chain.h, nvn_vk.c)
    h = g.table('dmsh_chain.h')
    for ln in open(h, encoding='utf-8'):
        if ln.startswith('#define DMSH_CHAIN '): return ln.split('"')[1]
    sys.exit(f'[RED] no DMSH_CHAIN in {h}')

def load_align(path):   # the smallest p_align of an ELF64's LOAD segments (0: not an ELF)
    with open(path, 'rb') as f: h = f.read(64)
    if h[:4] != b'\x7fELF' or h[4] != 2: return 0
    phoff, = struct.unpack_from('<Q', h, 0x20); phentsize, phnum = struct.unpack_from('<HH', h, 0x36)
    with open(path, 'rb') as f: f.seek(phoff); ph = f.read(phentsize * phnum)
    al = [struct.unpack_from('<Q', ph, i * phentsize + 0x30)[0] for i in range(phnum) if struct.unpack_from('<I', ph, i * phentsize)[0] == 1]
    return min(al) if al else 0

def validate_extractor(path):
    with open(path, 'rb') as f: data = f.read()
    obsolete_error = 'LibHac cache count was not found'
    stale_release_hash = 'd40d71f2f0faf4c54bf92476fb9e10c8866f132e296ca73f63aba30623cee6c8'
    if hashlib.sha256(data).hexdigest() == stale_release_hash or obsolete_error.encode('utf-8') in data or obsolete_error.encode('utf-16le') in data:
        sys.exit('[RED] stale libnx_extract.so: rebuild it from this checkout before packaging (run scripts/build_apk_pc.sh)')

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--libs', required=True); ap.add_argument('--pylib', required=True); ap.add_argument('--capstone-py', required=True); ap.add_argument('--out', required=True)
    ap.add_argument('--ks', default=os.path.join(ANDROID, 'probe/out/debug.keystore')); ap.add_argument('--bt'); ap.add_argument('--jar'); ap.add_argument('--name')
    ap.add_argument('--game', default='01008DB008C2C000'); ap.add_argument('--package'); ap.add_argument('--version', type=int, default=12)
    ap.add_argument('--debuggable', action='store_true', help='test builds: adb run-as can reach the private storage of the app')
    ap.add_argument('--profileable', action='store_true', help='test builds: simpleperf over adb can profile the app (<profileable android:shell="true"/>), without making it debuggable')
    a = ap.parse_args()
    g = _game.load(a.game); fam = _game.family(g)   # the games the engine built for g runs (Pokémon Shield: Shield and Sword, one app)
    ic = g.get('app_icon', 'dc2832 182040').split(); icon = [tuple(int(h[i:i + 2], 16) for i in (0, 2, 4)) for h in ic[:2]]   # app_icon: top colour, ground colour (hex)
    a.name = a.name or (g.get('app_name', 'Galar Native') if len(fam) > 1 else g.get('name'))   # the app's name (the base game's app_name: Galar Native runs Sword and Shield, Paldea Native Scarlet and Violet; each game's own shortcut names it)
    a.package = a.package or 'com.nxnative.' + _game.file_stem(g).lower()   # com.nxnative.pokemonshield: the Shield app's package, so its players update in place
    exe = '.exe' if os.name == 'nt' else ''; bat = '.bat' if os.name == 'nt' else ''
    bt = a.bt or os.path.expanduser('~/nxnative-work/tools/android-15'); jar = a.jar or os.path.expanduser('~/nxnative-work/tools/android-35/android.jar')
    missing = [l for l in LIBS if not os.path.isfile(os.path.join(a.libs, l))]
    if missing: sys.exit('[RED] missing native libraries in ' + a.libs + ': ' + ', '.join(missing))
    validate_extractor(os.path.join(a.libs, 'libnx_extract.so'))
    # 16 KB pages: Android 15+ warns about an app with a 4 KB aligned library ("Android App Compatibility"), and a phone with 16 KB pages cannot load one at all
    # (scripts/build_apk_pc.sh links them all 16 KB aligned). STRICT_16K=1 refuses to package one
    small = [l for l in LIBS + OPTIONAL if os.path.isfile(os.path.join(a.libs, l)) and 0 < load_align(os.path.join(a.libs, l)) < 0x4000]
    if small:
        msg = 'native libraries not 16 KB aligned (rebuild them with scripts/build_apk_pc.sh): ' + ', '.join(small)
        if os.environ.get('STRICT_16K') == '1': sys.exit('[RED] ' + msg)
        print('[WARNING] ' + msg)
    # the engine must be built for the chain setup makes (v0.36 shipped an engine built before a translation change: every fresh shader pack was refused, a blue screen)
    with open(os.path.join(a.libs, 'libnxnative.so'), 'rb') as f:
        so = f.read()
        if shader_chain(g).encode() not in so:
            sys.exit('[RED] libnxnative.so was not built for shader chain ' + shader_chain(g) + ' (dmsh_chain.h): rebuild it (delete it and run win_build_lib.py)')
        if any(f.id.encode() not in so for f in fam):   # the engine is built per profile (game_profile.h: DM_GAME_FAMILY, the games it runs)
            sys.exit(f'[RED] libnxnative.so was not built for {g.get("name")} ({g.id}): run win_build_lib.py {g.id}')
    w = tempfile.mkdtemp(prefix='apk_')
    # resources: the icon; the manifest: the setup screen launches, the native activity plays
    for dpi, sz in (('xxxhdpi', 192), ('xxhdpi', 144), ('xhdpi', 96)):
        os.makedirs(os.path.join(w, 'res', f'mipmap-{dpi}'), exist_ok=True); open(os.path.join(w, 'res', f'mipmap-{dpi}', 'ic_launcher.png'), 'wb').write(png(sz, *icon))
    open(os.path.join(w, 'AndroidManifest.xml'), 'w', encoding='utf-8').write(f'''<?xml version="1.0" encoding="utf-8"?>
<manifest xmlns:android="http://schemas.android.com/apk/res/android" package="{a.package}" android:versionCode="{a.version}" android:versionName="0.{a.version}">
    <uses-sdk android:minSdkVersion="31" android:targetSdkVersion="35" />
    <uses-permission android:name="android.permission.VIBRATE" />
    <uses-permission android:name="android.permission.INTERNET" />
    <uses-permission android:name="android.permission.ACCESS_NETWORK_STATE" />
    <uses-permission android:name="android.permission.ACCESS_WIFI_STATE" />
    <uses-permission android:name="android.permission.CHANGE_WIFI_MULTICAST_STATE" />
    <uses-permission android:name="android.permission.MANAGE_EXTERNAL_STORAGE" />
    <uses-feature android:name="android.hardware.vulkan.version" android:version="0x401000" android:required="true" />
    <uses-feature android:name="android.hardware.touchscreen" android:required="false" />
    <uses-feature android:name="android.hardware.gamepad" android:required="false" />
    <application android:label="{xml_escape(a.name)}" android:icon="@mipmap/ic_launcher" android:hasCode="true" android:extractNativeLibs="true"
                 android:allowNativeHeapPointerTagging="false" android:isGame="true" android:appCategory="game" android:largeHeap="true" android:hasFragileUserData="true"{' android:debuggable="true"' if a.debuggable else ''}>
        {'<profileable android:shell="true" />' if a.profileable else ''}
        <activity android:name="com.nxnative.SetupActivity" android:exported="true" android:theme="@android:style/Theme.DeviceDefault"
                  android:configChanges="orientation|screenSize|screenLayout|keyboard|keyboardHidden|navigation|uiMode|smallestScreenSize|density">
            <intent-filter><action android:name="android.intent.action.MAIN" /><category android:name="android.intent.category.LAUNCHER" /></intent-filter>
        </activity>
        <activity android:name="com.nxnative.GameActivity" android:exported="false" android:screenOrientation="sensorLandscape" android:launchMode="singleTask"
                  android:theme="@android:style/Theme.Black.NoTitleBar.Fullscreen"
                  android:configChanges="orientation|screenSize|screenLayout|keyboard|keyboardHidden|navigation|uiMode|smallestScreenSize|density">
            <meta-data android:name="android.app.lib_name" android:value="nxnative" />
        </activity>
    </application>
</manifest>
''')
    run = lambda *c: subprocess.run(list(map(str, c)), check=True)
    run(os.path.join(bt, 'aapt2' + exe), 'compile', '--dir', os.path.join(w, 'res'), '-o', os.path.join(w, 'res.zip'))
    run(os.path.join(bt, 'aapt2' + exe), 'link', '-o', os.path.join(w, 'base.apk'), '-I', jar, '--manifest', os.path.join(w, 'AndroidManifest.xml'), os.path.join(w, 'res.zip'))
    # Java: the setup screen, the keyboard dialog, the immersive helper
    cls = os.path.join(w, 'classes'); os.makedirs(cls)
    srcs = [os.path.join(d, f) for d, _, fs in os.walk(os.path.join(ANDROID, 'app/java')) for f in fs if f.endswith('.java')]
    run('javac', '-source', '8', '-target', '8', '-nowarn', '-cp', jar, '-d', cls, *srcs)
    run(os.path.join(bt, 'd8' + bat), '--release', '--min-api', '33', '--lib', jar, '--output', w, *[os.path.join(d, f) for d, _, fs in os.walk(cls) for f in fs])
    # the payload: native libraries, the Python standard library, the setup scripts
    with zipfile.ZipFile(os.path.join(w, 'base.apk'), 'a') as z:
        z.write(os.path.join(w, 'classes.dex'), 'classes.dex')
        for l in LIBS + [o for o in OPTIONAL if os.path.isfile(os.path.join(a.libs, o))]: z.write(os.path.join(a.libs, l), 'lib/arm64-v8a/' + l, compress_type=zipfile.ZIP_DEFLATED)
        b = io.BytesIO()
        with zipfile.ZipFile(b, 'w', zipfile.ZIP_DEFLATED) as pz: zipdir(pz, a.pylib, 'lib/python3.14', PY_SKIP)
        z.writestr('assets/python.zip', b.getvalue(), compress_type=zipfile.ZIP_STORED)
        b = io.BytesIO()
        with zipfile.ZipFile(b, 'w', zipfile.ZIP_DEFLATED) as dz:
            for part in DM_PARTS + ['games/' + p.id for p in _game.all_games()]:
                p = os.path.join(DM, part)
                if os.path.isdir(p): zipdir(dz, p, part)
                else: dz.write(p, part)
            zipdir(dz, os.path.join(ANDROID, 'pyshim'), 'android/pyshim')
            zipdir(dz, a.capstone_py, 'android/pyshim/capstone', ('lib', 'include', '__pycache__'))
            for f in ('nintendo_ext.ttf', 'nintendo_ext.LICENSE.txt'):
                p = os.path.join(ANDROID, 'app/assets', f)
                if os.path.isfile(p): dz.write(p, 'android/app/assets/' + f)
        z.writestr('assets/dm.zip', b.getvalue(), compress_type=zipfile.ZIP_STORED)
        for f in ('cjk_fallback.ttf', 'cjk_fallback.LICENSE.txt'):   # the Chinese / Korean shared-font stand-in (GameActivity copies it into the game's font/)
            p = os.path.join(ANDROID, 'app/assets', f)
            if os.path.isfile(p): z.write(p, 'assets/' + f)
        z.writestr('assets/shader_chain', shader_chain(g) + '\n')   # the launcher sends players back to setup when their shader pack was made for another chain
        z.writestr('assets/games', ''.join(f'{f.id}\t{f.get("name")}\t{_game.file_stem(f)}\t{list(f.builds().values())[-1]["display"]}\t{"heavy" if f.get("android_heavy", "off") == "on" else ""}\n' for f in fam))   # the games this app runs, its own first, with the version they need (GameSettings.games)
        z.writestr('assets/app', f'{a.name}\t{g.get("app_repo", "github.com/simipoB/Galar-Native-Android")}\n')   # the app's name and where its official releases are (SetupActivity)
        for t in sorted(glob.glob(os.path.join(ANDROID, 'app/assets/lang/*.tsv'))): z.write(t, 'assets/lang/' + os.path.basename(t))   # the app's text in other languages (T.java)
        # the licences (GameMenu's About page): the app's own (NOTICE, then the GPL-3.0 text and the MIT notice its older code keeps), whose notice must stay with every
        #   copy, then the third-party code and fonts the APK carries.
        #   licenses/index.tsv: file, name, what it is; the app's own first. A build without the app's LICENSE fails: an APK must not go out without its notice
        T3 = DM + '/third_party/'
        ROOT = os.path.dirname(DM)
        lic = [('LICENSE.txt', [os.path.join(ROOT, 'NOTICE'), os.path.join(ROOT, 'LICENSE'), os.path.join(ROOT, 'LICENSE-MIT')], a.name, 'this app'),
               ('libvpx.txt', [T3 + 'libvpx/LICENSE', T3 + 'libvpx/PATENTS'], 'libvpx', "VP9 video decoder (the games' movies)"),
               ('stb_vorbis.txt', [T3 + 'stb_vorbis/LICENSE'], 'stb_vorbis', "Ogg Vorbis audio decoder (the movies' sound)"),
               ('sgsr.txt', [T3 + 'sgsr/LICENSE'], 'Snapdragon Game Super Resolution', 'the sharp upscaler'),
               ('ryujinx.txt', [T3 + 'ryujinx/LICENSE.txt'], 'Ryujinx shader translator', 'turns the Switch shaders into Vulkan ones'),
               ('vulkan-headers.txt', [T3 + 'vulkan-headers/LICENSE.md'], 'Vulkan headers', 'the Vulkan interface'),
               ('dxbc-spirv.txt', [T3 + 'dxbc-spirv/LICENSE', T3 + 'dxbc-spirv/submodules/spirv_headers/LICENSE'], 'dxbc-spirv', "converts your Lossless Scaling's shaders for Vulkan"),
               ('nvn-header.txt', [DM + '/gfx/nvn/include/nvn.h'], 'NVN header (h1k421)', "the Switch graphics interface's definitions"),
               ('cjk_fallback.txt', [ANDROID + '/app/assets/cjk_fallback.LICENSE.txt'], 'CJK fallback font', 'the Chinese and Korean text'),
               ('nintendo_ext.txt', [ANDROID + '/app/assets/nintendo_ext.LICENSE.txt'], 'Button icon font', 'the controller-button icons')]
        for p_ in lic[0][1]:
            if not os.path.isfile(p_): sys.exit("build_apk: the app's license file is missing (" + p_ + ")")
        idx = []
        for f, srcs, name, what in lic:
            texts = []
            for src in srcs:
                if not os.path.isfile(src): continue
                t_ = open(src, encoding='utf-8', errors='replace').read()
                if src.endswith('nvn.h'): t_ = t_[:t_.find('*/') + 2] if '*/' in t_ else ''   # (its notice: the comment it opens with)
                texts.append(t_.strip() + '\n')
            if texts: z.writestr('assets/licenses/' + f, '\n'.join(texts)); idx.append(f'{f}\t{name}\t{what}\n')
        z.writestr('assets/licenses/index.tsv', ''.join(idx))
    if not os.path.isfile(a.ks):
        os.makedirs(os.path.dirname(a.ks), exist_ok=True)
        run('keytool', '-genkeypair', '-keystore', a.ks, '-storepass', 'android', '-keypass', 'android', '-alias', 'debug', '-keyalg', 'RSA', '-keysize', '2048', '-validity', '10000', '-dname', 'CN=nxnative debug')
    run(os.path.join(bt, 'zipalign' + exe), '-f', '-P', '16', '4', os.path.join(w, 'base.apk'), os.path.join(w, 'aligned.apk'))
    run(os.path.join(bt, 'apksigner' + bat), 'sign', '--ks', a.ks, '--ks-pass', 'pass:android', '--key-pass', 'pass:android', '--v1-signing-enabled', 'false', '--out', a.out, os.path.join(w, 'aligned.apk'))
    shutil.rmtree(w, ignore_errors=True)
    print('built', a.out, f'({os.path.getsize(a.out) / 1e6:.0f} MB)')

if __name__ == '__main__': main()
