#!/usr/bin/env python3
# A first setup has no shaders captured from the game yet: shader_split.py notes that without a [RED] line (players took it for the setup failing)
import contextlib, io, os, sys, tempfile

os.environ.setdefault('DM_GAME', '01008DB008C2C000')
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '../gfx/tools'))
import shader_split

def captured(cap):
    out = io.StringIO()
    with contextlib.redirect_stdout(out): got = shader_split.nvn_captured(cap)
    return got, out.getvalue()

with tempfile.TemporaryDirectory() as tmp:
    got, text = captured(os.path.join(tmp, 'nvn_capture'))
    assert got == [] and '[RED]' not in text and 'no captured shaders yet' in text, text
    os.mkdir(os.path.join(tmp, 'nvn_capture'))
    got, text = captured(os.path.join(tmp, 'nvn_capture'))
    assert got == [] and '[RED]' in text, text
print('first setup without captured shaders is not reported as an error')
