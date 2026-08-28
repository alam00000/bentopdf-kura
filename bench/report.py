#!/usr/bin/env python3
import csv
import os
import platform
import statistics
import subprocess
import sys
from collections import Counter, defaultdict

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BENCH = os.path.join(ROOT, "bench")
DOCS = os.path.join(BENCH, "documents")
CHUNK = 2000
TRANCHES = {
    "safedocs": ("Web PDFs, DARPA SafeDocs CC-MAIN-2021-31",
                 "a sample of the 7.9 million PDFs crawled from the open web in 2021 and archived by DARPA SafeDocs, in every state of repair"),
    "cc": ("Web PDFs, Common Crawl CC-MAIN-2021-31",
           "PDFs as they sit on the open web, fetched straight from the July 2021 crawl archive: every producer, every era, every state of repair"),
    "pdfjs": ("The pdf.js regression corpus",
              "the test files the pdf.js viewer project has collected from bug reports: broken cross-reference tables, exotic fonts, damaged streams"),
}
REJECT_TEXT = {
    "PASSWORD_REQUIRED": "needs a password",
    "FONT_UNEMBEDDABLE": "a font whose licence forbids embedding, so no conforming file can be written",
    "PARSE_ERROR": "not a parseable PDF",
    "TRANSPARENCY_P1": "transparency at PDF/A-1",
    "ENCRYPTED_ADEPT": "DRM-bound",
    "ENCRYPTED_UNSUPPORTED": "unsupported encryption",
    "SCAN_TIMEOUT": "scan budget exceeded",
}


def human(n):
    n = float(n)
    for unit in ("B", "KB", "MB", "GB"):
        if n < 1000 or unit == "GB":
            return f"{n:.0f} {unit}" if unit == "B" else f"{n:.1f} {unit}"
        n /= 1000


def machine():
    chip = ""
    try:
        chip = subprocess.run(["sysctl", "-n", "machdep.cpu.brand_string"], capture_output=True, text=True).stdout.strip()
    except OSError:
        pass
    return f"{platform.system()} {platform.machine()}" + (f", {chip}" if chip else "")


def level_name(level):
    if level[0].isdigit():
        return "PDF/A-" + level
    if level.startswith("vt"):
        return "PDF/VT-" + level[2:]
    return "PDF/" + level[0].upper() + "-" + level[1:]


def engine_version():
    cli = os.path.join(ROOT, "pdfa-engine", "build", "cli", "kura")
    try:
        return subprocess.run([cli, "--version"], capture_output=True, text=True).stdout.strip().splitlines()[0]
    except (OSError, IndexError):
        return "kura"


def main():
    rows = list(csv.DictReader(open(os.path.join(BENCH, "RESULTS.csv"), newline="")))
    if not rows:
        sys.exit("no results")
    by = defaultdict(list)
    for r in rows:
        by[r["tranche"]].append(r)
    os.makedirs(DOCS, exist_ok=True)
    for f in os.listdir(DOCS):
        os.remove(os.path.join(DOCS, f))
    total = len(rows)
    level = rows[0]["level"]
    out = []
    out.append("# Benchmark\n\n")
    out.append(f"This document records a robustness benchmark of the Kura engine ({engine_version()}) on {total:,} real PDFs from public corpora, on {machine()}. Every file was converted to {level_name(level)} with the CLI exactly as shipped, one process per file, with the default 120-second watchdog. The raw per-file data is in [bench/RESULTS.csv](bench/RESULTS.csv); every document's origin and SHA-256 are in [bench/MANIFEST.csv](bench/MANIFEST.csv), so the run can be reproduced with `make bench`.\n\n")
    out.append("## What is measured\n\n")
    out.append("A conversion engine that takes untrusted files has two ways to fail badly: it can crash or hang, and it can write a file that claims a standard it does not meet. So each run is graded on four outcomes, in order of severity:\n\n")
    out.append("- **crash**: the process died or produced no report. This is the number that matters most, and the target is zero.\n- **timeout**: the watchdog fired. A hang is a crash that has not happened yet.\n- **rejected**: the engine refused with an error code and a reason. A password-protected or unparseable file is supposed to land here; it is not a failure of the engine, and the code says why.\n- **converted**: a conforming file was written. A fixed one-in-ten sample of these outputs is validated independently with veraPDF, the reference validator for the standard, and the pass rate is reported.\n\n")
    out.append("Conversion is the hardest path through the engine: it parses, repairs, embeds, converts colour, rebuilds metadata and serializes, so it exercises far more code than a check does.\n\n")
    out.append("## Corpora\n\n")
    out.append("| tranche | source | files | what it stresses |\n|---|---|---:|---|\n")
    for t, rs in by.items():
        title, what = TRANCHES.get(t, (t, ""))
        out.append(f"| {t} | {title} | {len(rs):,} | {what} |\n")
    notes = []
    if "cc" in by:
        notes.append("The web sample is pre-registered: the Common Crawl CC-MAIN-2021-31 index was read directly from its published shards, taking index blocks at a fixed stride under ten top-level domain prefixes (gov, edu, org, com, uk, de, fr, jp, au, ca), every record in those blocks served as PDF with HTTP status 200 and an archive record of at most 512 KB, in index order until each prefix had its share, and nothing was excluded afterwards. Each file's archive locator is in MANIFEST.csv, so the exact bytes can be fetched again.")
    if "safedocs" in by:
        notes.append("The SafeDocs sample is pre-registered: zip files `0000` to `0999` of the corpus were taken at a fixed stride of 20, and inside each chosen zip every PDF member of at most 512 KB was taken in directory order until the target was reached; nothing was excluded afterwards. Each file's origin names the zip and the member, so the exact bytes can be fetched again.")
    if "pdfjs" in by:
        notes.append("The pdf.js files are the complete set of PDFs checked into that project's test directory, nothing excluded.")
    out.append("\n" + " ".join(notes) + "\n\n")
    out.append("## Results\n\n")
    out.append("| tranche | files | converted | rejected | timeouts | crashes | verified sample | median time | p95 time |\n|---|---:|---:|---:|---:|---:|---:|---:|---:|\n")
    grand = Counter()
    for t, rs in list(by.items()) + [("all", rows)]:
        c = Counter(r["result"] for r in rs)
        sample = [r for r in rs if r["verified"]]
        passed = sum(1 for r in sample if r["verified"] == "pass")
        times = sorted(int(r["ms"]) for r in rs if r["ms"])
        med = statistics.median(times) if times else 0
        p95 = times[int(len(times) * 0.95) - 1] if times else 0
        label = "**all**" if t == "all" else t
        out.append(f"| {label} | {len(rs):,} | {c['converted']:,} | {c['rejected']:,} | {c['timeout']:,} | **{c['crash']:,}** | {passed:,}/{len(sample):,} | {med / 1000:.2f} s | {p95 / 1000:.1f} s |\n")
    out.append("\n### Why files were rejected\n\n| code | meaning | files |\n|---|---|---:|\n")
    codes = Counter(r["code"] for r in rows if r["result"] == "rejected")
    for code, n in codes.most_common():
        out.append(f"| `{code}` | {REJECT_TEXT.get(code, '')} | {n:,} |\n")
    bad = [r for r in rows if r["result"] in ("crash", "timeout")]
    if bad:
        out.append("\n### Crashes and timeouts\n\n| tranche | file | outcome | detail |\n|---|---|---|---|\n")
        for r in bad:
            out.append(f"| {r['tranche']} | {r['file']} | {r['result']} | {r['code']} |\n")
    failed = [r for r in rows if r["verified"] == "fail"]
    if failed:
        out.append("\n### Verified sample: files that did not pass veraPDF\n\n| tranche | file |\n|---|---|\n")
        for r in failed:
            out.append(f"| {r['tranche']} | {r['file']} |\n")
    out.append("\n## Appendix: every document tested\n\nThe per-file lists are split into parts of up to 2,000 rows so they stay readable on GitHub. Each row gives the file, its page count, its size, the outcome and where it came from.\n\n")
    for t, rs in by.items():
        title, _ = TRANCHES.get(t, (t, ""))
        parts = [rs[i:i + CHUNK] for i in range(0, len(rs), CHUNK)]
        for k, part in enumerate(parts, 1):
            name = f"{t}-{k:02d}.md"
            with open(os.path.join(DOCS, name), "w") as fh:
                fh.write(f"# {title}, part {k} of {len(parts)}\n\n{len(part):,} files. Back to [benchmark.md](../../benchmark.md).\n\n")
                fh.write("| file | pages | size | outcome | source |\n|---|---:|---:|---|---|\n")
                for r in part:
                    outcome = r["result"] if r["result"] != "rejected" else f"rejected: {r['code']}"
                    fh.write(f"| {r['file']} | {r['pages']} | {human(r['bytes'])} | {outcome} | {r['source']} |\n")
            out.append(f"- [{title}, part {k} of {len(parts)}](bench/documents/{name}) ({len(part):,} files)\n")
    with open(os.path.join(ROOT, "benchmark.md"), "w") as fh:
        fh.write("".join(out))
    repo = "https://github.com/alam00000/bentopdf-kura/blob/main/"
    docs_page = "".join(out).replace("](bench/documents/", "](" + repo + "bench/documents/") \
        .replace("[bench/RESULTS.csv](bench/RESULTS.csv)", "[bench/RESULTS.csv](" + repo + "bench/RESULTS.csv)") \
        .replace("[bench/MANIFEST.csv](bench/MANIFEST.csv)", "[bench/MANIFEST.csv](" + repo + "bench/MANIFEST.csv)") \
        .replace("so the run can be reproduced with `make bench`.", "so the run can be reproduced with `make bench` from a checkout of the repository.")
    with open(os.path.join(ROOT, "docs", "benchmark.md"), "w") as fh:
        fh.write(docs_page)
    print(f"benchmark.md + docs/benchmark.md: {total:,} files, {len(by)} tranche(s), {sum(1 for _ in os.listdir(DOCS))} document lists")


if __name__ == "__main__":
    main()
