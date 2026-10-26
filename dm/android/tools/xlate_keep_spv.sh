#!/bin/sh
# Translator wrapper for the Vulkan shader build (vk_shaders.sh): runs the real translator (dmsh_xlate, unchanged) on dmsh_build.py's job file, then copies the SPIR-V it wrote
# into $VK_SPV_OUT before dmsh_build.py's Metal-only rewrites touch it. Called by dmsh_build.py as: $XLATE <job.json> <work dir>
case "$1" in --*) exec "$XLATE_REAL" "$@";; esac   # tool queries (--source-sha) go straight through
"$XLATE_REAL" "$@"; rc=$?
python3 - "$1" "$2" "$VK_SPV_OUT" <<'PY'
import json, os, shutil, sys
job, w, out = sys.argv[1:4]
for j in json.load(open(job)):
    k = j.get('key') or j.get('name'); src = os.path.join(w, f'{k}.spv')
    if k and os.path.exists(src): shutil.copyfile(src, os.path.join(out, f'{k}.spv'))
PY
exit $rc
