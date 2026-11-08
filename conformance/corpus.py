#!/usr/bin/env python3
import argparse
import concurrent.futures
import csv
import hashlib
import io
import os
import random
import subprocess
import sys
import tarfile
import time
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(os.path.dirname(HERE), "bench"))
from bench import RangeFile, content_length, fetch_member, log  # noqa: E402

SEED = 20261107
FIELDS = ["tranche", "file", "sha256", "bytes", "origin"]
MAX_BYTES = 60 * 1024 * 1024
SAFEDOCS = "https://digitalcorpora.s3.amazonaws.com/corpora/files/CC-MAIN-2021-31-PDF-UNTRUNCATED/zipfiles"
GOVDOCS = "https://digitalcorpora.s3.amazonaws.com/corpora/files/govdocs1/zipfiles"
PMC = "https://pmc-oa-opendata.s3.amazonaws.com"
ARXIV = "https://export.arxiv.org/pdf"
EURLEX = "https://eur-lex.europa.eu/legal-content/{lang}/TXT/PDF/?uri=CELEX:{celex}"
EU_LANGS = ["BG", "CS", "DA", "DE", "EL", "EN", "ES", "ET", "FI", "FR", "GA", "HR", "HU", "IT", "LT", "LV", "MT",
            "NL", "PL", "PT", "RO", "SK", "SL", "SV"]
TARBALLS = {
    "pdfjs": "https://codeload.github.com/mozilla/pdf.js/tar.gz/refs/heads/master",
    "verapdf": "https://codeload.github.com/veraPDF/veraPDF-corpus/tar.gz/refs/heads/staging",
    "pdfium": "https://pdfium.googlesource.com/pdfium/+archive/refs/heads/main/testing/resources.tar.gz",
}
TARGETS = {"web": 2140, "govdocs": 1000, "pmc": 500, "arxiv": 500, "eurlex": 160, "testsuites": 700}


def curl(url, timeout=180):
    p = subprocess.run(["curl", "-sL", "-A", "Mozilla/5.0 (kura-conformance)", "--max-time", str(timeout), url],
                       capture_output=True)
    return p.stdout if p.returncode == 0 else b""


class Corpus:
    def __init__(self, out, tranche):
        self.dir = os.path.join(out, "files")
        os.makedirs(self.dir, exist_ok=True)
        os.makedirs(os.path.join(out, "manifests"), exist_ok=True)
        self.manifest = os.path.join(out, "manifests", tranche + ".csv")
        self.rows = []
        self.hashes = set()
        if os.path.exists(self.manifest):
            with open(self.manifest) as fh:
                self.rows = list(csv.DictReader(fh))
            self.hashes = {r["sha256"] for r in self.rows}

    def count(self, tranche):
        return sum(1 for r in self.rows if r["tranche"] == tranche)

    def add(self, tranche, name, data, origin):
        if not data.startswith(b"%PDF") or len(data) > MAX_BYTES:
            return False
        digest = hashlib.sha256(data).hexdigest()
        if digest in self.hashes:
            return False
        file = f"{tranche}-{len(self.rows):04d}-{name[-60:]}"
        file = "".join(ch if ch.isalnum() or ch in "._-" else "_" for ch in file)
        if not file.lower().endswith(".pdf"):
            file += ".pdf"
        with open(os.path.join(self.dir, file), "wb") as fh:
            fh.write(data)
        self.hashes.add(digest)
        self.rows.append({"tranche": tranche, "file": file, "sha256": digest, "bytes": len(data), "origin": origin})
        with open(self.manifest, "w", newline="") as fh:
            w = csv.DictWriter(fh, FIELDS)
            w.writeheader()
            w.writerows(self.rows)
        return True


def pdf_members(url):
    size = content_length(url)
    if size <= 0:
        return []
    try:
        zf = zipfile.ZipFile(RangeFile(url, size))
    except (zipfile.BadZipFile, OSError):
        return []
    return sorted(((i.filename, i.header_offset, i.compress_size, i.compress_type) for i in zf.infolist()
                   if i.filename.lower().endswith(".pdf") and 0 < i.file_size <= MAX_BYTES and i.compress_type in (0, 8)),
                  key=lambda m: m[0])


def zip_tranche(corpus, rng, tranche, zips, per_zip):
    for url in zips:
        if corpus.count(tranche) >= TARGETS[tranche]:
            return
        members = pdf_members(url)
        pick = rng.sample(members, min(per_zip, len(members)))
        tmp = os.path.join(corpus.dir, ".member")

        def one(m):
            path = f"{tmp}-{abs(hash(m[0]))}"
            ok = fetch_member(url, m, path)
            data = open(path, "rb").read() if ok else b""
            if ok:
                os.remove(path)
            return m, data

        with concurrent.futures.ThreadPoolExecutor(16) as ex:
            for m, data in ex.map(one, pick):
                if corpus.count(tranche) < TARGETS[tranche]:
                    corpus.add(tranche, os.path.basename(m[0]), data, f"{url}#member={m[0]}")
        log(f"{tranche}: {corpus.count(tranche)}/{TARGETS[tranche]}")


def url_tranche(corpus, rng, tranche, candidates, delay=0, workers=8):
    while corpus.count(tranche) < TARGETS[tranche]:
        batch = [next(candidates) for _ in range(workers)]
        with concurrent.futures.ThreadPoolExecutor(workers) as ex:
            for (name, url), data in zip(batch, ex.map(lambda c: curl(c[1]), batch)):
                if corpus.count(tranche) < TARGETS[tranche]:
                    corpus.add(tranche, name, data, url)
        if delay:
            time.sleep(delay)
        log(f"{tranche}: {corpus.count(tranche)}/{TARGETS[tranche]}")


def pmc_ids(rng):
    while True:
        after = f"PMC{rng.randint(1_000_000, 11_800_000)}"
        listing = curl(f"{PMC}/?list-type=2&max-keys=40&start-after={after}", timeout=60).decode("utf-8", "replace")
        keys = [k for k in listing.split("<Key>")[1:] if k.split("</Key>")[0].endswith(".pdf")]
        if keys:
            key = keys[0].split("</Key>")[0]
            yield os.path.basename(key), f"{PMC}/{key}"


def arxiv_ids(rng):
    while True:
        yymm = f"{rng.randint(15, 24):02d}{rng.randint(1, 12):02d}"
        ident = f"{yymm}.{rng.randint(1, 15000):05d}"
        yield f"arxiv-{ident}.pdf", f"{ARXIV}/{ident}"


def eurlex_ids(rng):
    while True:
        celex = f"3{rng.randint(2000, 2023)}{rng.choice('RL')}{rng.randint(1, 1500):04d}"
        lang = rng.choice(EU_LANGS)
        yield f"eurlex-{lang}-{celex}.pdf", EURLEX.format(lang=lang, celex=celex)


def test_suites(corpus, rng):
    pool = []
    for name, url in sorted(TARBALLS.items()):
        data = curl(url, timeout=900)
        if not data:
            log(f"testsuites: could not fetch {name}")
            continue
        with tarfile.open(fileobj=io.BytesIO(data), mode="r:*") as tf:
            for m in tf.getmembers():
                if m.isfile() and m.name.lower().endswith(".pdf") and 0 < m.size <= MAX_BYTES:
                    pool.append((name, url, m.name, tf.extractfile(m).read()))
        log(f"testsuites: {name} read, {len(pool)} PDFs in the pool")
    pool.sort(key=lambda p: (p[0], p[2]))
    for name, url, member, data in rng.sample(pool, len(pool)):
        if corpus.count("testsuites") >= TARGETS["testsuites"]:
            break
        corpus.add("testsuites", f"{name}-{os.path.basename(member)}", data, f"{url}#member={member}")
    log(f"testsuites: {corpus.count('testsuites')}/{TARGETS['testsuites']}")


def main():
    ap = argparse.ArgumentParser(description="Download the conformance corpus chosen by a fixed seed.")
    ap.add_argument("--out", default=os.path.join(HERE, "corpus"))
    ap.add_argument("tranche", choices=list(TARGETS) + ["merge"])
    ap.add_argument("--limit", type=int, default=0)
    args = ap.parse_args()
    if args.tranche == "merge":
        rows = []
        for t in TARGETS:
            path = os.path.join(args.out, "manifests", t + ".csv")
            if os.path.exists(path):
                with open(path) as fh:
                    rows += list(csv.DictReader(fh))
        seen, out = set(), []
        for r in rows:
            if r["sha256"] not in seen:
                seen.add(r["sha256"])
                out.append(r)
        with open(os.path.join(HERE, "MANIFEST.csv"), "w", newline="") as fh:
            w = csv.DictWriter(fh, FIELDS)
            w.writeheader()
            w.writerows(out)
        log(f"MANIFEST.csv: {len(out)} files")
        return
    if args.limit:
        TARGETS[args.tranche] = args.limit
    args.only = [args.tranche]
    corpus = Corpus(args.out, args.tranche)
    if "web" in args.only:
        rng = random.Random(f"{SEED}-web")
        zips = [f"{SAFEDOCS}/{i // 1000 * 1000:04d}-{i // 1000 * 1000 + 999:04d}/{i:04d}.zip" for i in rng.sample(range(1000, 7933), 400)]
        zip_tranche(corpus, rng, "web", zips, 10)
    if "govdocs" in args.only:
        rng = random.Random(f"{SEED}-govdocs")
        zip_tranche(corpus, rng, "govdocs", [f"{GOVDOCS}/{i:03d}.zip" for i in rng.sample(range(1000), 1000)], 5)
    if "pmc" in args.only:
        rng = random.Random(f"{SEED}-pmc")
        url_tranche(corpus, rng, "pmc", pmc_ids(rng), workers=16)
    if "arxiv" in args.only:
        rng = random.Random(f"{SEED}-arxiv")
        url_tranche(corpus, rng, "arxiv", arxiv_ids(rng), delay=3, workers=1)
    if "eurlex" in args.only:
        rng = random.Random(f"{SEED}-eurlex")
        url_tranche(corpus, rng, "eurlex", eurlex_ids(rng), workers=4)
    if "testsuites" in args.only:
        test_suites(corpus, random.Random(f"{SEED}-testsuites"))
    log(f"done: {len(corpus.rows)} files, manifest {corpus.manifest}")


if __name__ == "__main__":
    main()
