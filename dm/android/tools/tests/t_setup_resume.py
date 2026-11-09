#!/usr/bin/env python3
# setup_game.py's kept translation (#100): a fresh setup keeps the last translation only for the same shader list and chain; the step-5 progress counts the files a
# step writes. Expected values are hand-written. Run: python3 -I dm/android/tools/tests/t_setup_resume.py
import io, os, sys, tempfile, threading, contextlib
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))
import setup_game as sg

checks = fails = 0
def check(ok, what):
    global checks, fails
    checks += 1; fails += not ok; print(('✓ ' if ok else '✗ ') + what)

with tempfile.TemporaryDirectory() as w:
    found = os.path.join(w, 'shader_map.found.tsv')
    open(found, 'w').write('package\tkind\tstage\tkey\tsource\nbin/a.bnsh\tpixel\tpixel\t00112233445566778899\tx\n')
    check(not sg.translation_kept(w, 'c1'), 'no marker: nothing kept')
    import json
    json.dump({'chain': 'c1', 'found': sg.found_digest(w)}, open(sg.translated_mark(w), 'w'))
    check(sg.translation_kept(w, 'c1'), 'same shader list, same chain: kept')
    check(not sg.translation_kept(w, 'c2'), 'another chain (a new translator): not kept')
    open(found, 'a').write('bin/b.bnsh\tvertex\tvertex\taabbccddeeff00112233\tx\n')
    check(not sg.translation_kept(w, 'c1'), 'another shader list (another dump or game): not kept')
    open(sg.translated_mark(w), 'w').write('{not json')
    check(not sg.translation_kept(w, 'c1'), 'a damaged marker: not kept')
    os.remove(found); json.dump({'chain': 'c1', 'found': ''}, open(sg.translated_mark(w), 'w'))
    check(not sg.translation_kept(w, 'c1'), 'no shader list at all: not kept')

with tempfile.TemporaryDirectory() as d:
    for i in range(3): open(os.path.join(d, f'k{i}.vkr'), 'w').close()
    open(os.path.join(d, 'k9.spv'), 'w').close()
    stop = threading.Event(); out = io.StringIO()
    with contextlib.redirect_stdout(out):
        t = threading.Thread(target=sg.watch_files, args=(d, '.vkr', 10, stop)); t.start()
        stop.wait(5.5); stop.set(); t.join()
    check('PROGRESS 3/10' in out.getvalue(), 'step 5 progress: 3 of 10 reflection files written')

print(f'{checks} checks, {fails} failed'); sys.exit(fails != 0)
