#!/usr/bin/env python3
import argparse
import json
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CATALOGUE = os.path.join(ROOT, "core", "registry", "catalogue.json")
INC = os.path.join(ROOT, "core", "src", "registry_data.inc")
DOCS = os.path.join(ROOT, "docs", "profile-checks.md")

DOMAINS = {
    "document": "kDoc",
    "barcode": "kDoc",
    "structure_element": "kDoc",
    "page": "kPage",
    "annotation": "kAnnot",
    "font": "kFont",
}
OBJECT_GROUPS = {"text": "kText", "image": "kImage"}


def domain(check):
    applies = check["applies_to"]
    if applies == "object":
        return OBJECT_GROUPS.get(check["group"], "kAny")
    if applies not in DOMAINS:
        sys.exit(f"{check['id']}: unknown applies_to {applies!r}")
    return DOMAINS[applies]


def text(v):
    if isinstance(v, bool):
        return "true" if v else "false"
    if isinstance(v, list):
        return "\n".join(text(x) for x in v)
    if isinstance(v, float) and v.is_integer():
        return str(int(v))
    return str(v)


def cstr(s):
    out = ['"']
    for ch in s:
        if ch in '"\\':
            out.append("\\" + ch)
        elif ch == "\n":
            out.append("\\n")
        elif ord(ch) < 32:
            out.append("\\%03o" % ord(ch))
        else:
            out.append(ch)
    out.append('"')
    return "".join(out)


def ident(s):
    return "".join(c if c.isalnum() else "_" for c in s)


def params_block(prefix, item, value_types):
    rows = []
    for p in item.get("params", []):
        if p["type"] not in value_types:
            sys.exit(f"{item['id']}.{p['name']}: unknown parameter type {p['type']!r}")
        has = "default" in p
        rows.append("    {%s, %s, %s, %s, %s, %s}," % (
            cstr(p["name"]), cstr(p["type"]), cstr(text(p["default"]) if has else ""),
            "true" if has else "false", "true" if p.get("required") else "false",
            cstr("|".join(text(e) for e in p.get("enum", [])))))
    if not rows:
        return None, ""
    name = "kParams_%s_%s" % (prefix, ident(item["id"]))
    return name, "static const ParamDef %s[] = {\n%s\n};\n" % (name, "\n".join(rows))


def gen_inc(cat):
    vt = cat["value_types"]
    ops = [o for o in cat["operators"] if o != "atom options"]
    out = ["static const char* const kOperators[] = {" + ", ".join(cstr(o) for o in ops) + "};", ""]
    out.append("static const char* const kValueTypes[] = {" + ", ".join(cstr(v) for v in vt) + "};")
    out.append("")
    checks, fixes = [], []
    seen = set()
    for c in cat["checks"]:
        if c["id"] in seen:
            sys.exit(f"duplicate check id {c['id']}")
        seen.add(c["id"])
        if c["value"] not in vt:
            sys.exit(f"{c['id']}: unknown value type {c['value']!r}")
        for o in c["operators"]:
            if o not in ops:
                sys.exit(f"{c['id']}: unknown operator {o!r}")
        name, block = params_block("c", c, vt)
        if block:
            out.append(block)
        checks.append("    {%s, %s, %s, %s, Domain::%s, %s, %s, %d}," % (
            cstr(c["id"]), cstr(c["group"]), cstr(c["title"]), cstr(c["value"]), domain(c),
            cstr("|".join(c["operators"])), name or "nullptr", len(c.get("params", []))))
    for f in cat["fixes"]:
        if f["id"] in seen:
            sys.exit(f"duplicate id {f['id']}")
        seen.add(f["id"])
        name, block = params_block("f", f, vt)
        if block:
            out.append(block)
        fixes.append("    {%s, %s, %s, %s, %d}," % (
            cstr(f["id"]), cstr(f["group"]), cstr(f["title"]), name or "nullptr",
            len(f.get("params", []))))
    out.append("static const CheckDef kChecks[] = {\n%s\n};\n" % "\n".join(checks))
    out.append("static const FixDef kFixes[] = {\n%s\n};\n" % "\n".join(fixes))
    codes = [(k, v) for k, v in cat["output_codes"].items() if k.isupper()]
    out.append("static const OutputCodeDef kOutputCodes[] = {\n%s\n};" % "\n".join(
        "    {%s, %s}," % (cstr(k), cstr(v)) for k, v in codes))
    return "\n".join(out) + "\n"


def md_cell(s):
    return str(s).replace("|", "\\|").replace("\n", " ")


def md_params(item):
    ps = item.get("params", [])
    if not ps:
        return ["No parameters.", ""]
    lines = ["| Parameter | Type | Default | Values | Notes |", "|---|---|---|---|---|"]
    for p in ps:
        if "default" in p:
            dflt = "`%s`" % text(p["default"]).replace("\n", ", ")
        elif p.get("required"):
            dflt = "required"
        else:
            dflt = "empty"
        note = " ".join(x for x in (p.get("description", ""), p.get("default_reason", "")) if x)
        lines.append("| `%s` | %s | %s | %s | %s |" % (
            p["name"], p["type"], dflt, md_cell(", ".join(text(e) for e in p.get("enum", []))), md_cell(note)))
    return lines + [""]


def gen_docs(cat):
    out = ["# Kura profile checks and fixes", "",
           "Generated from `core/registry/catalogue.json` by `scripts/gen_registry.py`. Do not edit by hand.", "",
           "A condition names a check, an operator and a value, plus any named parameters:", "",
           "```json", '{"check": "page.box_size", "box": "trim", "dimension": "width", "op": ">=", "value": "210mm"}', "```", "",
           "Parameters left out take the default shown below. Unknown checks, parameters, operators or",
           "enumeration values make the profile invalid (output code `PROFILE_INVALID`).", "",
           "## Operators", "", "| Operator | Meaning |", "|---|---|"]
    for k, v in cat["operators"].items():
        if isinstance(v, dict):
            continue
        out.append("| `%s` | %s |" % (md_cell(k), md_cell(v)))
    out += ["", "Every condition also accepts:", ""]
    for k, v in cat["operators"].get("atom options", {}).items():
        out.append("- `%s`: %s" % (k, v))
    out += ["", "## Value types", "", "| Type | Meaning |", "|---|---|"]
    for k, v in cat["value_types"].items():
        out.append("| `%s` | %s |" % (k, md_cell(v)))
    out.append("")
    groups = {g["id"]: g for g in cat["groups"]}
    out += ["## Checks", ""]
    for gid, g in groups.items():
        items = [c for c in cat["checks"] if c["group"] == gid]
        if not items:
            continue
        out += ["### %s" % g["title"], "", g["description"], ""]
        for c in items:
            out += ["#### `%s`: %s" % (c["id"], c["title"]), "", c["description"], "",
                    "- Value: `%s`" % c["value"],
                    "- Operators: %s" % ", ".join("`%s`" % o for o in c["operators"]),
                    "- Applies to: %s" % c["applies_to"].replace("_", " ")]
            if c.get("standards"):
                out.append("- References: %s" % "; ".join(c["standards"]))
            if c.get("notes"):
                out.append("- Notes: %s" % c["notes"])
            out.append("")
            out += md_params(c)
    out += ["## Fixes", ""]
    for gid, g in groups.items():
        items = [f for f in cat["fixes"] if f["group"] == gid]
        if not items:
            continue
        out += ["### %s" % g["title"], ""]
        for f in items:
            out += ["#### `%s`: %s" % (f["id"], f["title"]), "", f["description"], ""]
            if f.get("standards"):
                out += ["- References: %s" % "; ".join(f["standards"]), ""]
            if f.get("notes"):
                out += ["- Notes: %s" % f["notes"], ""]
            out += md_params(f)
    out += ["## Output codes", "", "| Code | Meaning |", "|---|---|"]
    for k, v in cat["output_codes"].items():
        if k.isupper():
            out.append("| `%s` | %s |" % (k, md_cell(v)))
    if "hit_message" in cat["output_codes"]:
        out += ["", "Hit messages read: %s" % cat["output_codes"]["hit_message"]]
    return "\n".join(out) + "\n"


def write(path, content, check):
    old = open(path, encoding="utf-8").read() if os.path.exists(path) else None
    if old == content:
        return True
    if check:
        print(f"{path} is out of date; run scripts/gen_registry.py", file=sys.stderr)
        return False
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", encoding="utf-8") as f:
        f.write(content)
    return True


def main():
    ap = argparse.ArgumentParser(description="Generate the profile registry table and reference from the catalogue.")
    ap.add_argument("--catalogue", default=CATALOGUE)
    ap.add_argument("--inc", default=INC)
    ap.add_argument("--docs", default=DOCS)
    ap.add_argument("--check", action="store_true", help="fail when the generated files are out of date")
    a = ap.parse_args()
    cat = json.load(open(a.catalogue, encoding="utf-8"))
    ok = write(a.inc, gen_inc(cat), a.check)
    ok = write(a.docs, gen_docs(cat), a.check) and ok
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
