#!/usr/bin/env python3
import argparse
import concurrent.futures
import csv
import gzip
import hashlib
import json
import os
import shutil
import subprocess
import sys
import struct
import threading
import time
import zipfile
import zlib

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CLI = os.path.join(ROOT, "pdfa-engine", "build", "cli", "kura")
S3 = "https://digitalcorpora.s3.amazonaws.com/corpora/files/CC-MAIN-2021-31-PDF-UNTRUNCATED/zipfiles"
PDFJS_RAW = "https://raw.githubusercontent.com/mozilla/pdf.js/master/test/pdfs/"
RESULT_FIELDS = ["tranche", "file", "sha256", "bytes", "pages", "source", "origin", "level", "result", "code", "ms", "out_bytes", "verified"]
MANIFEST_FIELDS = ["tranche", "file", "sha256", "bytes", "source", "origin"]
CC_DATA = "https://data.commoncrawl.org/"
CC_INDEXES = CC_DATA + "cc-index/collections/CC-MAIN-2021-31/indexes"
CC_PREFIXES = ["gov,", "edu,", "org,", "com,", "uk,", "de,", "fr,", "jp,", "au,", "ca,"]
BLOCK_WORKERS = 32
SAFEDOCS_MAX_MEMBER = 512 * 1024
SAFEDOCS_STRIDE = 20
MEMBER_WORKERS = 96
CC_MAX_LENGTH = 512 * 1024
CC_CHUNK = 1000
FETCH_WORKERS = 96
MAX_MEMBER = 80 * 1024 * 1024
CONVERT_TIMEOUT = 150
RANGE_PARTS = 16


def log(msg):
    print(f"[{time.strftime('%H:%M:%S')}] {msg}", flush=True)


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as fh:
        for chunk in iter(lambda: fh.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def content_length(url):
    p = subprocess.run(["curl", "-sIL", url], capture_output=True, text=True)
    size = 0
    for line in p.stdout.splitlines():
        if line.lower().startswith("content-length:"):
            size = int(line.split(":", 1)[1].strip())
    return size


def fetch(url, dst):
    size = content_length(url)
    if size <= 0:
        return False
    part_size = (size + RANGE_PARTS - 1) // RANGE_PARTS
    parts = []
    for i in range(RANGE_PARTS):
        a = i * part_size
        b = min(size - 1, a + part_size - 1)
        if a > b:
            break
        parts.append((i, a, b, f"{dst}.part{i}"))

    def get(part):
        i, a, b, path = part
        for attempt in range(4):
            p = subprocess.run(["curl", "-sL", "--max-time", "3600", "-r", f"{a}-{b}", "-o", path, url],
                               capture_output=True, text=True)
            if p.returncode == 0 and os.path.exists(path) and os.path.getsize(path) == b - a + 1:
                return True
            time.sleep(5 * (attempt + 1))
        return False

    with concurrent.futures.ThreadPoolExecutor(RANGE_PARTS) as ex:
        ok = all(ex.map(get, parts))
    if not ok:
        return False
    with open(dst, "wb") as out:
        for _, _, _, path in parts:
            with open(path, "rb") as fh:
                shutil.copyfileobj(fh, out, 1 << 20)
            os.remove(path)
    return os.path.getsize(dst) == size


def extract_pdfs(zpath, outdir):
    got = []
    try:
        with zipfile.ZipFile(zpath) as z:
            for info in z.infolist():
                if not info.filename.lower().endswith(".pdf"):
                    continue
                if info.file_size > MAX_MEMBER or info.file_size == 0:
                    continue
                base = os.path.basename(info.filename)
                if not base:
                    continue
                dst = os.path.join(outdir, base)
                with z.open(info) as src, open(dst, "wb") as fh:
                    shutil.copyfileobj(src, fh, 1 << 20)
                got.append(dst)
    except (zipfile.BadZipFile, OSError) as e:
        log(f"extract error {os.path.basename(zpath)}: {e}")
    return sorted(got)


def page_count(path):
    try:
        p = subprocess.run(["qpdf", "--show-npages", path], capture_output=True, text=True, timeout=30)
        return int(p.stdout.strip()) if p.returncode == 0 else ""
    except (subprocess.TimeoutExpired, ValueError):
        return ""


def convert(src, level):
    dst = src + ".out.pdf"
    env = dict(os.environ, PDFA_TIMEOUT="120")
    started = time.time()
    try:
        p = subprocess.run([CLI, "--level", level, src, dst], capture_output=True, text=True,
                           errors="replace", timeout=CONVERT_TIMEOUT, env=env)
    except subprocess.TimeoutExpired:
        return "timeout", "", int((time.time() - started) * 1000), 0, None
    ms = int((time.time() - started) * 1000)
    if p.returncode == 3:
        return "timeout", "", ms, 0, None
    try:
        rep = json.loads(p.stdout)
    except json.JSONDecodeError:
        return "crash", f"exit {p.returncode}", ms, 0, None
    if rep.get("ok"):
        return "converted", "", ms, os.path.getsize(dst) if os.path.exists(dst) else 0, dst
    return "rejected", rep.get("errorCode", "?"), ms, 0, None


def verapdf(paths, level):
    verdicts = {}
    chunks = [paths[i:i + 8] for i in range(0, len(paths), 8)]
    with concurrent.futures.ThreadPoolExecutor(4) as ex:
        for part in ex.map(lambda c: verapdf_chunk(c, level), chunks):
            verdicts.update(part)
    return verdicts


def verapdf_chunk(chunk, level):
    verdicts = {}
    if True:
        try:
            p = subprocess.run(["verapdf", "-f", level, "--format", "json"] + chunk,
                               capture_output=True, text=True, timeout=1800,
                               env=dict(os.environ, JAVA_TOOL_OPTIONS="-Xmx1g"))
            jobs = json.loads(p.stdout)["report"]["jobs"]
        except (subprocess.TimeoutExpired, json.JSONDecodeError, KeyError):
            for path in chunk:
                verdicts[path] = "error"
            return verdicts
        for job in jobs:
            name = job.get("itemDetails", {}).get("name", "")
            try:
                compliant = job["validationResult"][0]["compliant"]
            except (KeyError, IndexError):
                compliant = None
            for path in chunk:
                if name.endswith(os.path.basename(path)):
                    verdicts[path] = "pass" if compliant else ("fail" if compliant is not None else "error")
    return verdicts


class Results:
    def __init__(self, outdir):
        self.path = os.path.join(outdir, "RESULTS.csv")
        self.manifest = os.path.join(outdir, "MANIFEST.csv")
        self.done = set()
        for path, fields in ((self.path, RESULT_FIELDS), (self.manifest, MANIFEST_FIELDS)):
            if os.path.exists(path):
                with open(path, newline="") as fh:
                    reader = csv.DictReader(fh)
                    rows = list(reader)
                    stale = reader.fieldnames != fields
                if stale:
                    with open(path, "w", newline="") as fh:
                        w = csv.DictWriter(fh, fieldnames=fields)
                        w.writeheader()
                        for row in rows:
                            row.setdefault("origin", row.get("source", ""))
                            w.writerow({k: row.get(k, "") for k in fields})
        if os.path.exists(self.path):
            with open(self.path, newline="") as fh:
                for row in csv.DictReader(fh):
                    self.done.add((row["tranche"], row["file"]))
        new = not os.path.exists(self.path)
        self.fh = open(self.path, "a", newline="")
        self.w = csv.DictWriter(self.fh, fieldnames=RESULT_FIELDS)
        if new:
            self.w.writeheader()
        newm = not os.path.exists(self.manifest)
        self.mh = open(self.manifest, "a", newline="")
        self.mw = csv.DictWriter(self.mh, fieldnames=MANIFEST_FIELDS)
        if newm:
            self.mw.writeheader()

    def add(self, row):
        self.w.writerow(row)
        self.fh.flush()
        self.mw.writerow({k: row[k] for k in MANIFEST_FIELDS})
        self.mh.flush()
        self.done.add((row["tranche"], row["file"]))


def run_batch(tranche, pdfs, source_of, level, results, repro, verify_every, workers, origin_of=None):
    todo = [p for p in pdfs if (tranche, os.path.basename(p)) not in results.done]
    rows = []

    def one(src):
        result, code, ms, out_bytes, out = convert(src, level)
        return src, result, code, ms, out_bytes, out, sha256(src), page_count(src)

    with concurrent.futures.ThreadPoolExecutor(workers) as ex:
        for src, result, code, ms, out_bytes, out, digest, pages in ex.map(one, todo):
            rows.append({"tranche": tranche, "file": os.path.basename(src), "sha256": digest,
                         "bytes": os.path.getsize(src), "pages": pages, "source": source_of(src),
                         "origin": origin_of(src) if origin_of else source_of(src),
                         "level": level, "result": result, "code": code, "ms": ms,
                         "out_bytes": out_bytes, "verified": "", "_out": out, "_src": src})
    outputs = [r for r in rows if r["_out"]]
    sample = outputs[::verify_every] if verify_every > 0 else []
    if sample:
        verdicts = verapdf([r["_out"] for r in sample], level)
        for r in sample:
            r["verified"] = verdicts.get(r["_out"], "error")
    for r in rows:
        if r["result"] in ("crash", "timeout"):
            try:
                shutil.copy(r["_src"], os.path.join(repro, f"{r['result']}_{r['file']}"))
            except OSError:
                pass
        results.add({k: r[k] for k in RESULT_FIELDS})
    counts = {}
    for r in rows:
        counts[r["result"]] = counts.get(r["result"], 0) + 1
    passed = sum(1 for r in sample if r["verified"] == "pass")
    log(f"{tranche}: {len(rows)} files {counts}; verified sample {passed}/{len(sample)}")
    return len(rows)


def cc_cluster(work):
    path = os.path.join(work, "cluster.idx")
    if not os.path.exists(path) or os.path.getsize(path) < 100000000:
        log("cc index: downloading the cluster table")
        if not fetch(f"{CC_INDEXES}/cluster.idx", path):
            raise SystemExit("cluster.idx download failed")
    return path


def cc_blocks(cluster_path):
    blocks = {p: [] for p in CC_PREFIXES}
    with open(cluster_path, errors="replace") as fh:
        for line in fh:
            parts = line.rstrip("\n").split("\t")
            if len(parts) < 4:
                continue
            for p in CC_PREFIXES:
                if parts[0].startswith(p):
                    blocks[p].append((parts[1], int(parts[2]), int(parts[3])))
                    break
    return blocks


def cc_block_records(block):
    shard, off, ln = block
    for attempt in range(4):
        p = subprocess.run(["curl", "-s", "--max-time", "120", "-r", f"{off}-{off + ln - 1}", f"{CC_INDEXES}/{shard}"],
                           capture_output=True)
        if p.returncode == 0 and len(p.stdout) == ln:
            try:
                text = gzip.decompress(p.stdout).decode("utf-8", "replace")
            except (OSError, EOFError):
                return []
            out = []
            for line in text.splitlines():
                if '"mime": "application/pdf"' not in line:
                    continue
                try:
                    rec = json.loads(line.split(" ", 2)[2])
                except (ValueError, IndexError):
                    continue
                if rec.get("status") != "200" or int(rec.get("length", 0)) > CC_MAX_LENGTH:
                    continue
                out.append(rec)
            return out
        time.sleep(3 * (attempt + 1))
    return []


def cc_select(args):
    blocks = cc_blocks(cc_cluster(args.work))
    per = -(-args.cc // len(CC_PREFIXES))
    seen = set()
    recs = []
    for prefix in CC_PREFIXES:
        avail = blocks[prefix]
        if not avail:
            log(f"cc index: no blocks under {prefix}")
            continue
        stride = max(1, len(avail) // max(1, per // 4))
        chosen = avail[::stride]
        got = []
        used = 0
        with concurrent.futures.ThreadPoolExecutor(BLOCK_WORKERS) as ex:
            for round_start in range(0, len(chosen), 100):
                for block_recs in ex.map(cc_block_records, chosen[round_start:round_start + 100]):
                    used += 1
                    for r in block_recs:
                        d = r.get("digest")
                        if not d or d in seen:
                            continue
                        seen.add(d)
                        got.append(r)
                if len(got) >= per:
                    break
        got = got[:per]
        log(f"cc index: {prefix} {used} of {len(avail)} blocks at stride {stride} -> {len(got)} records")
        recs.extend(got)
    return cc_top_up(recs, blocks, seen, args.cc)


def cc_top_up(recs, blocks, seen, target):
    for prefix in sorted(CC_PREFIXES, key=lambda p: -len(blocks[p])):
        if len(recs) >= target:
            break
        avail = blocks[prefix]
        stride = max(1, len(avail) // max(1, (target - len(recs)) // 4))
        chosen = avail[stride // 2::stride]
        used = 0
        with concurrent.futures.ThreadPoolExecutor(BLOCK_WORKERS) as ex:
            for round_start in range(0, len(chosen), 100):
                for block_recs in ex.map(cc_block_records, chosen[round_start:round_start + 100]):
                    used += 1
                    for r in block_recs:
                        d = r.get("digest")
                        if not d or d in seen or len(recs) >= target:
                            continue
                        seen.add(d)
                        recs.append(r)
                if len(recs) >= target:
                    break
        log(f"cc index: top-up from {prefix}, {used} blocks -> {len(recs)} records")
    return recs[:target]


def cc_fetch(rec, dst):
    off = int(rec["offset"])
    ln = int(rec["length"])
    url = CC_DATA + rec["filename"]
    for attempt in range(3):
        p = subprocess.run(["curl", "-s", "--max-time", "120", "-r", f"{off}-{off + ln - 1}", url],
                           capture_output=True)
        if p.returncode == 0 and len(p.stdout) == ln:
            try:
                data = gzip.decompress(p.stdout)
            except (OSError, EOFError):
                return False
            i = data.find(b"\r\n\r\n")
            j = data.find(b"\r\n\r\n", i + 4) if i >= 0 else -1
            body = data[j + 4:] if j >= 0 else b""
            k = body[:1024].find(b"%PDF")
            if k < 0:
                return False
            with open(dst, "wb") as fh:
                fh.write(body[k:])
            return True
        time.sleep(2 * (attempt + 1))
    return False


def cc_locator(rec):
    return f"{CC_DATA}{rec['filename']}#offset={rec['offset']}&length={rec['length']}"


def run_cc(args, results, repro):
    selection = os.path.join(args.out, "cc-index.jsonl")
    recs = [json.loads(l) for l in open(selection) if l.strip()] if os.path.exists(selection) else []
    if not recs:
        recs = cc_select(args)
    elif len(recs) < args.cc:
        seen = {r.get("digest") for r in recs}
        recs = cc_top_up(recs, cc_blocks(cc_cluster(args.work)), seen, args.cc)
    with open(selection, "w") as fh:
        for r in recs:
            fh.write(json.dumps(r) + "\n")
    log(f"cc: {len(recs)} records selected")
    total = sum(1 for t, _ in results.done if t == "cc")
    for start in range(0, len(recs), CC_CHUNK):
        chunk = recs[start:start + CC_CHUNK]
        items = [(r, f"cc-{start + k:05d}-{r['digest'][:10].lower()}.pdf") for k, r in enumerate(chunk)]
        pending = [(r, n) for r, n in items if ("cc", n) not in results.done]
        if not pending:
            continue
        cdir = os.path.join(args.work, f"cc-{start:05d}")
        if os.path.isdir(cdir):
            shutil.rmtree(cdir)
        os.makedirs(cdir)
        by_name = {n: r for r, n in pending}

        def fetch_one(item):
            r, n = item
            dst = os.path.join(cdir, n)
            return dst if cc_fetch(r, dst) else None

        with concurrent.futures.ThreadPoolExecutor(FETCH_WORKERS) as ex:
            got = sorted(p for p in ex.map(fetch_one, pending) if p)
        log(f"cc chunk {start}: fetched {len(got)}/{len(pending)}")
        total += run_batch("cc", got, lambda p: by_name[os.path.basename(p)]["url"], args.level, results, repro,
                           args.verify_every, args.workers,
                           origin_of=lambda p: cc_locator(by_name[os.path.basename(p)]))
        shutil.rmtree(cdir, ignore_errors=True)
        log(f"cc total {total}/{len(recs)}")


class RangeFile:
    def __init__(self, url, size):
        self.url = url
        self.size = size
        self.pos = 0

    def seek(self, offset, whence=0):
        if whence == 0:
            self.pos = offset
        elif whence == 1:
            self.pos += offset
        else:
            self.pos = self.size + offset
        return self.pos

    def tell(self):
        return self.pos

    def read(self, n=-1):
        if n < 0:
            n = self.size - self.pos
        if n <= 0:
            return b""
        data = range_get(self.url, self.pos, self.pos + n - 1)
        self.pos += len(data)
        return data

    def seekable(self):
        return True


def range_get(url, a, b):
    for attempt in range(4):
        p = subprocess.run(["curl", "-s", "--max-time", "120", "-r", f"{a}-{b}", url], capture_output=True)
        if p.returncode == 0 and len(p.stdout) == b - a + 1:
            return p.stdout
        time.sleep(3 * (attempt + 1))
    return b""


def zip_members(url):
    size = content_length(url)
    if size <= 0:
        return []
    try:
        zf = zipfile.ZipFile(RangeFile(url, size))
    except (zipfile.BadZipFile, OSError):
        return []
    out = []
    for info in zf.infolist():
        if not info.filename.lower().endswith(".pdf") or info.file_size == 0:
            continue
        if info.file_size > SAFEDOCS_MAX_MEMBER or info.compress_type not in (0, 8):
            continue
        out.append((info.filename, info.header_offset, info.compress_size, info.compress_type))
    return out


def fetch_member(url, member, dst):
    name, header_offset, compress_size, compress_type = member
    head = range_get(url, header_offset, header_offset + 29)
    if len(head) < 30 or head[:4] != b"PK\x03\x04":
        return False
    n, m = struct.unpack("<HH", head[26:30])
    start = header_offset + 30 + n + m
    raw = range_get(url, start, start + compress_size - 1)
    if len(raw) != compress_size:
        return False
    try:
        data = zlib.decompress(raw, -15) if compress_type == 8 else raw
    except zlib.error:
        return False
    if not data.startswith(b"%PDF"):
        k = data[:1024].find(b"%PDF")
        if k < 0:
            return False
        data = data[k:]
    with open(dst, "wb") as fh:
        fh.write(data)
    return True


def run_safedocs_members(args, results, repro):
    total = sum(1 for t, _ in results.done if t == "safedocs")
    for i in range(args.start, 1000, SAFEDOCS_STRIDE):
        if total >= args.safedocs_members:
            break
        url = f"{S3}/0000-0999/{i:04d}.zip"
        tag = f"safedocs-{i:04d}"
        members = zip_members(url)
        if not members:
            log(f"{tag}: could not read the zip directory, skipping")
            continue
        zdir = os.path.join(args.work, tag)
        if os.path.isdir(zdir):
            shutil.rmtree(zdir)
        os.makedirs(zdir)
        pending = [(mem, f"{i:04d}-{os.path.basename(mem[0])}") for mem in members]
        pending = [(mem, n) for mem, n in pending if ("safedocs", n) not in results.done]
        by_name = {n: mem[0] for mem, n in pending}

        def one(item):
            mem, n = item
            dst = os.path.join(zdir, n)
            return dst if fetch_member(url, mem, dst) else None

        with concurrent.futures.ThreadPoolExecutor(MEMBER_WORKERS) as ex:
            got = sorted(p for p in ex.map(one, pending) if p)
        log(f"{tag}: {len(members)} members under the size cap, fetched {len(got)}")
        total += run_batch("safedocs", got, lambda p, u=url: u, args.level, results, repro,
                           args.verify_every, args.workers,
                           origin_of=lambda p, u=url: f"{u}#member={by_name[os.path.basename(p)]}")
        shutil.rmtree(zdir, ignore_errors=True)
        log(f"safedocs total {total}/{args.safedocs_members}")


def safedocs_zips(stride, start):
    for i in range(start, 1000, stride):
        yield i, f"{S3}/0000-0999/{i:04d}.zip"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default=os.path.join(ROOT, "bench"))
    ap.add_argument("--work", default=os.path.join(ROOT, "bench", "work"))
    ap.add_argument("--level", default="2b")
    ap.add_argument("--target", type=int, default=30000)
    ap.add_argument("--stride", type=int, default=33)
    ap.add_argument("--start", type=int, default=0)
    ap.add_argument("--verify-every", type=int, default=10)
    ap.add_argument("--workers", type=int, default=6)
    ap.add_argument("--pdfjs", default="")
    ap.add_argument("--skip-safedocs", action="store_true")
    ap.add_argument("--cc", type=int, default=0)
    ap.add_argument("--safedocs-members", type=int, default=0)
    args = ap.parse_args()
    os.makedirs(args.work, exist_ok=True)
    repro = os.path.join(args.out, "repro")
    os.makedirs(repro, exist_ok=True)
    results = Results(args.out)
    if args.pdfjs:
        pdfs = sorted(os.path.join(args.pdfjs, f) for f in os.listdir(args.pdfjs) if f.lower().endswith(".pdf"))
        run_batch("pdfjs", pdfs, lambda p: PDFJS_RAW + os.path.basename(p), args.level, results, repro,
                  args.verify_every, args.workers)
    if args.cc > 0:
        run_cc(args, results, repro)
    if args.safedocs_members > 0:
        run_safedocs_members(args, results, repro)
    if args.skip_safedocs:
        return
    total = sum(1 for t, _ in results.done if t == "safedocs")
    queue = list(safedocs_zips(args.stride, args.start))
    fetched = {}

    def prefetch(i, url):
        zpath = os.path.join(args.work, f"{i:04d}.zip")
        fetched[i] = zpath if fetch(url, zpath) else None

    ahead = None
    for n, (i, url) in enumerate(queue):
        if total >= args.target:
            break
        tag = f"safedocs-{i:04d}"
        zdir = os.path.join(args.work, tag)
        if os.path.isdir(zdir):
            shutil.rmtree(zdir)
        os.makedirs(zdir)
        if ahead is None:
            log(f"{tag}: downloading")
            prefetch(i, url)
        else:
            ahead.join()
        if n + 1 < len(queue):
            ahead = threading.Thread(target=prefetch, args=queue[n + 1], daemon=True)
            ahead.start()
        zpath = fetched.pop(i, None)
        if not zpath:
            log(f"{tag}: download failed, skipping")
            continue
        pdfs = extract_pdfs(zpath, zdir)
        os.remove(zpath)
        log(f"{tag}: {len(pdfs)} pdfs extracted, converting")
        total += run_batch("safedocs", pdfs, lambda p, u=url: u, args.level, results, repro,
                           args.verify_every, args.workers)
        shutil.rmtree(zdir, ignore_errors=True)
        log(f"safedocs total {total}/{args.target}")


if __name__ == "__main__":
    main()
