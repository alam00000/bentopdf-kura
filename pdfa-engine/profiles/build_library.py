#!/usr/bin/env python3
import json
import os
import re
import shutil

ROOT = os.path.dirname(os.path.abspath(__file__))
FOLDERS = ["report", "press", "gwg", "online", "archive", "accessibility", "standards",
           "images", "colour", "objects", "pages", "document", "actions"]
MM = 72 / 25.4
PROFILES = []


def cond(prop, op, value):
    return {"prop": prop, "op": op, "value": value}


def check(name, severity, conds, scope=None):
    c = {"name": name, "severity": severity, "all": conds}
    if scope:
        c["scope"] = scope
    return c


def builtin(name, severity, level=None, **params):
    b = {"name": name, "severity": severity}
    if level:
        b["level"] = level
    if params:
        b["params"] = params
    return b


def fix(op, *params):
    return {"op": op, "params": [str(p) for p in params]}


def slug(name):
    s = re.sub(r"[^a-z0-9]+", "-", name.lower()).strip("-")
    return re.sub(r"-+", "-", s)


UNSUPPORTED_PROPS = {"page.isScaled"}


def profile(folder, name, description, checks=(), builtins=(), fixes=()):
    kept = [c for c in checks if not any(a["prop"] in UNSUPPORTED_PROPS for a in c["all"])]
    if not kept and not builtins and not fixes:
        return
    body = {"kura-profile": 1, "name": name, "description": description,
            "checks": kept, "builtins": list(builtins), "fixes": list(fixes)}
    PROFILES.append((folder, slug(name), body))


def mm(v):
    return round(v * MM, 2)


def hairline_checks(error_pt=0.125, warn_pt=0.25):
    return [
        check(f"Stroke thinner than {error_pt} pt", "error",
              [cond("stroke.width", "<=", error_pt)]),
        check(f"Stroke between {error_pt} and {warn_pt} pt", "warning",
              [cond("stroke.width", "<=", warn_pt), cond("stroke.width", ">", error_pt)]),
        check(f"Thin stroke below {warn_pt} pt in more than one ink", "error",
              [cond("stroke.width", "<=", warn_pt), cond("paint.inkCount", ">", 1)]),
    ]


def small_text_checks(min_pt=5, min_multi_pt=8):
    return [
        check(f"Text smaller than {min_pt} pt", "warning",
              [cond("text.size", "<", min_pt), cond("text.size", ">", 0.01)]),
        check(f"Text smaller than {min_multi_pt} pt in more than one ink", "warning",
              [cond("text.size", "<", min_multi_pt), cond("text.size", ">", 0.01),
               cond("paint.inkCount", ">", 1)]),
        check(f"White text smaller than {min_multi_pt} pt", "warning",
              [cond("text.size", "<", min_multi_pt), cond("text.size", ">", 0.01),
               cond("paint.isWhite", "==", True)]),
    ]


def image_builtins(min_ppi, min_bitmap_ppi, max_ppi=None):
    out = [builtin("imageResolutionBelow", "error", ppi=min_ppi),
           builtin("bitmapResolutionBelow", "warning", ppi=min_bitmap_ppi)]
    if max_ppi:
        out.append(builtin("imageResolutionAbove", "info", ppi=max_ppi))
    return out


def colour_builtins(policy, spot_limit=2):
    out = [check("Registration colour used for content", "error",
                 [cond("paint.isRegistration", "==", True)])]
    if policy in ("cmyk", "cmyk_spot"):
        out += [builtin("rgbUsed", "error"),
                builtin("deviceIndependentColour", "warning")]
    if policy == "cmyk":
        out.append(builtin("spotColoursMoreThan", "error", count=0))
    if policy == "cmyk_spot":
        out.append(builtin("spotColoursMoreThan", "warning", count=spot_limit))
        out.append(builtin("spotNamesInconsistent", "warning"))
    if policy == "rgb":
        out.append(builtin("spotColoursMoreThan", "warning", count=0))
    if policy == "black":
        out.append(builtin("colourPlatesUsed", "error"))
        out.append(builtin("spotColoursMoreThan", "error", count=0))
    return out


def ink_checks(tac):
    return [
        check(f"Total ink above {tac}% in fills", "error", [cond("fill.totalInk", ">", tac)]),
        check(f"Total ink above {tac}% in strokes", "error", [cond("stroke.totalInk", ">", tac)]),
        check("Rich black on text smaller than 12 pt", "warning",
              [cond("content.isText", "==", True), cond("text.size", "<", 12),
               cond("paint.richBlackCmyPercent", ">", 0)]),
    ]


def overprint_checks():
    return [
        check("White object set to overprint", "error",
              [cond("paint.isWhite", "==", True), cond("fill.overprint", "==", True)]),
        check("Black text not set to overprint", "warning",
              [cond("content.isText", "==", True), cond("paint.is100Black", "==", True),
               cond("fill.overprint", "==", False)]),
    ]


def transparency_checks(severity):
    return [
        check("Transparency in use", severity, [cond("gstate.transparency", "==", True)]),
        check("Blend mode other than Normal", severity,
              [cond("gstate.blendMode", "!=", "Normal"), cond("gstate.blendMode", "!=", "Compatible")]),
    ]


def font_checks():
    return [
        builtin("fontsNotEmbedded", "error"),
        check("Type 3 font in use", "warning", [cond("font.isType3", "==", True)]),
        check("Glyph falls back to .notdef", "error", [cond("font.notdefUsed", "==", True)]),
        check("Font program is not valid", "error", [cond("font.invalid", "==", True)]),
    ]


def page_checks(safety_mm=3):
    return [
        builtin("pagesDifferInSize", "warning"),
        builtin("emptyPage", "warning"),
        check("Page scaling in use", "error", [cond("page.isScaled", "==", True)]),
        check(f"Object within {safety_mm} mm of the trim edge", "info",
              [cond("content.distanceInsideTrimBox", "<", mm(safety_mm))], scope="trim"),
        check("Annotation that prints", "warning", [cond("annot.prints", "==", True)]),
        check("Content on a layer", "info", [cond("layers.onLayer", "==", True)]),
    ]


def document_builtins(min_version=None):
    out = [builtin("encrypted", "error"), builtin("damaged", "error"),
           builtin("syntaxProblems", "error")]
    if min_version:
        out.append(builtin("pdfVersionBelow", "warning", version=min_version))
    return out


def press_fixes(bleed_mm=3, min_stroke=0.25, cmyk_blend=True):
    out = [
        fix("setpagebox", "TrimBox", "RelativeToCropBox", 0, 0, 0, 0, "pt"),
        fix("generatebleed", "Amount", bleed_mm, "mm"),
        fix("removepagescaling"),
        fix("increaselinewidth", min_stroke, "", "pt"),
        fix("overprintblack", "Text"),
        fix("knockoutwhite", ""),
        fix("trappedkey", "false"),
        fix("removeflatness"),
        fix("removesmoothness"),
        fix("removeunnecessarytransparencygroups"),
    ]
    if cmyk_blend:
        out.append(fix("settransparencyblendcs", "CMYK"))
    return out


WORKFLOWS = [
    ("Sheetfed offset, CMYK", "sheetfed offset on coated or uncoated stock, four process inks only",
     dict(policy="cmyk", ppi=300, bmp=1200, maxppi=450, tac=330, transparency="info", version=1.6, spot=0)),
    ("Sheetfed offset with spot colours", "sheetfed offset with up to two spot inks alongside CMYK",
     dict(policy="cmyk_spot", ppi=300, bmp=1200, maxppi=450, tac=330, transparency="info", version=1.6, spot=2)),
    ("Web offset, heatset", "heatset web offset for magazines and catalogues",
     dict(policy="cmyk", ppi=300, bmp=1200, maxppi=450, tac=300, transparency="info", version=1.6, spot=0)),
    ("Newspaper, coldset web", "coldset web offset on newsprint",
     dict(policy="cmyk", ppi=200, bmp=600, maxppi=300, tac=240, transparency="info", version=1.6, spot=0, text=6, multi=9)),
    ("Gravure", "rotogravure for long runs",
     dict(policy="cmyk_spot", ppi=300, bmp=1200, maxppi=450, tac=330, transparency="info", version=1.6, spot=2)),
    ("Flexography", "flexographic printing for labels, corrugated and flexible packaging",
     dict(policy="cmyk_spot", ppi=300, bmp=1200, maxppi=450, tac=320, transparency="info", version=1.6, spot=4, hair=0.25, warn=0.5, text=6, multi=9)),
    ("Screen printing", "screen printing on textiles, signage and rigid materials",
     dict(policy="cmyk_spot", ppi=200, bmp=600, maxppi=300, tac=300, transparency="warning", version=1.6, spot=6, hair=0.5, warn=1.0, text=8, multi=12)),
    ("Digital print, toner", "toner-based digital presses for short runs",
     dict(policy="cmyk_spot", ppi=300, bmp=1200, maxppi=450, tac=320, transparency="info", version=1.6, spot=1)),
    ("Digital print, inkjet", "production inkjet presses",
     dict(policy="cmyk_spot", ppi=300, bmp=1200, maxppi=450, tac=320, transparency="info", version=1.6, spot=1)),
    ("Large format", "large-format inkjet for posters, banners and displays",
     dict(policy="cmyk_spot", ppi=150, bmp=600, maxppi=300, tac=300, transparency="info", version=1.6, spot=2, text=8, multi=12, hair=0.25, warn=0.5)),
    ("Packaging, CMYK", "folding cartons and packaging in process colours",
     dict(policy="cmyk", ppi=300, bmp=1200, maxppi=450, tac=320, transparency="info", version=1.6, spot=0)),
    ("Packaging with spot colours", "packaging with brand spot inks alongside CMYK",
     dict(policy="cmyk_spot", ppi=300, bmp=1200, maxppi=450, tac=320, transparency="info", version=1.6, spot=6)),
    ("Labels", "label presses, often with spot inks and varnishes",
     dict(policy="cmyk_spot", ppi=300, bmp=1200, maxppi=450, tac=320, transparency="info", version=1.6, spot=6)),
    ("Book, text pages", "single-colour text pages for book interiors",
     dict(policy="black", ppi=300, bmp=1200, maxppi=450, tac=100, transparency="info", version=1.6, spot=0)),
]


def workflow_checks(w):
    hair = w.get("hair", 0.125)
    warn = w.get("warn", 0.25)
    text = w.get("text", 5)
    multi = w.get("multi", 8)
    checks = hairline_checks(hair, warn) + small_text_checks(text, multi) + ink_checks(w["tac"]) \
        + overprint_checks() + transparency_checks(w["transparency"])
    builtins = image_builtins(w["ppi"], w["bmp"], w["maxppi"]) + document_builtins(w["version"])
    for item in colour_builtins(w["policy"], w.get("spot", 2)) + font_checks() + page_checks():
        (checks if "prop" in json.dumps(item) and "all" in item else builtins).append(item)
    return checks, builtins


for name, what, w in WORKFLOWS:
    checks, builtins = workflow_checks(w)
    profile("press", f"{name}: check",
            f"Checks a file bound for {what}: resolution, ink, hairlines, small text, fonts, colour, pages and document health.",
            checks, builtins)
    profile("press", f"{name}: check and fix",
            f"The same checks for {what}, with the safe repairs applied: trim and bleed boxes, hairlines, black overprint, white knockout, page scaling and transparency groups.",
            checks, builtins, press_fixes(cmyk_blend=w["policy"] != "black"))

profile("press", "Prepress basics", "First-pass checks a prepress operator runs before anything else.",
        hairline_checks() + small_text_checks(4, 8) + [
            check("Image below 250 ppi", "warning", [cond("image.ppi", "<", 250), cond("image.ppi", ">", 0)])])
profile("press", "Digital print quality", "The checks a short-run digital job usually fails: soft images, weak lines, heavy black.",
        hairline_checks(0.125, 0.25) + ink_checks(320) + [
            check("Image below 200 ppi", "error", [cond("image.ppi", "<", 200), cond("image.ppi", ">", 0)]),
            check("Image between 200 and 300 ppi", "warning", [cond("image.ppi", "<", 300), cond("image.ppi", ">=", 200)])])

GWG = [
    ("GWG 2022 sheetfed offset, CMYK", dict(policy="cmyk", ppi=300, bmp=1200, maxppi=450, tac=330, transparency="info", version=1.6, spot=0)),
    ("GWG 2022 sheetfed offset with spot colours", dict(policy="cmyk_spot", ppi=300, bmp=1200, maxppi=450, tac=330, transparency="info", version=1.6, spot=2)),
    ("GWG 2022 sheetfed offset with spot colours, low resolution", dict(policy="cmyk_spot", ppi=150, bmp=600, maxppi=300, tac=330, transparency="info", version=1.6, spot=2)),
    ("GWG 2022 heatset web offset", dict(policy="cmyk", ppi=300, bmp=1200, maxppi=450, tac=300, transparency="info", version=1.6, spot=0)),
    ("GWG 2022 coldset web offset", dict(policy="cmyk", ppi=200, bmp=600, maxppi=300, tac=240, transparency="info", version=1.6, spot=0, text=6, multi=9)),
    ("GWG 2022 gravure", dict(policy="cmyk_spot", ppi=300, bmp=1200, maxppi=450, tac=330, transparency="info", version=1.6, spot=2)),
    ("GWG 2022 screen printing", dict(policy="cmyk_spot", ppi=200, bmp=600, maxppi=300, tac=300, transparency="warning", version=1.6, spot=6, hair=0.5, warn=1.0, text=8, multi=12)),
    ("GWG 2022 digital print", dict(policy="cmyk_spot", ppi=300, bmp=1200, maxppi=450, tac=320, transparency="info", version=1.6, spot=1)),
    ("GWG 2022 large format", dict(policy="cmyk_spot", ppi=150, bmp=600, maxppi=300, tac=300, transparency="info", version=1.6, spot=2, text=8, multi=12, hair=0.25, warn=0.5)),
    ("GWG 2022 packaging, CMYK", dict(policy="cmyk", ppi=300, bmp=1200, maxppi=450, tac=320, transparency="info", version=1.6, spot=0)),
    ("GWG 2022 packaging with spot colours", dict(policy="cmyk_spot", ppi=300, bmp=1200, maxppi=450, tac=320, transparency="info", version=1.6, spot=6)),
    ("GWG 2022 display and screen", dict(policy="rgb", ppi=150, bmp=600, maxppi=300, tac=400, transparency="info", version=1.6, spot=0)),
]
for name, w in GWG:
    checks, builtins = workflow_checks(w)
    builtins = [builtin("conformsTo", "error", level="x4")] + builtins
    profile("gwg", f"{name}: check",
            "Written from the Ghent Workgroup 2022 specification for this workflow: PDF/X-4 conformance plus the workflow's resolution, ink, hairline, text, font, colour and page requirements.",
            checks, builtins)
    profile("gwg", f"{name}: check and fix",
            "The Ghent Workgroup 2022 checks for this workflow, with the safe repairs applied; convert with --level x4 to reach PDF/X-4.",
            checks, builtins, press_fixes(cmyk_blend=w["policy"] != "rgb"))

online_checks = [
    check("Spot colour in use", "warning", [cond("paint.isSpot", "==", True)]),
    check("Registration colour used for content", "error", [cond("paint.isRegistration", "==", True)]),
    check("Text smaller than 6 pt", "info", [cond("text.size", "<", 6), cond("text.size", ">", 0.01)]),
    check("Page scaling in use", "warning", [cond("page.isScaled", "==", True)]),
]
online_builtins = [builtin("imageResolutionAbove", "warning", ppi=150), builtin("bitmapResolutionAbove", "info", ppi=600),
                   builtin("imageResolutionBelow", "info", ppi=72), builtin("fontsNotEmbedded", "error"),
                   builtin("encrypted", "warning"), builtin("damaged", "error"), builtin("syntaxProblems", "error"),
                   builtin("uncompressedImages", "warning")]
profile("online", "Online publishing: check",
        "Checks a file meant for screens and downloads: images above 150 ppi that add weight, fonts, spot colours, encryption and syntax.",
        online_checks, online_builtins)
profile("online", "Online publishing: check and fix",
        "The online checks, then the safe cleanups: page scaling removed, flatness and smoothness dropped, interpolation removed, sRGB blending; convert with --image-max-ppi 150 to downsample.",
        online_checks, online_builtins,
        [fix("removepagescaling"), fix("removeflatness"), fix("removesmoothness"),
         fix("settransparencyblendcs", "sRGB"), fix("setinitialviewdocumentoptions", "UseNone", "SinglePage")])
profile("online", "Email attachment: check", "Everything in the online check plus a 5 MB size limit.",
        online_checks + [check("File larger than 5 MB", "error", [cond("doc.fileSizeBytes", ">", 5 * 1024 * 1024)])], online_builtins)
profile("online", "Screen viewing at 96 ppi: report", "Reports images above 96 ppi, the point past which a screen shows no more detail.",
        [], [builtin("imageResolutionAbove", "info", ppi=96)])
profile("online", "Mobile: check", "The online check with a 2 MB size limit and 100 pages, for files opened on phones.",
        online_checks + [check("File larger than 2 MB", "warning", [cond("doc.fileSizeBytes", ">", 2 * 1024 * 1024)]),
                         check("More than 100 pages", "info", [cond("doc.pages", ">", 100)])], online_builtins)

LEVELS = {"1b": "PDF/A-1b", "1a": "PDF/A-1a", "2b": "PDF/A-2b", "2u": "PDF/A-2u", "2a": "PDF/A-2a",
          "3b": "PDF/A-3b", "3u": "PDF/A-3u", "3a": "PDF/A-3a", "4": "PDF/A-4", "4f": "PDF/A-4f", "4e": "PDF/A-4e"}
for lv, label in LEVELS.items():
    profile("archive", f"Check {label}", f"Reports every deviation from {label}; convert with --level {lv} to fix them.",
            [], [builtin("conformsTo", "error", level=lv)])
profile("archive", "Embedded files conform to PDF/A-2b", "Checks that every embedded PDF is itself PDF/A-2b, as a PDF/A-2 container requires.",
        [], [builtin("embeddedFilesConformTo", "error", level="2b")])
profile("archive", "Archive readiness: report", "The things that block archiving: encryption, damage, fonts not embedded, invisible text, layers and JavaScript-bearing annotations.",
        [check("Invisible text", "info", [cond("text.isInvisible", "==", True)]),
         check("Content on a layer", "warning", [cond("layers.onLayer", "==", True)]),
         check("Annotation with opacity", "info", [cond("annot.hasOpacity", "==", True)])],
        [builtin("encrypted", "error"), builtin("damaged", "error"), builtin("fontsNotEmbedded", "error"),
         builtin("transparencyUsed", "info")])

profile("accessibility", "Check PDF/A-2a", "Reports every deviation from PDF/A-2a, the tagged archival level; run the CLI with --check --level 2a --ua for the PDF/UA-1 check on top.",
        [], [builtin("conformsTo", "error", level="2a")])
profile("accessibility", "Check PDF/A-4 for accessibility", "Reports every deviation from PDF/A-4; run the CLI with --check --level 4 --ua for the PDF/UA-2 check on top.",
        [], [builtin("conformsTo", "error", level="4")])
profile("accessibility", "Accessibility readiness: report", "What an accessible document needs before tagging: text with Unicode, no invisible text, no text as clipping paths, fonts embedded.",
        [check("Text without a Unicode mapping", "error", [cond("text.hasUnicode", "==", False)]),
         check("Invisible text", "warning", [cond("text.isInvisible", "==", True)]),
         check("Text used as a clipping path", "warning", [cond("text.isClippingPath", "==", True)])],
        [builtin("fontsNotEmbedded", "error"), builtin("pagesDifferInSize", "info")])

XLEVELS = {"x1a": "PDF/X-1a", "x3": "PDF/X-3", "x4": "PDF/X-4", "x4p": "PDF/X-4p", "x5g": "PDF/X-5g", "x5n": "PDF/X-5n",
           "x5pg": "PDF/X-5pg", "x6": "PDF/X-6", "x6n": "PDF/X-6n", "x6p": "PDF/X-6p", "e1": "PDF/E-1",
           "vt1": "PDF/VT-1", "vt2": "PDF/VT-2", "vt3": "PDF/VT-3"}
for lv, label in XLEVELS.items():
    hint = "validated only; it references an external press profile" if lv in ("x4p", "x5g", "x5n", "x5pg", "x6n", "x6p", "vt2") else f"convert with --level {lv} to fix them"
    profile("standards", f"Check {label}", f"Reports every deviation from {label}; {hint}.",
            [], [builtin("conformsTo", "error", level=lv)])

for n in (72, 96, 150, 200, 250, 300, 350, 400):
    profile("images", f"Images below {n} ppi", f"Reports colour and grayscale images whose effective resolution is below {n} ppi.",
            [], [builtin("imageResolutionBelow", "warning", ppi=n)])
for n in (300, 600, 800, 1000, 1200):
    profile("images", f"Bitmaps below {n} ppi", f"Reports 1-bit images whose effective resolution is below {n} ppi.",
            [], [builtin("bitmapResolutionBelow", "warning", ppi=n)])
for n in (300, 450, 600, 800):
    profile("images", f"Images above {n} ppi", f"Reports images above {n} ppi, candidates for downsampling with --image-max-ppi.",
            [], [builtin("imageResolutionAbove", "info", ppi=n)])
for n in (1200, 2400):
    profile("images", f"Bitmaps above {n} ppi", f"Reports 1-bit images above {n} ppi.", [], [builtin("bitmapResolutionAbove", "info", ppi=n)])
for label, needle in (("JPEG", "DCT"), ("JPEG 2000", "JPX"), ("CCITT fax", "CCITT"), ("JBIG2", "JBIG2"), ("Flate", "Flate"), ("LZW", "LZW"), ("run-length", "RunLength")):
    profile("images", f"{label}-compressed images", f"Reports images stored with {label} compression.",
            [check(f"{label}-compressed image", "info", [cond("image.filter", "contains", needle)])])
profile("images", "Uncompressed images", "Reports images stored without any compression.", [], [builtin("uncompressedImages", "warning")])
profile("images", "16-bit images", "Reports images with 16 bits per component, which many workflows cannot take.",
        [check("16-bit image", "warning", [cond("image.bitsPerComponent", "==", 16)])])
profile("images", "1-bit images", "Reports bitmap images and image masks.", [check("1-bit image", "info", [cond("image.bitsPerComponent", "==", 1)])])
profile("images", "Images with soft masks", "Reports images carrying a soft mask, a form of transparency.",
        [check("Image with a soft mask", "info", [cond("image.hasSoftMask", "==", True)])])
profile("images", "Interpolated images", "Reports images with the interpolation flag, which renderers treat differently.",
        [check("Interpolated image", "info", [cond("image.interpolate", "==", True)])])
for n in (2000, 5000, 10000):
    profile("images", f"Images larger than {n} pixels", f"Reports images wider or taller than {n} pixels.",
            [check(f"Image wider than {n} px", "info", [cond("image.width", ">", n)]),
             check(f"Image taller than {n} px", "info", [cond("image.height", ">", n)])])
profile("images", "Invalid images", "Reports images the renderer cannot decode.", [check("Invalid image", "error", [cond("image.invalid", "==", True)])])
profile("images", "Image masks", "Reports stencil masks painted with the current colour.", [check("Image mask", "info", [cond("content.isImageMask", "==", True)])])
profile("images", "Image formats: report", "One report of every compression filter and bit depth in use.",
        [check(f"{label}-compressed image", "info", [cond("image.filter", "contains", needle)]) for label, needle in (("JPEG", "DCT"), ("JPEG 2000", "JPX"), ("CCITT fax", "CCITT"), ("JBIG2", "JBIG2"))] +
        [check("16-bit image", "info", [cond("image.bitsPerComponent", "==", 16)]), check("1-bit image", "info", [cond("image.bitsPerComponent", "==", 1)])])

profile("colour", "RGB in use", "Reports every object painted in RGB.", [], [builtin("rgbUsed", "warning")])
profile("colour", "Device-independent colour in use", "Reports Lab, calibrated and ICC-based colour.", [], [builtin("deviceIndependentColour", "info")])
profile("colour", "Colour plates used", "Reports every object that produces output on the cyan, magenta or yellow plates; useful for jobs meant to be black only.", [], [builtin("colourPlatesUsed", "warning")])
for label, prop in (("Lab colour", "paint.isLab"), ("Calibrated colour spaces", "paint.isCalibrated"), ("ICC-based colour", "paint.isIccBased"),
                    ("DeviceN colour", "paint.deviceNColorants"), ("Separation colour", "paint.isSeparation"), ("Pattern fills", "paint.isPattern"),
                    ("ICC-based CMYK", "paint.usesIccCmyk"), ("ICC-based RGB", "paint.usesIccRgb"), ("Process colour defined as a spot", "paint.processColourAsSpot"),
                    ("Process colours defined as DeviceN", "paint.processColoursAsDeviceN"), ("Spot colours in use", "paint.isSpot")):
    op = ">" if prop == "paint.deviceNColorants" else "=="
    val = 0 if prop == "paint.deviceNColorants" else True
    profile("colour", label, f"Reports every object painted with {label.lower()}.", [check(label, "info", [cond(prop, op, val)])])
for n in (1, 2, 3, 4, 6, 8):
    profile("colour", f"More than {n} spot colour{'s' if n > 1 else ''}", f"Reports pages that use more than {n} spot colour{'s' if n > 1 else ''}.",
            [], [builtin("spotColoursMoreThan", "warning", count=n)])
profile("colour", "Registration colour in use", "Reports content painted in the registration colour, which prints on every plate.",
        [check("Registration colour used for content", "error", [cond("paint.isRegistration", "==", True)])])
profile("colour", "Pantone-suffixed spot names", "Reports spot colours whose names carry a Pantone suffix, which often signals duplicates.",
        [check("Spot name with a Pantone suffix", "info", [cond("paint.spotNameHasPantoneSuffix", "==", True)])])
profile("colour", "Spot names that differ only in spelling", "Reports spot colours that are the same ink under two names.",
        [check("Equivalent spot names", "warning", [cond("doc.spotNamesEquivalent", "==", True)])], [builtin("spotNamesInconsistent", "warning")])
profile("colour", "Inconsistent spot definitions", "Reports a spot colour defined with different alternate values in different places.",
        [check("Inconsistent spot representation", "warning", [cond("doc.spotRepresentationsInconsistent", "==", True)])])
for n in (0, 200, 240, 280):
    label = "Rich black" if n == 0 else f"Rich black above {n}% total"
    profile("colour", label, "Reports black objects that also carry cyan, magenta or yellow." if n == 0 else f"Reports black objects whose added CMY brings total ink above {n}%.",
            [check(label, "warning", [cond("paint.richBlackCmyPercent", ">", 0)] + ([cond("fill.totalInk", ">", n)] if n else []))])
profile("colour", "Rich black on text", "Reports text painted in rich black, a registration risk at small sizes.",
        [check("Rich black text", "warning", [cond("content.isText", "==", True), cond("paint.richBlackCmyPercent", ">", 0)])])
for n in (240, 260, 280, 300, 320, 340, 360):
    profile("colour", f"Total ink above {n}%", f"Reports fills and strokes whose total area coverage exceeds {n}%.",
            [check(f"Total ink above {n}% in fills", "error", [cond("fill.totalInk", ">", n)]),
             check(f"Total ink above {n}% in strokes", "error", [cond("stroke.totalInk", ">", n)])])
profile("colour", "White objects set to overprint", "Reports white objects that overprint and would vanish on press.",
        [check("White object set to overprint", "error", [cond("paint.isWhite", "==", True), cond("fill.overprint", "==", True)])])
profile("colour", "Black text not set to overprint", "Reports 100% black text that knocks out, a registration risk.",
        [check("Black text not set to overprint", "warning", [cond("content.isText", "==", True), cond("paint.is100Black", "==", True), cond("fill.overprint", "==", False)])])
profile("colour", "Overprinting objects", "Reports every object with overprint switched on.",
        [check("Fill set to overprint", "info", [cond("fill.overprint", "==", True)]), check("Stroke set to overprint", "info", [cond("stroke.overprint", "==", True)])])
profile("colour", "Illustrator overprint mode", "Reports graphics states using overprint mode 1.",
        [check("Overprint mode 1", "info", [cond("gstate.overprintModeIllustrator", "==", True)])])
profile("colour", "100% black objects", "Reports objects painted in pure black.", [check("100% black object", "info", [cond("paint.is100Black", "==", True)])])
profile("colour", "Objects using black only", "Reports objects that print on the black plate alone.", [check("Black-only object", "info", [cond("paint.isBlackOnly", "==", True)])])
profile("colour", "CMYK-only objects", "Reports objects painted in process colours without spots.", [check("CMYK-only object", "info", [cond("paint.cmykOnly", "==", True)])])
profile("colour", "Spot-only objects", "Reports objects painted in spot colours without process inks.", [check("Spot-only object", "info", [cond("paint.spotOnly", "==", True)])])
profile("colour", "Gray objects", "Reports objects painted in DeviceGray.", [check("DeviceGray object", "info", [cond("paint.isGray", "==", True)])])
profile("colour", "Colour usage: report", "One report of RGB, Lab, ICC, spot, DeviceN, pattern and registration colour in use.",
        [check("RGB object", "info", [cond("paint.isRgb", "==", True)]), check("Lab object", "info", [cond("paint.isLab", "==", True)]),
         check("ICC-based object", "info", [cond("paint.isIccBased", "==", True)]), check("Spot object", "info", [cond("paint.isSpot", "==", True)]),
         check("DeviceN object", "info", [cond("paint.deviceNColorants", ">", 0)]), check("Pattern fill", "info", [cond("paint.isPattern", "==", True)]),
         check("Registration colour", "warning", [cond("paint.isRegistration", "==", True)])])

for n in (0.1, 0.125, 0.25, 0.5):
    profile("objects", f"Hairlines below {n} pt", f"Reports strokes thinner than {n} pt.",
            [check(f"Stroke thinner than {n} pt", "error", [cond("stroke.width", "<=", n)])])
profile("objects", "Report hairlines", "Finds strokes too thin to print reliably, graded by rendered width.", hairline_checks() + [
    check("Stroke between 0.25 and 0.5 pt", "info", [cond("stroke.width", "<=", 0.5), cond("stroke.width", ">", 0.25)])])
profile("objects", "Thin strokes in more than one ink", "Reports strokes below 0.25 pt built from several inks, which misregister.",
        [check("Thin stroke in more than one ink", "error", [cond("stroke.width", "<=", 0.25), cond("paint.inkCount", ">", 1)])])
for n in (4, 5, 6, 7, 8, 9, 10):
    profile("objects", f"Text below {n} pt", f"Reports text smaller than {n} pt.",
            [check(f"Text smaller than {n} pt", "warning", [cond("text.size", "<", n), cond("text.size", ">", 0.01)])])
for n in (8, 9, 10, 12):
    profile("objects", f"Small text below {n} pt in more than one ink", f"Reports text under {n} pt built from several inks.",
            [check(f"Text smaller than {n} pt in more than one ink", "warning", [cond("text.size", "<", n), cond("text.size", ">", 0.01), cond("paint.inkCount", ">", 1)])])
    profile("objects", f"White text below {n} pt", f"Reports knockout white text under {n} pt, which fills in on press.",
            [check(f"White text smaller than {n} pt", "warning", [cond("text.size", "<", n), cond("text.size", ">", 0.01), cond("paint.isWhite", "==", True)])])
profile("objects", "Report small text", "Reports text at sizes that print or read poorly.", small_text_checks(5, 8))
profile("objects", "Report invisible text", "Finds text drawn in invisible rendering mode, typically OCR layers.",
        [check("Invisible text", "info", [cond("text.isInvisible", "==", True)])])
profile("objects", "Text used as a clipping path", "Reports text that clips instead of painting.",
        [check("Text as clipping path", "info", [cond("text.isClippingPath", "==", True)])])
profile("objects", "Stroked text", "Reports text that is stroked rather than filled.", [check("Stroked text", "info", [cond("text.isStroked", "==", True)])])
profile("objects", "Text render modes: report", "Reports every text rendering mode other than plain fill.",
        [check("Text render mode other than fill", "info", [cond("text.renderMode", "!=", 0)])])
profile("objects", "Report white objects", "Finds white fills and strokes, which either knock out or vanish when overprinting.",
        [check("White fill", "info", [cond("paint.isWhite", "==", True), cond("content.isFilled", "==", True)]),
         check("White stroke", "info", [cond("paint.isWhite", "==", True), cond("content.isStroked", "==", True)]),
         check("White object set to overprint", "error", [cond("paint.isWhite", "==", True), cond("fill.overprint", "==", True)])])
profile("objects", "Report rich black", "Finds black objects that also carry cyan, magenta or yellow.",
        [check("Rich black object", "info", [cond("paint.richBlackCmyPercent", ">", 0)]),
         check("Rich black text below 12 pt", "warning", [cond("content.isText", "==", True), cond("text.size", "<", 12), cond("paint.richBlackCmyPercent", ">", 0)])])
profile("objects", "Transparency in use", "Reports every object drawn with transparency.", transparency_checks("info"))
profile("objects", "Transparency: report", "Blend modes, soft masks, constant alpha and transparency groups, each reported separately.",
        [check("Blend mode other than Normal", "info", [cond("gstate.blendMode", "!=", "Normal"), cond("gstate.blendMode", "!=", "Compatible")]),
         check("Soft mask in use", "info", [cond("gstate.hasSoftMask", "==", True)]),
         check("Fill alpha below 100%", "info", [cond("fill.alpha", "<", 1)]),
         check("Stroke alpha below 100%", "info", [cond("stroke.alpha", "<", 1)]),
         check("Object inside a transparency group", "info", [cond("gstate.inTransparencyGroup", "==", True)])])
profile("objects", "Blend modes other than Normal", "Reports objects using Multiply, Screen, Overlay and the other blend modes.",
        [check("Blend mode other than Normal", "info", [cond("gstate.blendMode", "!=", "Normal"), cond("gstate.blendMode", "!=", "Compatible")])])
profile("objects", "Soft masks", "Reports objects drawn through a soft mask.", [check("Soft mask in use", "info", [cond("gstate.hasSoftMask", "==", True)])])
for n in (50, 90, 100):
    label = f"Constant alpha below {n}%"
    profile("objects", label, f"Reports fills and strokes with opacity below {n}%.",
            [check(f"Fill alpha below {n}%", "info", [cond("fill.alpha", "<", n / 100)]), check(f"Stroke alpha below {n}%", "info", [cond("stroke.alpha", "<", n / 100)])])
profile("objects", "Flatness set", "Reports graphics states that set a flatness tolerance, which changes curve rendering.",
        [check("Flatness set", "info", [cond("gstate.flatness", ">", 0)])])
profile("objects", "Black point compensation set", "Reports graphics states that switch black point compensation on.",
        [check("Black point compensation", "info", [cond("gstate.hasBlackPointCompensation", "==", True)])])
for n in (1000, 5000, 10000):
    profile("objects", f"Paths with more than {n} nodes", f"Reports vector paths with more than {n} nodes, which slow rendering and RIPs.",
            [check(f"Path with more than {n} nodes", "warning", [cond("content.pathNodes", ">", n)])])
profile("objects", "Empty vector objects", "Reports paths that neither fill nor stroke.", [check("Empty vector object", "info", [cond("content.emptyVector", "==", True)])])
profile("objects", "Objects outside the media box", "Reports content entirely outside the page.", [check("Object outside the media box", "warning", [cond("content.outsideMediaBox", "==", True)])])
profile("objects", "Objects outside the bleed box", "Reports content beyond the bleed box.", [check("Object outside the bleed box", "info", [cond("content.outsideBleedBox", "==", True)])])
for n in (2, 3, 5):
    profile("objects", f"Objects within {n} mm of the trim edge", f"Reports content closer than {n} mm to the trim edge, the usual safety margin.",
            [check(f"Object within {n} mm of the trim edge", "info", [cond("content.distanceInsideTrimBox", "<", mm(n))], scope="trim")])
profile("objects", "Shadings in use", "Reports smooth shades and gradients.", [check("Shading", "info", [cond("content.isShading", "==", True)])])
profile("objects", "Line art only: report", "Reports strokes that carry no fill.", [check("Stroked, not filled", "info", [cond("content.isStrokedOnly", "==", True)])])
profile("objects", "Unknown operators", "Reports content stream operators no PDF version defines.", [check("Unknown operator", "error", [cond("content.unknownOperator", "==", True)])])
profile("objects", "Overprint and knockout: report", "Reports overprinting fills and strokes plus white objects set to overprint.", overprint_checks() + [
    check("Fill set to overprint", "info", [cond("fill.overprint", "==", True)]), check("Stroke set to overprint", "info", [cond("stroke.overprint", "==", True)])])

profile("pages", "Pages of different sizes", "Reports documents whose pages differ in size or orientation.", [], [builtin("pagesDifferInSize", "warning")])
profile("pages", "Empty pages", "Reports pages with no visible content.", [], [builtin("emptyPage", "warning")])
profile("pages", "Rotated pages", "Reports pages with a rotation attribute.", [check("Rotated page", "info", [cond("page.isRotated", "==", True)])])
profile("pages", "Pages without a crop box", "Reports pages that rely on the media box alone.", [check("No crop box", "info", [cond("page.hasCropBox", "==", False)])])
profile("pages", "Crop box equals media box", "Reports pages whose crop box adds nothing.", [check("Crop box equals media box", "info", [cond("page.cropEqualsMedia", "==", True)])])
profile("pages", "Single-image pages", "Reports pages made of one image, typically scans.", [check("Single-image page", "info", [cond("page.singleImage", "==", True)])])
profile("pages", "Uncompressed page content", "Reports page content streams stored without compression.", [check("Uncompressed content stream", "info", [cond("page.contentCompressed", "==", False)])])
profile("pages", "Page-level output intents", "Reports pages that declare their own output intent (PDF/X-6).", [check("Page-level output intent", "info", [cond("page.hasOutputIntent", "==", True)])])
for n in (1, 2, 4, 10, 50, 100, 500, 1000):
    profile("pages", f"More than {n} page{'s' if n > 1 else ''}", f"Reports documents with more than {n} page{'s' if n > 1 else ''}.",
            [check(f"More than {n} pages", "info", [cond("doc.pages", ">", n)])])
profile("pages", "Set trim box from crop box", "Adds a trim box equal to the crop box on pages that lack one.",
        [], [], [fix("setpagebox", "TrimBox", "RelativeToCropBox", 0, 0, 0, 0, "pt")])
profile("pages", "Set trim box from media box", "Adds a trim box equal to the media box on pages that lack one.",
        [], [], [fix("setpagebox", "TrimBox", "RelativeToMediaBox", 0, 0, 0, 0, "pt")])
for n in (3, 5):
    profile("pages", f"Set trim box {n} mm inside the media box", f"Adds a trim box inset {n} mm from the media box on pages that lack one, for files delivered with bleed but no boxes.",
            [], [], [fix("setpagebox", "TrimBox", "RelativeToMediaBox", n, n, n, n, "mm")])
profile("pages", "Reset trim box to crop box", "Replaces the trim box with the crop box on every page.",
        [], [], [fix("setpagebox", "TrimBox", "RelativeToCropBox", 0, 0, 0, 0, "pt", "Always")])
profile("pages", "Set art box from trim box", "Adds an art box equal to the trim box where missing.",
        [], [], [fix("setpagebox", "ArtBox", "RelativeToTrimBox", 0, 0, 0, 0, "pt")])
profile("pages", "Set crop box from media box", "Adds a crop box equal to the media box where missing.",
        [], [], [fix("setpagebox", "CropBox", "RelativeToMediaBox", 0, 0, 0, 0, "pt")])
for n in (1, 2, 3, 5):
    profile("pages", f"Add {n} mm bleed box", f"Sets a bleed box {n} mm outside the trim box, limited by the media box.",
            [], [], [fix("generatebleed", "Amount", n, "mm")])
profile("pages", "Clip content to the media box", "Removes anything drawn outside the media box.", [], [], [fix("removeobjectsoutofbox", "MediaBox")])
profile("pages", "Clip content to the bleed box", "Removes anything drawn outside the bleed box.", [], [], [fix("removeobjectsoutofbox", "BleedBox")])
profile("pages", "Clip content to the trim box", "Removes anything drawn outside the trim box.", [], [], [fix("removeobjectsoutofbox", "TrimBox")])
for ang in (90, 180, 270):
    profile("pages", f"Rotate pages {ang} degrees", f"Rotates every page by {ang} degrees clockwise.", [], [], [fix("rotatepages", ang)])
for label, w, h, unit in (("A4", 210, 297, "mm"), ("A3", 297, 420, "mm"), ("A5", 148, 210, "mm"), ("US Letter", 8.5, 11, "inch"), ("US Legal", 8.5, 14, "inch"), ("Tabloid", 11, 17, "inch")):
    profile("pages", f"Scale pages to {label}", f"Scales every page proportionally to fit {label} ({w} x {h} {unit}).", [], [], [fix("scalepagesex", w, h, unit)])
profile("pages", "Remove page scaling", "Removes the user unit so pages output at their nominal size.", [], [], [fix("removepagescaling")])

for n in (1, 2, 5, 10, 20, 50, 100):
    profile("document", f"File larger than {n} MB", f"Reports files above {n} MB.", [check(f"File larger than {n} MB", "warning", [cond("doc.fileSizeBytes", ">", n * 1024 * 1024)])])
for v in (1.4, 1.5, 1.6, 1.7, 2.0):
    profile("document", f"PDF version below {v}", f"Reports files written against a PDF version older than {v}.", [], [builtin("pdfVersionBelow", "info", version=v)])
profile("document", "PDF 2.0 required", "Reports files whose requirements say they need a PDF 2.0 reader.", [check("Requires PDF 2.0", "info", [cond("doc.requiresPdf20", "==", True)])])
profile("document", "Encrypted document", "Reports files protected by encryption.", [], [builtin("encrypted", "warning")])
profile("document", "Damaged document", "Reports files the parser had to repair.", [], [builtin("damaged", "error")])
profile("document", "Syntax problems", "Reports structural problems found while parsing.", [], [builtin("syntaxProblems", "error")])
profile("document", "Data after end of file", "Reports bytes after the final %%EOF marker.", [check("Data after end of file", "info", [cond("doc.dataAfterEof", "==", True)])])
profile("document", "Invalid hex strings", "Reports hexadecimal strings with characters outside 0-9 and A-F.", [check("Invalid hex string", "warning", [cond("doc.hexStringInvalid", "==", True)])])
profile("document", "XMP metadata not plain text", "Reports XMP packets that are compressed or encoded, which PDF/A forbids.", [check("XMP not plain text", "warning", [cond("doc.xmpIsPlainText", "==", False)])])
profile("document", "Signature fields present", "Reports documents that carry signature fields.", [check("Signature field", "info", [cond("signature.hasFields", "==", True)])])
profile("document", "Document health: check", "Encryption, damage, syntax, data after the end marker and hex strings, in one run.",
        [check("Data after end of file", "info", [cond("doc.dataAfterEof", "==", True)]), check("Invalid hex string", "warning", [cond("doc.hexStringInvalid", "==", True)])],
        [builtin("encrypted", "warning"), builtin("damaged", "error"), builtin("syntaxProblems", "error")])
profile("document", "Annotations that print", "Reports annotations flagged to appear in print.", [check("Annotation that prints", "warning", [cond("annot.prints", "==", True)])])
for t in ("Link", "Widget", "FileAttachment", "Stamp", "Text", "Highlight", "FreeText", "3D", "RichMedia", "Movie", "Sound", "Screen", "Watermark", "Redact"):
    profile("document", f"{t} annotations", f"Reports annotations of type {t}.", [check(f"{t} annotation", "info", [cond("annot.type", "==", t)])])
profile("document", "Annotations with opacity", "Reports annotations drawn with reduced opacity.", [check("Annotation with opacity", "info", [cond("annot.hasOpacity", "==", True)])])
profile("document", "Annotations of unknown type", "Reports annotation types the PDF specification does not define.", [check("Unknown annotation type", "warning", [cond("annot.unknownType", "==", True)])])
profile("document", "Annotations outside the trim area", "Reports annotations lying outside the bleed and trim boxes.", [check("Annotation outside bleed and trim", "info", [cond("annot.insideBleedOrTrim", "==", False)])])
profile("document", "Layers in use", "Reports content placed on optional content layers.", [check("Content on a layer", "info", [cond("layers.onLayer", "==", True)])])
profile("document", "Hidden layers", "Reports content on layers that are switched off.", [check("Content on a hidden layer", "warning", [cond("layers.onLayer", "==", True), cond("layers.visible", "==", False)])])
profile("document", "Layer configurations", "Reports documents with alternate layer configurations.", [check("Layer configurations present", "info", [cond("layers.hasConfigs", "==", True)])])
profile("document", "Processing steps layers", "Reports processing-steps metadata (ISO 19593) on layers, used in packaging.", [check("Processing steps present", "info", [cond("layers.hasProcessingSteps", "==", True)])])
profile("document", "Trapped key not set", "Reports files that do not say whether they are trapped, which PDF/X requires.", [check("Trapped key missing", "warning", [cond("docinfo.trapped", "!=", "True"), cond("docinfo.trapped", "!=", "False")])])
profile("document", "PDF/X info fields present", "Reports files carrying the PDF/X identification fields.", [check("PDF/X fields", "info", [cond("docinfo.hasPdfxFields", "==", True)])])
profile("document", "No output intent", "Reports files without an output intent, which every print standard needs.", [check("No output intent", "warning", [cond("outputIntent.count", "==", 0)])])
profile("document", "Output intent without a profile", "Reports output intents that name a condition but embed no ICC profile.", [check("Output intent without profile", "warning", [cond("outputIntent.hasProfile", "==", False), cond("outputIntent.count", ">", 0)])])
profile("document", "Several output intents", "Reports files with more than one output intent.", [check("More than one output intent", "info", [cond("outputIntent.count", ">", 1)])])
profile("document", "PDF/X output intent", "Reports files carrying a PDF/X output intent.", [check("PDF/X output intent", "info", [cond("outputIntent.isPdfx", "==", True)])])
profile("document", "PDF/A output intent", "Reports files carrying a PDF/A output intent.", [check("PDF/A output intent", "info", [cond("outputIntent.isPdfa", "==", True)])])
profile("document", "Fonts not embedded", "Reports every font the file does not carry.", [], [builtin("fontsNotEmbedded", "error")])
profile("document", "Fonts: report", "Embedding, subsetting, type, validity and Unicode coverage of every font.",
        [check("Font not embedded", "error", [cond("font.notEmbedded", "==", True)]), check("Type 3 font", "info", [cond("font.isType3", "==", True)]),
         check("TrueType font", "info", [cond("font.isTrueType", "==", True)]), check("CID font", "info", [cond("font.isCid", "==", True)]),
         check("Font not subset", "info", [cond("font.notSubset", "==", True)]), check("Subset missing glyphs in use", "error", [cond("font.subsetComplete", "==", False)]),
         check("Text without Unicode", "warning", [cond("font.unicodeComplete", "==", False)]), check("Invalid font program", "error", [cond("font.invalid", "==", True)]),
         check("Glyph falls back to .notdef", "error", [cond("font.notdefUsed", "==", True)])])
for label, prop in (("Type 3 fonts", "font.isType3"), ("TrueType fonts", "font.isTrueType"), ("CID fonts", "font.isCid"), ("Fonts with restricted licences", "font.restrictedLicense"),
                    ("Bitmap-only fonts", "font.bitmapOnly"), ("Fonts not subset", "font.notSubset"), ("Glyphs falling back to .notdef", "font.notdefUsed"), ("Invalid font programs", "font.invalid")):
    profile("document", label, f"Reports {label.lower()}.", [check(label, "warning" if prop in ("font.notdefUsed", "font.invalid") else "info", [cond(prop, "==", True)])])
profile("document", "Fonts missing glyphs", "Reports subset fonts that lack glyphs the text uses.", [check("Subset missing glyphs", "error", [cond("font.subsetComplete", "==", False)])])
profile("document", "Text without Unicode mapping", "Reports fonts whose text cannot be mapped to Unicode, so it cannot be searched or read aloud.", [check("Text without Unicode", "warning", [cond("font.unicodeComplete", "==", False)])])
profile("document", "Font widths mismatch", "Reports fonts whose declared widths disagree with the embedded program.", [check("Width mismatch", "warning", [cond("font.widthsMatch", "==", False)])])
for label, name in (("Type 1 CID fonts", "type1CidFonts"), ("TrueType CID fonts", "trueTypeCidFonts"), ("OpenType fonts", "openTypeFonts")):
    profile("document", label, f"Reports {label.lower()} in use.", [], [builtin(name, "info")])
profile("document", "Transfer curves", "Reports transfer functions, which PDF/X forbids.", [], [builtin("transferCurves", "warning")])
profile("document", "Halftones", "Reports custom halftone settings.", [], [builtin("halftones", "info")])
profile("document", "PostScript fragments", "Reports PostScript XObjects, which most renderers ignore.", [], [builtin("postscript", "warning")])
profile("document", "Halftone origin set", "Reports halftone dictionaries that fix a screen origin.", [check("Halftone origin", "info", [cond("halftone.hasOrigin", "==", True)])])
profile("document", "ICC profile versions", "Reports ICC profiles of version 4 or later, which older RIPs reject.", [check("ICC version 4 or later", "info", [cond("icc.version", ">=", 4)])])
profile("document", "PDF/VT document parts", "Reports the document-part hierarchy PDF/VT builds.", [check("Document parts present", "info", [cond("vt.hasDocumentParts", "==", True)])])

for ang in (90, 180, 270):
    profile("actions", f"Rotate all pages {ang} degrees", f"Rotates every page by {ang} degrees.", [], [], [fix("rotatepages", ang)])
profile("actions", "Set trapped flag to false", "Declares the file untrapped.", [], [], [fix("trappedkey", "false")])
profile("actions", "Set trapped flag to true", "Declares the file trapped.", [], [], [fix("trappedkey", "true")])
for ri in ("RelativeColorimetric", "Perceptual", "Saturation", "AbsoluteColorimetric"):
    profile("actions", f"Set rendering intent to {ri}", f"Sets every graphics state's rendering intent to {ri}.", [], [], [fix("setrenderingintent", ri)])
profile("actions", "Remove rendering intents", "Removes rendering intents so the output device decides.", [], [], [fix("removerenderingintents")])
profile("actions", "Blend transparency in CMYK", "Sets a CMYK press profile as the page blending colour space.", [], [], [fix("settransparencyblendcs", "CMYK")])
profile("actions", "Blend transparency in sRGB", "Sets sRGB as the page blending colour space.", [], [], [fix("settransparencyblendcs", "sRGB")])
profile("actions", "Remove image interpolation", "Removes the interpolation flag from every image.", [], [], [fix("modifyinterpolateentry", "Remove")])
profile("actions", "Remove flatness", "Removes flatness tolerances from graphics states.", [], [], [fix("removeflatness")])
profile("actions", "Remove smoothness", "Removes smoothness tolerances from graphics states.", [], [], [fix("removesmoothness")])
profile("actions", "Remove transfer curves", "Removes transfer functions.", [], [], [fix("transfercurves")])
profile("actions", "Remove black generation", "Removes black-generation functions.", [], [], [fix("removebg")])
profile("actions", "Remove undercolour removal", "Removes undercolour-removal functions.", [], [], [fix("removeucr")])
profile("actions", "Remove unnecessary transparency groups", "Drops page transparency groups that contain no transparent objects.", [], [], [fix("removeunnecessarytransparencygroups")])
profile("actions", "Merge spot colour names", "Merges spot colours whose names differ only in case, spacing or suffix.", [], [], [fix("mergespotcolornames")])
profile("actions", "Make spot colour definitions consistent", "Gives every use of a spot colour the same definition.", [], [], [fix("makecustomspotcolornamesconsistent")])
profile("actions", "Registration colour to black", "Repaints registration-colour content in black.", [], [], [fix("convertregistrationcolortoblack")])
profile("actions", "NChannel to DeviceN", "Converts NChannel colour spaces to plain DeviceN.", [], [], [fix("convertnchtodevn")])
profile("actions", "Black text to overprint", "Sets 100% black text to overprint.", [], [], [fix("overprintblack", "Text")])
profile("actions", "All black objects to overprint", "Sets every 100% black object to overprint.", [], [], [fix("overprintblack", "")])
profile("actions", "White text to knockout", "Switches overprint off for white text.", [], [], [fix("knockoutwhite", "Text")])
profile("actions", "All white objects to knockout", "Switches overprint off for every white object.", [], [], [fix("knockoutwhite", "")])
profile("actions", "Black overprint and white knockout", "The two standard overprint repairs in one step.", [], [], [fix("setoverprintandknockout", "")])
for n in (0.25, 0.3, 0.5):
    profile("actions", f"Thicken hairlines to {n} pt", f"Raises every stroke thinner than {n} pt to {n} pt.", [], [], [fix("increaselinewidth", n, "", "pt")])
profile("actions", "Make invisible text visible", "Sets invisible text to plain fill so it shows and prints.", [], [], [fix("settextrendermode", 0)])
profile("actions", "Stamp the conversion date", "Places the conversion date on every page.", [], [], [fix("placetext", "Date", "", 9)])
profile("actions", "Stamp DRAFT", "Places the word DRAFT on every page.", [], [], [fix("placetext", "DRAFT", "", 36)])
profile("actions", "Stamp CONFIDENTIAL", "Places the word CONFIDENTIAL on every page.", [], [], [fix("placetext", "CONFIDENTIAL", "", 24)])
profile("actions", "Put page content on a layer", "Wraps every page's content in a layer named Content.", [], [], [fix("putobjectsonlayer", "Content", "Content")])
profile("actions", "Mark content as a processing step", "Places page content on a processing-steps layer (ISO 19593).", [], [], [fix("putobjpsteps", "Structural", "Structural")])
profile("actions", "Flatten layers", "Discards hidden layers and merges the visible ones into the page.", [], [], [fix("dscdhdnlycntfltnvsblyrs")])
profile("actions", "Open in single-page view", "Sets the initial view to one page at a time, no panels.", [], [], [fix("setinitialviewdocumentoptions", "UseNone", "SinglePage")])
profile("actions", "Open with bookmarks panel", "Sets the initial view to show the bookmarks panel.", [], [], [fix("setinitialviewdocumentoptions", "UseOutlines", "SinglePage")])
profile("actions", "Open as two-page spread", "Sets the initial view to facing pages with the first page on the right.", [], [], [fix("setinitialviewdocumentoptions", "UseNone", "TwoPageRight")])
profile("actions", "Open in full screen", "Sets the initial view to full-screen presentation.", [], [], [fix("setinitialviewdocumentoptions", "FullScreen", "SinglePage")])
profile("actions", "Set title when missing", "Sets the document title from the file name if none is set.", [], [], [fix("settitle", "IfEmpty", "Untitled")])
profile("actions", "Prepress cleanup", "The safe prepress repairs in one step: trim and bleed boxes, hairlines, overprint, knockout, scaling, transparency groups.", [], [], press_fixes())

REPORTS = [
    ("Report low resolution images", "Reports colour and grayscale images below 300 ppi and bitmaps below 1200 ppi.",
     [], [builtin("imageResolutionBelow", "warning", ppi=300), builtin("bitmapResolutionBelow", "warning", ppi=1200)]),
    ("Report high resolution images", "Reports images above 450 ppi and bitmaps above 2400 ppi, candidates for downsampling.",
     [], [builtin("imageResolutionAbove", "info", ppi=450), builtin("bitmapResolutionAbove", "info", ppi=2400)]),
    ("Report spot colours", "Reports every spot colour object and pages with more than two spot inks.",
     [check("Spot object", "info", [cond("paint.isSpot", "==", True)])], [builtin("spotColoursMoreThan", "warning", count=2)]),
    ("Report overprint", "Reports overprinting fills and strokes and white objects set to overprint.", overprint_checks() + [
        check("Fill set to overprint", "info", [cond("fill.overprint", "==", True)])], []),
    ("Report transparency", "Reports transparency, blend modes and soft masks.", transparency_checks("info") + [
        check("Soft mask in use", "info", [cond("gstate.hasSoftMask", "==", True)])], []),
    ("Report page geometry", "Reports page sizes, rotation, scaling, empty pages and boxes.",
     [check("Rotated page", "info", [cond("page.isRotated", "==", True)]), check("Page scaling in use", "warning", [cond("page.isScaled", "==", True)]),
      check("No crop box", "info", [cond("page.hasCropBox", "==", False)])], [builtin("pagesDifferInSize", "info"), builtin("emptyPage", "info")]),
    ("Report fonts", "Reports fonts not embedded, Type 3 fonts, missing glyphs and text without Unicode.", font_checks()[1:] + [
        check("Text without Unicode", "info", [cond("font.unicodeComplete", "==", False)])], [builtin("fontsNotEmbedded", "warning")]),
    ("Report annotations", "Reports annotations that print, carry opacity or lie outside the trim area.",
     [check("Annotation that prints", "info", [cond("annot.prints", "==", True)]), check("Annotation with opacity", "info", [cond("annot.hasOpacity", "==", True)]),
      check("Annotation outside bleed and trim", "info", [cond("annot.insideBleedOrTrim", "==", False)])], []),
    ("Report layers", "Reports content on layers, hidden layers and layer configurations.",
     [check("Content on a layer", "info", [cond("layers.onLayer", "==", True)]), check("Content on a hidden layer", "info", [cond("layers.onLayer", "==", True), cond("layers.visible", "==", False)]),
      check("Layer configurations present", "info", [cond("layers.hasConfigs", "==", True)])], []),
    ("Report document health", "Reports encryption, damage, syntax problems, data after the end marker and the PDF version.",
     [check("Data after end of file", "info", [cond("doc.dataAfterEof", "==", True)])],
     [builtin("encrypted", "info"), builtin("damaged", "warning"), builtin("syntaxProblems", "warning"), builtin("pdfVersionBelow", "info", version=1.7)]),
    ("Report ink coverage", "Reports fills and strokes above 300% total ink and rich black on text.", ink_checks(300), []),
    ("Report everything", "Every report in one run: images, colour, ink, hairlines, text, transparency, fonts, pages, annotations, layers and document health.",
     hairline_checks() + small_text_checks() + ink_checks(300) + overprint_checks() + transparency_checks("info") + [
         check("RGB object", "info", [cond("paint.isRgb", "==", True)]), check("Spot object", "info", [cond("paint.isSpot", "==", True)]),
         check("Invisible text", "info", [cond("text.isInvisible", "==", True)]), check("Content on a layer", "info", [cond("layers.onLayer", "==", True)]),
         check("Annotation that prints", "info", [cond("annot.prints", "==", True)]), check("Rotated page", "info", [cond("page.isRotated", "==", True)])],
     [builtin("imageResolutionBelow", "warning", ppi=300), builtin("imageResolutionAbove", "info", ppi=450), builtin("fontsNotEmbedded", "error"),
      builtin("spotColoursMoreThan", "info", count=2), builtin("pagesDifferInSize", "info"), builtin("emptyPage", "info"),
      builtin("encrypted", "info"), builtin("damaged", "warning")]),
]
for name, desc, checks, builtins in REPORTS:
    profile("report", name, desc, checks, builtins)
for src, name in (("objects", "Report hairlines"), ("objects", "Report small text"), ("objects", "Report invisible text"),
                  ("objects", "Report white objects"), ("objects", "Report rich black")):
    for folder, sl, body in PROFILES:
        if folder == src and body["name"] == name:
            PROFILES.append(("report", sl, dict(body)))
            break
for name, desc, needle_pairs in (("Report image formats", "Reports the compression filters and bit depths in use.", None),):
    for folder, sl, body in PROFILES:
        if body["name"] == "Image formats: report":
            b = dict(body)
            b["name"] = name
            b["description"] = desc
            PROFILES.append(("report", slug(name), b))
            break


def main():
    for folder in FOLDERS:
        path = os.path.join(ROOT, folder)
        if os.path.isdir(path):
            shutil.rmtree(path)
        os.makedirs(path)
    seen = set()
    for folder, sl, body in PROFILES:
        key = (folder, sl)
        if key in seen:
            raise SystemExit(f"duplicate profile {folder}/{sl}")
        seen.add(key)
        with open(os.path.join(ROOT, folder, sl + ".json"), "w") as fh:
            json.dump(body, fh, indent=2, ensure_ascii=False)
            fh.write("\n")
    counts = {}
    for folder, _, _ in PROFILES:
        counts[folder] = counts.get(folder, 0) + 1
    for folder in FOLDERS:
        print(f"{folder:14s} {counts.get(folder, 0)}")
    print(f"{'total':14s} {len(PROFILES)}")


if __name__ == "__main__":
    main()
