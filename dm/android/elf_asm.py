#!/usr/bin/env python3
# Mach-O assembly (dm/host/entry.S, dm/gfx/nvn/*.S) to ELF for the Android host: C symbols lose the leading underscore, sym@PAGE / sym@PAGEOFF become sym / :lo12:sym, .cstring becomes a string section.
# Usage: elf_asm.py in.S out.S
import re, sys
s = open(sys.argv[1]).read()
s = re.sub(r'\b_([A-Za-z]\w*)@PAGEOFF\b', r':lo12:\1', s)
s = re.sub(r'\b_([A-Za-z]\w*)@PAGE\b', r'\1', s)
s = re.sub(r'(?<![\w.])_([A-Za-z]\w*)', r'\1', s)   # _sym → sym (labels, .globl, bl / b / adr operands); .p2align and other directives start with a dot and are left alone
s = re.sub(r'^(\s*)\.cstring\b', r'\1.section .rodata.str1.1,"aMS",@progbits,1', s, flags=re.M)   # Mach-O's C string section
open(sys.argv[2], 'w').write(s)
