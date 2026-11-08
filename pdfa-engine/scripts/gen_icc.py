#!/usr/bin/env python3
import math
import os
import struct
import sys

OUT = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "core", "icc")
D50 = (0xF6D6, 0x10000, 0xD32D)
NOTICE = "No rights reserved. Made by Kura from published formulas; free to use, copy and embed."


def s15(v):
    return struct.pack(">i", int(math.floor(v * 65536 + 0.5)))


def xyz_raw(x, y, z):
    return b"XYZ " + b"\0" * 4 + struct.pack(">iii", x, y, z)


def xyz(v):
    return b"XYZ " + b"\0" * 4 + b"".join(s15(c) for c in v)


def gamma(g):
    return b"curv" + b"\0" * 4 + struct.pack(">IH", 1, int(math.floor(g * 256 + 0.5)))


def table(values):
    return b"curv" + b"\0" * 4 + struct.pack(">I", len(values)) + b"".join(struct.pack(">H", v) for v in values)


def text(s):
    return b"text" + b"\0" * 4 + s.encode("ascii") + b"\0"


def desc(s):
    a = s.encode("ascii") + b"\0"
    return b"desc" + b"\0" * 4 + struct.pack(">I", len(a)) + a + b"\0" * 8 + b"\0" * 3 + b"\0" * 67


def mluc(s):
    u = s.encode("utf-16-be")
    return b"mluc" + b"\0" * 4 + struct.pack(">II", 1, 12) + b"enUS" + struct.pack(">II", len(u), 28) + u


def para0(g):
    return b"para" + b"\0" * 4 + struct.pack(">HH", 0, 0) + s15(g)


def mft(kind, ni, no, grid, matrix, intabs, clut, outtabs):
    head = (b"mft1" if kind == 1 else b"mft2") + b"\0" * 4 + bytes([ni, no, grid, 0])
    head += b"".join(struct.pack(">i", m) for m in matrix)
    if kind == 1:
        return head + bytes(sum(intabs, [])) + bytes(clut) + bytes(sum(outtabs, []))
    head += struct.pack(">HH", len(intabs[0]), len(outtabs[0]))
    pack = lambda vals: b"".join(struct.pack(">H", v) for v in vals)
    return head + pack(sum(intabs, [])) + pack(clut) + pack(sum(outtabs, []))


def profile(cls, space, pcs, tags, version=0x02100000, intent=0, illuminant=D50):
    tags = [(sig, data) for sig, data in tags]
    n = len(tags)
    offset = 128 + 4 + 12 * n
    table_ = b""
    body = b""
    for sig, data in tags:
        while (offset + len(body)) % 4:
            body += b"\0"
        table_ += sig.encode("ascii") + struct.pack(">II", offset + len(body), len(data))
        body += data
    while (offset + len(body)) % 4:
        body += b"\0"
    size = offset + len(body)
    header = struct.pack(">I", size) + b"\0\0\0\0" + struct.pack(">I", version) + cls + space + pcs
    header += struct.pack(">HHHHHH", 2026, 11, 7, 0, 0, 0) + b"acsp" + b"\0" * 4 + b"\0" * 4
    header += b"\0" * 4 + b"\0" * 4 + b"\0" * 8 + struct.pack(">I", intent) + struct.pack(">iii", *illuminant) + b"\0" * 4
    header += b"\0" * 16 + b"\0" * 28
    return header + struct.pack(">I", n) + table_ + body


def dot_gain(gain):
    g = math.log(0.5 - gain) / math.log(0.5)
    return [int(math.floor(65535 * max((j / 255) ** g, (j / 255) / 16) + 0.5)) for j in range(256)]


def build():
    files = {}
    for pct, stem in ((10, "Dot Gain 10%"), (15, "Dot Gain 15%"), (20, "Dot Gain 20%"), (25, "Dot Gain 25%"), (30, "Dot Gain 30%")):
        files[stem] = profile(b"prtr", b"GRAY", b"XYZ ", [
            ("desc", desc(stem)), ("cprt", text(NOTICE)), ("wtpt", xyz_raw(*D50)), ("bkpt", xyz_raw(0, 0, 0)),
            ("kTRC", table(dot_gain(pct / 100)))])
    for g, stem in ((461 / 256, "Gray Gamma 1.8"), (563 / 256, "Gray Gamma 2.2")):
        files[stem] = profile(b"mntr", b"GRAY", b"XYZ ", [
            ("desc", desc(stem)), ("cprt", text(NOTICE)), ("wtpt", xyz_raw(0xF354, 0x10000, 0x116CF)),
            ("bkpt", xyz_raw(0, 0, 0)), ("kTRC", gamma(g))])
    romm = [(0.7347, 0.2653), (0.1596, 0.8404), (0.0366, 0.0001)]
    w = [0.9642, 1.0, 0.8249]
    P = [[x / y for x, y in romm], [1, 1, 1], [(1 - x - y) / y for x, y in romm]]
    a, b, c = P[0]
    d, e, f = P[1]
    gg, h, i = P[2]
    det = a * (e * i - f * h) - b * (d * i - f * gg) + c * (d * h - e * gg)
    inv = [[(e * i - f * h) / det, (c * h - b * i) / det, (b * f - c * e) / det],
           [(f * gg - d * i) / det, (a * i - c * gg) / det, (c * d - a * f) / det],
           [(d * h - e * gg) / det, (b * gg - a * h) / det, (a * e - b * d) / det]]
    S = [sum(inv[r][k] * w[k] for k in range(3)) for r in range(3)]
    col = [[P[r][k] * S[k] for r in range(3)] for k in range(3)]
    files["ProPhoto RGB"] = profile(b"mntr", b"RGB ", b"XYZ ", [
        ("desc", desc("ROMM RGB (ISO 22028-2)")), ("cprt", text(NOTICE)), ("wtpt", xyz_raw(0xF6D6, 0x10000, 0xD32C)),
        ("rTRC", gamma(461 / 256)), ("gTRC", gamma(461 / 256)), ("bTRC", gamma(461 / 256)),
        ("rXYZ", xyz(col[0])), ("gXYZ", xyz(col[1])), ("bXYZ", xyz(col[2]))], illuminant=(0xF6D6, 0x10000, 0xD32B))
    ident = list(range(256))
    cube = [0, 0, 0, 0, 0, 255, 0, 255, 0, 0, 255, 255, 255, 0, 0, 255, 0, 255, 255, 255, 0, 255, 255, 255]
    lab = mft(1, 3, 3, 2, [65536, 0, 0, 0, 65536, 0, 0, 0, 65536], [ident] * 3, cube, [ident] * 3)
    files["Lab D50"] = profile(b"spac", b"Lab ", b"Lab ", [
        ("desc", desc("Lab D50")), ("cprt", text(NOTICE)), ("wtpt", xyz_raw(0xF6D6, 0x10000, 0xD32C)), ("A2B0", lab), ("B2A0", lab)], illuminant=(0xF6D6, 0x10000, 0xD32C))
    cube16 = [0, 0, 0, 0, 0, 0xFFFF, 0, 0xFFFF, 0, 0, 0xFFFF, 0xFFFF, 0xFFFF, 0, 0, 0xFFFF, 0, 0xFFFF, 0xFFFF, 0xFFFF, 0, 0xFFFF, 0xFFFF, 0xFFFF]
    ramp = [[0, 0xFFFF]] * 3
    pcs = mft(2, 3, 3, 2, [65536, 0, 0, 0, 65536, 0, 0, 0, 65536], ramp, cube16, ramp)
    files["PCS XYZ"] = profile(b"spac", b"XYZ ", b"XYZ ", [
        ("desc", desc("PCS XYZ")), ("cprt", text(NOTICE)), ("wtpt", xyz_raw(0xF6D6, 0x10000, 0xD32C)), ("A2B0", pcs), ("B2A0", pcs)], illuminant=(0xF6D6, 0x10000, 0xD32C))
    fa = mft(2, 3, 3, 2, [0xF6D6, 0, 0, 0, 0x10000, 0, 0, 0, 0xD32C], ramp, cube16, ramp)
    fb = mft(2, 3, 3, 2, [0x10981, 0, 0, 0, 0x10000, 0, 0, 0, 0x13658], ramp, cube16, ramp)
    files["Flat XYZ"] = profile(b"spac", b"XYZ ", b"XYZ ", [
        ("desc", desc("Flat XYZ")), ("cprt", text(NOTICE)), ("wtpt", xyz_raw(0xF6D6, 0x10000, 0xD32C)), ("A2B0", fa), ("B2A0", fb)], illuminant=(0xF6D6, 0x10000, 0xD32C))
    bw_out = [255 if j >= 254 else 0 for j in range(256)]
    bw = mft(1, 3, 1, 2, [65536, 0, 0, 0, 131072, 0, 0, 0, 65536], [ident] * 3, [0, 0, 255, 255, 0, 0, 255, 255], [bw_out])
    files["Black & White"] = profile(b"prtr", b"GRAY", b"XYZ ", [
        ("desc", mluc("Black & White")), ("cprt", mluc(NOTICE)), ("wtpt", xyz_raw(0xF6D6, 0x10000, 0xD32D)),
        ("kTRC", para0(1.8)), ("B2A0", bw)], version=0x04000000, intent=1)
    return files


if __name__ == "__main__":
    os.makedirs(OUT, exist_ok=True)
    for stem, data in build().items():
        with open(os.path.join(OUT, stem + ".icc"), "wb") as f:
            f.write(data)
        print(f"{stem}.icc  {len(data)} bytes")
