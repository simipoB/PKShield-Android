#!/bin/bash
# Builds the NSP / XCI extractor as an Android arm64 executable with .NET NativeAOT (Linux or WSL, .NET 10 SDK, Android NDK r27c). Output: out/libnx_extract.so
set -e
export DOTNET_SYSTEM_GLOBALIZATION_INVARIANT=1 DOTNET_CLI_TELEMETRY_OPTOUT=1
HERE=$(cd "$(dirname "$0")" && pwd)
DOTNET=${DOTNET:-$HOME/.dotnet/dotnet}; NDK=${NDK:-$HOME/aot/android-ndk-r27c}; API=${API:-31}
TC=$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin
PUB=${PUB:-$HOME/aot/extract/publish}
"$DOTNET" publish "$HERE/nx_extract_aot.csproj" -c Release -o "$PUB" -nologo -p:CppCompilerAndLinker="$TC/aarch64-linux-android$API-clang" -p:ObjCopyName="$TC/llvm-objcopy"
mkdir -p "$HERE/out"
cp "$PUB/nx_extract" "$HERE/out/libnx_extract.so"
ls -l "$HERE/out/libnx_extract.so"
