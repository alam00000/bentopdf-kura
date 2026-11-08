import copy

CONDITIONS = [
    ("Coated FOGRA39 (ISO 12647-2:2004)", "coated paper, FOGRA39"),
    ("ISO Coated v2 (ECI)", "coated paper, FOGRA39 (ECI profile)"),
    ("PSO Coated v3", "premium coated paper, FOGRA51"),
    ("Coated FOGRA27 (ISO 12647-2:2004)", "coated paper, FOGRA27"),
    ("Web Coated FOGRA28 (ISO 12647-2:2004)", "web offset on coated paper, FOGRA28"),
    ("Uncoated FOGRA29 (ISO 12647-2:2004)", "uncoated paper, FOGRA29"),
    ("Coated GRACoL 2006 (ISO 12647-2:2004)", "US sheetfed coated paper, GRACoL 2006"),
    ("U.S. Web Coated (SWOP) v2", "US web offset, SWOP v2"),
    ("Web Coated SWOP 2006 Grade 3 Paper", "US web offset, SWOP 2006 grade 3"),
    ("Web Coated SWOP 2006 Grade 5 Paper", "US web offset, SWOP 2006 grade 5"),
    ("Japan Color 2001 Coated", "Japanese coated paper, Japan Color 2001"),
    ("Japan Color 2001 Uncoated", "Japanese uncoated paper, Japan Color 2001"),
    ("Japan Color 2003 Web Coated", "Japanese web offset, Japan Color 2003"),
    ("Japan Color 2002 Newspaper", "Japanese newsprint, Japan Color 2002"),
    ("GRACoL2013_CRPC6", "US sheetfed coated paper, GRACoL 2013 (CGATS21-2 CRPC6)"),
    ("Japan Color 2011 Coated", "Japanese coated paper, Japan Color 2011"),
]

STEPS = [
    ("Cutting", "Cutting", r"die|die[-_ ]?lines?|dieline|die[-_ ]?cut|cut|cutting|cut[-_ ]?lines?|kiss[-_ ]?cut|stanze|stanzkontur|stanzlinie|schnitt|schnittlinie|d[eé]coupe|fustella|troquel|troquelado|corte|contour|kontur"),
    ("Creasing", "Creasing", r"crease|creases|creasing|rill|rillen|rillung|rainage|cordonatura|hendido|plegado"),
    ("Perforating", "Perforating", r"perf|perfo|perforation|perforating|perforierung|perforaci[oó]n|perforazione"),
    ("Folding", "Folding", r"fold|folds|folding|falz|falzlinie|pli|pliage|piega|pliegue"),
    ("Gluing", "Gluing", r"glue|gluing|glue[-_ ]?area|kleber|klebe|leim|leimung|colle|collage|colla|pegado|cola"),
    ("Embossing", "Embossing", r"emboss|embossing|pr[aä]gung|pr[aä]gen|gaufrage|embossage|rilievo|gofrado"),
    ("Foil stamping", "FoilStamping", r"foil|hot[-_ ]?foil|foil[-_ ]?stamping|hei(ss|ß)folie|pr[aä]gefolie|dorure|lamina|stampa a caldo|estampado"),
    ("Drilling", "Drilling", r"drill|drilling|bohrung|bohren|per[cç]age|foratura|taladro"),
    ("Dimensions", "Dimensions", r"dimension|dimensions|measure|measurement|bema(ss|ß)ung|ma(ss|ß)e|cotes|cotation|quote|quotatura|cotas"),
    ("Varnish", "Varnish", r"varnish|spot[-_ ]?varnish|uv[-_ ]?varnish|spot[-_ ]?uv|lack|lacquer|drucklack|vernis|vernice|barniz"),
    ("White", "White", r"white|opaque[-_ ]?white|white[-_ ]?ink|wei(ss|ß)|deckwei(ss|ß)|blanc|bianco|blanco"),
    ("Braille", "Braille", r"braille|blindenschrift|braile"),
    ("Legend", "Legend", r"legend|legende|leyenda|legenda|info[-_ ]?box|technical[-_ ]?info"),
]

STEP_PROBLEMS = [
    ("missing_type", "Structural or Positions step without a type"),
    ("custom_key_not_second_class", "Custom step key is not a second-class name"),
    ("same_step_in_several_layers", "The same step is used on several layers"),
    ("spot_used_by_several_steps", "One spot colour used by several steps"),
    ("spot_used_by_print_content", "A step's spot colour is also used for print content"),
    ("step_uses_several_spots", "One step uses several spot colours"),
    ("layers_missing_on_page", "Step layers missing on a page"),
]

LANGUAGES = [("English", "en"), ("German", "de"), ("French", "fr"), ("Italian", "it"), ("Spanish", "es")]


def add_extra(profile, profiles):
    for desc, what in CONDITIONS:
        for level, label in (("pdfx-1a", "PDF/X-1a"), ("pdfx-3", "PDF/X-3"), ("pdfx-4", "PDF/X-4"), ("pdfx-6", "PDF/X-6")):
            profile("print-conditions", f"Convert to {label} for {desc}",
                    f"Converts to {label} with the {desc} profile as output intent ({what}). The profile is taken from "
                    "Kura's bundled profiles or the computer's colour folders.",
                    convert={"level": level, "profile": desc})
        for a, x, an, xn in (("pdfa-2b", "pdfx-4", "PDF/A-2b", "PDF/X-4"), ("pdfa-4", "pdfx-6", "PDF/A-4", "PDF/X-6")):
            profile("print-conditions", f"Convert to {xn} and {an} for {desc}",
                    f"Makes one file that is both {xn} and {an}, with the {desc} profile ({what}).",
                    convert={"level": a, "also": x, "profile": desc})

    notes = {"1.3": "flattens transparency, merges layers, reduces 16-bit images and re-encodes JPEG 2000 as JPEG",
             "1.4": "merges layers, reduces 16-bit images and re-encodes JPEG 2000 as JPEG",
             "1.5": "writes the file as PDF 1.5", "1.6": "writes the file as PDF 1.6", "1.7": "writes the file as PDF 1.7"}
    for v, note in notes.items():
        profile("compatibility", f"Make compatible with PDF {v}",
                f"Removes what PDF {v} does not support and writes the file as PDF {v}: {note}. No standard is applied.",
                checks=[{"name": f"Newer than PDF {v}", "severity": "info",
                         "all": [{"check": "document.pdf_version", "op": ">", "value": v}]}],
                convert={"pdf_version": v})

    for ppi in (150, 200, 300, 350):
        profile("images", f"Downsample images to {ppi} ppi",
                f"Reduces colour and grey images above {int(ppi * 1.5)} ppi to {ppi} ppi and 1-bit images above "
                f"{ppi * 3} ppi to {ppi * 2} ppi.",
                fixes=[{"fix": "image.downsample", "kind": "colour_and_grey", "target": f"{ppi} ppi", "above": f"{int(ppi * 1.5)} ppi"},
                       {"fix": "image.downsample", "kind": "bitmap", "target": f"{ppi * 2} ppi", "above": f"{ppi * 3} ppi"}])
    profile("images", "Recompress images as JPEG (high quality)",
            "Re-encodes colour and grey images as high-quality JPEG to reduce file size.",
            fixes=[{"fix": "image.recompress", "quality": 90}])
    for ppi in (150, 300):
        profile("pages", f"Turn every page into an image ({ppi} ppi), keeping the text",
                f"Renders each page as a {ppi} ppi image; the original text stays as invisible, searchable text.",
                fixes=[{"fix": "page.rasterize", "resolution": f"{ppi} ppi"}])
    profile("actions", "Flatten annotations and form fields",
            "Draws comments and form fields into the page and removes them as interactive objects.",
            fixes=[{"fix": "annotation.handle", "action": "flatten",
                    "types": ["Text", "FreeText", "Line", "Square", "Circle", "Polygon", "PolyLine", "Highlight", "Underline",
                              "Squiggly", "StrikeOut", "Stamp", "Caret", "Ink", "FileAttachment", "Widget"]}])
    profile("actions", "Put text, images and vector graphics on separate layers",
            "Creates the layers Text, Images and Vector graphics and moves each object onto the matching layer.",
            checks=[{"name": "Text objects", "severity": "info", "all": [{"check": "content.kind", "op": "==", "value": "text"}]},
                    {"name": "Image objects", "severity": "info", "all": [{"check": "content.kind", "op": "==", "value": "image"}]},
                    {"name": "Vector objects", "severity": "info", "all": [{"check": "content.kind", "op": "==", "value": "path"}]}],
            fixes=[{"fix": "layer.move_objects", "layer": "Text", "objects": "Text objects"},
                   {"fix": "layer.move_objects", "layer": "Images", "objects": "Image objects"},
                   {"fix": "layer.move_objects", "layer": "Vector graphics", "objects": "Vector objects"}])
    profile("actions", "Put transparent objects on a layer",
            "Moves every object that uses transparency onto a layer named Transparency.",
            checks=[{"name": "Transparent objects", "severity": "info", "all": [{"check": "transparency.used", "op": "is", "value": True}]}],
            fixes=[{"fix": "layer.move_objects", "layer": "Transparency", "objects": "Transparent objects"}])
    profile("actions", "Put overprinting objects on a layer",
            "Moves every object set to overprint onto a layer named Overprint.",
            checks=[{"name": "Overprinting objects", "severity": "info",
                     "all": [{"check": "color.overprint", "op": "in", "value": ["on", "on_nonzero_mode"]}]}],
            fixes=[{"fix": "layer.move_objects", "layer": "Overprint", "objects": "Overprinting objects"}])
    profile("actions", "Flatten overprint",
            "Paints what overprinting objects leave of the inks underneath, then switches overprint off, so the page looks the same on devices that ignore overprint.",
            fixes=[{"fix": "ink.flatten_overprint"}])

    step_checks, step_fixes = [], []
    for label, step, words in STEPS:
        name = f"Objects in a {label.lower()} spot colour"
        step_checks.append({"name": name, "severity": "info",
                            "all": [{"check": "color.spot", "op": "finds", "value": rf"(^|[\W_])({words})($|[\W_])", "case_sensitive": False}]})
        step_fixes.append({"fix": "layer.move_objects", "layer": label, "processing_step": step, "objects": name})
    profile("packaging", "Add processing-step layers",
            "Finds die lines, creases, perforations, varnish, white, Braille and other technical inks by their spot colour "
            "names (English, German, French, Italian and Spanish) and puts them on processing-step layers (ISO 19593-1).",
            checks=step_checks, fixes=step_fixes)
    profile("packaging", "Prepare processing steps",
            "Merges existing layers, puts technical inks on processing-step layers (ISO 19593-1) and the remaining print "
            "content on a Design layer.",
            checks=step_checks + [{"name": "Objects on no layer", "severity": "info",
                                   "all": [{"check": "layer.membership", "op": "count", "value": 0, "compare": "=="}]}],
            fixes=[{"fix": "layer.flatten"}] + step_fixes + [{"fix": "layer.move_objects", "layer": "Design", "objects": "Objects on no layer"}])
    profile("packaging", "Check processing steps",
            "Checks processing-step layers in packaging and label files against ISO 19593-1.",
            checks=[{"name": n, "severity": "error", "all": [{"check": "layer.processing_step_problem", "op": "includes", "value": v}]}
                    for v, n in STEP_PROBLEMS])
    profile("packaging", "Report processing steps",
            "Lists the processing steps in the document and any problems, without changing it.",
            checks=[{"name": "Processing steps present", "severity": "info",
                     "all": [{"check": "layer.processing_step", "op": "count", "value": 0, "compare": ">"}]},
                    {"name": "Objects not on a processing-step or other layer", "severity": "info",
                     "all": [{"check": "layer.membership", "op": "count", "value": 0, "compare": "=="}]}]
            + [{"name": n, "severity": "warning", "all": [{"check": "layer.processing_step_problem", "op": "includes", "value": v}]}
               for v, n in STEP_PROBLEMS])

    views = []
    for name, code in LANGUAGES:
        others = "|".join(c for _, c in LANGUAGES if c != code)
        views.append({"fix": "layer.add_view", "name": name, "hide": rf"^({others})([\W_].*)?$|[\W_]({others})([\W_].*)?$"})
    profile("standards", "Convert to PDF/X-4 with language layer views",
            "Converts to PDF/X-4 and adds one layer view per language (English, German, French, Italian, Spanish); each view "
            "hides the layers named for the other languages, for example \"de\", \"Text_fr\" or \"es-Copy\".",
            checks=[{"name": "Layer views break PDF/X-4 rules", "severity": "error",
                     "all": [{"check": "layer.configuration", "op": "includes",
                              "value": ["config_missing_name", "config_name_not_unique", "has_as_entry"]}]}],
            fixes=views, convert={"level": "pdfx-4"})

    gwg = {body["name"]: body for folder, _, body in profiles if folder == "gwg"}

    def gwg_body(name):
        if name not in gwg:
            raise SystemExit(f"missing GWG base profile {name}")
        return copy.deepcopy(gwg[name])

    def add_gwg(body):
        profile("gwg", body["name"], body["description"], body["checks"], body.get("fixes", ()), body.get("convert"))
        gwg[body["name"]] = body

    for kind in ("check", "check and fix"):
        web = gwg_body(f"GWG 2022 heatset web offset: {kind}")
        sheet = gwg_body(f"GWG 2022 sheetfed offset, CMYK: {kind}")
        sheet_spot = gwg_body(f"GWG 2022 sheetfed offset with spot colours: {kind}")
        sheet_names = {c["name"] for c in sheet["checks"]}
        spot_names = {c["name"] for c in sheet_spot["checks"]}
        body = copy.deepcopy(web)
        body["checks"] = [c for c in body["checks"] if c["name"] not in sheet_names - spot_names]
        body["checks"] += [copy.deepcopy(c) for c in sheet_spot["checks"]
                           if c["name"] in spot_names - sheet_names and c["name"] not in {x["name"] for x in body["checks"]}]
        body["name"] = f"GWG 2022 web offset with spot colours: {kind}"
        body["description"] = "The Ghent Workgroup 2022 checks for web offset printing with spot colours" + (
            ", with the safe repairs applied, converted to PDF/X-4." if kind == "check and fix" else ".")
        add_gwg(body)

    def with_rgb(base, label):
        for kind in ("check", "check and fix"):
            body = gwg_body(f"{base}: {kind}")
            checks = []
            for c in body["checks"]:
                if c["name"] == "RGB colour":
                    c = {"name": "Uncalibrated RGB colour", "severity": c["severity"],
                         "any": [{"all": [{"check": "color.space", "op": "in", "value": ["DeviceRGB", "CalRGB"]}]}]}
                checks.append(c)
            body["checks"] = checks
            body["name"] = f"{label} (CMYK + RGB): {kind}"
            body["description"] = body["description"].rstrip(".") + ". Colour-managed RGB (with an ICC profile) is accepted; uncalibrated RGB is not."
            add_gwg(body)

    with_rgb("GWG 2022 sheetfed offset, CMYK", "GWG 2022 sheetfed offset")
    with_rgb("GWG 2022 sheetfed offset with spot colours", "GWG 2022 sheetfed offset with spot colours")
    with_rgb("GWG 2022 heatset web offset", "GWG 2022 heatset web offset")
    with_rgb("GWG 2022 coldset web offset", "GWG 2022 coldset web offset")
    with_rgb("GWG 2022 web offset with spot colours", "GWG 2022 web offset with spot colours")

    one_page = {"name": "More than one page", "severity": "error", "all": [{"check": "document.page_count", "op": ">", "value": 1}]}
    for base, label in (("GWG 2022 heatset web offset", "GWG 2022 magazine ads"), ("GWG 2022 coldset web offset", "GWG 2022 newspaper ads")):
        for rgb in (False, True):
            for kind in ("check", "check and fix"):
                src = f"{base} (CMYK + RGB): {kind}" if rgb else f"{base}: {kind}"
                body = gwg_body(src)
                body["checks"] = [c for c in body["checks"] if "page size" not in c["name"].lower() and "rotat" not in c["name"].lower()] + [copy.deepcopy(one_page)]
                body["name"] = f"{label}{' (CMYK + RGB)' if rgb else ', CMYK'}: {kind}"
                body["description"] = body["description"] + " A single-page advertisement is expected."
                add_gwg(body)


CORE = [
    ("Coated FOGRA39 (ISO 12647-2:2004)", "FOGRA39"),
    ("ISO Coated v2 (ECI)", "ISO Coated v2"),
    ("PSO Coated v3", "PSO Coated v3"),
    ("Web Coated FOGRA28 (ISO 12647-2:2004)", "FOGRA28"),
    ("Uncoated FOGRA29 (ISO 12647-2:2004)", "FOGRA29"),
    ("Coated GRACoL 2006 (ISO 12647-2:2004)", "GRACoL 2006"),
    ("U.S. Web Coated (SWOP) v2", "SWOP v2"),
    ("Japan Color 2001 Coated", "Japan Color 2001 Coated"),
]


def add_more(profile):
    for desc, what in CONDITIONS:
        profile("colour-conversion", f"Convert colours to {desc}, keeping spot colours",
                f"Converts RGB, calibrated and Lab colour to CMYK for {what} and sets it as the output intent; spot colours stay "
                "spot colours. No standard is applied.",
                convert={"colors": {"to": desc, "keep_spots": True}})
        profile("colour-conversion", f"Convert all colours to {desc} (CMYK only)",
                f"Converts every colour, spot colours included, to CMYK for {what}. No standard is applied.",
                convert={"colors": {"to": desc, "keep_spots": False}})
    for limit, use in ((240, "newspapers"), (260, "coldset and uncoated web"), (280, "uncoated sheetfed"),
                       (300, "coated sheetfed and heatset web"), (320, "coated sheetfed with a heavy press"),
                       (340, "gravure")):
        profile("colour-conversion", f"Reduce total ink to {limit}%",
                f"Lowers total ink coverage to at most {limit}% (typical for {use}) by taking cyan, magenta and yellow "
                "out first and keeping black; colour below the limit is left as it is and spot colours stay spot colours. "
                "RGB and other colour is converted to CMYK on the way. No standard is applied.",
                convert={"colors": {"to": "cmyk", "keep_spots": True, "ink_limit": limit}})
    profile("colour-conversion", "Convert RGB to CMYK, neutral greys to black only",
            "Converts RGB to CMYK (PSO Coated v3) and prints greys whose red, green and blue are within 1% of each other "
            "with black ink only. Spot colours stay spot colours. No standard is applied.",
            convert={"colors": {"to": "PSO Coated v3", "keep_spots": True, "rgb_gray_to_black": 1}})
    profile("colour-conversion", "Lower midtones by 10%",
            "Applies a tone curve to CMYK colour that prints 50% tones as 40%, leaving paper white and solids unchanged. "
            "No standard is applied.",
            convert={"colors": {"to": "cmyk", "keep_spots": True, "tone_curve": [0, 0, 50, 40, 100, 100]}})
    profile("colour-conversion", "Convert registration colour to black only",
            "Replaces the registration colour (all inks) with 100% black.",
            fixes=[{"fix": "spot.registration_to_black"}])
    profile("colour-conversion", "Convert spot colours to CMYK, keeping the look of overprints",
            "Flattens overprint first, so overprinting spot colours keep their appearance, then converts every colour to "
            "CMYK. No standard is applied.",
            fixes=[{"fix": "ink.flatten_overprint"}],
            convert={"colors": {"to": "cmyk", "keep_spots": False}})
    profile("colour-conversion", "Replace one colour with another",
            "Replaces every use of the colour given in {from} with the colour in {to}, written like cmyk 0 100 100 0, rgb 255 0 0 or gray 50.",
            fixes=[{"fix": "color.map", "from": "{from}", "to": "{to}"}])
    profiles_ref[-1][2]["variables"] = [{"name": "from", "label": "Colour to replace", "default": "cmyk 0 100 100 0", "type": "text"},
                                       {"name": "to", "label": "Replacement colour", "default": "cmyk 0 95 100 0", "type": "text"}]
    profile("report", "Overprinting spot colours that change when converted to CMYK",
            "Lists objects in a spot colour that are set to overprint; their look changes when the spot colour becomes CMYK.",
            checks=[{"name": "Overprinting spot colour", "severity": "warning",
                     "all": [{"check": "color.overprint", "op": "in", "value": ["on", "on_nonzero_mode"]},
                             {"check": "color.ink_mix", "op": "in", "value": ["spot_only", "spot_and_black", "process_and_spot"]}]}])
    profile("colour-conversion", "Convert all colours to grey",
            "Converts every colour to greyscale (Dot Gain 15%). No standard is applied.",
            convert={"colors": {"to": "gray"}})
    profile("colour-conversion", "Convert all colours to sRGB",
            "Converts every colour to sRGB, for screens and the web. No standard is applied.",
            convert={"colors": {"to": "rgb"}})
    for src, sname in CORE:
        for dst, dname in CORE:
            if src == dst:
                continue
            profile("colour-conversion", f"Convert CMYK from {sname} to {dname}",
                    f"Re-separates CMYK made for {src} so it prints the same on {dst}, through both profiles; spot colours "
                    "stay spot colours and the new profile becomes the output intent.",
                    convert={"colors": {"from": src, "to": dst, "keep_spots": True}})

    profile("place-content", "Place the date on every page",
            "Places today's date (YYYY-MM-DD) in the lower right corner of every page.",
            fixes=[{"fix": "place.text", "text": "{date}", "size": "8pt", "anchor": "lower_right", "x": "-10mm", "y": "5mm"}])
    profile("place-content", "Place the file name in the gutter",
            "Places the document's file name along the left edge of every page.",
            fixes=[{"fix": "place.text", "text": "{file}", "size": "7pt", "anchor": "left", "x": "4mm", "y": "0mm", "rotation": "90"}])
    profile("place-content", "Place page numbers in the outer corner",
            "Numbers the pages in the outer lower corner: right on odd pages, left on even pages.",
            fixes=[{"fix": "place.text", "text": "{page}", "size": "9pt", "anchor": "lower_right", "x": "-12mm", "y": "8mm", "pages": "odd"},
                   {"fix": "place.text", "text": "{page}", "size": "9pt", "anchor": "lower_left", "x": "12mm", "y": "8mm", "pages": "even"}])
    vars_text = [{"name": "text", "label": "Text", "type": "text", "default": "SAMPLE"}]
    vars_job = [{"name": "job", "label": "Job number", "type": "text", "default": "0000"}]
    vars_ean = [{"name": "barcode", "label": "EAN-13 number", "type": "text", "default": "4006381333931"}]
    vars_qr = [{"name": "qr", "label": "QR code content", "type": "text", "default": "https://example.com"}]
    for name, desc, var, fixes in (
        ("Place a text in the centre of every page", "Places a text of your choice in the centre of every page.", vars_text,
         [{"fix": "place.text", "text": "{text}", "size": "24pt", "anchor": "center", "x": "0mm", "y": "0mm"}]),
        ("Place the job number in the gutter", "Places a job number of your choice along the left edge of every page.", vars_job,
         [{"fix": "place.text", "text": "Job {job}", "size": "7pt", "anchor": "left", "x": "4mm", "y": "0mm", "rotation": "90"}]),
        ("Place an EAN-13 barcode", "Places an EAN-13 barcode with the number you give in the lower right corner of the first page.", vars_ean,
         [{"fix": "place.barcode", "symbology": "ean13", "data": "{barcode}", "width": "37.29mm", "height": "25.93mm",
           "anchor": "lower_right", "x": "-10mm", "y": "10mm", "pages": "1"}]),
        ("Place a QR code", "Places a QR code with the content you give in the lower right corner of the first page.", vars_qr,
         [{"fix": "place.barcode", "symbology": "qr", "data": "{qr}", "width": "20mm", "height": "20mm",
           "anchor": "lower_right", "x": "-10mm", "y": "10mm", "pages": "1"}])):
        profile("place-content", name, desc, fixes=fixes)
        profiles_ref[-1][2]["variables"] = var

    profile("plans", "Check for PDF/A-2b and convert",
            "Reports what keeps the file from PDF/A-2b, then converts it.",
            checks=[{"name": "Not PDF/A-2b", "severity": "error", "all": [{"check": "conformance.meets", "op": "is", "value": False, "standard": "pdfa-2b"}]}],
            convert={"level": "pdfa-2b"})
    profile("plans", "Report non-CMYK colour, convert to CMYK and flatten transparency",
            "Lists RGB, Lab and calibrated colour, converts everything to CMYK for Coated FOGRA39 (spot colours kept) and flattens transparency.",
            checks=[{"name": "RGB colour", "severity": "warning", "all": [{"check": "color.space", "op": "in", "value": ["DeviceRGB", "CalRGB"]}]},
                    {"name": "Lab colour", "severity": "warning", "all": [{"check": "color.space", "op": "==", "value": "Lab"}]}],
            fixes=[{"fix": "transparency.flatten"}],
            convert={"colors": {"to": "Coated FOGRA39 (ISO 12647-2:2004)", "keep_spots": True}})
    profile("plans", "Turn very complex pages into CMYK images",
            "Pages with a path of more than 10,000 points are rendered as 300 ppi images (text kept invisible) and converted to CMYK for Coated FOGRA39.",
            checks=[{"name": "Very complex page", "severity": "info", "all": [{"check": "content.path_measure", "op": ">", "value": 10000, "measure": "points"}]}],
            fixes=[{"fix": "page.rasterize", "resolution": "300 ppi", "only_where": ["Very complex page"]}],
            convert={"colors": {"to": "Coated FOGRA39 (ISO 12647-2:2004)", "keep_spots": False}})
    profile("plans", "Generate 3 mm bleed",
            "Adds 3 mm of bleed by mirroring the content at the trim edge where the page has none.",
            fixes=[{"fix": "bleed.generate", "amount": "3mm", "method": "mirror"}])
    profile("plans", "Prepare for A4",
            "Scales every page to A4.",
            fixes=[{"fix": "page.scale", "width": "210mm", "height": "297mm"}])


profiles_ref = []
