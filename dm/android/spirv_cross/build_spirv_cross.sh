#!/bin/sh
# Builds spirv-cross (Khronos, Apache-2.0) for the phone (out/libspirv_cross.so, an arm64 Android executable shipped as a library)
# and for Linux (out/spirv-cross, the reference run). The setup uses it for shader reflection (--msl --reflect), as on the Mac.
# Usage: build_spirv_cross.sh [tag]   (default: the newest vulkan-sdk-* tag). Env: NDK (default ~/aot/android-ndk-r27c), API (default 33)
set -e
HERE=$(cd "$(dirname "$0")" && pwd); NDK=${NDK:-$HOME/aot/android-ndk-r27c}; API=${API:-33}
TC=$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin
URL=https://github.com/KhronosGroup/SPIRV-Cross.git
TAG=${1:-$(git ls-remote --tags --refs "$URL" 'vulkan-sdk-*' | sed 's#.*refs/tags/##' | sort -V | tail -1)}
SRC=$HERE/src/$TAG; mkdir -p "$HERE/out" "$HERE/src"
[ -d "$SRC" ] || git clone -q --depth 1 -b "$TAG" "$URL" "$SRC"
FILES="main.cpp spirv_cross.cpp spirv_parser.cpp spirv_cross_parsed_ir.cpp spirv_cfg.cpp spirv_glsl.cpp spirv_msl.cpp spirv_hlsl.cpp spirv_cpp.cpp spirv_reflect.cpp spirv_cross_util.cpp"
cd "$SRC"
FLAGS="-O2 -std=c++11"
g++ $FLAGS $FILES -o "$HERE/out/spirv-cross"
"$TC/aarch64-linux-android$API-clang++" $FLAGS -fPIE -pie -static-libstdc++ $FILES -o "$HERE/out/libspirv_cross.so"
"$TC/llvm-strip" "$HERE/out/libspirv_cross.so"
echo "spirv-cross $TAG: $(ls -l "$HERE/out/libspirv_cross.so" | awk '{print $5}') bytes (android), linux build at out/spirv-cross"
