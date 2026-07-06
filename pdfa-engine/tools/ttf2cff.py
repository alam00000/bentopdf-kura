import sys
from fontTools.ttLib import TTFont
from fontTools.pens.qu2cuPen import Qu2CuPen
from fontTools.pens.t2CharStringPen import T2CharStringPen
from fontTools.fontBuilder import FontBuilder

src, dst = sys.argv[1], sys.argv[2]
f = TTFont(src)
upem = f["head"].unitsPerEm
glyphOrder = f.getGlyphOrder()
glyphSet = f.getGlyphSet()

charstrings = {}
for name in glyphOrder:
    g = glyphSet[name]
    t2pen = T2CharStringPen(g.width, glyphSet)
    pen = Qu2CuPen(t2pen, max_err=upem * 0.001, all_cubic=True)
    try:
        g.draw(pen)
    except Exception:
        t2pen = T2CharStringPen(g.width, glyphSet)
        charstrings[name] = t2pen.getCharString()
        continue
    charstrings[name] = t2pen.getCharString()

ps_name = f["name"].getDebugName(6) or "Converted"
fb = FontBuilder(unitsPerEm=upem, isTTF=False)
fb.setupGlyphOrder(list(glyphOrder))
cmap = f.getBestCmap()
fb.setupCharacterMap({cp: gn for cp, gn in cmap.items() if gn in charstrings})
fb.setupCFF(ps_name, {"FullName": ps_name, "FamilyName": ps_name}, charstrings, {})
metrics = {gn: (glyphSet[gn].width, 0) for gn in glyphOrder}
fb.setupHorizontalMetrics(metrics)
fb.setupHorizontalHeader(ascent=f["hhea"].ascent, descent=f["hhea"].descent)
fb.setupNameTable({"psName": ps_name, "familyName": ps_name, "styleName": "Regular"})
fb.setupOS2(sTypoAscender=f["OS/2"].sTypoAscender, sTypoDescender=f["OS/2"].sTypoDescender)
fb.setupPost()
cff_bytes = fb.font["CFF "].compile(fb.font)
open(dst, "wb").write(cff_bytes)
print(dst, len(cff_bytes))
