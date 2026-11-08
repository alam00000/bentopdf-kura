#!/usr/bin/env python3
import base64
import os
import re
import sys
import zlib

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(ROOT, "third_party", "cmap-resources")
OUT = os.path.join(ROOT, "core", "assets", "cjk_cmaps.cpp")
ORDERINGS = ["GB1", "CNS1", "Japan1", "Korea1"]
NAMES = [
    "GB-EUC", "GBpc-EUC", "GBK-EUC", "GBKp-EUC", "GBK2K", "UniGB-UCS2", "UniGB-UTF16",
    "B5pc", "HKscs-B5", "ETen-B5", "ETenms-B5", "CNS-EUC", "UniCNS-UCS2", "UniCNS-UTF16",
    "83pv-RKSJ", "90ms-RKSJ", "90msp-RKSJ", "90pv-RKSJ", "Add-RKSJ", "EUC", "Ext-RKSJ",
    "UniJIS-UCS2", "UniJIS-UCS2-HW", "UniJIS-UTF16",
    "KSC-EUC", "KSCms-UHC", "KSCms-UHC-HW", "KSCpc-EUC", "UniKS-UCS2", "UniKS-UTF16",
]


def find(name):
    for d in sorted(os.listdir(SRC)):
        p = os.path.join(SRC, d, "CMap", name)
        if os.path.isfile(p):
            return p
    return None


def parse(path):
    text = open(path, encoding="latin-1").read()
    text = "\n".join(l for l in text.splitlines() if not l.startswith("%"))
    info = {
        "ordering": re.search(r"/Ordering \((\w+)\)", text).group(1),
        "supplement": int(re.search(r"/Supplement (\d+)", text).group(1)),
        "wmode": int(m.group(1)) if (m := re.search(r"/WMode (\d)", text)) else 0,
        "use": m.group(1) if (m := re.search(r"/([\w-]+)\s+usecmap", text)) else None,
        "spaces": [],
        "cids": {},
        "notdefs": {},
    }
    hexnum = r"<([0-9a-fA-F]+)>"
    for block in re.findall(r"begincodespacerange(.*?)endcodespacerange", text, re.S):
        for lo, hi in re.findall(hexnum + r"\s*" + hexnum, block):
            info["spaces"].append((len(lo) // 2, int(lo, 16), int(hi, 16)))
    for kind, key in (("cid", "cids"), ("notdef", "notdefs")):
        for block in re.findall(r"begin%srange(.*?)end%srange" % (kind, kind), text, re.S):
            for lo, hi, cid in re.findall(hexnum + r"\s*" + hexnum + r"\s*(\d+)", block):
                lo, hi, cid = int(lo, 16), int(hi, 16), int(cid)
                for c in range(lo, hi + 1):
                    info[key][c] = cid + c - lo
        for block in re.findall(r"begin%schar(.*?)end%schar" % (kind, kind), text, re.S):
            for c, cid in re.findall(hexnum + r"\s*(\d+)", block):
                info[key][int(c, 16)] = int(cid)
    return info


def varint(v, out):
    while v >= 0x80:
        out.append((v & 0x7F) | 0x80)
        v >>= 7
    out.append(v)


def runs(mapping):
    res = []
    for code in sorted(mapping):
        cid = mapping[code]
        if res and res[-1][1] + 1 == code and res[-1][2] + code - res[-1][0] == cid:
            res[-1][1] = code
        else:
            res.append([code, code, cid])
    return res


def emit_runs(rs, out):
    varint(len(rs), out)
    prev_hi, prev_cid = -1, 0
    for lo, hi, cid in rs:
        varint(lo - prev_hi - 1, out)
        varint(hi - lo, out)
        d = cid - prev_cid
        varint(d * 2 if d >= 0 else -d * 2 - 1, out)
        prev_hi, prev_cid = hi, cid + hi - lo + 1


def main():
    entries = []
    for base in NAMES + [""]:
        for d in ("H", "V"):
            name = base + "-" + d if base else d
            path = find(name)
            if path:
                entries.append((name, parse(path)))
    index = {n: i for i, (n, _) in enumerate(entries)}
    out = bytearray()
    varint(len(entries), out)
    for name, info in entries:
        out.append(len(name))
        out += name.encode("ascii")
        out.append(ORDERINGS.index(info["ordering"]))
        out.append(info["supplement"])
        out.append(info["wmode"])
        varint(index[info["use"]] + 1 if info["use"] else 0, out)
        base = name.replace("UTF16", "UCS2") if "UTF16" in name else None
        varint(index[base] + 1 if base in index else 0, out)
        if base in index:
            own = entries[index[base]][1]
            for key in ("cids", "notdefs"):
                info[key] = {c: v for c, v in info[key].items() if own[key].get(c) != v}
        varint(len(info["spaces"]), out)
        for nb, lo, hi in info["spaces"]:
            out.append(nb)
            varint(lo, out)
            varint(hi, out)
        emit_runs(runs(info["notdefs"]), out)
        emit_runs(runs(info["cids"]), out)
    packed = zlib.compress(bytes(out), 9)
    text = base64.b64encode(packed).decode("ascii")
    lines = ["#include \"cjk_cmaps.hh\"", "", "namespace pdfa {", "", "const char* const kCjkCmaps[] = {"]
    for i in range(0, len(text), 100):
        lines.append("    \"%s\"," % text[i:i + 100])
    lines += ["    nullptr};", "", "const unsigned int kCjkCmapsLen = %d;" % len(packed),
              "const unsigned int kCjkCmapsRawLen = %d;" % len(out), "", "}", ""]
    with open(OUT, "w") as f:
        f.write("\n".join(lines))
    print("%d CMaps, %d bytes raw, %d packed" % (len(entries), len(out), len(packed)), file=sys.stderr)


if __name__ == "__main__":
    main()
