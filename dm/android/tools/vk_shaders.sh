#!/bin/sh
# Vulkan shader build for the Android port: the game's shaders as SPIR-V (<key>.spv, straight from the translator) with their reflection (<key>_b.json), in <out>/b-<chain version>/.
# Runs dmsh_build.py unchanged (the chain version, and so the Apple builds, stay as they are) with the translator wrapped by xlate_keep_spv.sh, an empty content cache so every
# shader is translated, and the existing Metal libraries as the compile cache so nothing is recompiled for Metal.
# Usage: vk_shaders.sh <shader package dir> <existing Metal cache root> <out>    Environment: DM_GAME (profile), J (jobs)
set -e
HERE=$(cd "$(dirname "$0")" && pwd); REPO=$(cd "$HERE/../../.." && pwd)
PKG=$1; METAL=$2; OUT=$3
TMP=$(mktemp -d "${TMPDIR:-/tmp}/vksh.XXXXXX")
CHAIN=$(cd "$REPO" && python3 dm/gfx/tools/dmsh_chain.py)
mkdir -p "$OUT/b-$CHAIN"
# the chain's tool check reads the translator's files beside $XLATE: a folder of links to them, with the wrapper as dmsh_xlate
XB="$REPO/dm/gfx/tools/dmsh_xlate_bin"; mkdir -p "$TMP/xbin"; for f in "$XB"/*; do ln -s "$f" "$TMP/xbin/"; done; rm "$TMP/xbin/dmsh_xlate"; cp "$HERE/xlate_keep_spv.sh" "$TMP/xbin/dmsh_xlate"
VK_SPV_OUT="$OUT/b-$CHAIN" XLATE_REAL="$XB/dmsh_xlate" XLATE="$TMP/xbin/dmsh_xlate" DMSH_CAS="$TMP/cas" DMSH_PREFILL="$METAL" WORK="$TMP" \
    python3 "$REPO/dm/gfx/tools/dmsh_build.py" "$PKG" "$TMP/out"
cp "$TMP/out/b-$CHAIN/"*_b.json "$OUT/b-$CHAIN/" 2>/dev/null || find "$TMP/out/b-$CHAIN" -name '*_b.json' -exec cp {} "$OUT/b-$CHAIN/" \;
rm -rf "$TMP"
echo "SPIR-V: $(find "$OUT/b-$CHAIN" -name '*.spv' | wc -l) shaders in $OUT/b-$CHAIN"
