# Copies an APK with entries replaced: apk_swap.py <src> <dst> <entry>=<file> ... (every entry keeps its compression; zipalign then realigns and apksigner signs)
import sys, zipfile
src, dst = sys.argv[1:3]; rep = dict(a.split("=", 1) for a in sys.argv[3:])
with zipfile.ZipFile(src) as zi, zipfile.ZipFile(dst, "w", allowZip64=True) as zo:
    for i in zi.infolist():
        data = open(rep.pop(i.filename), "rb").read() if i.filename in rep else zi.read(i)
        n = zipfile.ZipInfo(i.filename, i.date_time); n.compress_type = i.compress_type; n.external_attr = i.external_attr
        zo.writestr(n, data)
assert not rep, rep
print("wrote", dst)
