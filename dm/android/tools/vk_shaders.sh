#!/bin/sh
# Vulkan shader build for Android: translated SPIR-V (<key>.spv), GL-depth fallback modules (<key>.gl.spv) for vertex stages, and reflection (<key>_b.json).
# Runs dmsh_build.py with the translator wrapped by xlate_keep_spv.sh, an empty content cache so every shader is translated, and existing Metal libraries as compile cache.
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
echo "SPIR-V: $(find "$OUT/b-$CHAIN" -name '*.spv' ! -name '*.gl.spv' | wc -l) shaders and $(find "$OUT/b-$CHAIN" -name '*.gl.spv' | wc -l) GL-depth fallbacks in $OUT/b-$CHAIN"
