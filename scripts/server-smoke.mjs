import { spawn } from 'node:child_process';
import { mkdtemp, writeFile, rm } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import * as path from 'node:path';
import { fileURLToPath } from 'node:url';

const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const BIN = process.argv[2] ?? process.env.KURA_BIN ?? path.join(ROOT, 'pdfa-engine', 'build', 'cli', 'kura');
const PORT = 18000 + Math.floor(Math.random() * 1000);
const BASE = `http://127.0.0.1:${PORT}`;

let failures = 0;
let current = null;
function assert(cond, label) {
  console.log(`${cond ? 'ok  ' : 'FAIL'} ${label}`);
  if (!cond) {
    failures++;
    if (current) {
      const lines = current.log().trim().split('\n').slice(-12);
      if (lines.length && lines[0]) console.log(lines.map((l) => `      server: ${l}`).join('\n'));
    }
  }
}

function minimalPdf(text) {
  const content = `BT /F1 24 Tf 72 700 Td (${text}) Tj ET`;
  const objs = [
    '<< /Type /Catalog /Pages 2 0 R >>',
    '<< /Type /Pages /Kids [3 0 R] /Count 1 >>',
    '<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] /Resources << /Font << /F1 5 0 R >> >> /Contents 4 0 R >>',
    `<< /Length ${content.length} >>stream\n${content}\nendstream`,
    '<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>',
  ];
  let out = '%PDF-1.7\n';
  const offsets = [];
  objs.forEach((o, i) => { offsets.push(out.length); out += `${i + 1} 0 obj\n${o}\nendobj\n`; });
  const xref = out.length;
  out += `xref\n0 ${objs.length + 1}\n0000000000 65535 f \n`;
  for (const off of offsets) out += `${String(off).padStart(10, '0')} 00000 n \n`;
  out += `trailer\n<< /Size ${objs.length + 1} /Root 1 0 R >>\nstartxref\n${xref}\n%%EOF\n`;
  return Buffer.from(out, 'latin1');
}

function startServer(env) {
  const child = spawn(process.execPath, [path.join(ROOT, 'server', 'server.mjs')], {
    env: { ...process.env, KURA_BIN: BIN, KURA_PORT: String(PORT), KURA_HOST: '127.0.0.1', ...env },
    stdio: ['ignore', 'pipe', 'pipe'],
  });
  let log = '';
  child.stdout.on('data', (d) => { log += d; });
  child.stderr.on('data', (d) => { log += d; });
  return { child, log: () => log };
}

async function waitReady() {
  for (let i = 0; i < 100; i++) {
    try {
      const r = await fetch(`${BASE}/healthz`);
      if (r.ok) return await r.json();
    } catch {}
    await new Promise((r) => setTimeout(r, 100));
  }
  throw new Error('server did not become ready');
}

async function stop(s) {
  s.child.kill('SIGTERM');
  await new Promise((resolve) => { s.child.on('exit', resolve); setTimeout(resolve, 3000); });
}

const input = minimalPdf('Server smoke');
const dir = await mkdtemp(path.join(tmpdir(), 'kura-server-smoke-'));
const profile = JSON.stringify({
  'kura-profile': 1, name: 'Smoke', description: 'smoke profile',
  checks: [{ name: 'Any page over letter size', severity: 'warning', scope: 'page', all: [{ prop: 'page.width', op: '>', value: 700 }] }],
});
await writeFile(path.join(dir, 'profile.json'), profile);

let s = startServer({});
current = s;
try {
  const health = await waitReady();
  assert(health.ok === true && /\d+\.\d+\.\d+/.test(health.version || ''), `healthz -> ${health.version}`);

  let r = await fetch(`${BASE}/api/convert?level=2b`, { method: 'POST', body: input });
  const pdf = Buffer.from(await r.arrayBuffer());
  assert(r.status === 200 && pdf.subarray(0, 4).toString() === '%PDF', `convert raw body -> ${r.status}, ${pdf.length} bytes`);
  assert(r.headers.get('x-kura-level') === '2b' && Number(r.headers.get('x-kura-changes')) >= 1, 'convert response headers');

  const fd = new FormData();
  fd.append('file', new Blob([input]), 'in.pdf');
  r = await fetch(`${BASE}/api/convert?level=2b&report=json&ua=true&lang=en-US`, { method: 'POST', body: fd });
  let j = await r.json();
  assert(r.status === 200 && j.ok === true && typeof j.pdf === 'string' && (j.issues || []).some((i) => i.code === 'OUTPUT_INTENT_ADDED'), 'convert multipart with JSON report');
  assert(typeof j.pdf === 'string' && Buffer.from(j.pdf, 'base64').subarray(0, 4).toString() === '%PDF', 'JSON report carries the PDF');

  r = await fetch(`${BASE}/api/check?level=2b`, { method: 'POST', body: input });
  j = await r.json();
  assert(r.status === 200 && j.mode === 'check' && j.compliant === false && j.findings >= 1, `check on the input -> ${j.findings} finding(s)`);

  r = await fetch(`${BASE}/api/check?level=2b`, { method: 'POST', body: pdf });
  j = await r.json();
  assert(r.status === 200 && j.compliant === true && j.findings === 0, 'check on the converted output is clean');

  const fd2 = new FormData();
  fd2.append('file', new Blob([input]), 'in.pdf');
  fd2.append('profile', profile);
  r = await fetch(`${BASE}/api/check?level=2b`, { method: 'POST', body: fd2 });
  j = await r.json();
  assert(r.status === 200 && Array.isArray(j.analysis), `check with a custom profile -> ${(j.analysis || []).length} analysis line(s)`);

  r = await fetch(`${BASE}/api/profiles`);
  const list = await r.json();
  assert(Array.isArray(list) && list.length > 100 && list[0].id.includes('/'), `bundled profiles -> ${list.length}`);
  if (list.length) {
    r = await fetch(`${BASE}/api/check?level=2b&profile=${list[0].id}`, { method: 'POST', body: input });
    j = await r.json();
    assert(r.status === 200 && Array.isArray(j.analysis), `check with bundled profile ${list[0].id}`);
  }

  r = await fetch(`${BASE}/api/check?level=2b&profile=no/such`, { method: 'POST', body: input });
  assert(r.status === 400 && (await r.json()).errorCode === 'BAD_PROFILE', 'unknown bundled profile -> 400 BAD_PROFILE');

  r = await fetch(`${BASE}/api/convert?level=9z`, { method: 'POST', body: input });
  assert(r.status === 400 && (await r.json()).errorCode === 'BAD_LEVEL', 'bad level -> 400 BAD_LEVEL');

  r = await fetch(`${BASE}/api/convert?level=x4p`, { method: 'POST', body: input });
  assert(r.status === 400, 'check-only flavour on convert -> 400');

  r = await fetch(`${BASE}/api/convert?level=2b`, { method: 'POST', body: Buffer.from('garbage') });
  assert(r.status === 400 && (await r.json()).errorCode === 'NOT_A_PDF', 'garbage -> 400 NOT_A_PDF');

  r = await fetch(`${BASE}/api/convert?level=2b`, { method: 'POST', body: Buffer.from('%PDF-1.7 broken') });
  j = await r.json();
  assert(r.status === 422 && j.ok === false && j.errorCode === 'PARSE_ERROR', `broken PDF -> 422 ${j.errorCode}`);

  r = await fetch(`${BASE}/api/verify-password`, { method: 'POST', body: input });
  assert(r.status === 200 && (await r.json()).valid === true, 'verify-password on an open file -> valid');

  r = await fetch(`${BASE}/`);
  const html = await r.text();
  assert(r.status === 200 && html.includes('<title') && html.includes('app.js'), 'GET / serves the converter page');
  r = await fetch(`${BASE}/preflight`);
  assert(r.status === 200 && (await r.text()).includes('preflight.js'), 'GET /preflight serves the preflight page');
  r = await fetch(`${BASE}/engine.js`);
  assert(r.status === 200 && (await r.text()).includes('/api/convert'), 'GET /engine.js serves the HTTP engine');
  r = await fetch(`${BASE}/docs/preflight`, { redirect: 'manual' });
  assert(r.status === 302 && (r.headers.get('location') || '').endsWith('/docs/preflight'), 'docs links redirect to the hosted docs');
  r = await fetch(`${BASE}/worker.js`);
  assert(r.status === 404, 'files outside the whitelist are not served');
} finally {
  await stop(s);
}

s = startServer({ KURA_API_TOKEN: 'secret', KURA_MAX_UPLOAD_MB: '1' });
current = s;
try {
  await waitReady();
  let r = await fetch(`${BASE}/api/convert?level=2b`, { method: 'POST', body: input });
  assert(r.status === 401, 'token required -> 401 without one');
  r = await fetch(`${BASE}/api/convert?level=2b`, { method: 'POST', headers: { authorization: 'Bearer secret' }, body: input });
  assert(r.status === 200, 'token accepted with Authorization: Bearer');
  r = await fetch(`${BASE}/api/convert?level=2b`, { method: 'POST', headers: { 'x-api-token': 'secret' }, body: Buffer.alloc(2 * 1024 * 1024, 0x25) });
  assert(r.status === 413, 'oversized upload -> 413');
  r = await fetch(`${BASE}/healthz`);
  assert(r.status === 200, 'healthz stays open without a token');
} finally {
  await stop(s);
  await rm(dir, { recursive: true, force: true });
}

if (failures) {
  console.error(`${failures} check(s) failed`);
  process.exit(1);
}
console.log('server smoke: all checks passed');
