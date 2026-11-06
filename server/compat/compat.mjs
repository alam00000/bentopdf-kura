import * as http from 'node:http';
import * as fs from 'node:fs';
import * as os from 'node:os';
import * as path from 'node:path';
import * as zlib from 'node:zlib';
import { spawn } from 'node:child_process';
import { fileURLToPath } from 'node:url';

const HERE = path.dirname(fileURLToPath(import.meta.url));
const REPO = path.resolve(HERE, '..', '..');
const KURA = path.resolve(process.argv[2] ?? process.env.KURA_BIN ?? path.join(REPO, 'pdfa-engine/build/cli/kura'));
const RUST = path.resolve(process.argv[3] ?? path.join(REPO, 'server/target/release/kura-server'));
const FAKE = path.join(HERE, 'fake-kura.sh');
const PROFILES = path.join(REPO, 'pdfa-engine/profiles');
const SITE = path.join(REPO, 'site');
const IGNORED = new Set(['date', 'connection', 'keep-alive', 'transfer-encoding']);

process.on('uncaughtException', (e) => {
  if (e.code === 'EPIPE' || e.code === 'ECONNRESET') return;
  throw e;
});

const refRoot = fs.mkdtempSync(path.join(os.tmpdir(), 'kura-compat-'));
fs.mkdirSync(path.join(refRoot, 'server'));
fs.copyFileSync(path.join(HERE, 'reference-server.mjs'), path.join(refRoot, 'server', 'server.mjs'));
fs.symlinkSync(SITE, path.join(refRoot, 'site'));

let port = 19000 + Math.floor(Math.random() * 500);
let failures = 0;
let compared = 0;
let finished = false;
process.on('exit', (code) => {
  if (!finished && code === 0) {
    console.error('compatibility run did not reach the end');
    process.exitCode = 1;
  }
});

function minimalPdf(text, pages = 1) {
  const content = `BT /F1 24 Tf 72 700 Td (${text}) Tj ET`;
  const kids = Array.from({ length: pages }, (_, i) => `${3 + i} 0 R`).join(' ');
  const objs = ['<< /Type /Catalog /Pages 2 0 R >>', `<< /Type /Pages /Kids [${kids}] /Count ${pages} >>`];
  for (let i = 0; i < pages; i++) {
    objs.push(`<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] /Resources << /Font << /F1 ${4 + pages} 0 R >> >> /Contents ${3 + pages} 0 R >>`);
  }
  objs.push(`<< /Length ${content.length} >>stream\n${content}\nendstream`);
  objs.push('<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>');
  let out = '%PDF-1.7\n';
  const offsets = [];
  objs.forEach((o, i) => { offsets.push(out.length); out += `${i + 1} 0 obj\n${o}\nendobj\n`; });
  const xref = out.length;
  out += `xref\n0 ${objs.length + 1}\n0000000000 65535 f \n`;
  for (const off of offsets) out += `${String(off).padStart(10, '0')} 00000 n \n`;
  out += `trailer\n<< /Size ${objs.length + 1} /Root 1 0 R >>\nstartxref\n${xref}\n%%EOF\n`;
  return Buffer.from(out, 'latin1');
}

function multipart(parts) {
  const boundary = '----kuracompat' + 'x'.repeat(8);
  const chunks = [];
  for (const p of parts) {
    let head = `--${boundary}\r\nContent-Disposition: form-data; name="${p.name}"`;
    if (p.filename !== undefined) head += `; filename="${p.filename}"\r\nContent-Type: application/octet-stream`;
    chunks.push(Buffer.from(head + '\r\n\r\n'), Buffer.from(p.value), Buffer.from('\r\n'));
  }
  chunks.push(Buffer.from(`--${boundary}--\r\n`));
  return { body: Buffer.concat(chunks), type: `multipart/form-data; boundary=${boundary}` };
}

function start(kind, env) {
  const p = port++;
  const base = { ...process.env, KURA_BIN: env.KURA_BIN ?? KURA, KURA_PROFILES: PROFILES, KURA_PORT: String(p), KURA_HOST: '127.0.0.1', ...env };
  const child = kind === 'node'
    ? spawn(process.execPath, [path.join(refRoot, 'server', 'server.mjs')], { env: base, stdio: ['ignore', 'pipe', 'pipe'] })
    : spawn(RUST, ['serve'], { env: { ...base, KURA_SITE: SITE }, stdio: ['ignore', 'pipe', 'pipe'] });
  let log = '';
  child.stdout.on('data', (d) => { log += d; });
  child.stderr.on('data', (d) => { log += d; });
  return { kind, port: p, child, log: () => log };
}

async function ready(s) {
  for (let i = 0; i < 100; i++) {
    try {
      const r = await send(s, { path: '/healthz' });
      if (r.status === 200) return;
    } catch {}
    await new Promise((r) => setTimeout(r, 100));
  }
  throw new Error(`${s.kind} server did not start:\n${s.log()}`);
}

async function stop(s) {
  s.child.kill('SIGTERM');
  await new Promise((resolve) => { s.child.on('exit', resolve); setTimeout(resolve, 4000); });
}

function send(s, { method = 'GET', path: p, headers = {}, body, chunked = false, hold = null }) {
  return new Promise((resolve) => {
    const h = { ...headers };
    if (body !== undefined && !chunked) h['content-length'] = String(body.length);
    const req = http.request({ host: '127.0.0.1', port: s.port, method, path: p, headers: h, agent: false });
    let settled = false;
    let answered = false;
    const timer = setTimeout(() => { if (!settled) { settled = true; req.destroy(); resolve({ status: 'no response', headers: {}, body: Buffer.alloc(0) }); } }, 60000);
    req.on('close', () => clearTimeout(timer));
    req.on('socket', (sock) => sock.on('error', () => {}));
    req.on('response', (res) => {
      answered = true;
      const chunks = [];
      res.on('data', (c) => chunks.push(c));
      res.on('end', () => { settled = true; resolve({ status: res.statusCode, headers: res.headers, body: Buffer.concat(chunks) }); });
      res.on('error', (e) => { if (!settled) { settled = true; resolve({ status: res.statusCode, headers: res.headers, body: Buffer.concat(chunks), error: e.code }); } });
    });
    req.on('error', (e) => { if (!settled && !answered) { settled = true; resolve({ status: `connection error ${e.code}`, headers: {}, body: Buffer.alloc(0) }); } });
    if (hold) {
      req.write(body.subarray(0, 10));
      hold.then(() => req.end(body.subarray(10)));
      return;
    }
    if (body !== undefined && chunked) {
      const step = 64 * 1024;
      let off = 0;
      const pump = () => {
        while (off < body.length && !answered) {
          const ok = req.write(body.subarray(off, off + step));
          off += step;
          if (!ok) return req.once('drain', pump);
        }
        if (!answered) req.end();
      };
      pump();
      return;
    }
    req.end(body);
  });
}

function pdfText(s) {
  return s
    .replace(/stream\r?\n([\s\S]*?)endstream/g, (m, data) => {
      try {
        return `stream\n${zlib.inflateSync(Buffer.from(data, 'latin1')).toString('latin1')}endstream`;
      } catch {
        return m;
      }
    })
    .replace(/D:\d{14}/g, 'D:00000000000000')
    .replace(/\d{4}-\d\d-\d\dT\d\d:\d\d:\d\d/g, '0000-00-00T00:00:00')
    .replace(/uuid:[0-9a-fA-F-]{36}/g, 'uuid:0')
    .replace(/<[0-9a-fA-F]{32}>/g, '<0>')
    .replace(/(\/Type \/XRef[^]*?stream\n)[^]*?endstream/g, '$1endstream')
    .replace(/\/(Length|First) \d+/g, '/$1 0')
    .replace(/startxref\s+\d+/g, 'startxref 0')
    .replace(/\d{10} \d{5} ([nf])/g, '0 0 $1');
}

function normalize(r) {
  const headers = {};
  for (const k of Object.keys(r.headers).sort()) if (!IGNORED.has(k)) headers[k] = r.headers[k];
  let body = r.body.toString('latin1').replace(/kura-[A-Za-z0-9]{6}/g, 'kura-XXXXXX');
  const type = String(r.headers['content-type'] ?? '');
  if (type.includes('json')) {
    try {
      const j = JSON.parse(Buffer.from(body, 'latin1').toString('utf8'));
      if (typeof j.pdf === 'string') {
        j.pdf = pdfText(Buffer.from(j.pdf, 'base64').toString('latin1').replace(/kura-[A-Za-z0-9]{6}/g, 'kura-XXXXXX'));
        body = `json-with-pdf:${JSON.stringify(j)}`;
      }
    } catch {}
  }
  if (body.startsWith('%PDF-')) body = pdfText(body);
  if (body.length > 400 && !type.includes('json')) {
    body = `${body.length} bytes, sha=${hash(body)}`;
  }
  return { status: r.status, headers, body };
}

function hash(s) {
  let h = 2166136261;
  for (let i = 0; i < s.length; i++) { h ^= s.charCodeAt(i); h = Math.imul(h, 16777619) >>> 0; }
  return h.toString(16);
}

function compare(label, a, b) {
  compared++;
  const na = JSON.stringify(normalize(a));
  const nb = JSON.stringify(normalize(b));
  if (na === nb) {
    console.log(`ok   ${label} -> ${a.status}`);
    return;
  }
  failures++;
  console.log(`FAIL ${label}`);
  console.log(`  node: ${na.slice(0, 1200)}`);
  console.log(`  rust: ${nb.slice(0, 1200)}`);
}

async function pair(env, fn) {
  const servers = [start('node', env), start('rust', env)];
  try {
    await Promise.all(servers.map(ready));
    await fn(servers[0], servers[1]);
  } finally {
    await Promise.all(servers.map(stop));
  }
}

async function both(node, rust, label, req) {
  for (let attempt = 1; ; attempt++) {
    const a = await send(node, req);
    const b = await send(rust, req);
    const flaky = req.chunked && (typeof a.status !== 'number' || typeof b.status !== 'number');
    if (flaky && attempt < 5) continue;
    compare(label, a, b);
    return;
  }
}

const input = minimalPdf('Compat');
const jpeg = fs.existsSync(path.join(REPO, 'tests/fixtures/topdf/photo.jpg')) ? fs.readFileSync(path.join(REPO, 'tests/fixtures/topdf/photo.jpg')) : null;
const profile = JSON.stringify({
  'kura-profile': 1, name: 'Compat', description: 'compat profile',
  checks: [{ name: 'Any page over letter size', severity: 'warning', scope: 'page', all: [{ check: 'page.box_size', box: 'media', dimension: 'width', op: '>', value: '700pt' }] }],
});

const routes = [
  ['GET', '/'], ['GET', '/preflight'], ['GET', '/index.html'], ['GET', '/preflight.html'], ['GET', '/app.js'],
  ['GET', '/engine.js'], ['GET', '/app.css'], ['GET', '/logo.svg'], ['GET', '/icons/phosphor.js'],
  ['GET', '/fonts/dm-sans-latin.woff2'], ['GET', '/worker.js'], ['GET', '/engine-http.js'], ['GET', '/nope'],
  ['GET', '/docs'], ['GET', '/docs/'], ['GET', '/docs/preflight'], ['GET', '/docs/http-api?x=1'],
  ['GET', '/docs/../app.js'], ['GET', '/%2e%2e/index.html'], ['GET', '/app.js?v=2'], ['HEAD', '/'], ['HEAD', '/healthz'],
  ['HEAD', '/nope'], ['GET', '/healthz'], ['GET', '/api/profiles'], ['OPTIONS', '/'], ['PUT', '/api/convert'],
  ['POST', '/healthz'], ['POST', '/api/unknown'], ['GET', '/api/convert'], ['DELETE', '/'], ['POST', '/'],
];

await pair({}, async (node, rust) => {
  for (const [method, p] of routes) await both(node, rust, `${method} ${p}`, { method, path: p });

  const cases = [
    ['convert raw', '/api/convert?level=2b', input],
    ['convert report=json', '/api/convert?level=2b&report=json', input],
    ['convert ua+lang', '/api/convert?level=2b&ua=true&lang=en-US&report=json', input],
    ['convert 3b on', '/api/convert?level=3b&ua=on&outlineFonts=1', input],
    ['convert x4', '/api/convert?level=x4&report=json', input],
    ['convert bundled profile', '/api/convert?level=x4&profile=press/book-text-pages-check-and-fix&report=json', input],
    ['check', '/api/check?level=2b', input],
    ['check analyze', '/api/check?level=2b&analyze=true', input],
    ['check x4p', '/api/check?level=x4p', input],
    ['check bundled profile', '/api/check?level=x4&profile=press/book-text-pages-check', input],
    ['bad profile', '/api/check?level=2b&profile=no/such', input],
    ['profile traversal', '/api/check?level=2b&profile=../x', input],
    ['bad level', '/api/convert?level=9z', input],
    ['no level', '/api/convert', input],
    ['empty level', '/api/convert?level=', input],
    ['check-only on convert', '/api/convert?level=x4p', input],
    ['garbage', '/api/convert?level=2b', Buffer.from('garbage')],
    ['empty body', '/api/convert?level=2b', Buffer.alloc(0)],
    ['pdf marker late', '/api/check?level=2b', Buffer.concat([Buffer.alloc(1021, 32), Buffer.from('%PDF')])],
    ['broken pdf', '/api/convert?level=2b', Buffer.from('%PDF-1.7 broken')],
    ['control char in lang', '/api/convert?level=2b&lang=a%01b', input],
    ['513-char lang', `/api/convert?level=2b&lang=${'a'.repeat(513)}`, input],
    ['512-char registry', `/api/check?level=x4&registry=${'r'.repeat(512)}`, input],
    ['plus decodes to space', '/api/check?level=x4&outputCondition=FOGRA+39', input],
    ['unicode value', '/api/check?level=2b&lang=%C3%A9', input],
    ['repeated params', '/api/check?level=2b&level=9z', input],
    ['verify open file', '/api/verify-password', input],
    ['verify garbage', '/api/verify-password', Buffer.from('nope')],
    ['raster dpi', '/api/convert?level=1b&rasterizePages=true&rasterDpi=30&report=json', input],
    ['bad raster dpi', '/api/convert?level=1b&rasterizePages=true&rasterDpi=5', input],
  ];
  if (jpeg) cases.push(['jpeg input', '/api/convert?level=2b&report=json', jpeg]);
  for (const [label, p, body] of cases) await both(node, rust, label, { method: 'POST', path: p, body });

  await both(node, rust, 'verify with password header', { method: 'POST', path: '/api/verify-password', body: input, headers: { 'x-password': 'secret' } });
  await both(node, rust, 'convert with latin1 password', { method: 'POST', path: '/api/convert?level=2b', body: input, headers: { 'x-password': 'caf\xe9' } });

  const forms = [
    ['multipart file', '/api/convert?level=2b&report=json', [{ name: 'file', filename: 'in.pdf', value: input }]],
    ['multipart fields', '/api/convert?report=json', [{ name: 'level', value: '2b' }, { name: 'ua', value: 'true' }, { name: 'lang', value: 'de' }, { name: 'file', filename: 'a.pdf', value: input }]],
    ['query beats field', '/api/check?level=2b', [{ name: 'level', value: '9z' }, { name: 'file', filename: 'a.pdf', value: input }]],
    ['custom profile field', '/api/check?level=2b', [{ name: 'file', filename: 'in.pdf', value: input }, { name: 'profile', value: profile }]],
    ['custom profile as file', '/api/check?level=2b', [{ name: 'file', filename: 'in.pdf', value: input }, { name: 'profile', filename: 'p.json', value: profile }]],
    ['invalid profile json', '/api/check?level=2b', [{ name: 'file', filename: 'in.pdf', value: input }, { name: 'profile', value: '{nope' }]],
    ['xml attachment', '/api/convert?level=3b&report=json', [{ name: 'file', filename: 'in.pdf', value: input }, { name: 'xml', filename: 'i.xml', value: '<Invoice/>' }]],
    ['password field', '/api/check?level=2b', [{ name: 'file', filename: 'in.pdf', value: input }, { name: 'password', value: 'pw' }]],
    ['no file field', '/api/convert?level=2b', [{ name: 'level', value: '2b' }]],
    ['file as text field', '/api/convert?level=2b', [{ name: 'file', value: 'text' }]],
    ['two files, last wins', '/api/check?level=2b', [{ name: 'file', filename: 'a', value: 'junk' }, { name: 'file', filename: 'b', value: input }]],
    ['level as file part', '/api/convert', [{ name: 'level', filename: 'l', value: '2b' }, { name: 'file', filename: 'b', value: input }]],
  ];
  for (const [label, p, parts] of forms) {
    const { body, type } = multipart(parts);
    await both(node, rust, label, { method: 'POST', path: p, body, headers: { 'content-type': type } });
  }
  await both(node, rust, 'multipart without boundary', { method: 'POST', path: '/api/convert?level=2b', body: input, headers: { 'content-type': 'multipart/form-data' } });
  await both(node, rust, 'multipart type, raw body', { method: 'POST', path: '/api/convert?level=2b', body: input, headers: { 'content-type': 'multipart/form-data; boundary=zzz' } });
  await both(node, rust, 'verify ignores multipart', { method: 'POST', path: '/api/verify-password', body: input, headers: { 'content-type': 'multipart/form-data; boundary=zzz' } });

  const files = [];
  for (const dir of ['corpus-synth', 'tests/fixtures', 'tests/fixtures/topdf', 'tests/fixtures/profile', 'tests/fixtures/reports']) {
    const full = path.join(REPO, dir);
    if (!fs.existsSync(full)) continue;
    for (const f of fs.readdirSync(full).sort()) if (f.endsWith('.pdf')) files.push(path.join(full, f));
  }
  for (const f of files) {
    const body = fs.readFileSync(f);
    const name = path.relative(REPO, f);
    await both(node, rust, `corpus convert ${name}`, { method: 'POST', path: '/api/convert?level=2b', body });
    await both(node, rust, `corpus check ${name}`, { method: 'POST', path: '/api/check?level=2b&analyze=true', body });
  }
});

await pair({ KURA_API_TOKEN: 'secret', KURA_MAX_UPLOAD_MB: '1' }, async (node, rust) => {
  const big = Buffer.alloc(2 * 1024 * 1024, 0x25);
  const reqs = [
    ['no token', { method: 'POST', path: '/api/convert?level=2b', body: input }],
    ['bearer', { method: 'POST', path: '/api/convert?level=2b', body: input, headers: { authorization: 'Bearer secret' } }],
    ['x-api-token', { method: 'POST', path: '/api/check?level=2b', body: input, headers: { 'x-api-token': 'secret' } }],
    ['wrong token', { method: 'POST', path: '/api/convert?level=2b', body: input, headers: { authorization: 'Bearer nope' } }],
    ['basic auth falls back to x-api-token', { method: 'POST', path: '/api/check?level=2b', body: input, headers: { authorization: 'Basic abc', 'x-api-token': 'secret' } }],
    ['bearer wins over x-api-token', { method: 'POST', path: '/api/check?level=2b', body: input, headers: { authorization: 'Bearer nope', 'x-api-token': 'secret' } }],
    ['token on unknown api route', { method: 'POST', path: '/api/nope' }],
    ['token checked before level', { method: 'POST', path: '/api/convert?level=9z', body: input }],
    ['healthz open', { path: '/healthz' }],
    ['profiles open', { path: '/api/profiles' }],
    ['413 declared', { method: 'POST', path: '/api/convert?level=2b', body: big, headers: { 'x-api-token': 'secret' } }],
    ['413 chunked', { method: 'POST', path: '/api/convert?level=2b', body: big, chunked: true, headers: { 'x-api-token': 'secret' } }],
    ['413 multipart chunked', { method: 'POST', path: '/api/convert?level=2b', body: multipart([{ name: 'file', filename: 'a', value: big }]).body, chunked: true, headers: { 'x-api-token': 'secret', 'content-type': multipart([]).type } }],
    ['under the limit', { method: 'POST', path: '/api/check?level=2b', body: Buffer.concat([input, Buffer.alloc(1024 * 1024 - input.length - 1, 10)]), headers: { 'x-api-token': 'secret' } }],
  ];
  for (const [label, req] of reqs) await both(node, rust, label, req);
});

await pair({ KURA_CONCURRENCY: '1', KURA_QUEUE: '1' }, async (node, rust) => {
  for (const s of [node, rust]) s.results = [];
  for (const s of [node, rust]) {
    let release;
    const gate = new Promise((r) => { release = r; });
    const first = send(s, { method: 'POST', path: '/api/check?level=2b', body: input, hold: gate });
    await new Promise((r) => setTimeout(r, 300));
    const second = send(s, { method: 'POST', path: '/api/check?level=2b', body: input, hold: gate });
    await new Promise((r) => setTimeout(r, 300));
    s.results.push(await send(s, { path: '/healthz' }));
    s.results.push(await send(s, { method: 'POST', path: '/api/check?level=2b', body: input }));
    release();
    s.results.push(await first, await second);
    s.results.push(await send(s, { path: '/healthz' }));
  }
  ['healthz while busy', 'third request gets 429', 'held request', 'queued request', 'healthz after'].forEach((label, i) => compare(label, node.results[i], rust.results[i]));
});

await pair({ KURA_BIN: FAKE, KURA_TIMEOUT_MS: '1000' }, async (node, rust) => {
  const modes = ['sleep', 'exit3', 'exit64', 'exit2', 'exit2null', 'exit2noreport', 'exit5', 'garbage', 'check1', 'floats', 'noreport', 'nooutput', 'argv'];
  for (const m of modes) {
    for (const kind of ['convert', 'check']) {
      if (m === 'sleep' && kind === 'check') continue;
      await both(node, rust, `fake ${kind} ${m}`, { method: 'POST', path: `/api/${kind}?level=2b&lang=${m}`, body: input });
    }
  }
  await both(node, rust, 'fake floats report=json', { method: 'POST', path: '/api/convert?level=2b&lang=floats&report=json', body: input });
  const all = '&ua=1&allowVisualRisk=1&rasterizePages=1&outlineFonts=1&analyze=1&embedSource=1&outputCondition=oc&outputConditionInfo=oci&registry=r&vtRecords=1-2&attachXmlName=a.xml&facturxProfile=EN16931&embedSourceName=s.pdf&rasterDpi=100&imageMaxPpi=200';
  const { body, type } = multipart([
    { name: 'file', filename: 'in.pdf', value: input }, { name: 'profile', value: profile }, { name: 'xml', filename: 'x', value: '<x/>' },
    { name: 'destProfile', filename: 'd', value: 'icc' }, { name: 'defaultRgb', filename: 'r', value: 'icc' },
    { name: 'defaultCmyk', filename: 'c', value: 'icc' }, { name: 'defaultGray', value: 'icc' }, { name: 'password', value: 'pw' },
  ]);
  await both(node, rust, 'fake argv, every option', { method: 'POST', path: `/api/convert?level=2b&lang=argv${all}&report=json`, body, headers: { 'content-type': type } });
  await both(node, rust, 'fake argv, bundled profile', { method: 'POST', path: '/api/check?level=2b&lang=argv&profile=press/book-text-pages-check', body: input });
  await both(node, rust, 'fake healthz', { path: '/healthz' });
});

fs.rmSync(refRoot, { recursive: true, force: true });
finished = true;
console.log(`\n${compared - failures}/${compared} responses identical`);
if (failures) {
  console.error(`${failures} difference(s)`);
  process.exit(1);
}
