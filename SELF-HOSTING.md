# Self-hosting

The Docker image runs a small web service: the converter and preflight pages at `/` and `/preflight`, and an HTTP API, all backed by the native engine. Files are processed on your server and never leave it.

```bash
docker run -d -p 8080:8080 ghcr.io/alam00000/bentopdf-kura:latest
```

or with compose:

```bash
docker compose up -d     # uses the docker-compose.yml in this repository
```

Open http://localhost:8080 for the converter.

## API

`POST /api/convert?level=<level>` with the PDF as the raw request body. The response body is the converted PDF.

```bash
curl -o out.pdf --data-binary @in.pdf 'http://localhost:8080/api/convert?level=2b'
```

`POST /api/check?level=<level>` returns the report as JSON without changing anything; add `analyze=true` or `profile=<bundled profile>` for preflight findings. `GET /api/profiles` lists the bundled profiles, `POST /api/verify-password` checks a password, `GET /healthz` reports `{ok, version, active, queued}`.

Query parameters take the same names as the npm package options (`ua`, `lang`, `rasterDpi`, `outputCondition`, ...). Send `multipart/form-data` with a `file` field when you also need to send a custom `profile`, an e-invoice `xml` or ICC profiles. `X-Password` carries the password of an encrypted input.

Errors are JSON with `ok: false`, an `errorCode` and an `error`: `400 BAD_LEVEL` / `BAD_REQUEST` / `BAD_PROFILE` / `NOT_A_PDF`, `401 UNAUTHORIZED`, `413 TOO_LARGE`, `422` with the engine's rejection code, `429 BUSY`, `504 TIMEOUT`.

The full reference is in the docs: [HTTP API](https://kura.bentopdf.com/docs/http-api).

## Configuration

| env var | default | meaning |
|---|---|---|
| `KURA_HOST` | 0.0.0.0 | listen address; set to 127.0.0.1 to bind localhost only |
| `KURA_PORT` | 8080 | listen port |
| `KURA_MAX_UPLOAD_MB` | 500 | reject larger uploads |
| `KURA_CONCURRENCY` | 2 | parallel engine jobs |
| `KURA_QUEUE` | 8 | jobs allowed to wait before 429 |
| `KURA_TIMEOUT_MS` | 600000 | per-job time limit |
| `KURA_API_TOKEN` | (unset) | when set, `/api/*` requires `Authorization: Bearer <token>` (or `X-Api-Token`) |
| `KURA_LOG_FORMAT` | text | `json` for one JSON object per log line |

The server has no authentication unless `KURA_API_TOKEN` is set, and it binds all interfaces by default. On a public host, set a token or bind `KURA_HOST` to localhost behind an authenticating reverse proxy, which is also where TLS and rate limiting belong.

The container runs as a non-root user. Every job is a separate engine process with a time limit; a crash ends the job, not the service.

## CLI mode

The same image doubles as the CLI: pass arguments and it converts instead of serving. Run it as your own user so the output can be written into the mounted folder:

```bash
docker run --rm --user "$(id -u):$(id -g)" -v "$PWD":/work \
  ghcr.io/alam00000/bentopdf-kura:latest --level 2b in.pdf out.pdf
```

## Several machines

One machine runs the coordinator, the others run workers; programs use the same API against the coordinator.

```bash
docker run -d -p 8443:8080 -e KURA_CLUSTER_TOKEN=change-me -v kura-data:/data -v /etc/kura/tls:/tls:ro \
  -e KURA_TLS_CERT=/tls/cert.pem -e KURA_TLS_KEY=/tls/key.pem \
  ghcr.io/alam00000/bentopdf-kura:latest coordinator --hot-folder '/srv/scans?level=2b'
docker run -d -e KURA_CLUSTER_TOKEN=change-me -v /etc/kura/tls/ca.pem:/tls/ca.pem:ro -e KURA_CA_FILE=/tls/ca.pem \
  ghcr.io/alam00000/bentopdf-kura:latest worker --coordinator https://coordinator-host:8443 --slots 4
```

The queue is kept on disk in `KURA_DATA` and survives restarts. A job whose worker goes quiet for `KURA_LEASE_MS` (30 s) is handed to another worker, up to `KURA_MAX_ATTEMPTS` (3) tries. Watched folders get `input/`, `done/` (output plus `.json` report) and `failed/` (original plus `.json` reason). `dedupe=true` reuses an identical recent result. `/status` shows workers, queue and recent jobs; `/status.json` has the same as JSON. `serve --coordinator <url>` runs the web interface and API on another machine and forwards every job. The coordinator speaks HTTPS with `KURA_TLS_CERT`/`KURA_TLS_KEY`; workers trust an own authority with `KURA_CA_FILE`. Plain HTTP between machines is refused unless `KURA_INSECURE_HTTP=1`. Jobs may only carry the API's options, checked on the coordinator and again on each worker.

## Without Docker

```bash
cargo build --release --manifest-path server/Cargo.toml
KURA_BIN=/path/to/kura server/target/release/kura-server serve
```

More in the docs: [Self-hosting](https://kura.bentopdf.com/docs/self-hosting).
