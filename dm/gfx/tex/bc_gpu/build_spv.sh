#!/bin/bash
# The BC layer's compute shader → SPIR-V header: gen_tables.py (tables from ../bc_decode.c / ../etc_encode.c), then bc_gpu.comp → bc_gpu.comp.spv → bc_gpu_spv.h.
# GLSLC: the NDK's glslc (default: on PATH); its #include handles bc_gpu_tables.glsl
set -e
O=$(cd "$(dirname "$0")" && pwd)
python3 "$O/gen_tables.py" 2>/dev/null || python "$O/gen_tables.py"
${GLSLC:-glslc} -fshader-stage=compute --target-env=vulkan1.1 -O "$O/bc_gpu.comp" -o "$O/bc_gpu.comp.spv"
(python3 - "$O" 2>/dev/null || python - "$O") <<'PY'
import struct, sys
o = sys.argv[1]; b = open(o + '/bc_gpu.comp.spv', 'rb').read(); w = struct.unpack('<%dI' % (len(b) // 4), b)
open(o + '/bc_gpu_spv.h', 'w', newline='\n').write('// Generated from bc_gpu.comp (build_spv.sh, glslc -O): the BC layer\'s decoder / transcoder\nstatic const uint32_t bc_gpu_spv[] = {' + ','.join(hex(x) for x in w) + '};\n')
print('bc_gpu_spv.h:', len(w), 'words')
PY
