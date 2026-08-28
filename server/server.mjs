import * as http from 'node:http';
import * as fs from 'node:fs';
import * as os from 'node:os';
import * as path from 'node:path';
import * as crypto from 'node:crypto';
import { spawn, execFileSync } from 'node:child_process';
import { fileURLToPath } from 'node:url';

const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const SITE = path.join(ROOT, 'site');
const DOCS_URL = 'https://kura.bentopdf.com/docs/';

function intEnv(name, dflt, min, max) {
  const raw = process.env[name];
  if (raw === undefined || raw.trim() === '') return dflt;
  const n = Number(raw);
  if (!Number.isFinite(n)) return dflt;
  return Math.min(max, Math.max(min, Math.floor(n)));
}

const PORT = intEnv('KURA_PORT', 8080, 1, 65535);
const HOST = process.env.KURA_HOST ?? '0.0.0.0';
const MAX_UPLOAD = intEnv('KURA_MAX_UPLOAD_MB', 500, 1, 1024 * 1024) * 1024 * 1024;
const CONCURRENCY = intEnv('KURA_CONCURRENCY', 2, 1, 1024);
const QUEUE_LIMIT = intEnv('KURA_QUEUE', 8, 0, 100000);
const TIMEOUT_MS = intEnv('KURA_TIMEOUT_MS', 600000, 1000, 24 * 3600000);
const API_TOKEN = process.env.KURA_API_TOKEN ?? '';
const KURA_BIN = process.env.KURA_BIN ?? '/usr/local/bin/kura';
const PROFILES = process.env.KURA_PROFILES ?? path.join(ROOT, 'pdfa-engine', 'profiles');

const LEVELS = ['1b', '1a', '2b', '2u', '2a', '3b', '3u', '3a', '4', '4f', '4e', 'x1a', 'x3', 'x4', 'x6', 'e1', 'vt1', 'vt3'];
const CHECK_ONLY = ['x4p', 'x5g', 'x5n', 'x5pg', 'x6n', 'x6p', 'vt2'];

const BOOL_FLAGS = {
  ua: '--ua',
  allowVisualRisk: '--allow-visual-risk',
  rasterizePages: '--rasterize-pages',
  outlineFonts: '--outline-fonts',
  analyze: '--analyze',
  embedSource: '--embed-source',
};
const TEXT_FLAGS = {
  lang: '--lang',
  outputCondition: '--output-condition',
  outputConditionInfo: '--output-condition-info',
  registry: '--registry',
  vtRecords: '--vt-records',
  attachXmlName: '--attach-xml-name',
  facturxProfile: '--facturx-profile',
  embedSourceName: '--embed-source-name',
  rasterDpi: '--raster-dpi',
  imageMaxPpi: '--image-max-ppi',
};
const FILE_FIELDS = {
  xml: ['--attach-xml', 'attachment.xml'],
  destProfile: ['--dest-profile', 'dest.icc'],
  defaultRgb: ['--default-rgb', 'rgb.icc'],
  defaultCmyk: ['--default-cmyk', 'cmyk.icc'],
  defaultGray: ['--default-gray', 'gray.icc'],
};
const STATIC = {
  '/index.html': ['index.html', 'text/html; charset=utf-8'],
  '/preflight.html': ['preflight.html', 'text/html; charset=utf-8'],
  '/app.js': ['app.js', 'text/javascript; charset=utf-8'],
  '/preflight.js': ['preflight.js', 'text/javascript; charset=utf-8'],
  '/ui.js': ['ui.js', 'text/javascript; charset=utf-8'],
  '/pdf.js': ['pdf.js', 'text/javascript; charset=utf-8'],
  '/profiles.js': ['profiles.js', 'text/javascript; charset=utf-8'],
  '/engine.js': ['engine-http.js', 'text/javascript; charset=utf-8'],
  '/icons/phosphor.js': ['icons/phosphor.js', 'text/javascript; charset=utf-8'],
  '/app.css': ['app.css', 'text/css; charset=utf-8'],
  '/logo.svg': ['logo.svg', 'image/svg+xml'],
  '/fonts/dm-sans-latin.woff2': ['fonts/dm-sans-latin.woff2', 'font/woff2'],
  '/fonts/dm-sans-latin-ext.woff2': ['fonts/dm-sans-latin-ext.woff2', 'font/woff2'],
};

let VERSION = '';
try {
  VERSION = execFileSync(KURA_BIN, ['--version'], { encoding: 'utf8' }).trim();
} catch (e) {
  console.error(`cannot run the engine at ${KURA_BIN}: ${e.message}`);
  process.exit(1);
}

function loadProfiles() {
  const out = [];
  let folders = [];
  try { folders = fs.readdirSync(PROFILES, { withFileTypes: true }); } catch { return out; }
  for (const folder of folders) {
    if (!folder.isDirectory()) continue;
    for (const file of fs.readdirSync(path.join(PROFILES, folder.name))) {
      if (!file.endsWith('.json')) continue;
      try {
        const doc = JSON.parse(fs.readFileSync(path.join(PROFILES, folder.name, file), 'utf8'));
        out.push({ id: `${folder.name}/${file.slice(0, -5)}`, name: doc.name ?? '', description: doc.description ?? '' });
      } catch {}
    }
  }
  return out.sort((a, b) => a.id.localeCompare(b.id));
}
const PROFILE_LIST = loadProfiles();

function authorized(req) {
  if (!API_TOKEN) return true;
  const header = req.headers['authorization'];
  const xtoken = req.headers['x-api-token'];
  let provided = '';
  if (typeof header === 'string' && header.startsWith('Bearer ')) provided = header.slice(7);
  else if (typeof xtoken === 'string') provided = xtoken;
  const a = Buffer.from(provided);
  const b = Buffer.from(API_TOKEN);
  return a.length === b.length && crypto.timingSafeEqual(a, b);
}

let active = 0;
const waiting = [];

function acquire() {
  if (active < CONCURRENCY) {
    active++;
    return Promise.resolve(true);
  }
  if (waiting.length >= QUEUE_LIMIT) return Promise.resolve(false);
  return new Promise((resolve) => {
    waiting.push(() => { active++; resolve(true); });
  });
}

function release() {
  active--;
  const next = waiting.shift();
  if (next) next();
}

function sendJson(res, status, body) {
  const buf = Buffer.from(JSON.stringify(body));
  res.writeHead(status, { 'content-type': 'application/json', 'content-length': buf.length, 'x-content-type-options': 'nosniff' });
  res.end(buf);
}

function fail(res, status, code, message, extra = {}) {
  sendJson(res, status, { ok: false, errorCode: code, error: message, ...extra });
}

function readBody(req, res, limit) {
  const declared = Number(req.headers['content-length']);
  if (Number.isFinite(declared) && declared > limit) return Promise.resolve(null);
  return new Promise((resolve, reject) => {
    const chunks = [];
    let size = 0;
    let over = false;
    req.on('data', (c) => {
      if (over) return;
      size += c.length;
      if (size > limit) {
        over = true;
        chunks.length = 0;
        res.once('finish', () => req.destroy());
        resolve(null);
        return;
      }
      chunks.push(c);
    });
    req.on('end', () => { if (!over) resolve(Buffer.concat(chunks)); });
    req.on('error', (e) => { if (!over) reject(e); });
  });
}

async function readRequest(req, res) {
  const body = await readBody(req, res, MAX_UPLOAD);
  if (body === null) return { tooLarge: true };
  const type = req.headers['content-type'] ?? '';
  if (!type.startsWith('multipart/form-data')) return { file: body, fields: {} };
  const form = await new Response(body, { headers: { 'content-type': type } }).formData();
  const fields = {};
  let file = null;
  for (const [name, value] of form) {
    if (name === 'file') file = Buffer.from(await value.arrayBuffer());
    else if (typeof value === 'string') fields[name] = value;
    else fields[name] = Buffer.from(await value.arrayBuffer());
  }
  return { file, fields };
}

function looksLikeInput(bytes) {
  if (bytes.length >= 3 && bytes[0] === 0xff && bytes[1] === 0xd8 && bytes[2] === 0xff) return 'jpg';
  if (bytes.subarray(0, 1024).indexOf('%PDF') >= 0) return 'pdf';
  return null;
}

function textValue(v) {
  if (typeof v !== 'string') return null;
  if (v.length === 0 || v.length > 512) return null;
  for (const ch of v) if (ch.charCodeAt(0) < 0x20 || ch.charCodeAt(0) === 0x7f) return null;
  return v;
}

function isTrue(v) {
  return v === 'true' || v === '1' || v === 'on';
}

function libraryProfile(id) {
  if (!/^[a-z0-9-]+\/[a-z0-9-]+$/.test(id)) return null;
  const p = path.join(PROFILES, `${id}.json`);
  return fs.existsSync(p) ? p : null;
}

function buildArgs(url, req, fields, dir, mode) {
  const level = url.searchParams.get('level') ?? fields.level;
  const allowed = mode === 'check' ? [...LEVELS, ...CHECK_ONLY] : LEVELS;
  if (typeof level !== 'string' || !allowed.includes(level)) {
    return { error: [400, 'BAD_LEVEL', `level must be one of ${allowed.join(', ')}`, { levels: allowed }] };
  }
  const args = mode === 'check' ? ['--check', '--level', level] : ['--level', level];
  const get = (k) => url.searchParams.get(k) ?? (typeof fields[k] === 'string' ? fields[k] : null);
  for (const [k, flag] of Object.entries(BOOL_FLAGS)) {
    if (isTrue(get(k))) args.push(flag);
  }
  for (const [k, flag] of Object.entries(TEXT_FLAGS)) {
    const raw = get(k);
    if (raw === null || raw === '') continue;
    const v = textValue(raw);
    if (v === null) return { error: [400, 'BAD_REQUEST', `invalid value for ${k}`] };
    args.push(flag, v);
  }
  const password = req.headers['x-password'] ?? fields.password;
  if (typeof password === 'string' && password.length > 0) args.push('--password', password);
  const profileId = url.searchParams.get('profile');
  if (profileId) {
    const p = libraryProfile(profileId);
    if (!p) return { error: [400, 'BAD_PROFILE', 'no bundled profile with that name', { profiles: '/api/profiles' }] };
    args.push('--profile', p);
  } else if (fields.profile !== undefined) {
    const p = path.join(dir, 'profile.json');
    fs.writeFileSync(p, fields.profile);
    args.push('--profile', p);
  }
  for (const [k, [flag, name]] of Object.entries(FILE_FIELDS)) {
    if (fields[k] === undefined) continue;
    const p = path.join(dir, name);
    fs.writeFileSync(p, fields[k]);
    args.push(flag, p);
  }
  return { args };
}

function runKura(args, cwd) {
  return new Promise((resolve) => {
    const child = spawn(KURA_BIN, args, {
      cwd,
      env: { ...process.env, PDFA_TIMEOUT: String(Math.ceil(TIMEOUT_MS / 1000)) },
      stdio: ['ignore', 'pipe', 'pipe'],
    });
    let out = '';
    let err = '';
    let timedOut = false;
    const timer = setTimeout(() => { timedOut = true; child.kill('SIGKILL'); }, TIMEOUT_MS + 5000);
    child.stdout.on('data', (d) => { out += d; });
    child.stderr.on('data', (d) => { if (err.length < 65536) err += d; });
    child.on('error', (e) => { clearTimeout(timer); resolve({ code: -1, out, err: String(e.message), timedOut }); });
    child.on('close', (code) => { clearTimeout(timer); resolve({ code, out, err, timedOut }); });
  });
}

function parseReport(out) {
  const line = out.split('\n').find((l) => l.startsWith('{'));
  if (!line) return null;
  try {
    const r = JSON.parse(line);
    delete r.file;
    return r;
  } catch {
    return null;
  }
}

function engineFailure(res, r) {
  if (r.timedOut || r.code === 3) return fail(res, 504, 'TIMEOUT', 'the job exceeded the time limit');
  const report = parseReport(r.out);
  if (r.code === 2 && report) {
    sendJson(res, 422, { ...report, ok: false, errorCode: report.errorCode ?? 'REJECTED', error: report.error ?? 'input rejected' });
    return;
  }
  if (r.code === 64) return fail(res, 400, 'BAD_REQUEST', r.err.split('\n').find((l) => l.trim()) ?? 'invalid options');
  console.error(`engine exit ${r.code}: ${r.err.slice(0, 2000)}`);
  return fail(res, 500, 'INTERNAL', 'the engine did not produce a report');
}

async function withJob(res, fn) {
  const ok = await acquire();
  if (!ok) return fail(res, 429, 'BUSY', 'queue full, retry later');
  const dir = await fs.promises.mkdtemp(path.join(os.tmpdir(), 'kura-'));
  try {
    await fn(dir);
  } catch (e) {
    console.error(e);
    if (!res.headersSent) fail(res, 500, 'INTERNAL', 'unexpected server error');
  } finally {
    release();
    fs.promises.rm(dir, { recursive: true, force: true }).catch(() => {});
  }
}

async function handleConvert(req, res, url) {
  await withJob(res, async (dir) => {
    const parsed = await readRequest(req, res);
    if (parsed.tooLarge) return fail(res, 413, 'TOO_LARGE', `the upload limit is ${MAX_UPLOAD} bytes`, { maxBytes: MAX_UPLOAD });
    if (!parsed.file) return fail(res, 400, 'BAD_REQUEST', 'send the document as the request body or as the "file" field');
    const kind = looksLikeInput(parsed.file);
    if (!kind) return fail(res, 400, 'NOT_A_PDF', 'the body is neither a PDF nor a JPEG');
    const built = buildArgs(url, req, parsed.fields, dir, 'convert');
    if (built.error) return fail(res, ...built.error);
    const src = path.join(dir, `in.${kind}`);
    const dst = path.join(dir, 'out.pdf');
    await fs.promises.writeFile(src, parsed.file);
    const r = await runKura([...built.args, src, dst], dir);
    if (r.code !== 0) return engineFailure(res, r);
    const report = parseReport(r.out) ?? { ok: true };
    const pdf = await fs.promises.readFile(dst);
    if (url.searchParams.get('report') === 'json') {
      sendJson(res, 200, { ...report, pdf: pdf.toString('base64') });
      return;
    }
    res.writeHead(200, {
      'content-type': 'application/pdf',
      'content-length': pdf.length,
      'x-kura-level': String(report.level ?? ''),
      'x-kura-engine': String(report.engine ?? VERSION),
      'x-kura-changes': String((report.issues ?? []).length),
    });
    res.end(pdf);
  });
}

async function handleCheck(req, res, url) {
  await withJob(res, async (dir) => {
    const parsed = await readRequest(req, res);
    if (parsed.tooLarge) return fail(res, 413, 'TOO_LARGE', `the upload limit is ${MAX_UPLOAD} bytes`, { maxBytes: MAX_UPLOAD });
    if (!parsed.file) return fail(res, 400, 'BAD_REQUEST', 'send the document as the request body or as the "file" field');
    const kind = looksLikeInput(parsed.file);
    if (!kind) return fail(res, 400, 'NOT_A_PDF', 'the body is neither a PDF nor a JPEG');
    const built = buildArgs(url, req, parsed.fields, dir, 'check');
    if (built.error) return fail(res, ...built.error);
    const src = path.join(dir, `in.${kind}`);
    await fs.promises.writeFile(src, parsed.file);
    const r = await runKura([...built.args, src], dir);
    if (r.code !== 0 && r.code !== 1) return engineFailure(res, r);
    const report = parseReport(r.out);
    if (!report) return engineFailure(res, { ...r, code: -1 });
    sendJson(res, 200, report);
  });
}

async function handleVerify(req, res) {
  await withJob(res, async (dir) => {
    const body = await readBody(req, res, MAX_UPLOAD);
    if (body === null) return fail(res, 413, 'TOO_LARGE', `the upload limit is ${MAX_UPLOAD} bytes`, { maxBytes: MAX_UPLOAD });
    const src = path.join(dir, 'in.pdf');
    await fs.promises.writeFile(src, body);
    const password = req.headers['x-password'];
    const args = ['--verify-password'];
    if (typeof password === 'string' && password.length > 0) args.push('--password', password);
    const r = await runKura([...args, src], dir);
    if (r.code === 0 || r.code === 1) return sendJson(res, 200, { ok: true, valid: r.code === 0 });
    return engineFailure(res, r);
  });
}

function serveStatic(res, key) {
  const [file, type] = STATIC[key];
  fs.readFile(path.join(SITE, file), (err, buf) => {
    if (err) return fail(res, 404, 'NOT_FOUND', 'no such file');
    res.writeHead(200, { 'content-type': type, 'content-length': buf.length, 'cache-control': 'no-cache', 'x-content-type-options': 'nosniff' });
    res.end(buf);
  });
}

const server = http.createServer({ maxHeaderSize: 262144 }, (req, res) => {
  const url = new URL(req.url ?? '/', `http://${req.headers.host ?? 'localhost'}`);
  const p = url.pathname;
  if (req.method === 'GET' || req.method === 'HEAD') {
    if (p === '/') return serveStatic(res, '/index.html');
    if (p === '/preflight') return serveStatic(res, '/preflight.html');
    if (Object.hasOwn(STATIC, p)) return serveStatic(res, p);
    if (p === '/healthz') return sendJson(res, 200, { ok: true, version: VERSION, active, queued: waiting.length });
    if (p === '/api/profiles') return sendJson(res, 200, PROFILE_LIST);
    if (p === '/docs' || p.startsWith('/docs/')) {
      res.writeHead(302, { location: DOCS_URL + p.replace(/^\/docs\/?/, '') });
      return res.end();
    }
    return fail(res, 404, 'NOT_FOUND', 'no such route');
  }
  if (req.method === 'POST' && p.startsWith('/api/')) {
    if (!authorized(req)) return fail(res, 401, 'UNAUTHORIZED', 'a valid API token is required');
    if (p === '/api/convert') return void handleConvert(req, res, url);
    if (p === '/api/check') return void handleCheck(req, res, url);
    if (p === '/api/verify-password') return void handleVerify(req, res);
  }
  return fail(res, 404, 'NOT_FOUND', 'no such route');
});

server.requestTimeout = TIMEOUT_MS + 60000;
server.listen(PORT, HOST, () => {
  console.log(`${VERSION} serving on http://${HOST}:${PORT} (concurrency ${CONCURRENCY}, queue ${QUEUE_LIMIT}, max upload ${MAX_UPLOAD / 1024 / 1024} MB, ${PROFILE_LIST.length} bundled profiles${API_TOKEN ? ', token required' : ''})`);
});

for (const sig of ['SIGTERM', 'SIGINT']) {
  process.on(sig, () => {
    server.close(() => process.exit(0));
    setTimeout(() => process.exit(0), 3000).unref();
  });
}
