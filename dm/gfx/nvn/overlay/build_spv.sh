#!/bin/bash
# The present path's shaders → SPIR-V headers (glslangValidator -V; on Windows it runs in WSL from Ubuntu's glslang-tools package, unpacked without installing):
#   ovl.vert / ovl.frag → ovl_spv.h (the touch overlay), scale.vert / scale.frag → scale_spv.h (the sharpening upscaler),
#   sgsr.vert + dm/third_party/sgsr/sgsr1_shader_mobile.frag → sgsr_spv.h (Snapdragon Game Super Resolution 1, Qualcomm, BSD-3-Clause: dm/third_party/sgsr/LICENSE),
#   fg.vert + fg_luma / fg_flow / fg_stat / fg_gen.frag → fgen_spv.h (frame generation: nvn_fgen.h; their .spv files are not kept).
# SGSR's file is used as Qualcomm publishes it; only its two plain uniforms become Vulkan declarations here (ViewportInfo as a push-constant block, the sampler at binding 0)
set -e
O=$(cd "$(dirname "$0")" && pwd); T=$O/../../../third_party/sgsr
G=${GLSLANG:-glslangValidator}
$G -V "$O/ovl.vert" -o "$O/ovl.vert.spv"; $G -V "$O/ovl.frag" -o "$O/ovl.frag.spv"
$G -V "$O/scale.vert" -o "$O/scale.vert.spv"; $G -V "$O/scale.frag" -o "$O/scale.frag.spv"
W=$(mktemp -d)
for f in fg.vert fg_luma.frag fg_flow.frag fg_stat.frag fg_gen.frag; do $G -V "$O/$f" -o "$W/$f.spv"; done
python3 - "$W" "$O/fgen_spv.h" <<'EOF'
import struct, sys
out = '// Generated from fg.vert and fg_luma / fg_flow / fg_stat / fg_gen.frag (glslangValidator -V): frame generation (nvn_fgen.h)\n'
for name, f in (('fg_vert', 'fg.vert'), ('fg_luma_frag', 'fg_luma.frag'), ('fg_flow_frag', 'fg_flow.frag'), ('fg_stat_frag', 'fg_stat.frag'), ('fg_gen_frag', 'fg_gen.frag')):
    b = open(sys.argv[1] + '/' + f + '.spv', 'rb').read(); w = struct.unpack('<%dI' % (len(b) // 4), b)
    out += 'static const uint32_t %s_spv[] = {%s};\n' % (name, ','.join(hex(x) for x in w))
open(sys.argv[2], 'w').write(out); print('fgen_spv.h')
EOF
python3 - "$T/sgsr1_shader_mobile.frag" "$W/sgsr.frag" <<'EOF'
import sys
s = open(sys.argv[1], encoding='utf-8').read()
for old, new in (('#version 300 es', '#version 450'),
                 ('uniform highp vec4 ViewportInfo[1];', 'layout(push_constant) uniform UniformBlock { highp vec4 ViewportInfo[1]; };'),
                 ('uniform mediump sampler2D ps0;\n#endif', 'layout(set = 0, binding = 0) uniform mediump sampler2D ps0;\n#endif'),
                 ('int mode = OperationMode;', 'const int mode = OperationMode;')):   # (desktop GLSL wants textureGather's component a constant; ES takes the variable)
    assert s.count(old) == 1, old
    s = s.replace(old, new)
import re
s = re.sub(r'\b(?:highp|mediump|lowp) (?=(?:vec|ivec|uvec|mat)[234]\s*\()', '', s)   # (precision on a constructor in an expression: ES accepts it, desktop GLSL does not)
open(sys.argv[2], 'w', encoding='utf-8').write(s)
EOF
$G -V "$O/sgsr.vert" -o "$O/sgsr.vert.spv"; $G -V "$W/sgsr.frag" -o "$O/sgsr.frag.spv"; rm -rf "$W"
python3 - "$O" <<'EOF'
import struct, sys
o = sys.argv[1]
def arr(name, path):
    b = open(path, 'rb').read(); w = struct.unpack('<%dI' % (len(b) // 4), b)
    return 'static const uint32_t %s[] = {%s};\n' % (name, ','.join(hex(x) for x in w))
for out, head, parts in (('ovl_spv.h', 'Generated from ovl.vert / ovl.frag (glslangValidator -V): the touch overlay shaders', ('ovl_vert', 'ovl_frag')),
                         ('scale_spv.h', 'Generated from scale.vert / scale.frag (glslangValidator -V): the game picture\'s upscaler', ('scale_vert', 'scale_frag')),
                         ('sgsr_spv.h', 'Generated from sgsr.vert and dm/third_party/sgsr/sgsr1_shader_mobile.frag (glslangValidator -V): Snapdragon Game Super Resolution 1,\n'
                          '// Copyright (c) 2023 Qualcomm Innovation Center, Inc., BSD-3-Clause (dm/third_party/sgsr/LICENSE)', ('sgsr_vert', 'sgsr_frag'))):
    open(o + '/' + out, 'w').write('// ' + head + '\n' + ''.join(arr(p + '_spv', o + '/' + p.replace('_', '.') + '.spv') for p in parts))
    print(out)
EOF
