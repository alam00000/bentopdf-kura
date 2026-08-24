#!/bin/bash
set -u
DEST="$(cd "$(dirname "$0")/.." && pwd)/corpus"
mkdir -p "$DEST"

fetch_item() {
  local id="$1"
  local out="$DEST/${id}.pdf"
  [ -s "$out" ] && { echo "SKIP $id (exists)"; return 0; }
  local meta
  meta=$(curl -s --max-time 30 "https://archive.org/metadata/${id}")
  local file
  file=$(echo "$meta" | python3 -c '
import json, sys, urllib.parse
try:
    d = json.load(sys.stdin)
except Exception:
    sys.exit()
files = d.get("files", [])
cands = [f for f in files
         if f.get("name", "").lower().endswith(".pdf")
         and f.get("format", "") != "Additional Text PDF"]
cands.sort(key=lambda f: int(f.get("size", 0) or 0))
picks = [f for f in cands if 50_000 < int(f.get("size", 0) or 0) < 40_000_000]
if picks:
    print(urllib.parse.quote(picks[0]["name"]))
' 2>/dev/null)
  if [ -z "$file" ]; then echo "NOFILE $id"; return 1; fi
  echo "GET $id -> $file"
  curl -sL --max-time 300 -o "$out" "https://archive.org/download/${id}/${file}"
  if [ -s "$out" ] && head -c 5 "$out" | grep -q "%PDF-"; then
    echo "OK $id ($(du -h "$out" | cut -f1))"
  else
    echo "FAIL $id"; rm -f "$out"
  fi
}

search_ids() {
  local query="$1" rows="$2"
  local encoded
  encoded=$(python3 -c 'import urllib.parse,sys;print(urllib.parse.quote(sys.argv[1]))' "$query")
  curl -s --max-time 30 "https://archive.org/advancedsearch.php?q=${encoded}&fl%5B%5D=identifier&rows=${rows}&page=1&output=json" \
    | python3 -c '
import json, sys
try:
    [print(d["identifier"]) for d in json.load(sys.stdin)["response"]["docs"]]
except Exception:
    pass' 2>/dev/null
}

QUERIES=(
  'mediatype:texts AND format:("Text PDF") AND collection:gutenberg|3'
  'mediatype:texts AND format:("Text PDF") AND collection:(usgovernmentdocuments)|3'
  'mediatype:texts AND format:("Text PDF") AND collection:(opensource) AND year:[2015 TO 2024]|6'
  'mediatype:texts AND format:("Text PDF") AND collection:(internetarchivebooks)|3'
  'mediatype:texts AND format:("Text PDF") AND subject:(manual)|3'
  'mediatype:texts AND format:("Text PDF") AND collection:(magazine_rack)|6'
  'mediatype:texts AND format:("Text PDF") AND subject:(catalog) AND year:[2000 TO 2024]|5'
  'mediatype:texts AND format:("Text PDF") AND collection:(usgovernmentdocuments) AND year:[1995 TO 2023]|6'
  'mediatype:texts AND format:("Text PDF") AND subject:(form)|4'
  'mediatype:texts AND format:("Text PDF") AND subject:(mathematics)|5'
  'mediatype:texts AND format:("Text PDF") AND subject:(music AND sheet)|3'
  'mediatype:texts AND format:("Text PDF") AND collection:(opensource) AND year:[1998 TO 2008]|6'
  'mediatype:texts AND format:("Text PDF") AND subject:(brochure)|4'
  'mediatype:texts AND format:("Text PDF") AND subject:(presentation OR slides)|4'
  'mediatype:texts AND format:("Text PDF") AND language:(Japanese OR Chinese OR Arabic)|5'
)

echo "=== searching archive.org ==="
IDS=""
for entry in "${QUERIES[@]}"; do
  IDS+=$'\n'"$(search_ids "${entry%|*}" "${entry##*|}")"
done

echo "$IDS" | sort -u | while read -r id; do
  [ -z "$id" ] && continue
  fetch_item "$id"
done

echo "=== corpus summary ==="
ls -la "$DEST"
echo "count: $(ls "$DEST" | wc -l)"
