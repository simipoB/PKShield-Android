#!/usr/bin/env python3
# Mach-O assembly (dm/host/entry.S, dm/gfx/nvn/*.S) to ELF for the Android host: C symbols lose the leading underscore, sym@PAGE / sym@PAGEOFF become sym / :lo12:sym, .cstring becomes a string section.
# Usage: elf_asm.py in.S out.S
import re, sys
s = open(sys.argv[1]).read()
s = re.sub(r'\b_([A-Za-z]\w*)@PAGEOFF\b', r':lo12:\1', s)
s = re.sub(r'\b_([A-Za-z]\w*)@PAGE\b', r'\1', s)
s = re.sub(r'(?<![\w.])_([A-Za-z]\w*)', r'\1', s)   # _sym → sym (labels, .globl, bl / b / adr operands); .p2align and other directives start with a dot and are left alone
s = re.sub(r'^(\s*)\.cstring\b', r'\1.section .rodata.str1.1,"aMS",@progbits,1', s, flags=re.M)   # Mach-O's C string section
# LSE atomics (ARMv8.1 stadd: the NVN thunks' call counters) as exclusive load / store loops, which every ARMv8.0 CPU runs: the Cortex-A73 / A53 phones (Snapdragon 680 / 685,
# Adreno 610: Galar-Native-Android #222, SIGILL at the first NVN call) and the Android Emulator's ARM translation (ndk_translation) have no LSE. Scratch registers from x9-x15 (caller-saved: free at a call boundary, where these counters sit)
def _stadd(m):
    ind, rs, rn = m.group(1), m.group(2), m.group(3)
    a, b = [r for r in ('x11', 'x12', 'x13', 'x14', 'x15', 'x9', 'x10') if r not in (rs, rn)][:2]
    wb = 'w' + b[1:]
    return (f"{ind}1: ldxr {a}, [{rn}]\n{ind}add {a}, {a}, {rs}\n{ind}stxr {wb}, {a}, [{rn}]\n{ind}cbnz {wb}, 1b")
s = re.sub(r'^([ \t]*)stadd\s+(x\d+),\s*\[(x\d+)\]', _stadd, s, flags=re.M)
s = re.sub(r'^\s*\.arch_extension lse\s*$', '', s, flags=re.M)
open(sys.argv[2], 'w').write(s)
