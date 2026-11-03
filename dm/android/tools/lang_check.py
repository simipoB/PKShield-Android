#!/usr/bin/env python3
# The app's translations (T.java): every text the Java code hands to T.t (or GameMenu's t / row) must have a line in each app/assets/lang/<language>.tsv, with the same
# %s / %d placeholders in the same order. Usage: lang_check.py [--keys] (--keys: print the texts, one a line)
import glob, os, re, sys
HERE = os.path.dirname(os.path.abspath(__file__)); APP = os.path.join(HERE, '..', 'app')
LIT = r'"((?:[^"\\]|\\.)*)"'
CALL = re.compile(r'(?:\bT\.t\((?:this|a|c|ctx), |\bt\(a, |\brow\(a, rows, acts, )' + LIT)
TERN = re.compile(r'[?:] ' + LIT)   # the other branch of a choice inside a t(...) call: t(a, x ? "A" : "B")
STEPS = ['Display', 'Performance', 'Controls', 'Online', 'System', 'Saves',   # the settings panel's tabs (GameMenu.TABS, looked up by name)
         'unpack the game', 'rewrite the code', 'find the shaders', 'translate the shaders', 'pack the shaders', 'finish',   # setup_game.py STEP lines (SetupActivity.step)
         'read the captured shaders', 'translate %s shaders', "find the mods' shaders"]
def unq(s): return s.replace('\\"', '"').replace('\\n', '\n').replace('\\\\', '\\')
def keys():
    out = set(STEPS)
    for f in glob.glob(os.path.join(APP, 'java/com/nxnative/*.java')):
        for ln in open(f, encoding='utf-8'):
            for m in CALL.finditer(ln): out.add(unq(m.group(1)))
            if 't(a, ' in ln or 'T.t(' in ln:
                for m in TERN.finditer(ln):
                    v = unq(m.group(1))
                    if v and not re.fullmatch(r'[a-z0-9:]{0,8}', v): out.add(v)   # (setting values such as "high" / "20:9" are not texts)
    return {k for k in out if len(re.findall(r'[^\W\d_]', k)) >= 3 and 't(a, ' not in k and '(kc' not in k}   # (fragments of code, symbols: not texts)
def ph(s): return re.findall(r'%[sd%]', s)
def main():
    ks = keys()
    if '--keys' in sys.argv: print('\n'.join(sorted(ks))); return 0
    bad = 0
    for p in sorted(glob.glob(os.path.join(APP, 'assets/lang/*.tsv'))):
        tr = {}
        for n, ln in enumerate(open(p, encoding='utf-8'), 1):
            ln = ln.rstrip('\n')
            if not ln or ln.startswith('#'): continue
            if '\t' not in ln: print(f'{os.path.basename(p)}:{n}: no tab'); bad += 1; continue
            k, v = ln.split('\t', 1); tr[k.replace('\\n', '\n')] = v.replace('\\n', '\n')
        miss = sorted(k for k in ks if k not in tr); extra = sorted(k for k in tr if k not in ks)
        wrong = sorted(k for k in ks if k in tr and ph(k) != ph(tr[k]))
        for k in miss: print(f'{os.path.basename(p)}: missing: {k!r}')
        for k in wrong: print(f'{os.path.basename(p)}: placeholders differ: {k!r}')
        for k in extra: print(f'{os.path.basename(p)}: not used any more: {k!r}')
        bad += len(miss) + len(wrong)
        print(f'{os.path.basename(p)}: {len(ks) - len(miss)} of {len(ks)} texts')
    return 1 if bad else 0
if __name__ == '__main__': sys.exit(main())
