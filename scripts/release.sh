#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

BUMP="${1:-patch}"
DRY_RUN="${DRY_RUN:-0}"
HEADER="pdfa-engine/core/include/kura/kura.h"
PDFA_HH="pdfa-engine/core/include/pdfa/pdfa.hh"

usage() {
  echo "usage: scripts/release.sh [patch|minor|major|X.Y.Z]" >&2
  echo "  DRY_RUN=1 computes the version and runs the checks without changing anything" >&2
  exit 2
}

CURRENT="$(sed -n 's/^#define KURA_VERSION "\(.*\)"/\1/p' "$HEADER")"
[ -n "$CURRENT" ] || { echo "could not read KURA_VERSION from $HEADER" >&2; exit 1; }

case "$BUMP" in
  major|minor|patch)
    MAJ="${CURRENT%%.*}"
    REST="${CURRENT#*.}"
    MIN="${REST%%.*}"
    PAT="${REST#*.}"
    case "$BUMP" in
      major) VERSION="$((MAJ + 1)).0.0" ;;
      minor) VERSION="$MAJ.$((MIN + 1)).0" ;;
      patch) VERSION="$MAJ.$MIN.$((PAT + 1))" ;;
    esac
    ;;
  *)
    if printf '%s' "$BUMP" | grep -Eq '^[0-9]+\.[0-9]+\.[0-9]+$'; then
      VERSION="$BUMP"
    else
      usage
    fi
    ;;
esac

TAG="v$VERSION"
echo "==> releasing $CURRENT -> $VERSION"

[ "$(git branch --show-current)" = "main" ] || { echo "release only from main" >&2; exit 1; }
[ -z "$(git status --porcelain)" ] || { echo "working tree not clean; commit or stash first" >&2; exit 1; }
git fetch origin main --quiet
[ "$(git rev-parse HEAD)" = "$(git rev-parse origin/main)" ] || { echo "main is not up to date with origin/main" >&2; exit 1; }
git rev-parse "$TAG" >/dev/null 2>&1 && { echo "tag $TAG already exists" >&2; exit 1; }

if [ "$DRY_RUN" = "1" ]; then
  echo "==> dry run: would stamp $VERSION, run make check, commit, tag $TAG, and push"
  make check
  echo "==> dry run complete; nothing was changed"
  exit 0
fi

sed -i.bak "s/^#define KURA_VERSION \".*\"/#define KURA_VERSION \"$VERSION\"/" "$HEADER" && rm -f "$HEADER.bak"
sed -i.bak "s/kEngineVersion = \".*\"/kEngineVersion = \"$VERSION\"/" "$PDFA_HH" && rm -f "$PDFA_HH.bak"
VERSION="$VERSION" node <<'EOF'
const fs = require('fs');
const version = process.env.VERSION;
const stamp = (p) => {
  const pkg = JSON.parse(fs.readFileSync(p, 'utf8'));
  pkg.version = version;
  for (const name of Object.keys(pkg.optionalDependencies ?? {})) pkg.optionalDependencies[name] = version;
  fs.writeFileSync(p, JSON.stringify(pkg, null, 2) + '\n');
  console.log(`stamped ${pkg.name} ${version}`);
};
stamp('package.json');
for (const dir of fs.readdirSync('packages/npm')) stamp(`packages/npm/${dir}/package.json`);
EOF

make check

git add "$HEADER" "$PDFA_HH" package.json packages/npm/*/package.json
if git diff --cached --quiet; then
  echo "==> versions already at $VERSION; tagging current HEAD"
else
  git commit -m "chore: release $TAG"
fi
git tag -a "$TAG" -m "release $TAG"
git push origin main
git push origin "$TAG"

echo "==> $TAG pushed; CI takes it from here:"
echo "    1. release.yml builds Linux, Windows, macOS, the wasm module and the docker image"
echo "    2. it publishes the GitHub release with SHA256SUMS"
echo "    3. npm-publish.yml publishes kura-pdf, kura-pdf-wasm and the platform packages"
