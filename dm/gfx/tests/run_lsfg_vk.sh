#!/bin/bash
# t_lsfg_vk on this PC (Linux / WSL), on Mesa's software driver (lavapipe), with the NDK's Vulkan headers when the system has none.
#   bash run_lsfg_vk.sh <folder of the converted .spv shaders> [folder for .ppm pictures] [flow scale 0.25..1]
set -e
cd "$(dirname "$0")"
INC=${VULKAN_INC:-}; if [ -z "$INC" ] && [ ! -f /usr/include/vulkan/vulkan.h ]; then
    S=${NDK:-$HOME/psa-build/android-ndk-r27c}/toolchains/llvm/prebuilt/linux-x86_64/sysroot/usr/include; INC=$(mktemp -d); ln -s "$S/vulkan" "$INC/vulkan"; ln -s "$S/vk_video" "$INC/vk_video"; fi
LIB=${VULKAN_LIB:-$(ls /usr/lib/x86_64-linux-gnu/libvulkan.so.1 2>/dev/null || echo -lvulkan)}
make GAME=${GAME:-01008DB008C2C000} CC=${CC:-gcc} VULKAN_INC="$INC" VULKAN_LIB="$LIB" -B t_lsfg_vk >/dev/null
ICD=/usr/share/vulkan/icd.d/lvp_icd.json; [ -f "$ICD" ] && export VK_ICD_FILENAMES=$ICD
./t_lsfg_vk "$@"
