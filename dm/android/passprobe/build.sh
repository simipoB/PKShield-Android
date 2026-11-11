#!/bin/sh
# Builds the Mali pass test app (no game data): passes.h comes from a pass log (gen_passes.py); then NDK clang (the replay), javac + d8 (the screen),
# aapt2, zipalign, apksigner. Players install it next to Galar Native: its own package, nothing of the game's is touched.
# Usage: build.sh [output.apk]   Environment: ANDROID_SDK (default: the Windows SDK in %LOCALAPPDATA%), JAVA_HOME (a JDK 11+),
#        KS / KS_PASS / KS_ALIAS (keystore; default the test-builds key, C:\Users\<you>\galar-re\test-builds\galar-test-builds.keystore)
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
SDK=${ANDROID_SDK:-/c/Users/$USERNAME/AppData/Local/Android/Sdk}
NDK=$SDK/ndk/27.2.12479018
BT=$(ls -d "$SDK"/build-tools/* | sort -V | tail -1)
JAR=$(ls "$SDK"/platforms/android-3[4-9]*/android.jar | head -1)
case "$(uname -s)" in MINGW*|MSYS*) HOST=windows-x86_64; EXE=.exe; BAT=.bat ;; Darwin) HOST=darwin-x86_64 ;; *) HOST=linux-x86_64 ;; esac
[ -n "$JAVA_HOME" ] || for j in "/c/Program Files/Eclipse Adoptium"/jdk-* "/c/Program Files/Android/Android Studio/jbr"; do [ -x "$j/bin/javac$EXE" ] && JAVA_HOME=$j && break; done
OUT=${1:-$HERE/out/galar-mali-test.apk}
W=$HERE/out/work; rm -rf "$W"; mkdir -p "$W/lib/arm64-v8a" "$W/classes" "$(dirname "$OUT")"
"$NDK/toolchains/llvm/prebuilt/$HOST/bin/clang$EXE" --target=aarch64-linux-android31 -O1 -g -Wall -shared -fPIC -Wl,-z,max-page-size=16384 \
    -I"$HERE/../../third_party/vulkan-headers/include" -o "$W/lib/arm64-v8a/libpassprobe.so" "$HERE/passprobe.c" -landroid -lvulkan -llog
"$JAVA_HOME/bin/javac$EXE" --release 11 -classpath "$JAR" -d "$W/classes" "$HERE"/src/com/nxnative/passprobe/*.java
"$BT/d8$BAT" --min-api 31 --lib "$JAR" --output "$W" $(find "$W/classes" -name '*.class')
"$BT/aapt2$EXE" link -o "$W/base.apk" -I "$JAR" --manifest "$HERE/AndroidManifest.xml"
(cd "$W" && python -c 'import zipfile; z = zipfile.ZipFile("base.apk", "a"); z.write("classes.dex", compress_type=zipfile.ZIP_DEFLATED); z.write("lib/arm64-v8a/libpassprobe.so", compress_type=zipfile.ZIP_STORED); z.close()')
"$BT/zipalign$EXE" -f -P 16 4 "$W/base.apk" "$W/aligned.apk"
KS=${KS:-/c/Users/$USERNAME/galar-re/test-builds/galar-test-builds.keystore}
"$BT/apksigner$BAT" sign --ks "$KS" --ks-pass "pass:${KS_PASS:-galartest}" --key-pass "pass:${KS_PASS:-galartest}" --ks-key-alias "${KS_ALIAS:-test}" --out "$OUT" "$W/aligned.apk"
echo "built $OUT ($(du -h "$OUT" | cut -f1))"
