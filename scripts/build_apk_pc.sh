#!/bin/bash
# Builds the installable app (PokemonShield.apk) from this repository on a PC: Linux, or Windows through WSL (Ubuntu). No game files or keys are involved:
# the app sets the game up on the phone from the player's own dump.
#
#   bash scripts/build_apk_pc.sh            the app with the phone's own Vulkan driver
#   TURNIP=1 bash scripts/build_apk_pc.sh   also libadrenotools, so players can pick a custom Adreno driver (Mesa Turnip) on the setup screen
#
# What it fetches into $WORK (default ~/psa-build), each once:
#   the Android NDK r27c, the .NET 10 SDK, the Android command-line tools (build-tools 35, android.jar 35), CPython 3.14 for Android (python.org),
#   capstone 5.0.7 and lz4 1.10.0 (sources), spirv-cross (Khronos), and with TURNIP=1 libadrenotools
# Needs on the PC: git, curl, python3 (with venv), a C/C++ compiler (gcc / g++), make, and cmake + ninja for TURNIP=1,
#   e.g. sudo apt install git curl python3-venv build-essential cmake ninja-build   (a JDK is fetched too when none is installed)
# The APK is signed with a debug key made on first use ($WORK/debug.keystore); keep it to install updates over a first install.
set -euo pipefail
REPO=$(cd "$(dirname "$0")/.." && pwd); A=$REPO/dm/android
WORK=${WORK:-$HOME/psa-build}; mkdir -p "$WORK"; cd "$WORK"
GAME=01008DB008C2C000; API=33; PYV=3.14.9; CAPV=5.0.7; LZ4V=1.10.0
export NDK=${NDK:-$WORK/android-ndk-r27c} DOTNET=${DOTNET:-$WORK/dotnet/dotnet} DOTNET_SYSTEM_GLOBALIZATION_INVARIANT=1 DOTNET_CLI_TELEMETRY_OPTOUT=1
TC=$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin; CC="$TC/aarch64-linux-android$API-clang"
LIBS=$WORK/libs; mkdir -p "$LIBS"
step() { echo; echo "== $*"; }
need() { for t in "$@"; do command -v "$t" >/dev/null || { echo "[RED] $t is missing (see the top of this script for what to install)"; exit 1; }; done; }
need git curl python3 gcc g++ make
unz() { python3 - "$1" "$2" <<'PY'
# a zip with its Unix permissions and symlinks (the NDK's clang is a symlink; Python's zipfile alone drops both)
import os, stat, sys, zipfile
z = zipfile.ZipFile(sys.argv[1]); dest = sys.argv[2]
for i in z.infolist():
    p = os.path.join(dest, i.filename); mode = i.external_attr >> 16
    if i.is_dir(): os.makedirs(p, exist_ok=True); continue
    os.makedirs(os.path.dirname(p), exist_ok=True)
    if stat.S_ISLNK(mode):
        if os.path.lexists(p): os.remove(p)
        os.symlink(z.read(i).decode(), p); continue
    with open(p, 'wb') as f: f.write(z.read(i))
    if mode: os.chmod(p, mode & 0o7777)
PY
}
if ! command -v javac >/dev/null || ! command -v keytool >/dev/null; then   # a JDK 17 (Eclipse Temurin) in $WORK when the PC has none
    if [ ! -x "$WORK/jdk/bin/javac" ]; then
        curl -fsSLo jdk.tar.gz "https://api.adoptium.net/v3/binary/latest/17/ga/linux/x64/jdk/hotspot/normal/eclipse"; mkdir -p jdk; tar -xzf jdk.tar.gz -C jdk --strip-components=1; rm jdk.tar.gz
    fi
    export JAVA_HOME=$WORK/jdk PATH=$WORK/jdk/bin:$PATH
fi

step "1/8 tools: Android NDK, .NET SDK, Android build-tools"
[ -x "$CC" ] || { curl -fsSLo ndk.zip https://dl.google.com/android/repository/android-ndk-r27c-linux.zip; unz ndk.zip .; rm ndk.zip; }
[ -x "$DOTNET" ] || { curl -fsSLo dotnet-install.sh https://dot.net/v1/dotnet-install.sh; bash dotnet-install.sh --channel 10.0 --install-dir "$WORK/dotnet" >/dev/null; }
SDK=$WORK/android-sdk; BT=$SDK/build-tools/35.0.0; JAR=$SDK/platforms/android-35/android.jar
if [ ! -f "$JAR" ] || [ ! -x "$BT/aapt2" ]; then
    curl -fsSLo cmdline.zip https://dl.google.com/android/repository/commandlinetools-linux-11076708_latest.zip; mkdir -p "$SDK"; unz cmdline.zip "$SDK"; rm cmdline.zip
    yes | "$SDK/cmdline-tools/bin/sdkmanager" --sdk_root="$SDK" --licenses >/dev/null || true
    "$SDK/cmdline-tools/bin/sdkmanager" --sdk_root="$SDK" "build-tools;35.0.0" "platforms;android-35" >/dev/null
fi

step "2/8 the engine (libnxnative.so)"
make -C "$A" -f Makefile.host GAME=$GAME NDK="$NDK" API=$API -j"$(nproc)" lib | tail -1
cp "$A/out/host-$GAME-vulkan/lib/libnxnative.so" "$LIBS/"

step "3/8 the NSP / XCI unpacker and the shader translator (.NET NativeAOT for Android)"
bash "$A/extract_aot/build_extract_aot.sh" | tail -1; cp "$A/extract_aot/out/libnx_extract.so" "$LIBS/"
bash "$A/xlate_aot/build_xlate_aot.sh" | tail -1; cp "$A/xlate_aot/out/libdmsh_xlate.so" "$LIBS/"

step "4/8 spirv-cross"
NDK=$NDK API=$API bash "$A/spirv_cross/build_spirv_cross.sh" | tail -1; cp "$A/spirv_cross/out/libspirv_cross.so" "$LIBS/"

step "5/8 CPython $PYV for Android and its launcher"
PY=$WORK/python-android
if [ ! -f "$PY/prefix/lib/libpython3.14.so" ]; then
    mkdir -p "$PY"; curl -fsSLo py.tar.gz "https://www.python.org/ftp/python/$PYV/python-$PYV-aarch64-linux-android.tar.gz"; tar -xzf py.tar.gz -C "$PY"; rm py.tar.gz
fi
cp "$PY/prefix/lib/libpython3.14.so" "$LIBS/"
"$CC" -O2 -fPIE -pie -I"$PY/prefix/include/python3.14" "$A/python/pylaunch.c" -L"$PY/prefix/lib" -lpython3.14 -o "$LIBS/libpython_exe.so"

step "6/8 capstone $CAPV and lz4 $LZ4V for Android (the code rewriter's disassembler, the NSO decompressor)"
[ -d capstone-$CAPV ] || { curl -fsSLo cap.tar.gz https://github.com/capstone-engine/capstone/archive/refs/tags/$CAPV.tar.gz; tar -xzf cap.tar.gz; rm cap.tar.gz; }
( cd capstone-$CAPV && "$CC" -O2 -shared -fPIC -DCAPSTONE_HAS_ARM64 -DCAPSTONE_USE_SYS_DYN_MEM -Iinclude cs.c MCInst.c MCInstrDesc.c MCRegisterInfo.c SStream.c Mapping.c utils.c \
    arch/AArch64/*.c -o "$LIBS/libcapstone.so" )
[ -d lz4-$LZ4V ] || { curl -fsSLo lz4.tar.gz https://github.com/lz4/lz4/archive/refs/tags/v$LZ4V.tar.gz; tar -xzf lz4.tar.gz; rm lz4.tar.gz; }
"$CC" -O2 -shared -fPIC "lz4-$LZ4V/lib/lz4.c" -o "$LIBS/liblz4.so"
[ -d capstone-py ] || { python3 -m venv venv && venv/bin/pip install -q --target capstone-py capstone==$CAPV; }   # the Python binding (pure Python; its own library is not used)

step "7/8 libadrenotools (custom Adreno drivers)"
if [ "${TURNIP:-0}" = 1 ]; then
    need cmake ninja
    [ -d libadrenotools ] || git clone -q --recursive --depth 1 https://github.com/bylaws/libadrenotools.git
    cmake -S libadrenotools -B libadrenotools/build -G Ninja -DCMAKE_TOOLCHAIN_FILE="$NDK/build/cmake/android.toolchain.cmake" -DANDROID_ABI=arm64-v8a \
        -DANDROID_PLATFORM=android-$API -DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS=ON -DCMAKE_POLICY_VERSION_MINIMUM=3.5 >/dev/null
    ninja -C libadrenotools/build >/dev/null
    find libadrenotools/build -name "*.so" -exec cp {} "$LIBS/" \;
else echo "skipped (TURNIP=1 to include it)"; fi

step "8/8 the APK"
python3 "$A/tools/build_apk.py" --libs "$LIBS" --pylib "$PY/prefix/lib/python3.14" --capstone-py "$WORK/capstone-py/capstone" --bt "$BT" --jar "$JAR" \
    --ks "$WORK/debug.keystore" --out "$WORK/PokemonShield.apk"
echo; echo "done: $WORK/PokemonShield.apk (install it, open it, choose your game files and keys)"
