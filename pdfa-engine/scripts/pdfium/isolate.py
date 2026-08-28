#!/usr/bin/env python3
import glob
import os
import subprocess
import sys

STRONG = ("T", "D", "B", "S", "R")
PREFIX = "pdfium_"

ENGINE_LIBS = {
    "mac": [
        "/opt/homebrew/opt/qpdf/lib/libqpdf.a",
        "/opt/homebrew/opt/freetype/lib/libfreetype.a",
        "/opt/homebrew/opt/libpng/lib/libpng.a",
        "/opt/homebrew/opt/openjpeg/lib/libopenjp2.a",
        "/opt/homebrew/opt/jpeg-turbo/lib/libjpeg.a",
        "/opt/homebrew/opt/little-cms2/lib/liblcms2.a",
        "/opt/homebrew/opt/openssl@3/lib/libcrypto.a",
    ],
    "linux": [
        "~/qpdf-install/lib/libqpdf.a", "~/deps-install/lib/libfreetype.a",
        "~/deps-install/lib/liblcms2.a", "~/deps-install/lib/libopenjp2.a",
        "/usr/lib/*/libqpdf.a", "/usr/lib/*/libfreetype.a",
        "/usr/lib/*/libpng*.a", "/usr/lib/*/libopenjp2.a",
        "/usr/lib/*/libjpeg.a", "/usr/lib/*/liblcms2.a",
        "/usr/lib/*/libcrypto.a", "/usr/lib/*/libz.a",
    ],
    "win": ["C:/vcpkg/installed/x64-windows-static/lib/*.lib"],
    "wasm": [
        "third_party/build-qpdf-wasm/libqpdf/libqpdf.a",
        "third_party/build-openjpeg-wasm/bin/libopenjp2.a",
        "third_party/build-lcms2-wasm/src/liblcms2.a",
    ],
}

def platform_of(target):
    return target.split("-")[0]

def nm_bin(target, objcopy):
    if platform_of(target) in ("mac", "linux"):
        return "nm"
    exe = ".exe" if platform_of(target) == "win" else ""
    local = os.path.join(os.path.dirname(objcopy), "llvm-nm" + exe)
    return local if os.path.exists(local) else "llvm-nm"

def sym_prefix(target):
    return "_" if platform_of(target) == "mac" else ""

def defined_symbols(path, nm):
    r = subprocess.run([nm, "-g", "--defined-only", path],
                       capture_output=True, text=True)
    syms = set()
    for line in r.stdout.splitlines():
        parts = line.split()
        if len(parts) >= 3 and parts[1] in STRONG:
            name = parts[2]
            if name.startswith(("__Z", "_ZN", "?", "$", "__imp_", ".")):
                continue
            syms.add(name)
    return syms

def undefined_symbols(path, nm):
    r = subprocess.run([nm, "-u", path], capture_output=True, text=True)
    syms = set()
    for line in r.stdout.splitlines():
        parts = line.split()
        if len(parts) >= 2 and parts[-2] in ("U", "w", "v"):
            name = parts[-1]
            if name.startswith(("__Z", "_ZN", "?", "$", "__imp_", ".")):
                continue
            syms.add(name)
    return syms


def entry_points(path, nm, pfx):
    r = subprocess.run([nm, "--defined-only", path],
                       capture_output=True, text=True)
    return {p[2] for p in (l.split() for l in r.stdout.splitlines())
            if len(p) >= 3 and p[2].startswith(pfx + "FPDF")}

def resolve(patterns, base):
    out = []
    for p in patterns:
        p = os.path.expanduser(p)
        p = p if os.path.isabs(p) else os.path.join(base, p)
        hits = glob.glob(p)
        out.extend(h for h in hits if os.path.exists(h))
    return out

def main():
    if len(sys.argv) < 4:
        print("usage: isolate.py <libpdfium.a> <objcopy> <target>")
        return 2
    archive, objcopy, target = sys.argv[1], sys.argv[2], sys.argv[3]
    plat = platform_of(target)
    nm = nm_bin(target, objcopy)
    pfx = sym_prefix(target)

    engine_root = os.path.abspath(
        os.path.join(os.path.dirname(__file__), "..", ".."))
    libs = resolve(ENGINE_LIBS.get(plat, []), engine_root)
    if not libs:
        print(f"no engine libraries found for {plat}; nothing to isolate")
        return 0

    engine_syms = set()
    for lib in libs:
        engine_syms |= defined_symbols(lib, nm)

    pdfium_syms = defined_symbols(archive, nm)
    colliding = {s for s in (pdfium_syms & engine_syms)
                 if not s.startswith(pfx + "FPDF")}
    crossing = {s for s in (undefined_symbols(archive, nm) & engine_syms)
                if s not in pdfium_syms}
    if crossing:
        print(f"{len(crossing)} symbol(s) PDFium references but only the engine defines; "
              f"renaming them too so the link reports them instead of binding across libraries:")
        for s_ in sorted(crossing)[:40]:
            print(f"  {s_}")
    colliding |= crossing

    print(f"engine {len(engine_syms)} syms across {len(libs)} libs, "
          f"pdfium {len(pdfium_syms)}")
    if not colliding:
        print("no colliding symbols")
        return 0

    if plat == "wasm":
        print(f"{len(colliding)} colliding symbol(s); wasm objects cannot be "
              f"renamed by llvm-objcopy, so the link resolves them by order "
              f"(engine libraries precede libpdfium.a)")
        return 0

    print(f"renaming {len(colliding)} colliding symbol(s)")

    before = entry_points(archive, nm, pfx)
    mapping = "\n".join(
        f"{s} {pfx}{PREFIX}{s[len(pfx):]}" for s in sorted(colliding)) + "\n"
    map_path = archive + ".rename"
    with open(map_path, "w") as fh:
        fh.write(mapping)

    r = subprocess.run([objcopy, f"--redefine-syms={map_path}", archive],
                       capture_output=True, text=True)
    if r.returncode != 0:
        print(r.stderr[:2000], file=sys.stderr)
        if plat == "win":
            print("objcopy could not rewrite the COFF archive; the link resolves collisions by order")
            return 0
        return 3
    if plat in ("mac", "linux"):
        subprocess.run(["ranlib", archive], capture_output=True)

    left = defined_symbols(archive, nm) & engine_syms
    after = entry_points(archive, nm, pfx)
    print(f"remaining collisions: {len(left)}")
    print(f"FPDF entry points: {len(before)} -> {len(after)}")
    if left:
        for s in sorted(left)[:10]:
            print(f"  {s}", file=sys.stderr)
        return 4
    if after != before:
        print("entry points changed", file=sys.stderr)
        return 5
    return 0

if __name__ == "__main__":
    sys.exit(main())
