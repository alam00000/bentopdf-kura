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

The server has no authentication unless `KURA_API_TOKEN` is set, and it binds all interfaces by default. On a public host, set a token or bind `KURA_HOST` to localhost behind an authenticating reverse proxy, which is also where TLS and rate limiting belong.

The container runs as a non-root user. Every job is a separate engine process with a time limit; a crash ends the job, not the service.

## CLI mode

The same image doubles as the CLI: pass arguments and it converts instead of serving. Run it as your own user so the output can be written into the mounted folder:

```bash
docker run --rm --user "$(id -u):$(id -g)" -v "$PWD":/work \
  ghcr.io/alam00000/bentopdf-kura:latest --level 2b in.pdf out.pdf
```

## Without Docker

```bash
npm ci && KURA_BIN=/path/to/kura npm run serve
```

More in the docs: [Self-hosting](https://kura.bentopdf.com/docs/self-hosting).
