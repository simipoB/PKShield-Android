#!/bin/sh
# Builds the device probe APK without Gradle: NDK clang (the native report), javac + d8 (the activity), aapt2 (manifest), zipalign (16 KB for the .so), apksigner (a local debug key; Android installs only signed APKs).
# Usage: build.sh [output.apk]   (needs ANDROID_SDK, default ~/nxnative-work/android-sdk, and JDK 17)
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
SDK=${ANDROID_SDK:-$HOME/nxnative-work/android-sdk}
NDK=$SDK/ndk/27.2.12479018
BT=$SDK/build-tools/35.0.0
JAR=$SDK/platforms/android-35/android.jar
export JAVA_HOME=${JAVA_HOME:-/opt/homebrew/opt/openjdk@17/libexec/openjdk.jdk/Contents/Home}
export PATH=$JAVA_HOME/bin:$PATH
OUT=${1:-$HERE/out/nxnative-probe.apk}
W=$HERE/out/work; rm -rf "$W"; mkdir -p "$W/lib/arm64-v8a" "$W/classes" "$(dirname "$OUT")"

"$NDK/toolchains/llvm/prebuilt/darwin-x86_64/bin/clang" --target=aarch64-linux-android30 -O2 -Wall -shared -fPIC \
    -Wl,-z,max-page-size=16384 -o "$W/lib/arm64-v8a/libprobe.so" "$HERE/jni/probe.c" -lvulkan -llog -lnativewindow
javac -source 11 -target 11 -classpath "$JAR" -d "$W/classes" "$HERE"/src/com/nxnative/probe/*.java 2>&1 | grep -v "warning: \[options\]" || true
"$BT/d8" --min-api 30 --lib "$JAR" --output "$W" $(find "$W/classes" -name '*.class')
"$BT/aapt2" link -o "$W/base.apk" -I "$JAR" --manifest "$HERE/AndroidManifest.xml"
(cd "$W" && zip -q base.apk classes.dex && zip -q -0 base.apk lib/arm64-v8a/libprobe.so)
"$BT/zipalign" -f -P 16 4 "$W/base.apk" "$W/aligned.apk"
KS=$HERE/out/debug.keystore
[ -f "$KS" ] || keytool -genkeypair -keystore "$KS" -storepass android -keypass android -alias debug -keyalg RSA -keysize 2048 -validity 10000 -dname "CN=nxnative debug" >/dev/null 2>&1
"$BT/apksigner" sign --ks "$KS" --ks-pass pass:android --key-pass pass:android --out "$OUT" "$W/aligned.apk"
"$BT/apksigner" verify "$OUT"
echo "built $OUT ($(du -h "$OUT" | cut -f1))"
