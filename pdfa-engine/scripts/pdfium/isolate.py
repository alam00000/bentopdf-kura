#!/usr/bin/env python3
import glob
import os
import subprocess
import sys

import shutil
import struct
import tempfile

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
    for exe in (".exe", ""):
        local = os.path.join(os.path.dirname(objcopy), "llvm-nm" + exe)
        if os.path.exists(local):
            return local
    return "llvm-nm"

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



def coff_rename(data, mapping):
    if len(data) < 20:
        return data
    machine, nsec, _, symptr, nsyms, optsz, _ = struct.unpack_from("<HHIIIHH", data, 0)
    if machine == 0 and nsec == 0xFFFF:
        raise ValueError("bigobj members are not supported")
    if nsyms == 0 or symptr == 0 or symptr + nsyms * 18 > len(data):
        return data
    symtab = data[symptr:symptr + nsyms * 18]
    str_off = symptr + nsyms * 18
    str_size = struct.unpack_from("<I", data, str_off)[0] if str_off + 4 <= len(data) else 4
    strtab = data[str_off:str_off + str_size]

    def old_name(off):
        end = strtab.find(b"\0", off)
        return strtab[off:end] if end >= 0 else strtab[off:]

    new_str = bytearray(b"\0\0\0\0")

    def add(name):
        off = len(new_str)
        new_str.extend(name + b"\0")
        return off

    out = bytearray(data[:str_off] if str_off + str_size >= len(data) else data)
    sec0 = 20 + optsz
    for k in range(nsec):
        at = sec0 + k * 40
        name = bytes(out[at:at + 8])
        if name[:1] == b"/" and name[1:].rstrip(b"\0").isdigit():
            moved = add(old_name(int(name[1:].rstrip(b"\0"))))
            out[at:at + 8] = (b"/%d" % moved).ljust(8, b"\0")

    new_syms = bytearray()
    i = 0
    while i < nsyms:
        rec = bytearray(symtab[i * 18:(i + 1) * 18])
        naux = rec[17]
        if rec[:4] == b"\0\0\0\0":
            name = old_name(struct.unpack_from("<I", rec, 4)[0])
        else:
            name = bytes(rec[:8]).rstrip(b"\0")
        name = mapping.get(name, name)
        if len(name) <= 8:
            rec[:8] = name.ljust(8, b"\0")
        else:
            rec[:4] = b"\0\0\0\0"
            struct.pack_into("<I", rec, 4, add(name))
        new_syms += rec
        for a in range(naux):
            new_syms += symtab[(i + 1 + a) * 18:(i + 2 + a) * 18]
        i += 1 + naux

    struct.pack_into("<I", new_str, 0, len(new_str))
    struct.pack_into("<I", out, 8, len(out))
    out += new_syms + new_str
    return bytes(out)


def ar_members(data):
    if data[:8] != b"!<arch>\n":
        raise ValueError("not an archive")
    pos, longnames, members = 8, b"", []
    while pos + 60 <= len(data):
        hdr = data[pos:pos + 60]
        raw = hdr[:16].decode("latin-1").rstrip()
        size = int(hdr[48:58])
        body = data[pos + 60:pos + 60 + size]
        pos += 60 + size + (size & 1)
        if raw == "//":
            longnames = body
            continue
        if raw in ("/", "/<ECSYMBOLS>/") or raw.startswith("/<"):
            continue
        if raw.startswith("/") and raw[1:].isdigit():
            off = int(raw[1:])
            ends = [e for e in (longnames.find(b"\0", off), longnames.find(b"/\n", off)) if e >= 0]
            name = longnames[off:min(ends)] if ends else longnames[off:]
            name = name.decode("latin-1")
        else:
            name = raw[:-1] if raw.endswith("/") else raw
        members.append((name, body))
    return members


def rename_coff_archive(archive, mapping, librarian):
    members = ar_members(open(archive, "rb").read())
    work = tempfile.mkdtemp(prefix="isolate-")
    paths, skipped = [], []
    byte_map = {k.encode(): v.encode() for k, v in mapping.items()}
    for idx, (name, body) in enumerate(members):
        try:
            body = coff_rename(body, byte_map)
        except ValueError as e:
            skipped.append(f"{name}: {e}")
        safe = name.replace("\\", "/")
        if not safe.lower().endswith((".o", ".obj")):
            safe += ".obj"
        path = os.path.join(work, f"{idx:05d}", safe)
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, "wb") as fh:
            fh.write(body)
        paths.append(path)
    rsp = os.path.join(work, "members.rsp")
    with open(rsp, "w") as fh:
        fh.write("\n".join(f'"{os.path.relpath(p, work)}"' for p in paths) + "\n")
    fresh = os.path.abspath(archive) + ".fresh"
    r = subprocess.run([librarian, "/lib", "/nologo", f"/out:{fresh}", f"@{rsp}"],
                       capture_output=True, text=True, cwd=work)
    if r.returncode != 0:
        shutil.rmtree(work, ignore_errors=True)
        raise RuntimeError(r.stderr[:2000] or r.stdout[:2000])
    os.replace(fresh, archive)
    shutil.rmtree(work, ignore_errors=True)
    return len(paths), skipped


def indexed_symbols(path, nm):
    r = subprocess.run([nm, "--print-armap", path], capture_output=True, text=True)
    names, inside = set(), False
    for line in r.stdout.splitlines():
        if line.startswith("Archive map"):
            inside = True
            continue
        if inside:
            if not line.strip():
                break
            names.add(line.split(" in ")[0].strip())
    return names


def entry_points(path, nm, pfx):
    r = subprocess.run([nm, "--defined-only", path],
                       capture_output=True, text=True)
    return {p[2] for p in (l.split() for l in r.stdout.splitlines())
            if len(p) >= 3 and p[2].startswith(pfx + "FPDF")}

def resolve(patterns, base):
    override = os.environ.get("KURA_ISOLATE_ENGINE_LIBS")
    if override:
        patterns = override.split(os.pathsep)
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

    if plat == "win":
        librarian = os.path.join(os.path.dirname(objcopy), "lld-link.exe")
        if not os.path.exists(librarian):
            librarian = os.path.join(os.path.dirname(objcopy), "lld-link")
        mapping = {s: f"{pfx}{PREFIX}{s[len(pfx):]}" for s in colliding}
        try:
            count, skipped = rename_coff_archive(archive, mapping, librarian)
        except (RuntimeError, ValueError, OSError) as e:
            print(str(e)[:2000], file=sys.stderr)
            return 3
        print(f"rewrote {count} archive member(s) with the librarian at {librarian}")
        for line in skipped[:10]:
            print(f"  left unchanged: {line}", file=sys.stderr)
        indexed = indexed_symbols(archive, nm)
        renamed_indexed = {s_ for s_ in indexed if s_.startswith(pfx + PREFIX)}
        print(f"archive index: {len(indexed)} entries, {len(renamed_indexed)} renamed")
        missing = {new for old, new in mapping.items()
                   if old in pdfium_syms and new not in indexed}
        if missing:
            for s_ in sorted(missing)[:10]:
                print(f"  not in the archive index: {s_}", file=sys.stderr)
            return 6
    else:
        r = subprocess.run([objcopy, f"--redefine-syms={map_path}", archive],
                           capture_output=True, text=True)
        if r.returncode != 0:
            print(r.stderr[:2000], file=sys.stderr)
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
