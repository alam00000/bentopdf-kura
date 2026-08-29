import { mkdtemp, writeFile, readFile, rm } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import * as path from 'node:path';
import { spawnSync } from 'node:child_process';
import { fileURLToPath } from 'node:url';

const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const PKG = path.join(ROOT, 'packages', 'npm', 'kura-pdf');
if (process.argv[2]) process.env.KURA_BIN = path.resolve(process.argv[2]);
const { convert, check, version, verifyPassword, binaryPath, KuraError, LEVELS } = await import(path.join(PKG, 'index.js'));

let failures = 0;
function assert(cond, label) {
  console.log(`${cond ? 'ok  ' : 'FAIL'} ${label}`);
  if (!cond) failures++;
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
  return new TextEncoder().encode(out);
}

const input = minimalPdf('Native smoke');
const bin = binaryPath();
assert(typeof bin === 'string' && bin.length > 0, `engine binary resolved -> ${bin}`);

const v = await version();
assert(/Kura Engine \d+\.\d+\.\d+/.test(v), `version() -> ${v}`);

const r = await convert(input, '2b', { ua: false });
assert(r.pdf instanceof Uint8Array && r.pdf.length > input.length && r.level === '2b', `convert 2b -> ${r.pdf.length} bytes`);
assert(r.issues.some((i) => i.code === 'OUTPUT_INTENT_ADDED'), 'report carries issues');

const before = await check(input, '2b');
assert(before.compliant === false && before.findings >= 1, `check before -> ${before.findings} finding(s)`);
const after = await check(r.pdf, '2b');
assert(after.compliant === true && after.findings === 0, 'converted output checks clean');

const ua = await convert(input, '2a', { ua: true, lang: 'en-US' });
assert(ua.issues.some((i) => /STRUCT|TAG/.test(i.code)), 'PDF/A-2a with ua builds a structure tree');

const analyzed = await check(input, '2b', { analyze: true, profile: JSON.stringify({ 'kura-profile': 1, name: 'Smoke', checks: [{ name: 'Wide page', severity: 'warning', scope: 'page', all: [{ prop: 'page.width', op: '>', value: 700 }] }] }) });
assert(Array.isArray(analyzed.analysis), `check with a profile -> ${analyzed.analysis.length} analysis line(s)`);

let caught = null;
try { await convert(new Uint8Array([0x25, 0x50, 0x44, 0x46, 1, 2, 3]), '2b'); } catch (e) { caught = e; }
assert(caught instanceof KuraError && caught.code === 'PARSE_ERROR', `garbage -> KuraError ${caught && caught.code}`);

caught = null;
try { await convert(input, '9z'); } catch (e) { caught = e; }
assert(caught instanceof KuraError && caught.code === 'BAD_LEVEL', `bad level -> KuraError ${caught && caught.code}`);

caught = null;
try { await convert(input, 'x4p'); } catch (e) { caught = e; }
assert(caught instanceof KuraError && caught.code === 'BAD_LEVEL', 'check-only flavour on convert -> BAD_LEVEL');

caught = null;
try { await convert('not bytes', '2b'); } catch (e) { caught = e; }
assert(caught instanceof KuraError && caught.code === 'BAD_INPUT', 'string input -> BAD_INPUT');

assert(await verifyPassword(input, '') === true, 'verifyPassword is true for a file without encryption');
assert(LEVELS.length === 18, `LEVELS lists ${LEVELS.length} convertible targets`);

const dir = await mkdtemp(path.join(tmpdir(), 'kura-native-smoke-'));
try {
  const inPath = path.join(dir, 'in.pdf');
  const outPath = path.join(dir, 'out.pdf');
  await writeFile(inPath, input);
  const cli = path.join(PKG, 'bin', 'kura.js');
  const run = (...args) => spawnSync(process.execPath, [cli, ...args], { encoding: 'utf8' });
  let p = run('--version');
  assert(p.status === 0 && /Kura Engine/.test(p.stdout), `cli --version -> ${p.stdout.trim()}`);
  p = run('--level', '2b', inPath, outPath);
  assert(p.status === 0 && JSON.parse(p.stdout).ok === true && (await readFile(outPath)).length > 0, `cli convert -> exit ${p.status}`);
  p = run('--level', '2b', inPath);
  const dflt = inPath.replace(/\.pdf$/, '.2b.pdf');
  assert(p.status === 0 && JSON.parse(p.stdout).output === dflt && (await readFile(dflt)).length > 0, 'cli writes <input>.<level>.pdf when no output path is given');

  p = run('--check', '--level', '2b', inPath);
  assert(p.status === 1 && JSON.parse(p.stdout).compliant === false, `cli check on non-conforming -> exit ${p.status}`);
  p = run('--level', '9z', inPath, outPath);
  assert(p.status === 64, `cli bad level -> exit ${p.status}`);
  p = spawnSync(process.execPath, [cli, '--version'], { encoding: 'utf8', env: { ...process.env, KURA_BIN: path.join(dir, 'missing') } });
  assert(p.status === 64 && /cannot run/.test(p.stderr), 'cli reports a missing engine clearly');
} finally {
  await rm(dir, { recursive: true, force: true });
}

if (failures) {
  console.error(`${failures} check(s) failed`);
  process.exit(1);
}
console.log('native smoke: all checks passed');
