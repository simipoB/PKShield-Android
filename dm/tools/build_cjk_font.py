#!/usr/bin/env python3
# Usage: build_cjk_font.py <out.ttf>, run where NanumGothic.ttf (android.googlesource.com external/naver-fonts) and DroidSansFallbackFull.ttf (frameworks/base
#   data/fonts) are; fontTools. Output: dm/android/app/assets/cjk_fallback.ttf
# One TrueType font for the Switch's Chinese / Korean / Japanese shared fonts: NanumGothic (Latin, Korean; OFL) + Droid Sans Fallback (Chinese, Japanese; Apache 2.0),
# both plain glyf fonts; hinting and layout tables left out, Droid scaled to Nanum's 1000 units per em, Nanum first (its Latin wins), family renamed
import sys
from fontTools.ttLib import TTFont
from fontTools.ttLib.scaleUpem import scale_upem
from fontTools.merge import Merger, Options
def clean(fn, out, upm=None):
    from fontTools import subset
    f = TTFont(fn)
    o = subset.Options(); o.hinting = False; o.layout_features = []; o.drop_tables += ['GSUB', 'GPOS', 'GDEF', 'kern', 'vhea', 'vmtx', 'DSIG', 'hdmx', 'LTSH', 'VDMX', 'gasp']
    o.glyph_names = False; o.notdef_outline = True; o.name_IDs = ['*']; o.name_languages = ['*']
    sb = subset.Subsetter(o); sb.populate(unicodes=f.getBestCmap().keys()); sb.subset(f)
    if upm: scale_upem(f, upm)
    f.save(out)
clean('NanumGothic.ttf', 'a.ttf'); clean('DroidSansFallbackFull.ttf', 'b.ttf', 1000)
m = Merger(options=Options(drop_tables=['GSUB', 'GPOS', 'GDEF'])); f = m.merge(['a.ttf', 'b.ttf'])
for rec in f['name'].names:
    if rec.nameID in (1, 3, 4, 6, 16):
        rec.string = {1: 'DM CJK Fallback', 3: 'DM CJK Fallback 1.0', 4: 'DM CJK Fallback Regular', 6: 'DMCJKFallback-Regular', 16: 'DM CJK Fallback'}[rec.nameID]
f.save(sys.argv[1]); print('saved', sys.argv[1])
