#!/usr/bin/env python3
import argparse
import concurrent.futures
import csv
import json
import os
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
KURA = os.environ.get("KURA_CLI", os.path.join(ROOT, "pdfa-engine", "build", "cli", "kura"))
CALLAS = os.environ.get("CALLAS_PDFTOOLBOX", "")
FIELDS = ["file", "tranche", "level", "result", "code", "judge", "verdict", "errors", "ms"]

VERAPDF = {"1a": "1a", "1b": "1b", "2a": "2a", "2b": "2b", "2u": "2u", "3a": "3a", "3b": "3b", "3u": "3u",
           "4": "4", "4e": "4e", "4f": "4f", "ua1": "ua1", "ua2": "ua2"}
CALLAS_PROFILES = {"x1a": "PDFX compliance/Verify compliance with PDFX-1a", "x3": "PDFX compliance/Verify compliance with PDFX-3",
                   "x4": "PDFX compliance/Verify compliance with PDFX-4", "x6": "PDFX compliance/Verify compliance with PDFX-6",
                   "e1": "PDFE compliance/Verify compliance with PDFE-1", "vt1": "PDFVT compliance/Verify compliance with PDFVT-1",
                   "vt3": "PDFVT compliance/Verify compliance with PDFVT-3"}
LEVELS = list(VERAPDF) + list(CALLAS_PROFILES)


def convert(src, level, dst):
    args = [KURA, "--level", level, src, dst]
    if level == "ua1":
        args = [KURA, "--level", "2a", "--ua", src, dst]
    if level == "ua2":
        args = [KURA, "--level", "4", "--ua", src, dst]
    t = time.time()
    try:
        p = subprocess.run(args, capture_output=True, text=True, errors="replace", timeout=300)
    except subprocess.TimeoutExpired:
        return "timeout", "", int((time.time() - t) * 1000)
    ms = int((time.time() - t) * 1000)
    try:
        rep = json.loads(p.stdout)
    except json.JSONDecodeError:
        return "crash", f"exit {p.returncode}", ms
    return ("converted", "", ms) if rep.get("ok") else ("rejected", rep.get("errorCode", "?"), ms)


def verapdf(paths, flavour):
    out = {}
    for i in range(0, len(paths), 50):
        chunk = paths[i:i + 50]
        try:
            p = subprocess.run(["verapdf", "-f", flavour, "--format", "json"] + chunk, capture_output=True, text=True,
                               timeout=7200)
            jobs = json.loads(p.stdout)["report"]["jobs"]
        except (subprocess.TimeoutExpired, json.JSONDecodeError, KeyError):
            if len(chunk) > 1:
                for path in chunk:
                    out.update(verapdf([path], flavour))
            else:
                out[chunk[0]] = ("error", 0)
            continue
        for job in jobs:
            name = job.get("itemDetails", {}).get("name", "")
            try:
                vr = job["validationResult"][0]
                fails = sum(r.get("failedChecks", 1) for r in vr.get("details", {}).get("ruleSummaries", [])
                            if r.get("ruleStatus") == "FAILED" or r.get("status") == "failed")
                out[name] = ("pass" if vr["compliant"] else "fail", fails)
            except (KeyError, IndexError):
                out[name] = ("error", 0)
    return out


def callas(path, level):
    profile = os.path.join(os.path.dirname(CALLAS), "var", "Profiles", CALLAS_PROFILES[level] + ".kfpx")
    try:
        p = subprocess.run([CALLAS, profile, path], capture_output=True, text=True, errors="replace", timeout=600)
    except subprocess.TimeoutExpired:
        return "error", 0
    for line in p.stdout.splitlines():
        if line.startswith("Summary\tErrors\t"):
            n = int(line.split("\t")[2] or 0)
            return ("pass" if n == 0 else "fail"), n
    return "error", 0


def main():
    ap = argparse.ArgumentParser(description="Convert the conformance corpus to every level and judge each output.")
    ap.add_argument("--levels", nargs="*", default=LEVELS)
    ap.add_argument("--workers", type=int, default=max(2, (os.cpu_count() or 4) - 2))
    ap.add_argument("--corpus", default=os.path.join(HERE, "corpus", "files"))
    ap.add_argument("--manifest", default=os.path.join(HERE, "MANIFEST.csv"))
    args = ap.parse_args()
    with open(args.manifest) as fh:
        files = list(csv.DictReader(fh))
    results_path = os.path.join(HERE, "RESULTS.csv")
    done = set()
    if os.path.exists(results_path):
        with open(results_path) as fh:
            done = {(r["file"], r["level"]) for r in csv.DictReader(fh)}
    new = not os.path.exists(results_path)
    out = open(results_path, "a", newline="")
    w = csv.DictWriter(out, FIELDS)
    if new:
        w.writeheader()
    for level in args.levels:
        work = [f for f in files if (f["file"], level) not in done]
        if not work:
            continue
        outdir = os.path.join(HERE, "corpus", "out", level)
        os.makedirs(outdir, exist_ok=True)

        def one(f):
            dst = os.path.join(outdir, f["file"])
            return f, dst, convert(os.path.join(args.corpus, f["file"]), level, dst)

        with concurrent.futures.ThreadPoolExecutor(args.workers) as ex:
            conv = list(ex.map(one, work))
        made = [dst for _, dst, (res, _, _) in conv if res == "converted"]
        if level in VERAPDF:
            verdicts, judge = verapdf(made, VERAPDF[level]), "veraPDF"
        else:
            with concurrent.futures.ThreadPoolExecutor(args.workers) as ex:
                verdicts, judge = dict(zip(made, ex.map(lambda p: callas(p, level), made))), "callas pdfToolbox"
        for f, dst, (res, code, ms) in conv:
            verdict, errors = verdicts.get(dst, ("", "")) if res == "converted" else ("", "")
            w.writerow({"file": f["file"], "tranche": f["tranche"], "level": level, "result": res, "code": code,
                        "judge": judge if res == "converted" else "", "verdict": verdict, "errors": errors, "ms": ms})
            if res == "converted" and verdict == "pass":
                os.remove(dst)
        out.flush()
        passed = sum(1 for d in made if verdicts.get(d, ("",))[0] == "pass")
        print(f"{level}: {len(work)} files, {len(made)} converted, {passed} passed {judge}", flush=True)
    out.close()


if __name__ == "__main__":
    main()
