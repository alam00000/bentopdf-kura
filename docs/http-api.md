# HTTP API

The [self-hosted image](/self-hosting) serves the web interface at `/` and this API. Everything runs on your server; nothing leaves it. If you have not started the container yet, that is one command:

```bash
docker run -d -p 8080:8080 ghcr.io/alam00000/bentopdf-kura:latest
```

## Your first request

Send the PDF as the raw request body; the converted PDF comes back as the response body.

```bash
curl -o out.pdf --data-binary @in.pdf 'http://localhost:8080/api/convert?level=2b'
```

See what happened without opening the file:

```bash
curl -s -o out.pdf -D - --data-binary @in.pdf \
  'http://localhost:8080/api/convert?level=2b' | grep -i '^x-kura'
```

```
x-kura-level: 2b
x-kura-engine: BentoPDF Kura Engine 1.1.0
x-kura-changes: 3
```

`--data-binary` matters: plain `-d` would mangle the bytes. A JPEG works as input too; it becomes a one-page document first.

## POST /api/convert

Converts the document to the target and returns the result.

### Query parameters

The names match the [npm package](/npm) options; every one maps to a [CLI](/cli) flag.

| Parameter | Meaning |
|---|---|
| `level` | required: `1b`, `1a`, `2b`, `2u`, `2a`, `3b`, `3u`, `3a`, `4`, `4f`, `4e`, `x1a`, `x3`, `x4`, `x6`, `e1`, `vt1`, `vt3` |
| `ua=true` | add PDF/UA on top of the level |
| `lang` | document language tag, for example `en-US` |
| `allowVisualRisk=true` | accept repairs that may change appearance instead of rejecting |
| `rasterizePages=true` | rasterize every page |
| `rasterDpi` | resolution for rasterized pages, 24 to 1200 |
| `outlineFonts=true` | convert text to outlines |
| `imageMaxPpi` | downsample images above this resolution |
| `outputCondition`, `outputConditionInfo`, `registry` | the PDF/X output intent |
| `vtRecords` | PDF/VT record boundaries, for example `1-3,4-6` |
| `embedSource=true`, `embedSourceName` | attach the original file to the result |
| `attachXmlName`, `facturxProfile` | e-invoice attachment name and profile |
| `profile` | a bundled preflight profile to apply, by name from `/api/profiles` |
| `report=json` | return the JSON report with the PDF inside instead of raw bytes |

### Body

Either the document itself as the raw body, or `multipart/form-data` with these fields:

| Field | Meaning |
|---|---|
| `file` | the document (required) |
| `profile` | a [preflight profile](/preflight) in JSON or XML, when it is not one of the bundled ones |
| `xml` | an e-invoice XML to attach |
| `destProfile` | an ICC profile for the output intent |
| `defaultRgb`, `defaultCmyk`, `defaultGray` | ICC profiles for uncalibrated colour |

Any query parameter may also be sent as a text field.

### Headers

| Header | Direction | Meaning |
|---|---|---|
| `X-Password` | request | password for encrypted input |
| `Authorization: Bearer <token>` | request | required when the server has `KURA_API_TOKEN` set (`X-Api-Token` works too) |
| `X-Kura-Level` | response | the level the file was converted to |
| `X-Kura-Engine` | response | engine name and version |
| `X-Kura-Changes` | response | how many changes were made |

### With the report

`report=json` returns the same report the CLI prints, with the PDF base64-encoded in `pdf`:

```bash
curl -s -F file=@in.pdf 'http://localhost:8080/api/convert?level=2b&ua=true&report=json' \
  | jq '{level, changes: (.issues | length), first: .issues[0]}'
```

```json
{"level":"2b","changes":3,"first":{"code":"OUTPUT_INTENT_ADDED","detail":"sRGB IEC61966-2.1","fixed":true}}
```

### Examples

An e-invoice, attaching the XML and producing PDF/A-3:

```bash
curl -o invoice-a3.pdf -F file=@invoice.pdf -F xml=@invoice.xml \
  'http://localhost:8080/api/convert?level=3b'
```

Apply a bundled preflight profile's fixes:

```bash
curl -o fixed.pdf --data-binary @in.pdf \
  'http://localhost:8080/api/convert?level=x4&profile=press/book-text-pages-check-and-fix'
```

Apply your own profile:

```bash
curl -o fixed.pdf -F file=@in.pdf -F profile=@my-rules.json \
  'http://localhost:8080/api/convert?level=2b'
```

## POST /api/check

Checks without changing anything and returns the report. The level may also be a check-only flavour (`x4p`, `x5g`, `x5n`, `x5pg`, `x6n`, `x6p`, `vt2`).

```bash
curl -s --data-binary @in.pdf 'http://localhost:8080/api/check?level=2b'
```

```json
{"ok":true,"level":"2b","engine":"BentoPDF Kura Engine 1.1.0","mode":"check","compliant":false,"findings":2,"issues":[...]}
```

Add `analyze=true` for the document analysis lines, or `profile=<name>` (or a `profile` field) to run a preflight profile; the findings arrive in `analysis`:

```bash
curl -s -F file=@in.pdf 'http://localhost:8080/api/check?level=x4&profile=press/book-text-pages-check' \
  | jq '.analysis[] | select(.code=="PROFILE_HIT") | .detail'
```

The HTTP status is 200 whether or not the file conforms; read `compliant` and `findings`.

## GET /api/profiles

Lists the bundled preflight profiles as `[{id, name, description}]`. The `id` (for example `press/book-text-pages-check`) is what `profile=` takes.

## POST /api/verify-password

Send the encrypted PDF as the body with `X-Password`; returns `{"ok":true,"valid":true}` or `{"ok":true,"valid":false}` without converting. `true` means `/api/convert` will accept the same password.

```bash
curl -s --data-binary @locked.pdf -H 'X-Password: hunter2' http://localhost:8080/api/verify-password
```

## GET /healthz

```json
{"ok":true,"version":"BentoPDF Kura Engine (kura) 1.1.0","active":0,"queued":0}
```

`active` is jobs currently running, `queued` is jobs waiting for a slot. Wire it into your load balancer or uptime monitoring as is.

## Errors

Every error is a JSON body with `ok: false`, an `errorCode` and a plain-language `error`:

| Status | Code | Meaning | What to do |
|---|---|---|---|
| 400 | `BAD_LEVEL` | unknown level | the response lists valid ones |
| 400 | `BAD_REQUEST` | an option is malformed, or no document was sent | fix the request |
| 400 | `BAD_PROFILE` | no bundled profile with that name | see `/api/profiles` |
| 400 | `NOT_A_PDF` | the body is neither a PDF nor a JPEG | send the file with `--data-binary` |
| 401 | `UNAUTHORIZED` | token required | send `Authorization: Bearer <token>` |
| 413 | `TOO_LARGE` | body exceeds the upload limit | raise `KURA_MAX_UPLOAD_MB` |
| 422 | the engine's code | the document was rejected; `error` says why and `suggestedLevel` names a level that would accept it | see [Rejection codes](/rejections) |
| 429 | `BUSY` | queue full | retry with backoff, or raise `KURA_QUEUE` |
| 504 | `TIMEOUT` | the job exceeded the time limit | raise `KURA_TIMEOUT_MS` |

`PASSWORD_REQUIRED` is a 422 like any other rejection: send `X-Password`.

## Limits and tuning

Concurrency, queue depth, upload size and the time limit are environment variables on the container. Defaults: 2 parallel jobs, a queue of 8, 500 MB uploads, 10 minutes per job. See [Self-hosting](/self-hosting#configuration).
