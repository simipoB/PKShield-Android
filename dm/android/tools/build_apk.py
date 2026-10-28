#!/usr/bin/env python3
# Builds the installable app (no game content): the engine library, the setup screen and the tools the phone runs to set the game up from the player's dump.
# Runs on Windows, Linux and macOS (no Xcode, no sips): Android build-tools (aapt2, d8, zipalign, apksigner), a JDK (javac), Python 3.
#   lib/arm64-v8a: libnxnative.so (the engine, Makefile.host lib / win_build_lib.py), libnx_extract.so and libdmsh_xlate.so (.NET NativeAOT: extract_aot/, xlate_aot/),
#                  libspirv_cross.so (spirv_cross/), libpython3.14.so + libpython_exe.so (CPython for Android and its launcher), libcapstone.so, liblz4.so
#   assets: python.zip (CPython's standard library, trimmed), dm.zip (the setup scripts, the game profile, the Python stand-ins for lz4 and capstone)
# Usage: build_apk.py --libs <dir with the .so files> --pylib <CPython for Android's lib/python3.14> --capstone-py <the capstone Python package> --out <app.apk>
#        [--ks <keystore> (default dm/android/probe/out/debug.keystore; made when missing)] [--bt <build-tools dir>] [--jar <android.jar>] [--name "Pokémon Shield"]
import argparse, hashlib, io, os, re, shutil, struct, subprocess, sys, tempfile, zipfile, zlib

HERE = os.path.dirname(os.path.abspath(__file__)); ANDROID = os.path.dirname(HERE); DM = os.path.dirname(ANDROID)
LIBS = ['libnxnative.so', 'libnx_extract.so', 'libdmsh_xlate.so', 'libspirv_cross.so', 'libpython3.14.so', 'libpython_exe.so', 'libcapstone.so', 'liblz4.so', 'libssl_python.so', 'libcrypto_python.so']
OPTIONAL = ['libadrenotools.so', 'libhook_impl.so', 'libmain_hook.so', 'libfile_redirect_hook.so', 'libgsl_alloc_hook.so']   # libadrenotools: custom Adreno/PanVK drivers; without it the phone's own driver only
PY_SKIP = ('test', 'idlelib', 'tkinter', 'turtledemo', 'ensurepip', 'pydoc_data', 'lib2to3', 'venv', 'sqlite3', '__pycache__', 'unittest', 'wsgiref', 'xmlrpc', 'curses', 'dbm')
DM_PARTS = ['games/game.py', 'games/01008DB008C2C000', 'tools/patch_nso.py', 'gfx/tools', 'ios/tools/ios_metal.py', 'android/tools']
CHAIN_HEADER = os.path.join(DM, 'games/01008DB008C2C000/dmsh_chain.h')
CHAIN_TOOL = os.path.join(DM, 'gfx/tools/dmsh_chain.py')
DEFAULT_VERSION = 29
DEFAULT_MIN_SDK = 30

def png(size):   # the app's icon, drawn here (no game artwork ships): a red and white ball on dark blue
    px = []
    for y in range(size):
        row = bytearray([0])
        for x in range(size):
            dx, dy = (x + 0.5) / size - 0.5, (y + 0.5) / size - 0.5; d = (dx * dx + dy * dy) ** 0.5
            c = (24, 32, 64)
            if d < 0.40: c = (220, 40, 50) if dy < -0.03 else (245, 245, 245) if dy > 0.03 else (30, 30, 36)
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

def validate_extractor(path):
    with open(path, 'rb') as f: data = f.read()
    obsolete_error = 'LibHac cache count was not found'
    stale_release_hash = 'd40d71f2f0faf4c54bf92476fb9e10c8866f132e296ca73f63aba30623cee6c8'
    if hashlib.sha256(data).hexdigest() == stale_release_hash or obsolete_error.encode('utf-8') in data or obsolete_error.encode('utf-16le') in data:
        sys.exit('[RED] stale libnx_extract.so: rebuild it from this checkout before packaging (run scripts/build_apk_pc.sh)')

def shader_chain():
    with open(CHAIN_HEADER, encoding='utf-8') as f: match = re.search(r'^#define DMSH_CHAIN "([0-9a-f]{12})"$', f.read(), re.M)
    if not match: sys.exit('[RED] invalid shader-chain header: run dm/gfx/tools/dmsh_chain.py --header')
    env = dict(os.environ, DM_GAME='01008DB008C2C000')
    result = subprocess.run([sys.executable, CHAIN_TOOL], capture_output=True, check=False, env=env, text=True)
    current = result.stdout.strip()
    if result.returncode or current != match.group(1):
        sys.exit(f'[RED] stale shader-chain header: header {match.group(1)}, current tools {current or result.stderr.strip()}; run dm/gfx/tools/dmsh_chain.py --header')
    return current

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--libs', required=True); ap.add_argument('--pylib', required=True); ap.add_argument('--capstone-py', required=True); ap.add_argument('--out', required=True)
    ap.add_argument('--ks', default=os.path.join(ANDROID, 'probe/out/debug.keystore')); ap.add_argument('--bt'); ap.add_argument('--jar'); ap.add_argument('--name', default='Pokémon Shield')
    ap.add_argument('--package', default='com.nxnative.pokemonshield'); ap.add_argument('--version', type=int, default=DEFAULT_VERSION)
    ap.add_argument('--min-sdk', type=int, default=DEFAULT_MIN_SDK)
    ap.add_argument('--debuggable', action='store_true', help='test builds: adb run-as can reach the private storage of the app')
    a = ap.parse_args()
    chain = shader_chain()
    exe = '.exe' if os.name == 'nt' else ''; bat = '.bat' if os.name == 'nt' else ''
    bt = a.bt or os.path.expanduser('~/nxnative-work/tools/android-15'); jar = a.jar or os.path.expanduser('~/nxnative-work/tools/android-35/android.jar')
    missing = [l for l in LIBS if not os.path.isfile(os.path.join(a.libs, l))]
    if missing: sys.exit('[RED] missing native libraries in ' + a.libs + ': ' + ', '.join(missing))
    validate_extractor(os.path.join(a.libs, 'libnx_extract.so'))
    w = tempfile.mkdtemp(prefix='apk_')
    # resources: the icon; the manifest: the setup screen launches, the native activity plays
    for dpi, sz in (('xxxhdpi', 192), ('xxhdpi', 144), ('xhdpi', 96)):
        os.makedirs(os.path.join(w, 'res', f'mipmap-{dpi}'), exist_ok=True); open(os.path.join(w, 'res', f'mipmap-{dpi}', 'ic_launcher.png'), 'wb').write(png(sz))
    open(os.path.join(w, 'AndroidManifest.xml'), 'w', encoding='utf-8').write(f'''<?xml version="1.0" encoding="utf-8"?>
<manifest xmlns:android="http://schemas.android.com/apk/res/android" package="{a.package}" android:versionCode="{a.version}" android:versionName="0.{a.version}">
    <uses-sdk android:minSdkVersion="{a.min_sdk}" android:targetSdkVersion="35" />
    <uses-permission android:name="android.permission.VIBRATE" />
    <uses-permission android:name="android.permission.MANAGE_EXTERNAL_STORAGE" />
    <uses-feature android:name="android.hardware.vulkan.version" android:version="0x401000" android:required="true" />
    <uses-feature android:name="android.hardware.touchscreen" android:required="false" />
    <uses-feature android:name="android.hardware.gamepad" android:required="false" />
    <application android:label="{a.name}" android:icon="@mipmap/ic_launcher" android:hasCode="true" android:extractNativeLibs="true"
                 android:allowNativeHeapPointerTagging="false" android:isGame="true" android:appCategory="game" android:largeHeap="true" android:hasFragileUserData="true"{' android:debuggable="true"' if a.debuggable else ''}>
        <meta-data android:name="com.nxnative.shader_chain" android:value="{chain}" />
        <activity android:name="com.nxnative.SetupActivity" android:exported="true" android:theme="@android:style/Theme.DeviceDefault">
            <intent-filter><action android:name="android.intent.action.MAIN" /><category android:name="android.intent.category.LAUNCHER" /></intent-filter>
        </activity>
        <activity android:name="com.nxnative.SettingsActivity" android:exported="false" android:theme="@android:style/Theme.DeviceDefault" />
        <activity-alias android:name="com.nxnative.SettingsShortcut" android:targetActivity="com.nxnative.SettingsActivity"
                        android:exported="true" android:label="Pokémon Shield Settings" android:icon="@mipmap/ic_launcher">
            <intent-filter><action android:name="android.intent.action.MAIN" /><category android:name="android.intent.category.LAUNCHER" /></intent-filter>
        </activity-alias>
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
    run(os.path.join(bt, 'd8' + bat), '--release', '--min-api', str(a.min_sdk), '--lib', jar, '--output', w, *[os.path.join(d, f) for d, _, fs in os.walk(cls) for f in fs])
    # the payload: native libraries, the Python standard library, the setup scripts
    with zipfile.ZipFile(os.path.join(w, 'base.apk'), 'a') as z:
        z.write(os.path.join(w, 'classes.dex'), 'classes.dex')
        for l in LIBS + [o for o in OPTIONAL if os.path.isfile(os.path.join(a.libs, o))]: z.write(os.path.join(a.libs, l), 'lib/arm64-v8a/' + l, compress_type=zipfile.ZIP_DEFLATED)
        b = io.BytesIO()
        with zipfile.ZipFile(b, 'w', zipfile.ZIP_DEFLATED) as pz: zipdir(pz, a.pylib, 'lib/python3.14', PY_SKIP)
        z.writestr('assets/python.zip', b.getvalue(), compress_type=zipfile.ZIP_STORED)
        b = io.BytesIO()
        with zipfile.ZipFile(b, 'w', zipfile.ZIP_DEFLATED) as dz:
            for part in DM_PARTS:
                p = os.path.join(DM, part)
                if os.path.isdir(p): zipdir(dz, p, part)
                else: dz.write(p, part)
            zipdir(dz, os.path.join(ANDROID, 'pyshim'), 'android/pyshim')
            zipdir(dz, a.capstone_py, 'android/pyshim/capstone', ('lib', 'include', '__pycache__'))
            for f in ('nintendo_ext.ttf', 'nintendo_ext.LICENSE.txt'):
                p = os.path.join(ANDROID, 'app/assets', f)
                if os.path.isfile(p): dz.write(p, 'android/app/assets/' + f)
        z.writestr('assets/dm.zip', b.getvalue(), compress_type=zipfile.ZIP_STORED)
    if not os.path.isfile(a.ks):
        os.makedirs(os.path.dirname(a.ks), exist_ok=True)
        run('keytool', '-genkeypair', '-keystore', a.ks, '-storepass', 'android', '-keypass', 'android', '-alias', 'debug', '-keyalg', 'RSA', '-keysize', '2048', '-validity', '10000', '-dname', 'CN=nxnative debug')
    run(os.path.join(bt, 'zipalign' + exe), '-f', '-P', '16', '4', os.path.join(w, 'base.apk'), os.path.join(w, 'aligned.apk'))
    run(os.path.join(bt, 'apksigner' + bat), 'sign', '--ks', a.ks, '--ks-pass', 'pass:android', '--key-pass', 'pass:android', '--v1-signing-enabled', 'false', '--out', a.out, os.path.join(w, 'aligned.apk'))
    shutil.rmtree(w, ignore_errors=True)
    print('built', a.out, f'({os.path.getsize(a.out) / 1e6:.0f} MB)')

if __name__ == '__main__': main()
