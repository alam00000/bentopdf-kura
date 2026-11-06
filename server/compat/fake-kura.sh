#!/bin/sh
if [ "$1" = "--version" ]; then
  echo "Fake Engine (kura) 9.9.9"
  exit 0
fi
mode=""
prev=""
last=""
for a in "$@"; do
  if [ "$prev" = "--lang" ]; then mode="$a"; fi
  prev="$a"
  last="$a"
done
case "$last" in
  */out.pdf) out="$last" ;;
  *) out="" ;;
esac
if [ "$mode" = "slow" ]; then
  sleep 3
  mode=""
fi
case "$mode" in
  sleep) exec sleep 30 ;;
  exit3) exit 3 ;;
  exit64) printf '\n  \nunknown option --frob\nusage: kura\n' >&2; exit 64 ;;
  exit2) printf 'noise\n{"file":"x","ok":false,"level":"2b","errorCode":"PASSWORD_REQUIRED","error":"needs a password","suggestedLevel":"3b"}\n'; exit 2 ;;
  exit2null) printf '{"file":"x","ok":false,"errorCode":null,"error":null}\n'; exit 2 ;;
  exit2noreport) exit 2 ;;
  exit5) echo boom >&2; exit 5 ;;
  garbage) echo "not json"; exit 0 ;;
  check1) printf '{"file":"x","ok":true,"mode":"check","compliant":false}\n'; exit 1 ;;
  floats)
    printf '{"file":"x","ok":true,"level":7,"engine":"E","n":[1.0,1e21,1e-7,0.1,-0,12345678901234567890,2.50],"s":"\\u0001\\u2028\\u00e9/","issues":"abc","dup":1,"dup":2}\n'
    if [ -n "$out" ]; then printf '%%PDF-1.7 fake' > "$out"; fi
    exit 0 ;;
  noreport) if [ -n "$out" ]; then printf '%%PDF-1.7 fake' > "$out"; fi; exit 0 ;;
  nooutput) exit 0 ;;
  *)
    printf '{"file":"x","ok":true,"argc":%d,"timeout":"%s"}\n' "$#" "$PDFA_TIMEOUT"
    i=0
    for a in "$@"; do i=$((i+1)); printf 'arg%d=%s\n' "$i" "$a" >&2; done
    if [ -n "$out" ]; then printf '%%PDF-1.7 fake %s' "$*" > "$out"; fi
    exit 0 ;;
esac
