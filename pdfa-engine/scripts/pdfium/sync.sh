#!/usr/bin/env bash
set -euo pipefail

PDFIUM_BRANCH="${PDFIUM_BRANCH:-chromium/7961}"
DEPOT_TOOLS_REV="${DEPOT_TOOLS_REV:-c0148d63d4909b3f27c9df5b6273efc496bc4459}"

HERE="$(cd "$(dirname "$0")" && pwd)"
mkdir -p "$HERE/../../third_party/pdfium-build"
ROOT="$(cd "$HERE/../../third_party/pdfium-build" && pwd)"

cd "$ROOT"

if [[ ! -d depot_tools/.git ]]; then
  git clone https://chromium.googlesource.com/chromium/tools/depot_tools.git depot_tools
fi
git -C depot_tools checkout -q "$DEPOT_TOOLS_REV" 2>/dev/null || true

export PATH="$ROOT/depot_tools:$PATH"
case "$(uname -s)" in
  MINGW*|MSYS*|CYGWIN*) export DEPOT_TOOLS_UPDATE=1 ;;
  *) export DEPOT_TOOLS_UPDATE=0 ;;
esac

if [[ ! -d pdfium-src/.git ]]; then
  git clone --depth 1 -b "$PDFIUM_BRANCH" \
    https://pdfium.googlesource.com/pdfium.git pdfium-src
fi

python3 "$HERE/strip_reclient.py" "$ROOT/pdfium-src/DEPS"

cat > "$ROOT/.gclient" <<EOF
solutions = [
  {
    "name": "pdfium-src",
    "url": "https://pdfium.googlesource.com/pdfium.git",
    "managed": False,
    "custom_deps": {},
    "deps_file": "DEPS",
  },
]
EOF

gclient sync --no-history --shallow --force

if [[ -f "$ROOT/pdfium-src/tools/clang/scripts/update.py" ]]; then
  python3 "$ROOT/pdfium-src/tools/clang/scripts/update.py"
  python3 "$ROOT/pdfium-src/tools/clang/scripts/update.py" --package=objdump || true
fi

echo "pdfium $PDFIUM_BRANCH @ $(git -C pdfium-src rev-parse HEAD)"
