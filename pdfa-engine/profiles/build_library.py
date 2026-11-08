#!/usr/bin/env python3
import json
import os
import re
import shutil

ROOT = os.path.dirname(os.path.abspath(__file__))
FOLDERS = ["report", "press", "gwg", "online", "archive", "accessibility", "standards",
           "images", "colour", "objects", "pages", "document", "actions", "print-conditions",
           "compatibility", "packaging", "colour-conversion", "place-content", "plans"]
PROFILES = []


def c(check_id, op, value, **params):
    d = {"check": check_id, "op": op, "value": value}
    d.update(params)
    return d


def check(name, severity, conds, scope=None):
    out = {"name": name, "severity": severity, "all": conds}
    if scope:
        out["scope"] = scope
    return out


def either(name, severity, alternatives):
    return {"name": name, "severity": severity, "any": [{"all": a} for a in alternatives]}


def fix(fix_id, **params):
    d = {"fix": fix_id}
    d.update(params)
    return d


def slug(name):
    s = re.sub(r"[^a-z0-9]+", "-", name.lower()).strip("-")
    return re.sub(r"-+", "-", s)


def profile(folder, name, description, checks=(), fixes=(), convert=None):
    body = {"kura-profile": 1, "name": name, "description": description}
    if convert:
        body["convert"] = convert
    body["checks"] = list(checks)
    if fixes:
        body["fixes"] = list(fixes)
    PROFILES.append((folder, slug(name), body))


def above(n):
    s = str(n)
    alts = [f"[1-9][0-9]{{{len(s)},}}"]
    for i, d in enumerate(s):
        if d != "9":
            rest = len(s) - i - 1
            alts.append(s[:i] + f"[{int(d) + 1}-9]" + (f"[0-9]{{{rest}}}" if rest else ""))
    return "(" + "|".join(alts) + ")"


TEXT = c("content.kind", "==", "text")
WHITE = c("color.ink_mix", "==", "none")
SOLID_BLACK = c("color.ink_mix", "==", "solid_black")
REGISTRATION = c("color.ink_mix", "==", "registration")
SPOT = c("color.ink_mix", "in", ["spot_only", "spot_and_black", "process_and_spot"])
MULTI_INK = c("color.inks_used", ">", 1)
RICH_BLACK = [c("color.ink", ">=", 100, ink="black"), c("color.ink", ">", 0, ink="cmy_max")]
FILL_OVERPRINT = c("color.overprint", "in", ["on", "on_nonzero_mode"], paint="fill")
STROKE_OVERPRINT = c("color.overprint", "in", ["on", "on_nonzero_mode"], paint="stroke")
FILL_KNOCKOUT = c("color.overprint", "==", "off", paint="fill")
ON_LAYER = c("layer.membership", "count", 0, compare=">")
BLEND_NOT_NORMAL = c("transparency.blend_mode", "not_in", ["Normal", "Compatible"])
SOFT_MASK = c("transparency.used", "is", True, how="soft_mask")
INVISIBLE_TEXT = c("text.rendering", "==", "invisible")
ANNOT_PRINTS = c("annotation.flags", "includes", "print")
ANNOT_OPACITY = c("annotation.entry", "==", "true", entry="has_opacity")
ROTATED = c("page.rotation", "!=", 0)
SCALED = c("page.user_unit", "!=", 1)
NO_CROP_BOX = c("page.box_present", "is", False, box="crop")
DATA_AFTER_EOF = c("document.data_after_eof", "is", True)
NOT_EMBEDDED = c("font.embedding", "==", "not_embedded")
NOTDEF = c("text.glyph", "==", "notdef")
INVALID_FONT = c("font.problem", "includes", "invalid_program")
NO_UNICODE = c("font.all_text_unicode", "is", False)


def text_between(lo, hi):
    return [c("text.size", "<", hi), c("text.size", ">", lo)]


def image_below(n, severity="warning"):
    return check(f"Image below {n} ppi", severity, [c("image.resolution", "<", n, kind="colour_or_grey")])


def bitmap_below(n, severity="warning"):
    return check(f"Bitmap below {n} ppi", severity, [c("image.resolution", "<", n, kind="bitmap")])


def image_above(n, severity="info"):
    return check(f"Image above {n} ppi", severity, [c("image.resolution", ">", n, kind="colour_or_grey")])


def bitmap_above(n, severity="info"):
    return check(f"Bitmap above {n} ppi", severity, [c("image.resolution", ">", n, kind="bitmap")])


def spots_more_than(n, severity="warning"):
    return check(f"More than {n} spot colour{'s' if n != 1 else ''} on a page", severity,
                 [c("print.plates", "count", n, compare=">", kind="spot")])


def rgb_used(severity):
    return either("RGB colour", severity, [[c("color.space", "in", ["DeviceRGB", "CalRGB"])],
                                           [c("color.space", "==", "ICCBased", icc_components=3)]])


def device_independent(severity):
    return check("Device-independent colour", severity, [c("color.space", "in", ["device_independent"])])


def spot_names_inconsistent(severity):
    return check("Spot colour names that differ only in spelling", severity, [c("print.spot_naming", "includes", "similar_names")])


def colour_plates_used(severity):
    return check("Content on the cyan, magenta or yellow plate", severity,
                 [c("print.uses_plates", "is", True, plates=["Cyan", "Magenta", "Yellow"])])


def fonts_not_embedded(severity="error"):
    return check("Font not embedded", severity, [NOT_EMBEDDED])


def pages_differ(severity="warning"):
    return check("Pages differ in size or orientation", severity, [c("document.pages_uniform", "is", False)])


def empty_page(severity="warning"):
    return check("Empty page", severity, [c("page.empty", "is", True)])


def encrypted(severity):
    return check("Document is encrypted", severity, [c("security.encryption", "!=", "none")])


def damaged(severity):
    return check("Document is damaged", severity, [c("document.damaged", "is", True)])


def syntax_problems(severity):
    return either("Syntax problem", severity, [[c("syntax.content_stream_error", "count", 0, compare=">")],
                                               [c("syntax.xref_layout", "count", 0, compare=">")],
                                               [c("syntax.object_layout", "count", 0, compare=">")],
                                               [c("syntax.broken_reference", "in", ["object", "external_file"])]])


def version_below(v, severity):
    return check(f"PDF version below {v}", severity, [c("document.pdf_version", "<", v)])


def uncompressed_images(severity):
    return check("Uncompressed image", severity, [c("image.property", "==", "false", property="compressed")])


def hairline_checks(error_pt=0.125, warn_pt=0.25):
    return [
        check(f"Stroke thinner than {error_pt} pt", "error", [c("content.stroke_width", "<=", error_pt)]),
        check(f"Stroke between {error_pt} and {warn_pt} pt", "warning",
              [c("content.stroke_width", "<=", warn_pt), c("content.stroke_width", ">", error_pt)]),
        check(f"Thin stroke below {warn_pt} pt in more than one ink", "error",
              [c("content.stroke_width", "<=", warn_pt), MULTI_INK]),
    ]


def small_text_checks(min_pt=5, min_multi_pt=8):
    return [
        check(f"Text smaller than {min_pt} pt", "warning", text_between(0.01, min_pt)),
        check(f"Text smaller than {min_multi_pt} pt in more than one ink", "warning", text_between(0.01, min_multi_pt) + [MULTI_INK]),
        check(f"White text smaller than {min_multi_pt} pt", "warning", text_between(0.01, min_multi_pt) + [WHITE]),
    ]


def image_checks(min_ppi, min_bitmap_ppi, max_ppi=None):
    out = [image_below(min_ppi, "error"), bitmap_below(min_bitmap_ppi, "warning")]
    if max_ppi:
        out.append(image_above(max_ppi, "info"))
    return out


def colour_checks(policy, spot_limit=2):
    out = [check("Registration colour used for content", "error", [REGISTRATION])]
    if policy in ("cmyk", "cmyk_spot"):
        out += [rgb_used("error"), device_independent("warning")]
    if policy == "cmyk":
        out.append(spots_more_than(0, "error"))
    if policy == "cmyk_spot":
        out.append(spots_more_than(spot_limit, "warning"))
        out.append(spot_names_inconsistent("warning"))
    if policy == "rgb":
        out.append(spots_more_than(0, "warning"))
    if policy == "black":
        out.append(colour_plates_used("error"))
        out.append(spots_more_than(0, "error"))
    return out


def ink_checks(tac):
    return [
        check(f"Total ink above {tac}% in fills", "error", [c("color.total_ink", ">", tac, paint="fill")]),
        check(f"Total ink above {tac}% in strokes", "error", [c("color.total_ink", ">", tac, paint="stroke")]),
        check("Rich black on text smaller than 12 pt", "warning", [TEXT, c("text.size", "<", 12)] + RICH_BLACK),
    ]


def overprint_checks():
    return [
        check("White object set to overprint", "error", [WHITE, FILL_OVERPRINT]),
        check("Black text not set to overprint", "warning", [TEXT, SOLID_BLACK, FILL_KNOCKOUT]),
    ]


def transparency_checks(severity):
    return [
        check("Transparency in use", severity, [c("transparency.used", "is", True)]),
        check("Blend mode other than Normal", severity, [BLEND_NOT_NORMAL]),
    ]


def font_checks():
    return [
        fonts_not_embedded("error"),
        check("Type 3 font in use", "warning", [c("font.type", "==", "type3")]),
        check("Glyph falls back to .notdef", "error", [NOTDEF]),
        check("Font program is not valid", "error", [INVALID_FONT]),
    ]


def page_checks(safety_mm=3):
    return [
        pages_differ("warning"),
        empty_page("warning"),
        check("Page scaling in use", "error", [SCALED]),
        check(f"Object within {safety_mm} mm of the trim edge", "info",
              [c("content.distance_to_box", "<", safety_mm, box="trim", side="inside", unit="mm")], scope="trim"),
        check("Annotation that prints", "warning", [ANNOT_PRINTS]),
        check("Content on a layer", "info", [ON_LAYER]),
    ]


def document_checks(min_version=None):
    out = [encrypted("error"), damaged("error"), syntax_problems("error")]
    if min_version:
        out.append(version_below(min_version, "warning"))
    return out


def press_fixes(bleed_mm=3, min_stroke=0.25, cmyk_blend=True):
    out = [
        fix("page.set_box", box="trim", **{"from": "crop"}, when="missing"),
        fix("bleed.generate", amount=f"{bleed_mm}mm"),
        fix("page.remove_user_unit"),
        fix("ink.min_line_width", width=f"{min_stroke}pt"),
        fix("ink.overprint_black", objects="text"),
        fix("ink.knockout_white"),
        fix("document.set_info", key="Trapped", value="False"),
        fix("graphics.remove", settings=["flatness", "smoothness"]),
        fix("transparency.remove_unneeded_groups"),
    ]
    if cmyk_blend:
        out.append(fix("transparency.set_blend_space", space="CMYK"))
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
    return (hairline_checks(w.get("hair", 0.125), w.get("warn", 0.25))
            + small_text_checks(w.get("text", 5), w.get("multi", 8))
            + ink_checks(w["tac"]) + overprint_checks() + transparency_checks(w["transparency"])
            + image_checks(w["ppi"], w["bmp"], w["maxppi"]) + document_checks(w["version"])
            + colour_checks(w["policy"], w.get("spot", 2)) + font_checks() + page_checks())


for name, what, w in WORKFLOWS:
    checks = workflow_checks(w)
    profile("press", f"{name}: check",
            f"Checks a file bound for {what}: resolution, ink, hairlines, small text, fonts, colour, pages and document health.",
            checks)
    profile("press", f"{name}: check and fix",
            f"The same checks for {what}, with the safe repairs applied: trim and bleed boxes, hairlines, black overprint, white knockout, page scaling and transparency groups.",
            checks, press_fixes(cmyk_blend=w["policy"] != "black"))

profile("press", "Prepress basics", "First-pass checks a prepress operator runs before anything else.",
        hairline_checks() + small_text_checks(4, 8) + [image_below(250, "warning")])
profile("press", "Digital print quality", "The checks a short-run digital job usually fails: soft images, weak lines, heavy black.",
        hairline_checks(0.125, 0.25) + ink_checks(320) + [
            image_below(200, "error"),
            check("Image between 200 and 300 ppi", "warning", [c("image.resolution", "<", 300), c("image.resolution", ">=", 200)])])

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
    checks = [check("Not PDF/X-4", "error", [c("conformance.meets", "is", False, standard="pdfx-4")])] + workflow_checks(w)
    profile("gwg", f"{name}: check",
            "Written from the Ghent Workgroup 2022 specification for this workflow: PDF/X-4 conformance plus the workflow's resolution, ink, hairline, text, font, colour and page requirements.",
            checks)
    profile("gwg", f"{name}: check and fix",
            "The Ghent Workgroup 2022 checks for this workflow, with the safe repairs applied, converted to PDF/X-4.",
            checks, press_fixes(cmyk_blend=w["policy"] != "rgb"), convert={"level": "pdfx-4"})

PACK_PROCESSES = {
    "offset": dict(tac=320, img_error=240, img_warn=300, multi_text=8, multi_line=0.3, white_line=0.15),
    "flexo": dict(tac=320, img_error=200, img_warn=240, multi_text=14, multi_line=1.5, white_line=0.3),
    "gravure": dict(tac=310, img_error=240, img_warn=300, multi_text=6, multi_line=0.3, white_line=0.2),
}
CREASING_STEPS = ["Structural/Creasing", "Structural/ReverseCreasing", "Structural/CuttingCreasing",
                  "Structural/ReverseCuttingCreasing", "Structural/PartialCuttingCreasing",
                  "Structural/ReversePartialCuttingCreasing"]
PACK_PRODUCTS = {
    "folding carton box": dict(crease=True, banned=["Structural/Folding"]),
    "corrugated box": dict(crease=True, banned=["Structural/Folding"]),
    "corrugated display": dict(crease=True, banned=["Structural/Folding"]),
    "label": dict(crease=False, banned=CREASING_STEPS + ["Structural/Drilling", "Structural/Stapling"]),
    "leaflet": dict(crease=False, banned=CREASING_STEPS + ["Structural/Drilling", "Structural/Stapling"]),
    "flexible packaging": dict(crease=False, banned=CREASING_STEPS + [
        "Structural/PartialCutting", "Structural/ReversePartialCutting", "Structural/Drilling",
        "Structural/Embossing", "Structural/Debossing", "Structural/Stapling"]),
}
HAS_STEPS = c("page.processing_steps", "count", 0, compare=">")


def packaging_checks(p, product):
    out = [
        check("Not PDF/X-4", "error", [c("conformance.meets", "is", False, standard="pdfx-4")]),
        check(f"Total ink above {p['tac']}%", "error", [c("print.ink_coverage", ">", p["tac"])]),
        check(f"Image below {p['img_error']} ppi", "error", [c("image.resolution", "<", p["img_error"], kind="colour_or_grey")]),
        check(f"Image between {p['img_error']} and {p['img_warn']} ppi", "warning",
              [c("image.resolution", "<", p["img_warn"], kind="colour_or_grey"), c("image.resolution", ">=", p["img_error"], kind="colour_or_grey")]),
        check(f"Text smaller than {p['multi_text']} pt in more than one ink", "error", text_between(0.01, p["multi_text"]) + [MULTI_INK]),
        check(f"Line thinner than {p['multi_line']} pt in more than one ink", "error", [c("content.stroke_width", "<", p["multi_line"]), MULTI_INK]),
        check(f"White line thinner than {p['white_line']} pt", "error", [c("content.stroke_width", "<", p["white_line"]), WHITE]),
        check(f"White text smaller than {p['multi_text']} pt", "warning", text_between(0.01, p["multi_text"]) + [WHITE]),
        check("Black text below 12 pt not set to overprint", "warning", [TEXT, SOLID_BLACK, c("text.size", "<", 12), FILL_KNOCKOUT]),
        check("No processing steps in the file", "warning", [c("page.processing_steps", "count", 0, compare="==")]),
        check("Cutting die line missing", "error", [HAS_STEPS, c("page.processing_steps", "excludes", ["Structural/Cutting"])]),
        either(f"Processing step not allowed for this product ({product})", "error",
               [[c("page.processing_steps", "includes", [step])] for step in PACK_PRODUCTS[product]["banned"]]),
        check("Processing step layer without a type", "error", [c("layer.processing_step_problem", "includes", ["missing_type"])]),
        check("A step's spot colour is also used for print content", "error", [c("layer.processing_step_problem", "includes", ["spot_used_by_print_content"])]),
        check("RGB or calibrated alternate colour for a spot colour", "error",
              [c("color.space", "in", ["Separation", "DeviceN"]), c("color.space", "in", ["DeviceRGB", "CalRGB", "CalGray", "ICCBased", "Lab"], level="alternate")]),
    ]
    if PACK_PRODUCTS[product]["crease"]:
        out.append(check("Creasing line missing", "warning",
                         [HAS_STEPS, c("page.processing_steps", "excludes", ["Structural/Creasing", "Structural/ReverseCreasing"])]))
    return out + font_checks() + page_checks()


for process, p in PACK_PROCESSES.items():
    for product in PACK_PRODUCTS:
        checks = packaging_checks(p, product)
        name = f"GWG 2022 packaging, {process} {product}"
        what = (f"{process} printing, {product}: total ink {p['tac']}%, images from {p['img_error']} ppi, multi-ink text "
                f"from {p['multi_text']} pt, multi-ink lines from {p['multi_line']} pt, white lines from {p['white_line']} pt, "
                "and the processing steps this product may carry (ISO 19593-1)")
        profile("gwg", f"{name}: check", f"Written from the Ghent Workgroup 2022 packaging specification for {what}.", checks)
        profile("gwg", f"{name}: check and fix",
                "The same Ghent Workgroup 2022 packaging checks, with the safe repairs applied, converted to PDF/X-4.",
                checks, press_fixes(cmyk_blend=True), convert={"level": "pdfx-4"})

online_checks = [
    check("Spot colour in use", "warning", [SPOT]),
    check("Registration colour used for content", "error", [REGISTRATION]),
    check("Text smaller than 6 pt", "info", text_between(0.01, 6)),
    check("Page scaling in use", "warning", [SCALED]),
    image_above(150, "warning"), bitmap_above(600, "info"), image_below(72, "info"),
    fonts_not_embedded("error"), encrypted("warning"), damaged("error"), syntax_problems("error"),
    uncompressed_images("warning"),
]
profile("online", "Online publishing: check",
        "Checks a file meant for screens and downloads: images above 150 ppi that add weight, fonts, spot colours, encryption and syntax.",
        online_checks)
profile("online", "Online publishing: check and fix",
        "The online checks, then the safe cleanups: page scaling removed, flatness and smoothness dropped, colour images above 225 ppi brought down to 150 ppi, sRGB blending, single-page view.",
        online_checks,
        [fix("page.remove_user_unit"), fix("graphics.remove", settings=["flatness", "smoothness"]),
         fix("image.downsample", kind="colour_and_grey", target="150ppi", above="225ppi"),
         fix("transparency.set_blend_space", space="sRGB"), fix("document.set_view", page_mode="UseNone", page_layout="SinglePage")])
profile("online", "Email attachment: check", "Everything in the online check plus a 5 MB size limit.",
        online_checks + [check("File larger than 5 MB", "error", [c("document.file_size", ">", 5, unit="mb")])])
profile("online", "Screen viewing at 96 ppi: report", "Reports images above 96 ppi, the point past which a screen shows no more detail.",
        [image_above(96, "info")])
profile("online", "Mobile: check", "The online check with a 2 MB size limit and 100 pages, for files opened on phones.",
        online_checks + [check("File larger than 2 MB", "warning", [c("document.file_size", ">", 2, unit="mb")]),
                         check("More than 100 pages", "info", [c("document.page_count", ">", 100)])])

LEVELS = {"1b": "PDF/A-1b", "1a": "PDF/A-1a", "2b": "PDF/A-2b", "2u": "PDF/A-2u", "2a": "PDF/A-2a",
          "3b": "PDF/A-3b", "3u": "PDF/A-3u", "3a": "PDF/A-3a", "4": "PDF/A-4", "4f": "PDF/A-4f", "4e": "PDF/A-4e"}


def meets(std, label):
    return check(f"Does not meet {label}", "error", [c("conformance.meets", "is", False, standard=std)])


for lv, label in LEVELS.items():
    profile("archive", f"Check {label}", f"Reports whether the file meets {label}; convert with --level {lv} to fix it.",
            [meets(f"pdfa-{lv}", label)], convert={"level": f"pdfa-{lv}"})
VAGUE_LINK_WORDS = ["click here", "here", "read more", "more", "link", "this"]


def per_page(rule, n=1):
    return dict(rule, max_hits_per_page=n)


def a11y_checklist():
    alt = lambda v: c("structure.alternate_text", "includes", [v])
    nest = lambda v: c("structure.nesting", "includes", [v])
    title = lambda v: c("document.title", "includes", [v])
    table = lambda prop, op, v: c("structure.table", op, v, property=prop)
    return [
        check("Not marked as tagged", "error", [c("document.marked", "is", False)]),
        check("No structure tree", "error", [c("structure.tree", "includes", ["missing_root"])]),
        check("Structure tree problems", "info", [c("structure.tree", "excludes", ["missing_root"]), c("structure.tree", "count", 0, compare=">")]),
        per_page(check("Content neither tagged nor artifact", "error", [c("structure.content_tagging", "==", "neither")])),
        check("Non-standard structure type without role mapping", "error", [c("structure.standard_type", "includes", ["non_standard_type"])]),
        check("Document language missing", "error", [c("document.language", "==", "false", test="present")]),
        check("Language of some text cannot be determined", "error", [c("structure.language_unknown", "count", 0, compare=">")]),
        check("Change of language not marked", "warning", [c("structure.language_change", "count", 0, compare=">")]),
        check("Document title missing", "error", [title("missing")]),
        check("Title not in the XMP metadata", "warning", [title("not_in_metadata")]),
        check("Window shows the file name instead of the title", "error", [title("not_displayed")]),
        check("Figure without alternative text", "error", [alt("figure_without_alt")]),
        check("Alternative text is a placeholder", "warning", [alt("placeholder_alt")]),
        check("Formula without alternative text", "error", [alt("formula_without_alt")]),
        check("Annotation without description", "error", [alt("annotation_without_alt")]),
        check("Link without description", "warning", [alt("link_without_contents")]),
        check("Form field without tooltip", "error", [alt("form_field_without_alt")]),
        check("Heading levels skipped", "warning", [c("structure.headings", "includes", ["skipped_levels"])]),
        check("Table without summary or caption", "warning", [table("has_summary", "==", "false")]),
        check("Empty table header cell", "error", [table("empty_header_cells", "!=", "0")]),
        check("Table header cell without scope", "warning", [table("th_without_scope_or_headers", "!=", "0")]),
        check("Irregular table", "warning", [table("regular", "==", "false")]),
        check("Note without unique ID", "warning", [c("structure.notes", "count", 0, compare=">")]),
        check("Link not tagged as Link", "error", [nest("link_not_in_link")]),
        check("Link annotation without StructParent", "error", [nest("link_without_struct_parent")]),
        check("Form field not tagged as Form", "error", [nest("field_not_in_form")]),
        check("Annotation not tagged as Annot", "error", [nest("annotation_not_in_annot")]),
        per_page(either("Tab order does not follow the structure", "error",
                        [[c("annotation.entry", "==", "false", entry="has_tab_order")],
                         [c("annotation.entry", "!=", "S", entry="tab_order")]])),
        check("Link text does not say where it goes", "warning",
              [c("annotation.link_text", "in", VAGUE_LINK_WORDS, case_sensitive=False)]),
        check("Link marked by colour only", "warning", [c("annotation.link_colour_only", "is", True)]),
        check("Long document without bookmarks", "warning",
              [c("document.page_count", ">=", 10), c("document.has", "is", False, feature="bookmarks")]),
        check("Text contrast too low", "warning", [c("content.contrast_sufficient", "is", False)]),
        check("Graphic contrast too low", "info", [c("content.contrast_sufficient", "is", False, kind="graphics")]),
        check("Text without a Unicode mapping", "error", [c("text.unicode", "==", "unmapped")]),
        check("Assistive technology blocked by permissions", "error", [c("security.permission", "is", True, action="accessibility")]),
        fonts_not_embedded("error"),
    ]


profile("archive", "Embedded files conform to PDF/A-2b", "Checks that every embedded PDF is itself PDF/A-2b, as a PDF/A-2 container requires.",
        [check("Attachment fails PDF/A-2b", "error", [c("conformance.meets", "is", False, standard="pdfa-2b", embedded_files=True)])])
profile("archive", "Archive readiness: report", "The things that block archiving: encryption, damage, fonts not embedded, invisible text, layers and transparency.",
        [check("Invisible text", "info", [INVISIBLE_TEXT]),
         check("Content on a layer", "warning", [ON_LAYER]),
         check("Annotation with opacity", "info", [ANNOT_OPACITY]),
         encrypted("error"), damaged("error"), fonts_not_embedded("error"),
         check("Transparency in use", "info", [c("transparency.used", "is", True)])])

profile("accessibility", "Check PDF/A-2a", "Reports whether the file meets PDF/A-2a, the tagged archival level; run the CLI with --check --level 2a --ua for the PDF/UA-1 check on top.",
        [meets("pdfa-2a", "PDF/A-2a")], convert={"level": "pdfa-2a"})
profile("accessibility", "Check PDF/A-4 for accessibility", "Reports whether the file meets PDF/A-4; run the CLI with --check --level 4 --ua for the PDF/UA-2 check on top.",
        [meets("pdfa-4", "PDF/A-4")], convert={"level": "pdfa-4"})
profile("accessibility", "Accessibility readiness: report", "What an accessible document needs before tagging: text with Unicode, no invisible text, no text as clipping paths, fonts embedded.",
        [check("Text without a Unicode mapping", "error", [c("text.unicode", "==", "unmapped")]),
         check("Invisible text", "warning", [INVISIBLE_TEXT]),
         check("Text that clips instead of painting", "warning", [c("text.rendering", "==", "clip_only")]),
         fonts_not_embedded("error"), pages_differ("info")])
profile("accessibility", "Accessibility checklist (WCAG / Section 508)",
        "Checks a tagged PDF against the machine-testable items of WCAG 2.2 and Section 508: tags, language, title, alternative text, "
        "tables, links, form fields, tab order, bookmarks and colour contrast. Contrast and link checks are best-effort and need a human look.",
        a11y_checklist())

XLEVELS = {"x1a": ("pdfx-1a", "PDF/X-1a"), "x3": ("pdfx-3", "PDF/X-3"), "x4": ("pdfx-4", "PDF/X-4"), "x4p": ("pdfx-4p", "PDF/X-4p"),
           "x5g": ("pdfx-5g", "PDF/X-5g"), "x5n": ("pdfx-5n", "PDF/X-5n"), "x5pg": ("pdfx-5pg", "PDF/X-5pg"), "x6": ("pdfx-6", "PDF/X-6"),
           "x6n": ("pdfx-6n", "PDF/X-6n"), "x6p": ("pdfx-6p", "PDF/X-6p"), "e1": ("pdfe-1", "PDF/E-1"),
           "vt1": ("pdfvt-1", "PDF/VT-1"), "vt2": ("pdfvt-2", "PDF/VT-2"), "vt3": ("pdfvt-3", "PDF/VT-3")}
for lv, (std, label) in XLEVELS.items():
    check_only = lv in ("x4p", "x5g", "x5n", "x5pg", "x6n", "x6p", "vt2")
    hint = "checked only, because it refers to an external press profile" if check_only else f"convert with --level {lv} to fix it"
    profile("standards", f"Check {label}", f"Reports whether the file meets {label}; {hint}.",
            [meets(std, label)], convert=None if check_only else {"level": std})

for n in (72, 96, 150, 200, 250, 300, 350, 400):
    profile("images", f"Images below {n} ppi", f"Reports colour and grayscale images whose effective resolution is below {n} ppi.", [image_below(n)])
for n in (300, 600, 800, 1000, 1200):
    profile("images", f"Bitmaps below {n} ppi", f"Reports 1-bit images whose effective resolution is below {n} ppi.", [bitmap_below(n)])
for n in (300, 450, 600, 800):
    profile("images", f"Images above {n} ppi", f"Reports images above {n} ppi, candidates for downsampling.", [image_above(n)])
for n in (1200, 2400):
    profile("images", f"Bitmaps above {n} ppi", f"Reports 1-bit images above {n} ppi.", [bitmap_above(n)])
FILTERS = (("JPEG", "DCTDecode"), ("JPEG 2000", "JPXDecode"), ("CCITT fax", "CCITTFaxDecode"), ("JBIG2", "JBIG2Decode"),
           ("Flate", "FlateDecode"), ("LZW", "LZWDecode"), ("run-length", "RunLengthDecode"))


def filter_check(label, name, severity="info"):
    return check(f"{label}-compressed image", severity, [c("image.property", "contains", name, property="filter")])


for label, name in FILTERS:
    profile("images", f"{label}-compressed images", f"Reports images stored with {label} compression.", [filter_check(label, name)])
profile("images", "Uncompressed images", "Reports images stored without any compression.", [uncompressed_images("warning")])
profile("images", "16-bit images", "Reports images with 16 bits per component, which many workflows cannot take.",
        [check("16-bit image", "warning", [c("image.property", "==", "16", property="bits_per_component")])])
profile("images", "1-bit images", "Reports bitmap images and image masks.",
        [check("1-bit image", "info", [c("image.property", "==", "1", property="bits_per_component")])])
profile("images", "Images with soft masks", "Reports images carrying a soft mask, a form of transparency.",
        [check("Image with a soft mask", "info", [c("image.property", "==", "true", property="soft_mask")])])
profile("images", "Interpolated images", "Reports images with the interpolation flag, which renderers treat differently.",
        [check("Interpolated image", "info", [c("image.property", "==", "true", property="interpolate")])])
for n in (2000, 5000, 10000):
    profile("images", f"Images larger than {n} pixels", f"Reports images wider or taller than {n} pixels.",
            [check(f"Image wider than {n} px", "info", [c("image.property", "matches", above(n), property="width")]),
             check(f"Image taller than {n} px", "info", [c("image.property", "matches", above(n), property="height")])])
profile("images", "Invalid images", "Reports images that cannot be decoded.",
        [check("Invalid image", "error", [c("image.property", "==", "true", property="invalid")])])
profile("images", "Image masks", "Reports stencil masks painted with the current colour.", [check("Image mask", "info", [c("content.kind", "==", "image_mask")])])
profile("images", "Image formats: report", "One report of every compression filter and bit depth in use.",
        [filter_check(label, name) for label, name in FILTERS[:4]] +
        [check("16-bit image", "info", [c("image.property", "==", "16", property="bits_per_component")]),
         check("1-bit image", "info", [c("image.property", "==", "1", property="bits_per_component")])])

profile("colour", "RGB in use", "Reports every object painted in RGB.", [rgb_used("warning")])
profile("colour", "Device-independent colour in use", "Reports Lab, calibrated and ICC-based colour.", [device_independent("info")])
profile("colour", "Colour plates used", "Reports pages with output on the cyan, magenta or yellow plates; useful for jobs meant to be black only.", [colour_plates_used("warning")])
SPACES = (("Lab colour", [c("color.space", "==", "Lab")]),
          ("Calibrated colour spaces", [c("color.space", "in", ["CalGray", "CalRGB"])]),
          ("ICC-based colour", [c("color.space", "==", "ICCBased")]),
          ("DeviceN colour", [c("color.space", "in", ["DeviceN", "NChannel"])]),
          ("Separation colour", [c("color.space", "==", "Separation")]),
          ("Pattern fills", [c("color.space", "==", "Pattern")]),
          ("ICC-based CMYK", [c("color.space", "==", "ICCBased", icc_components=4)]),
          ("ICC-based RGB", [c("color.space", "==", "ICCBased", icc_components=3)]),
          ("Process colour defined as a spot", [c("color.spot", "==", "process", property="kind")]),
          ("Process colours defined as DeviceN", [c("color.devicen", "includes", "process_as_devicen")]),
          ("Spot colours in use", [SPOT]))
for label, conds in SPACES:
    profile("colour", label, f"Reports every object painted with {label.lower()}.", [check(label, "info", conds)])
for n in (1, 2, 3, 4, 6, 8):
    profile("colour", f"More than {n} spot colour{'s' if n > 1 else ''}", f"Reports pages that use more than {n} spot colour{'s' if n > 1 else ''}.",
            [spots_more_than(n)])
profile("colour", "Registration colour in use", "Reports content painted in the registration colour, which prints on every plate.",
        [check("Registration colour used for content", "error", [REGISTRATION])])
profile("colour", "Pantone-suffixed spot names", "Reports spot colours whose names carry a Pantone suffix, which often signals duplicates.",
        [check("Spot name with a Pantone suffix", "info", [c("color.spot", "==", "pantone", property="colour_system")])])
profile("colour", "Spot names that differ only in spelling", "Reports spot colours that are the same ink under two names.",
        [spot_names_inconsistent("warning")])
profile("colour", "Inconsistent spot definitions", "Reports a spot colour defined with different alternate values in different places.",
        [check("Inconsistent spot representation", "warning", [c("print.spot_naming", "includes", "same_name_different_look")])])
for n in (0, 200, 240, 280):
    label = "Rich black" if n == 0 else f"Rich black above {n}% total"
    profile("colour", label, "Reports solid black objects that also carry cyan, magenta or yellow." if n == 0 else f"Reports black objects whose added CMY brings total ink above {n}%.",
            [check(label, "warning", RICH_BLACK + ([c("color.total_ink", ">", n, paint="fill")] if n else []))])
profile("colour", "Rich black on text", "Reports text painted in rich black, a registration risk at small sizes.",
        [check("Rich black text", "warning", [TEXT] + RICH_BLACK)])
for n in (240, 260, 280, 300, 320, 340, 360):
    profile("colour", f"Total ink above {n}%", f"Reports fills and strokes whose total area coverage exceeds {n}%.",
            [check(f"Total ink above {n}% in fills", "error", [c("color.total_ink", ">", n, paint="fill")]),
             check(f"Total ink above {n}% in strokes", "error", [c("color.total_ink", ">", n, paint="stroke")])])
profile("colour", "White objects set to overprint", "Reports white objects that overprint and would vanish on press.",
        [check("White object set to overprint", "error", [WHITE, FILL_OVERPRINT])])
profile("colour", "Black text not set to overprint", "Reports 100% black text that knocks out, a registration risk.",
        [check("Black text not set to overprint", "warning", [TEXT, SOLID_BLACK, FILL_KNOCKOUT])])
profile("colour", "Overprinting objects", "Reports every object with overprint switched on.",
        [check("Fill set to overprint", "info", [FILL_OVERPRINT]), check("Stroke set to overprint", "info", [STROKE_OVERPRINT])])
profile("colour", "Overprint mode 1", "Reports objects drawn with the non-zero overprint mode, where zero ink values leave the inks below untouched.",
        [check("Overprint mode 1", "info", [c("color.overprint", "==", "on_nonzero_mode")])])
profile("colour", "100% black objects", "Reports objects painted in pure black.", [check("100% black object", "info", [SOLID_BLACK])])
profile("colour", "Objects using black only", "Reports objects that print on the black plate alone.", [check("Black-only object", "info", [c("color.ink_mix", "in", ["black_only", "solid_black"])])])
profile("colour", "CMYK-only objects", "Reports objects painted in process colours without spots.", [check("CMYK-only object", "info", [c("color.ink_mix", "==", "process_only")])])
profile("colour", "Spot-only objects", "Reports objects painted in spot colours without process inks.", [check("Spot-only object", "info", [c("color.ink_mix", "==", "spot_only")])])
profile("colour", "Gray objects", "Reports objects painted in DeviceGray.", [check("DeviceGray object", "info", [c("color.space", "==", "DeviceGray")])])
profile("colour", "Colour usage: report", "One report of RGB, Lab, ICC, spot, DeviceN, pattern and registration colour in use.",
        [check("RGB object", "info", [c("color.space", "==", "DeviceRGB")]), check("Lab object", "info", [c("color.space", "==", "Lab")]),
         check("ICC-based object", "info", [c("color.space", "==", "ICCBased")]), check("Spot object", "info", [SPOT]),
         check("DeviceN object", "info", [c("color.space", "in", ["DeviceN", "NChannel"])]), check("Pattern fill", "info", [c("color.space", "==", "Pattern")]),
         check("Registration colour", "warning", [REGISTRATION])])

for n in (0.1, 0.125, 0.25, 0.5):
    profile("objects", f"Hairlines below {n} pt", f"Reports strokes thinner than {n} pt.",
            [check(f"Stroke thinner than {n} pt", "error", [c("content.stroke_width", "<=", n)])])
profile("objects", "Report hairlines", "Finds strokes too thin to print reliably, graded by width.", hairline_checks() + [
    check("Stroke between 0.25 and 0.5 pt", "info", [c("content.stroke_width", "<=", 0.5), c("content.stroke_width", ">", 0.25)])])
profile("objects", "Thin strokes in more than one ink", "Reports strokes below 0.25 pt built from several inks, which misregister.",
        [check("Thin stroke in more than one ink", "error", [c("content.stroke_width", "<=", 0.25), MULTI_INK])])
for n in (4, 5, 6, 7, 8, 9, 10):
    profile("objects", f"Text below {n} pt", f"Reports text smaller than {n} pt.",
            [check(f"Text smaller than {n} pt", "warning", text_between(0.01, n))])
for n in (8, 9, 10, 12):
    profile("objects", f"Small text below {n} pt in more than one ink", f"Reports text under {n} pt built from several inks.",
            [check(f"Text smaller than {n} pt in more than one ink", "warning", text_between(0.01, n) + [MULTI_INK])])
    profile("objects", f"White text below {n} pt", f"Reports knockout white text under {n} pt, which fills in on press.",
            [check(f"White text smaller than {n} pt", "warning", text_between(0.01, n) + [WHITE])])
profile("objects", "Report small text", "Reports text at sizes that print or read poorly.", small_text_checks(5, 8))
profile("objects", "Report invisible text", "Finds text drawn in invisible rendering mode, typically OCR layers.",
        [check("Invisible text", "info", [INVISIBLE_TEXT])])
profile("objects", "Clipping text", "Reports text that clips instead of painting.",
        [check("Text as clipping path", "info", [c("text.rendering", "==", "clip_only")])])
profile("objects", "Stroked text", "Reports text that is stroked rather than filled.",
        [check("Stroked text", "info", [c("text.rendering", "in", ["stroke", "fill_and_stroke"])])])
profile("objects", "Text render modes: report", "Reports every text rendering mode other than plain fill.",
        [check("Text render mode other than fill", "info", [c("text.rendering", "!=", "fill")])])
profile("objects", "Report white objects", "Finds white fills and strokes, which either knock out or vanish when overprinting.",
        [check("White fill", "info", [WHITE, c("content.painting", "in", ["fill", "fill_and_stroke"])]),
         check("White stroke", "info", [WHITE, c("content.painting", "in", ["stroke", "fill_and_stroke"])]),
         check("White object set to overprint", "error", [WHITE, FILL_OVERPRINT])])
profile("objects", "Report rich black", "Finds solid black objects that also carry cyan, magenta or yellow.",
        [check("Rich black object", "info", RICH_BLACK),
         check("Rich black text below 12 pt", "warning", [TEXT, c("text.size", "<", 12)] + RICH_BLACK)])
profile("objects", "Transparency in use", "Reports every object drawn with transparency.", transparency_checks("info"))
profile("objects", "Transparency: report", "Blend modes, soft masks, constant alpha and transparency groups, each reported separately.",
        [check("Blend mode other than Normal", "info", [BLEND_NOT_NORMAL]),
         check("Soft mask in use", "info", [SOFT_MASK]),
         check("Fill alpha below 100%", "info", [c("transparency.alpha", "<", 1, paint="fill")]),
         check("Stroke alpha below 100%", "info", [c("transparency.alpha", "<", 1, paint="stroke")]),
         check("Object inside a transparency group", "info", [c("transparency.group", "includes", "member")])])
profile("objects", "Blend modes other than Normal", "Reports objects using Multiply, Screen, Overlay and the other blend modes.",
        [check("Blend mode other than Normal", "info", [BLEND_NOT_NORMAL])])
profile("objects", "Soft masks", "Reports objects drawn through a soft mask.", [check("Soft mask in use", "info", [SOFT_MASK])])
for n in (50, 90, 100):
    profile("objects", f"Constant alpha below {n}%", f"Reports fills and strokes with opacity below {n}%.",
            [check(f"Fill alpha below {n}%", "info", [c("transparency.alpha", "<", n / 100, paint="fill")]),
             check(f"Stroke alpha below {n}%", "info", [c("transparency.alpha", "<", n / 100, paint="stroke")])])
profile("objects", "Flatness set", "Reports objects drawn with a flatness tolerance, which changes curve rendering.",
        [check("Flatness set", "info", [c("print.flatness", ">", 0)])])
profile("objects", "Black point compensation set", "Reports objects drawn with black point compensation switched on.",
        [check("Black point compensation", "info", [c("color.black_point_compensation", "==", "ON")])])
for n in (1000, 5000, 10000):
    profile("objects", f"Paths with more than {n} nodes", f"Reports vector paths with more than {n} nodes, which slow rendering and output devices.",
            [check(f"Path with more than {n} nodes", "warning", [c("content.path_measure", ">", n, measure="points")])])
profile("objects", "Empty vector objects", "Reports paths that neither fill nor stroke.",
        [check("Empty vector object", "info", [c("content.kind", "==", "path"), c("content.painting", "==", "none")])])
profile("objects", "Objects outside the media box", "Reports content entirely outside the page.",
        [check("Object outside the media box", "warning", [c("content.box_relation", "==", "outside", box="media")])])
profile("objects", "Objects outside the bleed box", "Reports content reaching beyond the bleed box.",
        [check("Object outside the bleed box", "info", [c("content.box_relation", "in", ["outside", "crosses"], box="bleed")])])
for n in (2, 3, 5):
    profile("objects", f"Objects within {n} mm of the trim edge", f"Reports content closer than {n} mm to the trim edge, the usual safety margin.",
            [check(f"Object within {n} mm of the trim edge", "info", [c("content.distance_to_box", "<", n, box="trim", side="inside", unit="mm")], scope="trim")])
profile("objects", "Shadings in use", "Reports smooth shades and gradients.", [check("Shading", "info", [c("content.kind", "==", "shading")])])
profile("objects", "Line art only: report", "Reports strokes that carry no fill.", [check("Stroked, not filled", "info", [c("content.painting", "==", "stroke")])])
profile("objects", "Unknown operators", "Reports content stream operators no PDF version defines.",
        [check("Unknown operator", "error", [c("syntax.content_stream_error", "includes", "unknown_operator")])])
profile("objects", "Overprint and knockout: report", "Reports overprinting fills and strokes plus white objects set to overprint.", overprint_checks() + [
    check("Fill set to overprint", "info", [FILL_OVERPRINT]), check("Stroke set to overprint", "info", [STROKE_OVERPRINT])])

profile("pages", "Pages of different sizes", "Reports documents whose pages differ in size or orientation.", [pages_differ("warning")])
profile("pages", "Empty pages", "Reports pages with no visible content.", [empty_page("warning")])
profile("pages", "Rotated pages", "Reports pages with a rotation attribute.", [check("Rotated page", "info", [ROTATED])])
profile("pages", "Pages without a crop box", "Reports pages that rely on the media box alone.", [check("No crop box", "info", [NO_CROP_BOX])])
profile("pages", "Crop box equals media box", "Reports pages whose crop box adds nothing.",
        [check("Crop box equals media box", "info", [c("page.box_present", "is", True, box="crop"), c("page.box_relation", "==", "same", box="crop", other="media")])])
profile("pages", "Single-image pages", "Reports pages made of one image, typically scans.", [check("Single-image page", "info", [c("page.single_image", "is", True)])])
profile("pages", "Uncompressed page content", "Reports page content streams stored without compression.",
        [check("Uncompressed content stream", "info", [c("page.content_stream", "==", "none", property="compression")])])
profile("pages", "Page-level output intents", "Reports pages that declare their own output intent (PDF/X-6).",
        [check("Page-level output intent", "info", [c("page.output_intent", "!=", "none", subtype="any")])])
for n in (1, 2, 4, 10, 50, 100, 500, 1000):
    profile("pages", f"More than {n} page{'s' if n > 1 else ''}", f"Reports documents with more than {n} page{'s' if n > 1 else ''}.",
            [check(f"More than {n} pages", "info", [c("document.page_count", ">", n)])])


def box_fix(box, frm, inset=0, when="missing"):
    d = f"{inset}mm"
    return fix("page.set_box", box=box, **{"from": frm}, left=d, right=d, top=d, bottom=d, when=when)


profile("pages", "Set trim box from crop box", "Adds a trim box equal to the crop box on pages that lack one.", [], [box_fix("trim", "crop")])
profile("pages", "Set trim box from media box", "Adds a trim box equal to the media box on pages that lack one.", [], [box_fix("trim", "media")])
for n in (3, 5):
    profile("pages", f"Set trim box {n} mm inside the media box", f"Adds a trim box inset {n} mm from the media box on pages that lack one, for files delivered with bleed but no boxes.",
            [], [box_fix("trim", "media", n)])
profile("pages", "Reset trim box to crop box", "Replaces the trim box with the crop box on every page.", [], [box_fix("trim", "crop", when="always")])
profile("pages", "Set art box from trim box", "Adds an art box equal to the trim box where missing.", [], [box_fix("art", "trim")])
profile("pages", "Set crop box from media box", "Adds a crop box equal to the media box where missing.", [], [box_fix("crop", "media")])
for n in (1, 2, 3, 5):
    profile("pages", f"Add {n} mm bleed box", f"Sets a bleed box {n} mm outside the trim box, limited by the media box.",
            [], [fix("page.set_box", box="bleed", **{"from": "trim"}, left=f"-{n}mm", right=f"-{n}mm", top=f"-{n}mm", bottom=f"-{n}mm")])
for box, label in (("media", "media"), ("bleed", "bleed"), ("trim", "trim")):
    profile("pages", f"Clip content to the {label} box", f"Removes anything drawn outside the {label} box.", [], [fix("content.remove_outside", box=box)])
for ang in (90, 180, 270):
    profile("pages", f"Rotate pages {ang} degrees", f"Rotates every page by {ang} degrees clockwise.", [], [fix("page.rotate", angle=str(ang))])
for label, w, h, unit in (("A4", 210, 297, "mm"), ("A3", 297, 420, "mm"), ("A5", 148, 210, "mm"), ("US Letter", 8.5, 11, "in"), ("US Legal", 8.5, 14, "in"), ("Tabloid", 11, 17, "in")):
    profile("pages", f"Scale pages to {label}", f"Scales every page proportionally to fit {label} ({w} x {h} {unit}).", [],
            [fix("page.scale", width=f"{w}{unit}", height=f"{h}{unit}")])
profile("pages", "Remove page scaling", "Removes the user unit so pages output at their nominal size.", [], [fix("page.remove_user_unit")])

for n in (1, 2, 5, 10, 20, 50, 100):
    profile("document", f"File larger than {n} MB", f"Reports files above {n} MB.", [check(f"File larger than {n} MB", "warning", [c("document.file_size", ">", n, unit="mb")])])
for v in (1.4, 1.5, 1.6, 1.7, 2.0):
    profile("document", f"PDF version below {v}", f"Reports files written against a PDF version older than {v}.", [version_below(v, "info")])
profile("document", "PDF 2.0 required", "Reports files whose requirements say they need a PDF 2.0 reader.",
        [check("Requires PDF 2.0", "info", [c("document.requirements", "is", True, pdf2_only=True)])])
profile("document", "Encrypted document", "Reports files protected by encryption.", [encrypted("warning")])
profile("document", "Damaged document", "Reports files that had to be repaired while reading.", [damaged("error")])
profile("document", "Syntax problems", "Reports structural problems found while reading the file.", [syntax_problems("error")])
profile("document", "Data after end of file", "Reports bytes after the final %%EOF marker.", [check("Data after end of file", "info", [DATA_AFTER_EOF])])
profile("document", "Invalid hex strings", "Reports hexadecimal strings with characters outside 0-9 and A-F.",
        [check("Invalid hex string", "warning", [c("syntax.string_encoding", "includes", "hex_invalid_character")])])
profile("document", "XMP metadata not plain text", "Reports XMP packets that are compressed or encoded, which PDF/A forbids.",
        [check("XMP not plain text", "warning", [c("metadata.xmp_problem", "includes", "filtered")])])
profile("document", "Signature fields present", "Reports documents that carry signature fields.", [check("Signature field", "info", [c("signature.present", "is", True)])])
profile("document", "Document health: check", "Encryption, damage, syntax, data after the end marker and hex strings, in one run.",
        [check("Data after end of file", "info", [DATA_AFTER_EOF]),
         check("Invalid hex string", "warning", [c("syntax.string_encoding", "includes", "hex_invalid_character")]),
         encrypted("warning"), damaged("error"), syntax_problems("error")])
profile("document", "Annotations that print", "Reports annotations flagged to appear in print.", [check("Annotation that prints", "warning", [ANNOT_PRINTS])])
for t in ("Link", "Widget", "FileAttachment", "Stamp", "Text", "Highlight", "FreeText", "3D", "RichMedia", "Movie", "Sound", "Screen", "Watermark", "Redact"):
    profile("document", f"{t} annotations", f"Reports annotations of type {t}.", [check(f"{t} annotation", "info", [c("annotation.type", "==", t)])])
profile("document", "Annotations with opacity", "Reports annotations drawn with reduced opacity.", [check("Annotation with opacity", "info", [ANNOT_OPACITY])])
profile("document", "Annotations of unknown type", "Reports annotation types the PDF specification does not define.",
        [check("Unknown annotation type", "warning", [c("annotation.type", "==", "unknown")])])
profile("document", "Annotations outside the crop area", "Reports annotations lying outside the visible page area or with no size.",
        [check("Annotation outside the crop box", "info", [c("annotation.outside_crop", "in", ["outside", "zero_size"])])])
profile("document", "Layers in use", "Reports content placed on optional content layers.", [check("Content on a layer", "info", [ON_LAYER])])
profile("document", "Hidden layers", "Reports content on layers that are switched off.",
        [check("Content on a hidden layer", "warning", [ON_LAYER, c("layer.visible", "is", False)])])
profile("document", "Layer configurations", "Reports documents with alternate layer configurations.",
        [check("Layer configurations present", "info", [c("layer.configuration", "includes", "has_alternate_configs")])])
profile("document", "Processing steps layers", "Reports processing-steps metadata (ISO 19593) on layers, used in packaging.",
        [check("Processing steps present", "info", [c("layer.processing_step", "count", 0, compare=">")])])
profile("document", "Trapped key not set", "Reports files that do not say whether they are trapped, which PDF/X requires.",
        [check("Trapped key missing", "warning", [c("print.trapped", "in", ["Unknown", "missing"])])])
profile("document", "PDF/X info fields present", "Reports files carrying the PDF/X identification fields.",
        [check("PDF/X fields", "info", [c("metadata.info_keys", "includes", "pdfx")])])
profile("document", "No output intent", "Reports files without an output intent, which every print standard needs.",
        [check("No output intent", "warning", [c("output_intent.count", "==", 0, subtype="any")])])
profile("document", "Output intent without a profile", "Reports output intents that name a condition but embed no ICC profile.",
        [check("Output intent without profile", "warning", [c("output_intent.entry", "==", "false", subtype="any", entry="has_profile")])])
profile("document", "Several output intents", "Reports files with more than one output intent.",
        [check("More than one output intent", "info", [c("output_intent.count", ">", 1, subtype="any")])])
profile("document", "PDF/X output intent", "Reports files carrying a PDF/X output intent.", [check("PDF/X output intent", "info", [c("output_intent.count", ">", 0, subtype="pdfx")])])
profile("document", "PDF/A output intent", "Reports files carrying a PDF/A output intent.", [check("PDF/A output intent", "info", [c("output_intent.count", ">", 0, subtype="pdfa")])])
profile("document", "Fonts not embedded", "Reports every font the file does not carry.", [fonts_not_embedded("error")])
profile("document", "Fonts: report", "Embedding, subsetting, type, validity and Unicode coverage of every font.",
        [check("Font not embedded", "error", [NOT_EMBEDDED]), check("Type 3 font", "info", [c("font.type", "==", "type3")]),
         check("TrueType font", "info", [c("font.type", "in", ["truetype", "cid_truetype", "opentype_truetype"])]),
         check("CID font", "info", [c("font.type", "in", ["cid_type0", "cid_truetype"])]),
         check("Font not subset", "info", [c("font.embedding", "==", "embedded_full")]),
         check("Subset missing glyphs in use", "error", [c("font.problem", "includes", "subset_missing_glyph")]),
         check("Text without Unicode", "warning", [NO_UNICODE]), check("Invalid font program", "error", [INVALID_FONT]),
         check("Glyph falls back to .notdef", "error", [NOTDEF])])
FONT_KINDS = (("Type 3 fonts", "info", [c("font.type", "==", "type3")]),
              ("TrueType fonts", "info", [c("font.type", "in", ["truetype", "cid_truetype", "opentype_truetype"])]),
              ("CID fonts", "info", [c("font.type", "in", ["cid_type0", "cid_truetype"])]),
              ("Fonts with restricted licences", "info", [c("font.license", "==", "restricted")]),
              ("Bitmap-only fonts", "info", [c("font.license", "==", "bitmap_only")]),
              ("Fonts not subset", "info", [c("font.embedding", "==", "embedded_full")]),
              ("Glyphs falling back to .notdef", "warning", [NOTDEF]),
              ("Invalid font programs", "warning", [INVALID_FONT]),
              ("Type 1 CID fonts", "info", [c("font.type", "==", "cid_type0")]),
              ("TrueType CID fonts", "info", [c("font.type", "==", "cid_truetype")]),
              ("OpenType fonts", "info", [c("font.type", "in", ["opentype_cff", "opentype_truetype"])]))
for label, severity, conds in FONT_KINDS:
    profile("document", label, f"Reports {label.lower()} in use.", [check(label, severity, conds)])
profile("document", "Fonts missing glyphs", "Reports subset fonts that lack glyphs the text uses.",
        [check("Subset missing glyphs", "error", [c("font.problem", "includes", "subset_missing_glyph")])])
profile("document", "Text without Unicode mapping", "Reports fonts with characters that have no Unicode meaning, so the text cannot be searched or read aloud.",
        [check("Text without Unicode", "warning", [NO_UNICODE])])
profile("document", "Font widths mismatch", "Reports fonts whose declared widths disagree with the embedded program.",
        [check("Width mismatch", "warning", [c("font.problem", "includes", "width_mismatch")])])
profile("document", "Transfer curves", "Reports transfer functions, which PDF/X forbids.",
        [check("Transfer curve", "warning", [c("print.rendering_functions", "includes", "transfer")])])
profile("document", "Halftones", "Reports custom halftone settings.", [check("Custom halftone", "info", [c("print.halftone", "==", "true", property="present")])])
profile("document", "PostScript fragments", "Reports PostScript code, which most viewers ignore.",
        [check("PostScript fragment", "warning", [c("syntax.postscript", "in", ["operator", "xobject", "form"])])])
profile("document", "Halftone origin set", "Reports halftone dictionaries that fix a screen origin.",
        [check("Halftone origin", "info", [c("print.halftone", "==", "true", property="origin")])])
profile("document", "ICC profile versions", "Reports ICC profiles of version 4 or later, which older output devices reject.",
        [check("ICC version 4 or later", "info", [c("icc.header", "matches", "([4-9]|[1-9][0-9]).*", field="version")])])
profile("document", "PDF/VT document parts", "Reports the document-part hierarchy PDF/VT builds.",
        [check("Document parts present", "info", [c("variable_data.part_value", "!=", "", part_value="root_type")])])

for ang in (90, 180, 270):
    profile("actions", f"Rotate all pages {ang} degrees", f"Rotates every page by {ang} degrees.", [], [fix("page.rotate", angle=str(ang))])
profile("actions", "Set trapped flag to false", "Declares the file untrapped.", [], [fix("document.set_info", key="Trapped", value="False")])
profile("actions", "Set trapped flag to true", "Declares the file trapped.", [], [fix("document.set_info", key="Trapped", value="True")])
for ri in ("RelativeColorimetric", "Perceptual", "Saturation", "AbsoluteColorimetric"):
    profile("actions", f"Set rendering intent to {ri}", f"Sets the rendering intent of every object to {ri}.", [], [fix("graphics.set", rendering_intent=ri)])
profile("actions", "Remove rendering intents", "Removes rendering intents so the output device decides.", [], [fix("graphics.remove", settings=["rendering_intent"])])
profile("actions", "Blend transparency in CMYK", "Sets a CMYK press profile as the page blending colour space.", [], [fix("transparency.set_blend_space", space="CMYK")])
profile("actions", "Blend transparency in sRGB", "Sets sRGB as the page blending colour space.", [], [fix("transparency.set_blend_space", space="sRGB")])
profile("actions", "Remove image interpolation", "Removes the interpolation flag from every image.", [], [fix("image.set_interpolation", value="remove")])
profile("actions", "Remove flatness", "Removes flatness tolerances from graphics states.", [], [fix("graphics.remove", settings=["flatness"])])
profile("actions", "Remove smoothness", "Removes smoothness tolerances from graphics states.", [], [fix("graphics.remove", settings=["smoothness"])])
profile("actions", "Remove transfer curves", "Removes transfer functions.", [], [fix("graphics.remove", settings=["transfer"])])
profile("actions", "Remove black generation", "Removes black-generation functions.", [], [fix("graphics.remove", settings=["black_generation"])])
profile("actions", "Remove undercolour removal", "Removes undercolour-removal functions.", [], [fix("graphics.remove", settings=["undercolour_removal"])])
profile("actions", "Remove unnecessary transparency groups", "Drops page transparency groups that contain no transparent objects.", [], [fix("transparency.remove_unneeded_groups")])
profile("actions", "Merge spot colour names", "Writes Pantone and HKS spot names in one standard spelling, so one ink is not split across names that differ only in case, spacing or suffix.", [],
        [fix("spot.normalise_names", system="pantone"), fix("spot.normalise_names", system="hks")])
profile("actions", "Make spot colour definitions consistent", "Gives every use of a spot colour the same definition.", [], [fix("spot.unify_definitions")])
profile("actions", "Registration colour to black", "Repaints registration-colour content in black.", [], [fix("spot.registration_to_black")])
profile("actions", "NChannel to DeviceN", "Converts NChannel colour spaces to plain DeviceN.", [], [fix("color.nchannel_to_devicen")])
profile("actions", "Black text to overprint", "Sets 100% black text to overprint.", [], [fix("ink.overprint_black", objects="text")])
profile("actions", "All black objects to overprint", "Sets every 100% black object to overprint.", [], [fix("ink.overprint_black", objects="all")])
profile("actions", "White text to knockout", "Switches overprint off for white text.", [], [fix("ink.knockout_white", objects="text")])
profile("actions", "All white objects to knockout", "Switches overprint off for every white object.", [], [fix("ink.knockout_white", objects="all")])
profile("actions", "Black overprint and white knockout", "The two standard overprint repairs in one step.", [],
        [fix("ink.overprint_black", objects="all"), fix("ink.knockout_white", objects="all")])
for n in (0.25, 0.3, 0.5):
    profile("actions", f"Thicken hairlines to {n} pt", f"Raises every stroke thinner than {n} pt to {n} pt.", [], [fix("ink.min_line_width", width=f"{n}pt")])
profile("actions", "Make invisible text visible", "Sets invisible text to plain fill so it shows and prints.", [], [fix("text.set_render_mode", mode="fill")])
profile("actions", "Stamp the conversion date", "Places the conversion date on every page.",
        [], [fix("place.text", text="{date}", size="9pt", anchor="lower_right", x="-10mm", y="5mm")])
profile("actions", "Stamp DRAFT", "Places the word DRAFT on every page.", [], [fix("place.text", text="DRAFT", size="36pt")])
profile("actions", "Stamp CONFIDENTIAL", "Places the word CONFIDENTIAL on every page.", [], [fix("place.text", text="CONFIDENTIAL", size="24pt")])
profile("actions", "Put page content on a layer", "Moves every page's content onto a layer named Content.", [], [fix("layer.move_objects", layer="Content")])
profile("actions", "Mark content as a processing step", "Places page content on a processing-steps layer (ISO 19593).",
        [], [fix("layer.move_objects", layer="Structural", processing_step="Structural")])
profile("actions", "Flatten layers", "Discards hidden layers and merges the visible ones into the page.", [], [fix("layer.flatten")])
profile("actions", "Open in single-page view", "Sets the initial view to one page at a time, no panels.", [], [fix("document.set_view", page_mode="UseNone", page_layout="SinglePage")])
profile("actions", "Open with bookmarks panel", "Sets the initial view to show the bookmarks panel.", [], [fix("document.set_view", page_mode="UseOutlines", page_layout="SinglePage")])
profile("actions", "Open as two-page spread", "Sets the initial view to facing pages with the first page on the right.", [], [fix("document.set_view", page_mode="UseNone", page_layout="TwoPageRight")])
profile("actions", "Open in full screen", "Sets the initial view to full-screen presentation.", [], [fix("document.set_view", page_mode="FullScreen", page_layout="SinglePage")])
profile("actions", "Set title when missing", "Sets the document title to Untitled if none is set.", [], [fix("document.set_info", key="Title", value="Untitled", only_if_empty=True)])
profile("actions", "Prepress cleanup", "The safe prepress repairs in one step: trim and bleed boxes, hairlines, overprint, knockout, scaling, transparency groups.", [], press_fixes())

REPORTS = [
    ("Report low resolution images", "Reports colour and grayscale images below 300 ppi and bitmaps below 1200 ppi.",
     [image_below(300), bitmap_below(1200)]),
    ("Report high resolution images", "Reports images above 450 ppi and bitmaps above 2400 ppi, candidates for downsampling.",
     [image_above(450), bitmap_above(2400)]),
    ("Report spot colours", "Reports every spot colour object and pages with more than two spot inks.",
     [check("Spot object", "info", [SPOT]), spots_more_than(2)]),
    ("Report overprint", "Reports overprinting fills and strokes and white objects set to overprint.",
     overprint_checks() + [check("Fill set to overprint", "info", [FILL_OVERPRINT])]),
    ("Report transparency", "Reports transparency, blend modes and soft masks.",
     transparency_checks("info") + [check("Soft mask in use", "info", [SOFT_MASK])]),
    ("Report page geometry", "Reports page sizes, rotation, scaling, empty pages and boxes.",
     [check("Rotated page", "info", [ROTATED]), check("Page scaling in use", "warning", [SCALED]),
      check("No crop box", "info", [NO_CROP_BOX]), pages_differ("info"), empty_page("info")]),
    ("Report fonts", "Reports fonts not embedded, Type 3 fonts, missing glyphs and text without Unicode.",
     [fonts_not_embedded("warning")] + font_checks()[1:] + [check("Text without Unicode", "info", [NO_UNICODE])]),
    ("Report annotations", "Reports annotations that print, carry opacity or lie outside the crop box.",
     [check("Annotation that prints", "info", [ANNOT_PRINTS]), check("Annotation with opacity", "info", [ANNOT_OPACITY]),
      check("Annotation outside the crop box", "info", [c("annotation.outside_crop", "in", ["outside", "zero_size"])])]),
    ("Report layers", "Reports content on layers, hidden layers and layer configurations.",
     [check("Content on a layer", "info", [ON_LAYER]), check("Content on a hidden layer", "info", [ON_LAYER, c("layer.visible", "is", False)]),
      check("Layer configurations present", "info", [c("layer.configuration", "includes", "has_alternate_configs")])]),
    ("Report document health", "Reports encryption, damage, syntax problems, data after the end marker and the PDF version.",
     [check("Data after end of file", "info", [DATA_AFTER_EOF]), encrypted("info"), damaged("warning"), syntax_problems("warning"), version_below(1.7, "info")]),
    ("Report ink coverage", "Reports fills and strokes above 300% total ink and rich black on text.", ink_checks(300)),
    ("Report everything", "Every report in one run: images, colour, ink, hairlines, text, transparency, fonts, pages, annotations, layers and document health.",
     hairline_checks() + small_text_checks() + ink_checks(300) + overprint_checks() + transparency_checks("info") + [
         check("RGB object", "info", [c("color.space", "==", "DeviceRGB")]), check("Spot object", "info", [SPOT]),
         check("Invisible text", "info", [INVISIBLE_TEXT]), check("Content on a layer", "info", [ON_LAYER]),
         check("Annotation that prints", "info", [ANNOT_PRINTS]), check("Rotated page", "info", [ROTATED]),
         image_below(300), image_above(450), fonts_not_embedded("error"), spots_more_than(2, "info"),
         pages_differ("info"), empty_page("info"), encrypted("info"), damaged("warning")]),
]
for name, desc, checks in REPORTS:
    profile("report", name, desc, checks)
for src, name in (("objects", "Report hairlines"), ("objects", "Report small text"), ("objects", "Report invisible text"),
                  ("objects", "Report white objects"), ("objects", "Report rich black")):
    for folder, sl, body in PROFILES:
        if folder == src and body["name"] == name:
            PROFILES.append(("report", sl, dict(body)))
            break
for folder, sl, body in PROFILES:
    if body["name"] == "Image formats: report":
        b = dict(body)
        b["name"] = "Report image formats"
        b["description"] = "Reports the compression filters and bit depths in use."
        PROFILES.append(("report", slug(b["name"]), b))
        break


import extra_presets  # noqa: E402

extra_presets.add_extra(profile, PROFILES)
extra_presets.profiles_ref = PROFILES
extra_presets.add_more(profile)


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
        names = [ch["name"] for ch in body["checks"]]
        if len(names) != len(set(names)):
            raise SystemExit(f"duplicate check name in {folder}/{sl}")
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
