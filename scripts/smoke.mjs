import { mkdtemp, mkdir, writeFile, readFile, rm } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import * as path from 'node:path';
import { spawnSync } from 'node:child_process';
import { fileURLToPath } from 'node:url';

const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const PKG = path.join(ROOT, 'packages', 'npm', 'kura-pdf-wasm');
const { convert, check, version, verifyPassword, KuraError, LEVELS } = await import(path.join(PKG, 'index.js'));

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
  objs.forEach((o, i) => {
    offsets.push(out.length);
    out += `${i + 1} 0 obj\n${o}\nendobj\n`;
  });
  const xref = out.length;
  out += `xref\n0 ${objs.length + 1}\n0000000000 65535 f \n`;
  for (const off of offsets) out += `${String(off).padStart(10, '0')} 00000 n \n`;
  out += `trailer\n<< /Size ${objs.length + 1} /Root 1 0 R >>\nstartxref\n${xref}\n%%EOF\n`;
  return new TextEncoder().encode(out);
}

const input = minimalPdf('Smoke test');

const v = await version();
assert(v.includes('Kura') && /\d+\.\d+\.\d+/.test(v), `version() -> ${v}`);

const r = await convert(input, '2b');
assert(r.pdf instanceof Uint8Array && r.pdf.length > input.length, `convert 2b -> ${r.pdf.length} bytes`);
assert(new TextDecoder().decode(r.pdf.subarray(0, 5)) === '%PDF-', 'output starts with %PDF-');
assert(r.level === '2b' && r.issues.some((i) => i.code === 'OUTPUT_INTENT_ADDED'), 'report carries level and issues');

const before = await check(input, '2b');
assert(before.compliant === false && before.findings >= 1, `check before -> ${before.findings} finding(s)`);

const after = await check(r.pdf, '2b');
assert(after.compliant === true && after.findings === 0, 'converted output checks clean');

const ua = await convert(input, '2a', { ua: true, lang: 'en-US' });
assert(ua.issues.some((i) => /STRUCT|TAG/.test(i.code)), 'PDF/A-2a with --ua builds a structure tree');

let caught = null;
try { await convert(new Uint8Array([1, 2, 3]), '2b'); } catch (e) { caught = e; }
assert(caught instanceof KuraError && caught.code === 'PARSE_ERROR', `garbage -> KuraError ${caught && caught.code}`);

caught = null;
try { await convert(input, '9z'); } catch (e) { caught = e; }
assert(caught instanceof KuraError && caught.code === 'BAD_LEVEL', `bad level -> KuraError ${caught && caught.code}`);

caught = null;
try { await convert('not bytes', '2b'); } catch (e) { caught = e; }
assert(caught instanceof KuraError && caught.code === 'BAD_INPUT', 'string input -> BAD_INPUT');

const LOCKED_PDF = Uint8Array.from(atob('JVBERi0xLjcKJb/3ov4KMSAwIG9iago8PCAvRXh0ZW5zaW9ucyA8PCAvQURCRSA8PCAvQmFzZVZlcnNpb24gLzEuNyAvRXh0ZW5zaW9uTGV2ZWwgOCA+PiA+PiAvUGFnZXMgMiAwIFIgL1R5cGUgL0NhdGFsb2cgPj4KZW5kb2JqCjIgMCBvYmoKPDwgL0NvdW50IDEgL0tpZHMgWyAzIDAgUiBdIC9UeXBlIC9QYWdlcyA+PgplbmRvYmoKMyAwIG9iago8PCAvQ29udGVudHMgNCAwIFIgL01lZGlhQm94IFsgMCAwIDYxMiA3OTIgXSAvUGFyZW50IDIgMCBSIC9SZXNvdXJjZXMgPDwgL0ZvbnQgPDwgL0YxIDUgMCBSID4+ID4+IC9UeXBlIC9QYWdlID4+CmVuZG9iago0IDAgb2JqCjw8IC9MZW5ndGggODAgL0ZpbHRlciAvRmxhdGVEZWNvZGUgPj4Kc3RyZWFtCl68Nq1lYhEejSP13SjNdU2irum5Pj+ALJR1sfpMJfeYYRm0kJF0thqZiHkQ2+j12sNoGqIO+VyrUI9s03StCOMC94CwCqZXDBNc9D51B/zoZW5kc3RyZWFtCmVuZG9iago1IDAgb2JqCjw8IC9CYXNlRm9udCAvSGVsdmV0aWNhIC9TdWJ0eXBlIC9UeXBlMSAvVHlwZSAvRm9udCA+PgplbmRvYmoKNiAwIG9iago8PCAvQ0YgPDwgL1N0ZENGIDw8IC9BdXRoRXZlbnQgL0RvY09wZW4gL0NGTSAvQUVTVjMgL0xlbmd0aCAzMiA+PiA+PiAvRmlsdGVyIC9TdGFuZGFyZCAvTGVuZ3RoIDI1NiAvTyA8MDJmYjc2NWVmZjgzMzMwNGZlMmRkYzUyN2VkZWJjYWEyMzJkMTc1OWI4OGY4OGRhYTY3MTI2M2Y4ODMwZDU1MGRjZjFlNWE2ZDJiMjlkNzM5NzJjYjFjMGVhNGM2NTlhPiAvT0UgPDFlYjM5YTE5MjY1MGZkNzlmMWEzZGUxNTE5ZmRlZTRlYTY3Y2I0YjQ2ZDlhNzQyMTAwZGQzN2VkYmZmYThkZDk+IC9QIC00IC9QZXJtcyA8NTk5OWM1MjY3NGVmODI0MTdjMzcwM2U5YTAxYjY5ZGQ+IC9SIDYgL1N0bUYgL1N0ZENGIC9TdHJGIC9TdGRDRiAvVSA8YjY3ODM3NTI2ZjlkZjQwODY5YTNhNzk3MjBmZmI5MjVkNjYwMjVlZDI2ZTg2ZjhkNzMxNzAwNDczODRjYTU0M2RjMjk5YzljZGNhNzQwMjQ4ZTU0M2MwNTIxMDRlODkyPiAvVUUgPDI5YWM0ZGU3NTZkMDcxNzdjNjA3NGEwNmU4MGU1ZWUzZWE5YzFmYTJkMWExZWZlOTk5ZGJlZDUxMDFmYjkwYjg+IC9WIDUgPj4KZW5kb2JqCnhyZWYKMCA3CjAwMDAwMDAwMDAgNjU1MzUgZiAKMDAwMDAwMDAxNSAwMDAwMCBuIAowMDAwMDAwMTMwIDAwMDAwIG4gCjAwMDAwMDAxODkgMDAwMDAgbiAKMDAwMDAwMDMxNyAwMDAwMCBuIAowMDAwMDAwNDY3IDAwMDAwIG4gCjAwMDAwMDA1MzcgMDAwMDAgbiAKdHJhaWxlciA8PCAvUm9vdCAxIDAgUiAvU2l6ZSA3IC9JRCBbPDMyYjAxMmNiNjZjMzI3Zjc0ZTVmNGRiOGE5MjdmMmViPjwzMmIwMTJjYjY2YzMyN2Y3NGU1ZjRkYjhhOTI3ZjJlYj5dIC9FbmNyeXB0IDYgMCBSID4+CnN0YXJ0eHJlZgoxMDg0CiUlRU9GCg=='), (c) => c.charCodeAt(0));
assert(await verifyPassword(LOCKED_PDF, 'secret') === true, 'verifyPassword accepts the right password');
assert(await verifyPassword(LOCKED_PDF, 'nope') === false, 'verifyPassword rejects a wrong password');
assert(await verifyPassword(input, '') === true, 'verifyPassword is true for a file without encryption');
const unlocked = await convert(LOCKED_PDF, '2b', { password: 'secret' });
assert(unlocked.pdf.length > 0 && unlocked.issues.some((i) => i.code === 'ENCRYPTION_REMOVED'), 'locked file converts with its password');
caught = null;
try { await convert(LOCKED_PDF, '2b'); } catch (e) { caught = e; }
assert(caught instanceof KuraError && caught.code === 'PASSWORD_REQUIRED', `locked file without password -> ${caught && caught.code}`);

assert(LEVELS.length === 18, `LEVELS lists ${LEVELS.length} convertible targets`);

const dir = await mkdtemp(path.join(tmpdir(), 'kura-smoke-'));
try {
  const inPath = path.join(dir, 'in.pdf');
  const outPath = path.join(dir, 'out.pdf');
  await writeFile(inPath, input);
  const cli = path.join(PKG, 'bin', 'kura.js');
  const run = (...args) => spawnSync(process.execPath, [cli, ...args], { encoding: 'utf8' });

  let p = run('--level', '2b', inPath, outPath);
  assert(p.status === 0 && JSON.parse(p.stdout).ok === true, `cli convert -> exit ${p.status}`);
  assert((await readFile(outPath)).length > 0, 'cli wrote the output');

  p = run('--check', '--level', '2b', inPath);
  assert(p.status === 1 && JSON.parse(p.stdout).compliant === false, `cli check on non-conforming -> exit ${p.status}`);

  p = run('--check', '--level', '2b', outPath);
  assert(p.status === 0 && JSON.parse(p.stdout).compliant === true, `cli check on conforming -> exit ${p.status}`);

  await writeFile(path.join(dir, 'bad.pdf'), 'garbage');
  p = run('--level', '2b', path.join(dir, 'bad.pdf'), path.join(dir, 'x.pdf'));
  assert(p.status === 2 && JSON.parse(p.stdout).errorCode === 'PARSE_ERROR', `cli garbage -> exit ${p.status}`);

  p = run('--level', '9z', inPath, outPath);
  assert(p.status === 64, `cli bad level -> exit ${p.status}`);

  p = run('--help');
  assert(p.status === 0 && p.stdout.includes('usage:'), 'cli --help -> stdout, exit 0');

  p = run('--sign', 'x.p12', '--level', '2b', inPath, outPath);
  assert(p.status === 64 && /native/.test(p.stderr), 'cli refuses native-only flags with a clear message');

  const batchDir = path.join(dir, 'batch');
  await mkdir(batchDir);
  await writeFile(path.join(batchDir, 'one.pdf'), minimalPdf('First'));
  await writeFile(path.join(batchDir, 'two.pdf'), minimalPdf('Second'));
  p = run('--level', '2b', '--batch', '-d', path.join(dir, 'out'), batchDir);
  const arr = JSON.parse(p.stdout);
  assert(p.status === 0 && Array.isArray(arr) && arr.length === 2, `cli batch -> ${Array.isArray(arr) ? arr.length : 0} report(s), exit ${p.status}`);
  p = run('--level', 'x4p', inPath, outPath);
  assert(p.status === 64 && /check-only/.test(p.stderr), 'cli refuses a check-only flavour without --check');
  p = run('--check', '--level', 'x4p', inPath);
  assert(p.status === 0 || p.status === 1, `cli check with a check-only flavour -> exit ${p.status}`);
} finally {
  await rm(dir, { recursive: true, force: true });
}

if (failures) {
  console.error(`${failures} check(s) failed`);
  process.exit(1);
}
console.log('smoke: all checks passed');
