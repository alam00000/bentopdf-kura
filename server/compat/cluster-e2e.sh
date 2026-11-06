#!/usr/bin/env bash
set -euo pipefail

REPO="$(cd "$(dirname "$0")/../.." && pwd)"
KURA="${1:-$REPO/pdfa-engine/build/cli/kura}"
SERVER="${2:-$REPO/server/target/release/kura-server}"
LEVEL="${E2E_LEVEL:-x1a}"
WORK="$(mktemp -d "${TMPDIR:-/tmp}/kura-e2e.XXXXXX")"
CPORT=$((23000 + RANDOM % 1000))
FPORT=$((CPORT + 1))
export KURA_BIN="$KURA" KURA_PROFILES="$REPO/pdfa-engine/profiles" KURA_HOST=127.0.0.1 KURA_CLUSTER_TOKEN=e2e-token
PIDS=()
CERTS="$WORK/certs"
mkdir -p "$CERTS"
printf 'subjectAltName=DNS:localhost,IP:127.0.0.1\nbasicConstraints=CA:FALSE\n' >"$CERTS/ext.cnf"
openssl req -x509 -newkey rsa:2048 -nodes -keyout "$CERTS/ca.key" -out "$CERTS/ca.pem" -days 2 -subj '/CN=kura e2e ca' 2>/dev/null
openssl req -newkey rsa:2048 -nodes -keyout "$CERTS/key.pem" -out "$CERTS/srv.csr" -subj '/CN=localhost' 2>/dev/null
openssl x509 -req -in "$CERTS/srv.csr" -CA "$CERTS/ca.pem" -CAkey "$CERTS/ca.key" -CAcreateserial \
  -out "$CERTS/cert.pem" -days 2 -extfile "$CERTS/ext.cnf" 2>/dev/null
export KURA_CA_FILE="$CERTS/ca.pem"
CURL=(curl --cacert "$CERTS/ca.pem")
COORD_URL="https://localhost:$CPORT"

cleanup() {
  for p in ${PIDS[@]+"${PIDS[@]}"}; do kill -9 "$p" 2>/dev/null || true; done
  if [ -z "${KEEP:-}" ]; then rm -rf "$WORK"; else echo "kept $WORK"; fi
}
trap cleanup EXIT

py() { python3 -c "import json,sys; d=json.load(sys.stdin); print($1)"; }
status() { "${CURL[@]}" -sf "$COORD_URL/status.json"; }
counts() { status | py "(d['queued'], d['running'], d['done'] + d['failed'])"; }
coordinator() {
  KURA_PORT=$CPORT KURA_LEASE_MS=3000 KURA_DATA="$WORK/data" \
    KURA_TLS_CERT="$CERTS/cert.pem" KURA_TLS_KEY="$CERTS/key.pem" \
    "$SERVER" coordinator --hot-folder "$WORK/hot?level=$LEVEL" >>"$WORK/coordinator.log" 2>&1 &
  COORD=$!
  disown
  PIDS+=("$COORD")
  for _ in $(seq 1 100); do "${CURL[@]}" -sf "$COORD_URL/healthz" >/dev/null && return; sleep 0.1; done
  echo "coordinator did not start"; cat "$WORK/coordinator.log"; exit 1
}
worker() {
  "$SERVER" worker --coordinator "$COORD_URL" --slots 1 --name "$1" >>"$WORK/worker-$1.log" 2>&1 &
  PIDS+=("$!")
  echo "$!" >"$WORK/$1.pid"
  disown
}

mkdir -p "$WORK/api-in" "$WORK/api-out" "$WORK/expected"
files=()
for f in "$REPO"/corpus/EB20-*.pdf "$REPO"/corpus/premier-*.pdf "$REPO"/corpus/pdf699_202002.pdf "$REPO"/corpus/EBSO-*.pdf \
         "$REPO"/corpus/rtn-219-*.pdf "$REPO"/corpus/panteao-*.pdf "$REPO"/corpus/rsk2-*.pdf "$REPO"/corpus/misc-post_*.pdf \
         "$REPO"/corpus/2000-5-30_*.pdf "$REPO"/corpus/heartland-*.pdf; do
  [ -f "$f" ] && files+=("$f")
done
while IFS= read -r f; do files+=("$f"); done < <(find "$REPO/corpus-synth" "$REPO/tests/fixtures" -name '*.pdf' 2>/dev/null | sort)
n=0
while [ "${#files[@]}" -lt 50 ]; do
  n=$((n + 1))
  synth="$WORK/synthetic-$n.pdf"
  printf '%%PDF-1.7\n1 0 obj\n<< /Type /Catalog /Pages 2 0 R >>\nendobj\n2 0 obj\n<< /Type /Pages /Kids [3 0 R] /Count 1 >>\nendobj\n3 0 obj\n<< /Type /Page /Parent 2 0 R /MediaBox [0 0 %d 792] /Resources << >> >>\nendobj\ntrailer\n<< /Size 4 /Root 1 0 R >>\n%%%%EOF\n' $((500 + n)) >"$synth"
  files+=("$synth")
done
files=("${files[@]:0:50}")
echo "inputs: ${#files[@]} PDFs, level $LEVEL, coordinator at $COORD_URL (TLS, private CA)"
[ "${#files[@]}" -eq 50 ] || { echo "need 50 PDFs"; exit 1; }

coordinator
KURA_PORT=$FPORT "$SERVER" serve --coordinator "$COORD_URL" >>"$WORK/front.log" 2>&1 &
PIDS+=("$!")
disown
for _ in $(seq 1 100); do curl -sf "http://127.0.0.1:$FPORT/healthz" >/dev/null && break; sleep 0.1; done
worker w1
worker w2

CURLS=()
for i in $(seq 0 49); do
  f="${files[$i]}"
  name="$(printf '%02d' "$i")-$(basename "$f")"
  if [ $((i % 2)) -eq 0 ]; then
    cp "$f" "$WORK/api-in/$name"
    curl -s -m 900 -o "$WORK/api-out/$name" -w '%{http_code}\n' --data-binary @"$f" \
      "http://127.0.0.1:$FPORT/api/convert?level=$LEVEL" >"$WORK/api-out/$name.status" &
    CURLS+=("$!")
  else
    cp "$f" "$WORK/hot/input/$name"
  fi
done
echo "submitted 25 via the API (through serve --coordinator) and 25 into the hot folder"

killed="[]"
for _ in $(seq 1 600); do
  killed="$(status | py "[j['id'] for j in d['jobs'] if j['state']=='running' and (j['worker'] or '').startswith('w1-')]" 2>/dev/null || echo '[]')"
  [ "$killed" != "[]" ] && break
  sleep 0.05
done
[ "$killed" != "[]" ] || { echo "w1 never ran a job"; exit 1; }
kill -9 "$(cat "$WORK/w1.pid")"
echo "killed worker w1 (kill -9) while it was running job(s) $killed"

before=""
for _ in $(seq 1 600); do
  before="$(counts)"
  [ "$(status | py "d['queued']")" -gt 0 ] && break
  sleep 0.05
done
kill -9 "$COORD"
echo "killed the coordinator (kill -9) at (queued, running, finished) = $before"
sleep 1
coordinator
echo "restarted the coordinator:            (queued, running, finished) = $(counts)"
worker w3

for p in "${CURLS[@]}"; do wait "$p" || true; done
for _ in $(seq 1 1200); do
  n=$(( $(find "$WORK/hot/done" -name '*.json' | wc -l) + $(find "$WORK/hot/failed" -name '*.json' | wc -l) ))
  [ "$n" -ge 25 ] && break
  sleep 0.1
done

fail=0
api_ok=0; api_rejected=0; identical=0
for st in "$WORK"/api-out/*.status; do
  name="$(basename "$st" .status)"
  code="$(cat "$st")"
  "$KURA" --level "$LEVEL" "$WORK/api-in/$name" "$WORK/expected/$name" >/dev/null 2>&1 || true
  case "$code" in
    200) api_ok=$((api_ok + 1))
         if python3 "$REPO/server/compat/same-pdf.py" "$WORK/api-out/$name" "$WORK/expected/$name"; then identical=$((identical + 1)); else echo "output differs from the CLI: $name"; fi ;;
    422) api_rejected=$((api_rejected + 1)) ;;
    *) echo "API job $name answered $code"; fail=1 ;;
  esac
done
hot_done=$(find "$WORK/hot/done" -name '*.json' | wc -l | tr -d ' ')
hot_failed=$(find "$WORK/hot/failed" -name '*.json' | wc -l | tr -d ' ')
hot_left=$(find "$WORK/hot/input" -type f | wc -l | tr -d ' ')
retried="$(status | py "[(j['id'], j['name'], j['attempts'], j['state'], 'exit %s' % j['exitCode']) for j in d['jobs'] if j['attempts'] > 1]")"
echo "API: $api_ok converted ($identical identical to a direct CLI run, timestamps and IDs aside), $api_rejected rejected by the engine (422)"
echo "hot folder: $hot_done done, $hot_failed failed (each with its report), $hot_left left in input/"
echo "retried jobs (id, name, attempts, state, engine exit): $retried"
echo "final status: $(status | py "[(k, d[k]) for k in ('queued', 'running', 'done', 'failed')]")"
echo "workers: $(status | py "[(w['name'], w['alive']) for w in d['workers']]")"
grep -h "lease expired" "$WORK/coordinator.log" | sed 's/^/  log: /' | head -5 || true
if [ $((hot_done + hot_failed)) -ne 25 ] || [ "$hot_left" -ne 0 ]; then fail=1; fi
if [ $((api_ok + api_rejected)) -ne 25 ] || [ "$identical" -ne "$api_ok" ]; then fail=1; fi
if [ "$retried" = "[]" ]; then echo "no job was retried"; fail=1; fi
if [ "$fail" -eq 0 ]; then echo "cluster e2e: passed"; else echo "cluster e2e: FAILED"; exit 1; fi
