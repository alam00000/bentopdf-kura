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

Each job is a separate engine process with its own memory, so size `KURA_CONCURRENCY` to your cores and memory: 2 is right for a small VPS, 4 to 8 for a real server. A 500-page scanned document at PDF/A-1 can need a few gigabytes while its pages are rasterized.

## Put TLS in front

The container speaks plain HTTP and, unless you set a token, does no authentication, which is the standard contract for a service container: terminate TLS at a reverse proxy. With [Caddy](https://caddyserver.com) the entire configuration is:

```
kura.example.com {
    reverse_proxy localhost:8080
}
```

Caddy obtains and renews the certificate itself. Any reverse proxy works (nginx, Traefik); raise its request body limit to match `KURA_MAX_UPLOAD_MB`, and if the instance is public, add rate limiting there too.

## Updating

```bash
docker pull ghcr.io/alam00000/bentopdf-kura:latest
docker stop kura && docker rm kura
docker run -d --name kura -p 8080:8080 ghcr.io/alam00000/bentopdf-kura:latest
```

The container is stateless; nothing needs migrating.

## Monitoring

`GET /healthz` returns `{ok, version, active, queued}`. Wire it into uptime checks as is; sustained `queued` near `KURA_QUEUE` means you should raise concurrency or add hardware.

## CLI mode

The same image doubles as the CLI: pass arguments and it converts instead of serving. Run it as your own user so the output can be written into the mounted folder:

```bash
docker run --rm --user "$(id -u):$(id -g)" -v "$PWD":/work \
  ghcr.io/alam00000/bentopdf-kura:latest --level 2b in.pdf out.pdf
```

Every [CLI option](/cli) works, including batch mode over a mounted folder.

## Without Docker

The service is a single Node.js file that runs the native `kura` binary, so it also runs directly on a machine with Node 22 and a native build or a release binary:

```bash
npm ci
KURA_BIN=/usr/local/bin/kura npm run serve
```

`KURA_PROFILES` points it at a folder of bundled profiles (the repository's `pdfa-engine/profiles` by default).

## Building the image yourself

```bash
docker build -f docker/Dockerfile -t kura .
```

The first build compiles the engine and its rendering library from source, which takes about forty minutes on four cores; later builds reuse the cached layers.

## Troubleshooting

**`port is already allocated`.** Something else owns 8080. Map another host port: `-p 9090:8080`, then use `localhost:9090`.

**Requests return 429.** The queue is full. Raise `KURA_QUEUE` for burst tolerance or `KURA_CONCURRENCY` for throughput, and check `/healthz` to see the live numbers.

**Requests return 504.** A job hit the time limit, usually a very large scan being rasterized. Raise `KURA_TIMEOUT_MS`.

**Requests return 413.** The upload is larger than `KURA_MAX_UPLOAD_MB`; the reverse proxy may have its own, smaller limit as well.

**CLI mode says it cannot write the output.** The mounted folder is not writable by the container's user; add `--user "$(id -u):$(id -g)"` as shown above.

**The container runs but the page will not load.** Confirm the port mapping (`docker ps` shows `0.0.0.0:8080->8080`) and that you are browsing the host port from the left side of that arrow.
