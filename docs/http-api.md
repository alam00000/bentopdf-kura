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

The names match the [npm package](/npm) options; every one maps to a [CLI](/cli) flag. A switch is on when its value is `true`, `1` or `on`; anything else, or leaving it out, means off. Text values may be up to 512 characters and may not contain line breaks or other control characters. Each option may be given once.

| Parameter | Meaning |
|---|---|
| `level` | required: `1b`, `1a`, `2b`, `2u`, `2a`, `3b`, `3u`, `3a`, `4`, `4f`, `4e`, `x1a`, `x3`, `x4`, `x6`, `e1`, `vt1`, `vt3`, or `none` to apply a preset or colour conversion without a standard. The check-only flavours are refused here with `BAD_LEVEL`; use them with `/api/check` |
| `pdfVersion` | with `none`: the PDF version to write, `1.3` to `2.0` |
| `ua=true` | add PDF/UA on top of the level |
| `lang` | document language tag, for example `en-US` |
| `allowVisualRisk=true` | accept repairs that may change appearance instead of rejecting |
| `rasterizePages=true` | rasterize every page |
| `rasterDpi` | resolution for rasterized pages, 24 to 1200; a plain number such as `150` |
| `outlineFonts=true` | convert text to outlines |
| `imageMaxPpi` | downsample images above this resolution; a plain number such as `300` |
| `outputCondition`, `outputConditionInfo`, `registry` | the PDF/X output intent |
| `convertColors` | convert colour to `cmyk`, `gray` or `rgb` through ICC profiles; `cmyk` uses the uploaded `destProfile` when there is one. Profile names and file paths are not accepted over HTTP: upload the ICC file as `destProfile` instead. See [converting colour](/cli#converting-colour) |
| `intent` | rendering intent: `perceptual`, `relative`, `saturation` or `absolute` |
| `convertObjects` | limit conversion to a comma list of `text`, `vector`, `shading`, `images`, `lossy`, `lossless` |
| `keepSpots=true` | keep spot colours and convert only their alternate |
| `noBpc=true` | turn off black point compensation |
| `grayToK=true`, `rgbGrayToK` | put gray, and RGB within this many percent of neutral, on the black plate only |
| `inkLimit` | total ink coverage limit, 100 to 400 percent |
| `vtRecords` | PDF/VT record boundaries, for example `1-3,4-6` |
| `embedSource=true`, `embedSourceName` | attach the original file to the result |
| `attachXmlName`, `facturxProfile` | e-invoice attachment name and profile; `facturxProfile` is up to 32 letters, digits, spaces, `_` or `-`, for example `EN 16931` |
| `profile` | a bundled preflight profile to apply, by its `id` from `/api/profiles`, for example `press/book-text-pages-check-and-fix` |
| `report=json` | return the JSON report with the PDF inside instead of raw bytes |
| `password` | the password of an encrypted input, as a form field when you cannot send the `X-Password` header; the header wins when both are sent. It is not read from the URL, because URLs end up in logs |
| `dedupe=true` | through a [coordinator](/self-hosting#several-machines): reuse the result of an identical recent job instead of converting again |

### Body

Either the document itself as the raw body, or `multipart/form-data` with these fields:

| Field | Meaning |
|---|---|
| `file` | the document (required) |
| `profile` | a [preflight profile](/preflight) in JSON, when it is not one of the bundled ones |
| `xml` | an e-invoice XML to attach |
| `destProfile` | an ICC profile for the output intent |
| `defaultRgb`, `defaultCmyk`, `defaultGray` | ICC profiles for uncalibrated colour |
| `deviceLink` | a device link profile to convert through |

Any query parameter may also be sent as a text field, `level` included; when both are sent, the query string wins. A file field may be sent as a file part or as a text field holding the content. A `profile` sent as a text field is read as profile JSON, not as a bundled id: name a bundled profile with the `profile` query parameter, which takes precedence over a `profile` field.

### What the API does not take

The API carries the options above and nothing else, so a request can never point the engine at a path on the server. In particular:

- profile variables (`--var`, `--var-file`) cannot be set: a profile with variables runs with each variable's `default`;
- colour profiles cannot be named by description or file name; upload the ICC file in a field;
- `ua1` and `ua2` are not levels here; check PDF/UA with `level=2u&ua=true` or `level=4&ua=true`;
- signing, OCR, font folders, batch and watch mode, and report files are CLI-only.

### Headers

| Header | Direction | Meaning |
|---|---|---|
| `X-Password` | request | password for encrypted input |
| `Authorization: Bearer <token>` | request | required when the server has `KURA_API_TOKEN` set (`X-Api-Token` works too) |
| `X-Kura-Level` | response | the level the file was converted to |
| `X-Kura-Engine` | response | engine name and version |
| `X-Kura-Changes` | response | how many entries the report's `issues` list has |
| `Content-Type` | response | `application/pdf` for a converted file, `application/json` for every report and error |

### With the report

`report=json` returns the same report the CLI prints, minus the `file` field, with the PDF base64-encoded in `pdf`:

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
  | jq '.analysis[] | select(.code=="CHECK_HIT") | .detail'
```

The HTTP status is 200 whether or not the file conforms; read `compliant` and `findings`. The body is the CLI's [check report](/check-mode) without the `file` field. Form fields work as for `/api/convert`, so `-F file=@in.pdf -F profile=@my-rules.json` checks against your own profile.

## GET /api/profiles

Lists the bundled preflight profiles as `[{id, name, description}]`, sorted by `id`. The `id` is the folder and file name without `.json` (for example `press/book-text-pages-check`), and is what `profile=` takes. The list is read once when the server starts. It needs no token.

```bash
curl -s http://localhost:8080/api/profiles | jq -r '.[].id' | grep '^press/'
```

## POST /api/verify-password

Send the encrypted PDF as the raw body (not a form) with `X-Password`; returns `{"ok":true,"valid":true}` or `{"ok":true,"valid":false}` without converting. `true` means `/api/convert` will accept the same password. A file that is not encrypted is `valid: true` with any password.

```bash
curl -s --data-binary @locked.pdf -H 'X-Password: hunter2' http://localhost:8080/api/verify-password
```

## GET /healthz

```json
{"ok":true,"version":"BentoPDF Kura Engine (kura) 1.1.0","active":0,"queued":0}
```

`active` is jobs currently running, `queued` is jobs waiting for a slot. Wire it into your load balancer or uptime monitoring as is. It needs no token.

## Other routes

| Route | What it returns |
|---|---|
| `GET /` | the converter page |
| `GET /preflight` | the preflight page |
| `GET /docs/...` | a redirect to the same page of this documentation |
| anything else | `404` with `errorCode` `NOT_FOUND` |

Every `GET` route also answers `HEAD`.

## Authentication

With `KURA_API_TOKEN` set, every `POST /api/...` request needs `Authorization: Bearer <token>` or `X-Api-Token: <token>`; without it the answer is `401 UNAUTHORIZED`. The pages, `GET /api/profiles` and `/healthz` stay open. On a coordinator, `/status.json` needs the token too.

```bash
curl -o out.pdf -H 'Authorization: Bearer my-token' --data-binary @in.pdf \
  'http://localhost:8080/api/convert?level=2b'
```

## Errors

Every error is a JSON body with `ok: false`, an `errorCode` and a plain-language `error`:

| Status | Code | Meaning | What to do |
|---|---|---|---|
| 400 | `BAD_LEVEL` | unknown level, or a check-only level sent to `/api/convert` | the response lists valid ones in `levels` |
| 400 | `BAD_REQUEST` | an option is malformed (`error` names it, for example `invalid value for rasterDpi`), the engine refused the options, or no document was sent | fix the request |
| 400 | `BAD_PROFILE` | no bundled profile with that id | see `/api/profiles`; the response's `profiles` field points there |
| 400 | `NOT_A_PDF` | the body is neither a PDF nor a JPEG | send the file with `--data-binary` |
| 401 | `UNAUTHORIZED` | token required | send `Authorization: Bearer <token>` |
| 404 | `NOT_FOUND` | no such route | check the method and path; the API routes are `POST` |
| 413 | `TOO_LARGE` | body exceeds the upload limit; `maxBytes` gives the limit | raise `KURA_MAX_UPLOAD_MB` |
| 422 | the engine's code | the document was rejected; `error` says why and `suggestedLevel` names a level that would accept it. The body is the engine's full report, `issues` included | see [Rejection codes](/rejections) |
| 429 | `BUSY` | queue full | retry with backoff, or raise `KURA_QUEUE` |
| 500 | `INTERNAL` | the server could not run the job or the engine produced no report; the server log has the detail | check the log; please report it with the file |
| 504 | `TIMEOUT` | the job exceeded the time limit | raise `KURA_TIMEOUT_MS` |

`PASSWORD_REQUIRED` is a 422 like any other rejection: send `X-Password`.

## Limits and tuning

Concurrency, queue depth, upload size and the time limit are environment variables on the container. Defaults: 2 parallel jobs, a queue of 8, 500 MB uploads, 10 minutes per job. See [Self-hosting](/self-hosting#configuration).

## Several machines

A [coordinator](/self-hosting#several-machines) answers this same API, with the work spread over its workers. Everything above applies unchanged; `/healthz` then counts the jobs running and waiting across all workers, and the coordinator adds:

| Route | Meaning |
|---|---|
| `GET /status` | a status page: workers, queue, recent jobs |
| `GET /status.json` | the same as JSON: `queued`, `running`, `done`, `failed`, `workers[]` (each `id`, `name`, `slots`, `running`, `version`, `lastSeenMs`, `alive`), `jobs[]` (the latest 200, each `id`, `name`, `kind`, `state`, `attempts`, `worker`, `origin` `api` or `hot-folder`, `created`, `updated`, `exitCode`) and `folders` (needs the API token when one is set) |
