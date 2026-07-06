#!/usr/bin/env python3
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
ASSETS = os.path.join(ROOT, "core", "assets")
TP = os.path.join(ROOT, "third_party")
FTPY = "/opt/homebrew/opt/fonttools/libexec/bin/python"

FONTS = [
    ("sans",     "LiberationSans",            "LiberationSans-Regular"),
    ("sans_b",   "LiberationSans-Bold",       "LiberationSans-Bold"),
    ("sans_i",   "LiberationSans-Italic",     "LiberationSans-Italic"),
    ("sans_bi",  "LiberationSans-BoldItalic", "LiberationSans-BoldItalic"),
    ("serif",    "LiberationSerif",           "LiberationSerif-Regular"),
    ("serif_b",  "LiberationSerif-Bold",      "LiberationSerif-Bold"),
    ("serif_i",  "LiberationSerif-Italic",    "LiberationSerif-Italic"),
    ("serif_bi", "LiberationSerif-BoldItalic","LiberationSerif-BoldItalic"),
    ("mono",     "LiberationMono",            "LiberationMono-Regular"),
    ("mono_b",   "LiberationMono-Bold",       "LiberationMono-Bold"),
    ("mono_i",   "LiberationMono-Italic",     "LiberationMono-Italic"),
    ("mono_bi",  "LiberationMono-BoldItalic", "LiberationMono-BoldItalic"),
]
LIB_DIR = os.path.join(TP, "fonts", "liberation-fonts-ttf-2.1.5")
DEJAVU = os.path.join(TP, "fonts", "dejavu-fonts-ttf-2.37", "ttf", "DejaVuSans.ttf")
CFF_DIR = os.path.join(TP, "cff")

def byte_array(data, name):
    lines = []
    for i in range(0, len(data), 20):
        lines.append(",".join(str(b) for b in data[i:i + 20]))
    return "const unsigned char %s[] = {\n%s\n};" % (name, ",\n".join(lines))

def gen_icc():
    exe = os.path.join(HERE, "icc_gen_bin")
    subprocess.run(["cc", os.path.join(HERE, "icc_gen.c"),
                    "-I/opt/homebrew/opt/lcms2/include",
                    "-L/opt/homebrew/opt/lcms2/lib", "-llcms2", "-o", exe], check=True)
    srgb, cmyk = os.path.join(HERE, "srgb.icc"), os.path.join(HERE, "cmyk.icc")
    subprocess.run([exe, srgb, cmyk], check=True)
    for path, sym, hh in [(srgb, "kSrgbIcc", "srgb_icc"), (cmyk, "kCmykIcc", "cmyk_icc")]:
        data = open(path, "rb").read()
        cpp = '#include "%s.hh"\n\nnamespace pdfa {\n\n%s\n\nconst unsigned int %sLen = %d;\n\n}\n' % (
            hh, byte_array(data, sym + "[]").replace("[][]", "[]"), sym, len(data))
        cpp = cpp.replace("const unsigned char %s[][]" % sym, "const unsigned char %s[]" % sym)
        open(os.path.join(ASSETS, hh + ".cpp"), "w").write(cpp)
        open(os.path.join(ASSETS, hh + ".hh"), "w").write(
            "#pragma once\n\nnamespace pdfa {\n\nextern const unsigned char %s[];\nextern const unsigned int %sLen;\n\n}\n" % (sym, sym))
    print("icc assets written")

def gen_cff():
    os.makedirs(CFF_DIR, exist_ok=True)
    script = os.path.join(HERE, "ttf2cff.py")
    for _, _, base in FONTS:
        src = os.path.join(LIB_DIR, base + ".ttf")
        dst = os.path.join(CFF_DIR, base + ".cff")
        subprocess.run([FTPY, script, src, dst], check=True)
    subprocess.run([FTPY, script, DEJAVU, os.path.join(CFF_DIR, "DejaVuSans.cff")], check=True)
    print("cff conversions written")

def gen_fonts():
    entries = []
    for key, ps, base in FONTS:
        entries.append((key, ps, os.path.join(CFF_DIR, base + ".cff")))
        entries.append((key + ":ttf", ps, os.path.join(LIB_DIR, base + ".ttf")))
    entries.append(("symbol", "DejaVuSans", os.path.join(CFF_DIR, "DejaVuSans.cff")))
    entries.append(("symbol:ttf", "DejaVuSans", DEJAVU))

    parts = ['#include "fonts_data.hh"', '', 'namespace pdfa {', '', 'namespace {', '']
    table = []
    for i, (key, ps, path) in enumerate(entries):
        data = open(path, "rb").read()
        parts.append(byte_array(data, "k_f%d" % i))
        table.append('    {"%s", "%s", k_f%d, %d},' % (key, ps, i, len(data)))
    parts += ['', '}', '', 'const FontAsset kFontAssets[] = {'] + table + [
        '};', '', 'const unsigned int kFontAssetCount = %d;' % len(entries), '', '}', '']
    open(os.path.join(ASSETS, "fonts_data.cpp"), "w").write("\n".join(parts))
    print("font assets written (%d entries)" % len(entries))

def gen_agl():
    entries = {}
    for fname in ["glyphlist.txt", "zapfdingbats.txt"]:
        for line in open(os.path.join(TP, fname)):
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            name, codes = line.split(";")
            cps = codes.split()
            if cps and name not in entries:
                entries[name] = int(cps[0], 16)
    rows = "\n".join('    {"%s", 0x%X},' % (n, u) for n, u in sorted(entries.items()))
    print("agl table: %d entries (regenerate core/assets/agl_names.cpp manually if the parser changes)" % len(entries))
    return rows

if __name__ == "__main__":
    which = sys.argv[1] if len(sys.argv) > 1 else "all"
    if which in ("all", "icc"):
        gen_icc()
    if which in ("all", "cff"):
        gen_cff()
    if which in ("all", "fonts"):
        gen_fonts()
    if which in ("all", "agl"):
        gen_agl()
