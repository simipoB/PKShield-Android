#!/bin/sh
# Builds the device probe APK without Gradle: NDK clang (the native report), javac + d8 (the activity), aapt2 (manifest), zipalign (16 KB for the .so), apksigner (a local debug key; Android installs only signed APKs).
# Usage: build.sh [output.apk]   (needs ANDROID_SDK and JDK 17; on Linux / WSL the defaults are build_apk_pc.sh's ~/psa-build)
# The GPU self-tests (jni/selftest.c) link the engine's own BC layer (gfx/tex) and SPIR-V patcher (gfx/nvn/nvn_spv.c); their shaders are jni/selftest_spv.h
# (shaders/build_spv.sh). No game data goes into this APK.
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
GFX=$HERE/../../gfx
case "$(uname -s)" in
    Darwin) HOST=darwin-x86_64; DEF_SDK=$HOME/nxnative-work/android-sdk; DEF_JAVA=/opt/homebrew/opt/openjdk@17/libexec/openjdk.jdk/Contents/Home ;;
    *)      HOST=linux-x86_64;  DEF_SDK=$HOME/psa-build/android-sdk;      DEF_JAVA=$HOME/psa-build/jdk ;;
esac
SDK=${ANDROID_SDK:-$DEF_SDK}
NDK=${NDK:-$SDK/ndk/27.2.12479018}
[ -d "$NDK" ] || NDK=$HOME/psa-build/android-ndk-r27c
BT=$SDK/build-tools/35.0.0
JAR=$SDK/platforms/android-35/android.jar
if [ -z "$JAVA_HOME" ] && [ -d "$DEF_JAVA" ]; then export JAVA_HOME=$DEF_JAVA; fi
if [ -n "$JAVA_HOME" ]; then export PATH=$JAVA_HOME/bin:$PATH; fi
OUT=${1:-$HERE/out/nxnative-probe.apk}
W=$HERE/out/work; rm -rf "$W"; mkdir -p "$W/lib/arm64-v8a" "$W/classes" "$(dirname "$OUT")"

"$NDK/toolchains/llvm/prebuilt/$HOST/bin/clang" --target=aarch64-linux-android30 -O2 -Wall -shared -fPIC \
    -I"$GFX/tex" -I"$GFX/nvn" -Wl,-z,max-page-size=16384 -o "$W/lib/arm64-v8a/libprobe.so" \
    "$HERE/jni/probe.c" "$HERE/jni/selftest.c" "$GFX/tex/bc_gpu.c" "$GFX/tex/bc_decode.c" "$GFX/tex/etc_encode.c" "$GFX/nvn/nvn_spv.c" \
    -lvulkan -llog -lnativewindow -lm
javac -source 11 -target 11 -classpath "$JAR" -d "$W/classes" "$HERE"/src/com/nxnative/probe/*.java 2>&1 | grep -v "warning: \[options\]" || true
"$BT/d8" --min-api 30 --lib "$JAR" --output "$W" $(find "$W/classes" -name '*.class')
"$BT/aapt2" link -o "$W/base.apk" -I "$JAR" --manifest "$HERE/AndroidManifest.xml"
# classes.dex compressed, the .so stored (it is mapped in place); Python's zipfile where there is no zip (a fresh WSL)
if command -v zip >/dev/null; then (cd "$W" && zip -q base.apk classes.dex && zip -q -0 base.apk lib/arm64-v8a/libprobe.so)
else (cd "$W" && python3 -c 'import zipfile; z = zipfile.ZipFile("base.apk", "a"); z.write("classes.dex", compress_type=zipfile.ZIP_DEFLATED); z.write("lib/arm64-v8a/libprobe.so", compress_type=zipfile.ZIP_STORED); z.close()'); fi
"$BT/zipalign" -f -P 16 4 "$W/base.apk" "$W/aligned.apk"
KS=$HERE/out/debug.keystore
[ -f "$KS" ] || keytool -genkeypair -keystore "$KS" -storepass android -keypass android -alias debug -keyalg RSA -keysize 2048 -validity 10000 -dname "CN=nxnative debug" >/dev/null 2>&1
"$BT/apksigner" sign --ks "$KS" --ks-pass pass:android --key-pass pass:android --out "$OUT" "$W/aligned.apk"
"$BT/apksigner" verify "$OUT"
echo "built $OUT ($(du -h "$OUT" | cut -f1))"
