# modcheck.py <image> <patched image> <pchtxt>...: every patch site: in range, what the original instruction is, whether the host rewriter changed it,
# and the patch disassembled
import sys, re, struct, os
sys.path.insert(0, r'C:\Users\daniel\nxnative-work\aot\capsrc\capstone-5.0.7\bindings\python')
try:
    import capstone; md = capstone.Cs(capstone.CS_ARCH_ARM64, capstone.CS_MODE_ARM)
except Exception as e:
    md = None; print('(no capstone:', e, ')')
img = open(sys.argv[1], 'rb').read(); pat = open(sys.argv[2], 'rb').read()
def dis(b, a):
    if not md or len(b) % 4: return b.hex()
    return '; '.join(f'{i.mnemonic} {i.op_str}' for i in md.disasm(b, a)) or '(data)'
# text end: the MOD0 / NSO layout is not at hand here; the first segment ends where code stops: take the patched image's own size as the limit
for p in sys.argv[3:]:
    print('=' * 8, os.path.relpath(p, os.path.dirname(os.path.dirname(os.path.dirname(p)))))
    shift = 0; bid = None; en = False; n = touched = 0
    for ln in open(p, encoding='utf-8', errors='replace'):
        t = re.split(r'//|#', ln)[0].strip()
        if not t: continue
        if t.startswith('@'):
            m = re.match(r'@nsobid-([0-9A-Fa-f]+)', t)
            if m: bid = m.group(1)[:32].lower()
            m = re.match(r'@flag\s+offset_shift\s+(\S+)', t)
            if m: shift = int(m.group(1), 0)
            if t.lower() == '@enabled': en = True
            if t.lower() == '@disabled': en = False
            if t.lower() == '@stop': break
            continue
        if not en: continue
        a, v = t.split(None, 1); off = int(a, 16) + shift - 0x100; b = bytes.fromhex(v.replace(' ', '')); n += 1
        if off + len(b) > len(img): print(f'  {off:#010x}: OUTSIDE the image'); continue
        o, r = img[off:off + len(b)], pat[off:off + len(b)]
        rw = r != o; touched += rw
        print(f'  {off:#010x} {"REWRITTEN " if rw else ""}orig: {dis(o, off)}  ->  mod: {dis(b, off)}')
    print(f'  build {bid}, {n} patches, {touched} on code the host rewrote')
