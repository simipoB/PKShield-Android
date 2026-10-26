#!/usr/bin/env python3
# Reflection for the Vulkan replay: each <key>_b.json the shader build wrote (dmsh_build.py) → <key>.vkr, a line format the C replay reads without a JSON parser (nvn_vk.c vkr_load).
# Lines: "chain <version>", "stage <vs|ps|cs|...>", "b <name> <kind> <reg> <off> <bank> <rw> <n>" per binding (reg -1 = none), "c1 <hex>", "drv <c0 read> <c2 read>",
# "tg <x> <y> <z>", "colors <mask> <known>", "vin <location> <u|i|f>", "unfill <what>" when the shader reads driver-segment locations the replay cannot fill.
# "tf <buffer> <output location> <components> <stride>" per transform feedback buffer of a vertex shader with a capture entry point, read from that entry point in the Metal
# source of the same chain (dmsh_build.py vs_tf stores output n's components as dm_tf<b>[vid * <components> + i]; the .metal files are the only place the layout is kept).
# Usage: vk_meta.py <json dir> <out dir> [Metal source dir b-<chain>]   (only <key>.spv files present in <out dir> get a .vkr)
import json, os, re, sys

def tf_lines(j, metal):
    """The capture layout from the Metal source's main0_tf: [(buffer, location, components, stride)]"""
    if not j.get('entry_tf') or not metal: return []
    try: src = open(os.path.join(metal, j['key'] + ('' if j['key'].endswith('_b') else '_b') + '.metal')).read()
    except OSError: print(f"[RED] {j.get('key')}: transform feedback shader without its Metal source in {metal}: no capture on Vulkan"); return []
    st = re.search(r'struct main0_out\s*\{(.*?)\};', src, re.S)
    names = {nm: int(l) for nm, l in re.findall(r'(\w+) \[\[user\(locn(\d+)\)\]\]', st.group(1) if st else '')}
    m = re.search(r'vertex void main0_tf\(.*?\{(.*)$', src, re.S); body = m.group(1) if m else ''
    strides = j.get('tf_strides') or [0, 0, 0, 0]; out = []
    for b in range(4):
        if not strides[b]: continue
        st_b = re.findall(r'dm_tf%d\[\w+ \* (\d+) \+ (\d+)\] = (?:as_type<uint>\(out\.(\w+)\.[xyzw]\)|0u)' % b, body)
        if not st_b: print(f"[RED] {j['key']}: transform feedback buffer {b} not found in main0_tf"); continue
        locs = {names.get(nm, -1) for _, _, nm in st_b if nm}
        out.append((b, locs.pop() if len(locs) == 1 else -1, int(st_b[0][0]), strides[b]))
    return out

def conv(j, metal=None):
    out = [f"chain {j.get('chain', '')}", f"stage {j.get('stage', '')}"]
    for b in j.get('bindings', []):
        name = b.get('msl_name', '?'); kind = b.get('kind', '?'); reg = b.get('reg'); ty = b.get('msl_type', '')
        n = 1
        m = re.match(r'array<.*, (\d+)>$', ty or '')
        if m: n = int(m.group(1))
        out.append(f"b {name} {kind} {reg if isinstance(reg, int) else -1} {b.get('off', 0)} {b.get('bank', 0)} {1 if b.get('rw') else 0} {n}")
    c1 = j.get('c1') or ''
    out.append(f"c1 {c1}")
    dr = j.get('drv_reads') or {}
    out.append(f"drv {1 if '0' in dr else 0} {1 if '2' in dr else 0}")
    tg = j.get('threadgroup_size')
    if isinstance(tg, list) and len(tg) == 3: out.append(f"tg {tg[0]} {tg[1]} {tg[2]}")
    ra = j.get('raster') or {}
    cols = ra.get('colors')
    if isinstance(cols, list): out.append(f"colors {sum(1 << c for c in cols if isinstance(c, int) and 0 <= c < 8)} 1")
    for v in j.get('vertex_inputs', []):
        t = v.get('msl_type', '')
        out.append(f"vin {v.get('location', 0)} {'u' if t.startswith('uint') else 'i' if t.startswith('int') else 'f'}")
    for u in j.get('drv_unfillable') or []: out.append(f"unfill {u}")
    for b, l, n, st in tf_lines(j, metal): out.append(f"tf {b} {l} {n} {st}")
    return '\n'.join(out) + '\n'

def main():
    src, dst = sys.argv[1], sys.argv[2]; metal = sys.argv[3] if len(sys.argv) > 3 else None
    n = 0
    for f in os.listdir(dst):
        if not f.endswith('.spv'): continue
        k = f[:-4]; jp = os.path.join(src, k + '_b.json')
        if not os.path.exists(jp): continue
        open(os.path.join(dst, k + '.vkr'), 'w').write(conv(json.load(open(jp)), metal))
        n += 1
    print(f'vk_meta: {n} reflection files in {dst}')

if __name__ == '__main__': main()
