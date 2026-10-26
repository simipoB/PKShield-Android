#!/bin/bash
# Builds the shader translator as an Android arm64 executable with .NET NativeAOT. Linux only (NativeAOT does not cross-compile across OSes; WSL works).
# Needs: .NET 10 SDK (DOTNET, default ~/.dotnet/dotnet) and the Android NDK r27c (NDK, default ~/aot/android-ndk-r27c).
# Output: out/libdmsh_xlate.so - an executable, named like a library so Android installs it executable in the app's native library folder
set -e
export DOTNET_SYSTEM_GLOBALIZATION_INVARIANT=1 DOTNET_CLI_TELEMETRY_OPTOUT=1   # the SDK itself runs without ICU (WSL has none by default)
HERE=$(cd "$(dirname "$0")" && pwd)
DOTNET=${DOTNET:-$HOME/.dotnet/dotnet}; NDK=${NDK:-$HOME/aot/android-ndk-r27c}; API=${API:-33}
TC=$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin
PUB=${PUB:-$HOME/aot/xlate/publish}
# the NDK's clang wrapper for the target carries the target triple and the sysroot; NativeAOT links with it
"$DOTNET" publish "$HERE/dmsh_xlate_aot.csproj" -c Release -o "$PUB" -nologo -p:CppCompilerAndLinker="$TC/aarch64-linux-android$API-clang" -p:ObjCopyName="$TC/llvm-objcopy"
mkdir -p "$HERE/out"
cp "$PUB/dmsh_xlate" "$HERE/out/libdmsh_xlate.so"
ls -l "$HERE/out/libdmsh_xlate.so"
