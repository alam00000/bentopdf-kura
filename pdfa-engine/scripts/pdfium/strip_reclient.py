#!/usr/bin/env python3
import re
import sys

PATTERN = re.compile(
    r"\n  'buildtools/reclient': \{.*?\n  \},\n", re.DOTALL)

def main():
    path = sys.argv[1]
    with open(path) as fh:
        text = fh.read()
    stripped, n = PATTERN.subn("\n", text)
    if n == 0:
        print("buildtools/reclient not present; nothing to strip")
        return 0
    with open(path, "w") as fh:
        fh.write(stripped)
    print(f"stripped {n} reclient dep(s) from {path}")
    return 0

if __name__ == "__main__":
    sys.exit(main())
