# Self-hosting

Run Kura on your own hardware: the web interface, the [HTTP API](/http-api) and the CLI in one container. Files are processed on your server and never leave it, which is usually the entire reason to self-host a PDF tool.

## What you need

Docker. If `docker --version` prints a version, you are ready; otherwise install [Docker Engine](https://docs.docker.com/engine/install/) (Linux) or [Docker Desktop](https://docs.docker.com/desktop/) (macOS, Windows). The published image is `linux/amd64`.

## Start it

```bash
docker run -d --name kura -p 8080:8080 ghcr.io/alam00000/bentopdf-kura:latest
```

Open `http://localhost:8080`. That is the same converter as [kura.bentopdf.com](https://kura.bentopdf.com), with the preflight page at `/preflight`, except the work happens on this machine through the native engine: every standard, every bundled preflight profile, raster flattening for PDF/A-1 and PDF/X-1a, and no upload limit other than the one you set. Programs talk to `/api/convert` and `/api/check`; see the [HTTP API](/http-api).

Or with compose, using the `docker-compose.yml` from the repository:

```bash
docker compose up -d
```

Images are published to GitHub's registry on every release, tagged with the version (`v1.1.0`) and `latest`. Pin the version tag in production.

## Configuration

Set these with `-e` flags or in your compose file:

| Env var | Default | Meaning |
|---|---|---|
| `KURA_HOST` | 0.0.0.0 | listen address; set to 127.0.0.1 to bind localhost only |
| `KURA_PORT` | 8080 | listen port inside the container |
| `KURA_MAX_UPLOAD_MB` | 500 | reject larger uploads |
| `KURA_CONCURRENCY` | 2 | parallel engine jobs |
| `KURA_QUEUE` | 8 | jobs allowed to wait before new ones get 429 |
| `KURA_TIMEOUT_MS` | 600000 | per-job time limit (10 minutes); the engine's own watchdog is set from it |
| `KURA_API_TOKEN` | (unset) | when set, `/api/*` requires `Authorization: Bearer <token>` (or `X-Api-Token`); unauthenticated otherwise |
| `KURA_LOG_FORMAT` | text | `json` writes one JSON object per log line, for log collectors |
| `KURA_LOG_LEVEL` | info | `warn` or `error` for quieter logs, `debug` for more |
| `KURA_TLS_CERT`, `KURA_TLS_KEY` | (unset) | PEM certificate chain and key; set both and the service speaks HTTPS itself |
| `KURA_BIN` | `/usr/local/bin/kura` | the engine binary the service runs for each job |
| `KURA_PROFILES` | `pdfa-engine/profiles` (`/kura/pdfa-engine/profiles` in the image) | the folder of bundled preflight profiles listed at `/api/profiles` |
| `KURA_SITE` | `site` (`/kura/site` in the image) | the folder with the web pages |

Numbers outside their range are clamped: `KURA_CONCURRENCY` 1 to 1024, `KURA_QUEUE` 0 to 100000, `KURA_TIMEOUT_MS` at least 1000, `KURA_MAX_UPLOAD_MB` at least 1. A value that is not a number is ignored and the default used. The service prints the concurrency, queue and upload limit it ended up with in its first log line.

The engine inside the container also reads its own variables, such as `PDFA_TIMEOUT` and `KURA_FONT_DIR`; see [CLI environment](/cli#environment). The service sets `PDFA_TIMEOUT` itself from `KURA_TIMEOUT_MS`.

Each job is a separate engine process with its own memory, so size `KURA_CONCURRENCY` to your cores and memory: 2 is right for a small VPS, 4 to 8 for a real server. A 500-page scanned document at PDF/A-1 can need a few gigabytes while its pages are rasterized.

## Put TLS in front

The container speaks plain HTTP and, unless you set a token, does no authentication, which is the standard contract for a service container: terminate TLS at a reverse proxy. With [Caddy](https://caddyserver.com) the entire configuration is:

```
kura.example.com {
    reverse_proxy localhost:8080
}
```

Caddy obtains and renews the certificate itself. If you already have a certificate, the service can also speak HTTPS directly: set `KURA_TLS_CERT` and `KURA_TLS_KEY` to the PEM files. Any reverse proxy works (nginx, Traefik); raise its request body limit to match `KURA_MAX_UPLOAD_MB`, and if the instance is public, add rate limiting there too.

## Updating

```bash
docker pull ghcr.io/alam00000/bentopdf-kura:latest
docker stop kura && docker rm kura
docker run -d --name kura -p 8080:8080 ghcr.io/alam00000/bentopdf-kura:latest
```

The single-machine container is stateless; nothing needs migrating. A coordinator keeps its queue in `KURA_DATA`, so mount that as a volume and the queue carries over the update.

## Monitoring

`GET /healthz` returns `{ok, version, active, queued}`. Wire it into uptime checks as is; sustained `queued` near `KURA_QUEUE` means you should raise concurrency or add hardware.

## CLI mode

The same image doubles as the CLI: pass arguments and it converts instead of serving. Run it as your own user so the output can be written into the mounted folder:

```bash
docker run --rm --user "$(id -u):$(id -g)" -v "$PWD":/work \
  ghcr.io/alam00000/bentopdf-kura:latest --level 2b in.pdf out.pdf
```

Every [CLI option](/cli) works, including batch mode over a mounted folder and the [tools](/cli#tools):

```bash
docker run --rm -v "$PWD":/work ghcr.io/alam00000/bentopdf-kura:latest tool info in.pdf
```

The rule is simple: a first argument of `serve`, `coordinator` or `worker`, or none at all, starts the service; anything else is handed to `kura`. The bundled colour profiles and the world and CJK font packs are in `/usr/local/share/kura`, where the CLI finds them on its own.

## Without Docker

The service is one native program, `kura-server`, that runs the `kura` engine for each job. Build it with a current stable Rust and point it at a native build or a release binary of the engine:

```bash
cargo build --release --manifest-path server/Cargo.toml
KURA_BIN=/usr/local/bin/kura server/target/release/kura-server serve
```

Run it from the repository folder, or set `KURA_SITE` to the folder with the web interface and `KURA_PROFILES` to the bundled profiles (`site` and `pdfa-engine/profiles` by default).

`kura-server` has three roles. Every option can also be set by the environment variable shown, which is how the container is usually configured:

| Command | Options |
|---|---|
| `kura-server serve` (the default with no command) | `--coordinator <url>` (`KURA_COORDINATOR`): pass every job to a coordinator instead of running it here |
| `kura-server coordinator` | `--data <dir>` (`KURA_DATA`, default `kura-data`), `--hot-folder <spec>` (repeatable; `KURA_HOT_FOLDERS`, separated by `;`), `--max-attempts <n>` (`KURA_MAX_ATTEMPTS`, 3), `--lease-ms <ms>` (`KURA_LEASE_MS`, 30000), `--result-ttl-s <s>` (`KURA_RESULT_TTL_S`, 3600) |
| `kura-server worker` | `--coordinator <url>` (`KURA_COORDINATOR`, required), `--slots <n>` (`KURA_SLOTS`), `--name <name>` (`KURA_WORKER_NAME`, `worker`) |
| any role | `--tls-cert`, `--tls-key` (`KURA_TLS_CERT`, `KURA_TLS_KEY`), `--ca-file` (`KURA_CA_FILE`), `--insecure-http` (`KURA_INSECURE_HTTP`), `--help`, `--version` |

The service stops cleanly on `SIGTERM` or Ctrl-C: it stops accepting connections and gives running requests up to 10 seconds. A worker finishes the jobs it is running first; a second signal stops it at once.

## Several machines

When one server is not enough, spread the work over several. One machine runs the **coordinator**: it takes jobs from the API and from watched folders, keeps them in a queue on disk, and hands them out. Every other machine runs a **worker** that pulls jobs, converts them and sends the results back. Programs keep using the same [HTTP API](/http-api) against the coordinator; nothing changes for them.

The coordinator speaks HTTPS itself. Give it a certificate and key (from your own certificate authority, or a public one for its host name); workers check that certificate before they send anything:

```bash
# on the coordinator machine
docker run -d --name kura-coordinator -p 8443:8080 \
  -e KURA_CLUSTER_TOKEN=change-me -v kura-data:/data -v /etc/kura/tls:/tls:ro \
  -e KURA_TLS_CERT=/tls/cert.pem -e KURA_TLS_KEY=/tls/key.pem \
  ghcr.io/alam00000/bentopdf-kura:latest coordinator

# on each worker machine (4 jobs at a time on this one)
docker run -d --name kura-worker -e KURA_CLUSTER_TOKEN=change-me \
  -v /etc/kura/tls/ca.pem:/tls/ca.pem:ro -e KURA_CA_FILE=/tls/ca.pem \
  ghcr.io/alam00000/bentopdf-kura:latest worker --coordinator https://coordinator-host:8443 --slots 4
```

`KURA_CA_FILE` is only needed when the certificate comes from your own authority; certificates from public authorities are trusted as they are. To try it with a self-made authority:

```bash
openssl req -x509 -newkey rsa:2048 -nodes -keyout ca.key -out ca.pem -days 825 -subj '/CN=kura ca'
openssl req -newkey rsa:2048 -nodes -keyout key.pem -out cert.csr -subj '/CN=coordinator-host'
printf 'subjectAltName=DNS:coordinator-host\nbasicConstraints=CA:FALSE\n' > ext.cnf
openssl x509 -req -in cert.csr -CA ca.pem -CAkey ca.key -CAcreateserial -out cert.pem -days 825 -extfile ext.cnf
```

Plain HTTP is refused by default: a coordinator without a certificate only starts when it listens on the machine itself (`KURA_HOST=127.0.0.1`, for a reverse proxy that does TLS), and workers and front ends only accept `http://` addresses on the same machine. On a private network you trust, `KURA_INSECURE_HTTP=1` (or `--insecure-http`) on every machine allows plain HTTP; the shared token then travels unencrypted.

Open `https://coordinator-host:8443/status` for the live picture: workers and whether they are alive, queue length, running, finished and failed counts, and the most recent jobs. The same numbers are at `/status.json` for monitoring.

What happens when something breaks:

- **A worker dies or loses its network.** Workers check in every few seconds. When one goes quiet for `KURA_LEASE_MS`, its jobs go back into the queue and another worker picks them up, up to `KURA_MAX_ATTEMPTS` tries per job. When a worker starts again, it removes the job folders its crashed predecessor left behind.
- **The coordinator restarts.** The queue lives in `KURA_DATA` (a small SQLite database plus the files), so waiting and running jobs survive. Workers reconnect on their own and deliver results they finished in the meantime.
- **Identical work.** Add `dedupe=true` to a request (or to a watched folder) and a job with the same file and options as one finished within `KURA_RESULT_TTL_S` returns that result instead of converting again.

### What a job may contain

Jobs carry only the options of the [HTTP API](/http-api), checked against a fixed list on the coordinator and again on every worker: a worker does not trust the coordinator either. Levels, numbers and names are checked for their allowed values. Files a client sends (a preflight profile, an ICC profile, an e-invoice XML) travel inside the job and are written into that job's own folder on the worker. Bundled profiles are named by id and looked up in the worker's own profile folder. Nothing in a job can point the engine at a path on the worker, a font folder, a batch or watch run, or a report file. A job outside the list is refused with `400 BAD_REQUEST`.

### Watched folders

Give the coordinator one or more folders to watch, each with the options its files get, written like the API's query string:

```bash
docker run -d --name kura-coordinator -p 8080:8080 -e KURA_CLUSTER_TOKEN=change-me \
  -v kura-data:/data -v /srv/scans:/srv/scans \
  ghcr.io/alam00000/bentopdf-kura:latest coordinator \
  --hot-folder '/srv/scans?level=2b&ua=true' --hot-folder '/srv/print?level=x4&profile=press/book-text-pages-check-and-fix'
```

A folder's options take the same names and values as the [HTTP API](/http-api#query-parameters), plus `mode=convert` (the default) or `mode=check`, and `dedupe=true`. They are checked when the coordinator starts: a folder with a bad option stops it with a message naming the folder. The folders must be writable by the container's user (uid 10001). Each gets `input/`, `done/` and `failed/`. Drop files into `input/`; once a file has stopped growing it is queued. A converted file lands in `done/` under the same name with `.pdf`, with its report next to it as `.json`. A file the engine rejects moves to `failed/` with a `.json` that says why. Add `mode=check` to only check: the report goes to `done/` with the untouched original.

### Coordinator and worker settings

| Env var | Default | Meaning |
|---|---|---|
| `KURA_CLUSTER_TOKEN` | (required) | shared secret between the coordinator, its workers and `serve --coordinator` front ends |
| `KURA_DATA` | `/data` in the image, `kura-data` otherwise | where the coordinator keeps its queue and job files |
| `KURA_HOT_FOLDERS` | (none) | watched folders, separated by `;`, same form as `--hot-folder` |
| `KURA_MAX_ATTEMPTS` | 3 | tries per job before it is marked failed |
| `KURA_LEASE_MS` | 30000 | how long a silent worker keeps its jobs before they are handed to another |
| `KURA_RESULT_TTL_S` | 3600 | how long finished results are kept for `dedupe=true` and late pickups |
| `KURA_COORDINATOR` | (none) | the coordinator's address, for `worker` and `serve` |
| `KURA_SLOTS` | `KURA_CONCURRENCY` | jobs a worker runs at once |
| `KURA_WORKER_NAME` | worker | name shown on the status page |
| `KURA_TLS_CERT`, `KURA_TLS_KEY` | (none) | PEM certificate chain and key; the coordinator (or `serve`) then speaks HTTPS |
| `KURA_CA_FILE` | (none) | extra certificate authority, in PEM, that workers and front ends trust for the coordinator |
| `KURA_INSECURE_HTTP` | (off) | `1` allows plain HTTP between machines |
| `KURA_WORK_DIR` | system temp `/kura-worker` | where a worker keeps job folders; leftovers from a crashed worker are removed at start |

On the coordinator, `KURA_CONCURRENCY` and `KURA_QUEUE` default to 64 and 1000: the API answers 429 when 1000 API jobs are already waiting. Watched folders are never refused. `KURA_TIMEOUT_MS` on the coordinator sets the time limit every worker applies.

Workers need no inbound port; they only connect out to the coordinator. Tokens are compared in constant time and never written to the logs. The status page and `/status.json` follow `KURA_API_TOKEN`; open `/status#token=<token>` in a browser.

To keep the web interface on its own machine, run `serve --coordinator https://coordinator-host:8443` there with the same `KURA_CLUSTER_TOKEN`: it serves the pages and the API and passes every job to the coordinator, which survives coordinator restarts without failing the request.

## Building the image yourself

```bash
docker build -f docker/Dockerfile -t kura .
```

The first build compiles the engine and its rendering library from source, which takes about forty minutes on four cores; later builds reuse the cached layers.

## Troubleshooting

**`port is already allocated`.** Something else owns 8080. Map another host port: `-p 9090:8080`, then use `localhost:9090`.

**A worker shows `alive: no` on the status page.** It has not checked in for `KURA_LEASE_MS`. Check that it can reach the coordinator's address and that both use the same `KURA_CLUSTER_TOKEN`; its log says which.

**Requests return 429.** The queue is full. Raise `KURA_QUEUE` for burst tolerance or `KURA_CONCURRENCY` for throughput, and check `/healthz` to see the live numbers.

**Requests return 504.** A job hit the time limit, usually a very large scan being rasterized. Raise `KURA_TIMEOUT_MS`.

**Requests return 413.** The upload is larger than `KURA_MAX_UPLOAD_MB`; the reverse proxy may have its own, smaller limit as well.

**CLI mode says it cannot write the output.** The mounted folder is not writable by the container's user; add `--user "$(id -u):$(id -g)"` as shown above.

**The container runs but the page will not load.** Confirm the port mapping (`docker ps` shows `0.0.0.0:8080->8080`) and that you are browsing the host port from the left side of that arrow.
