import { spawn } from 'node:child_process';
import * as fs from 'node:fs';
import * as os from 'node:os';
import * as path from 'node:path';
import { createRequire } from 'node:module';
import { fileURLToPath } from 'node:url';

export const LEVELS = [
  '1b', '1a', '2b', '2u', '2a', '3b', '3u', '3a', '4', '4f', '4e',
  'x1a', 'x3', 'x4', 'x6', 'e1', 'vt1', 'vt3',
];

export const CHECK_ONLY_LEVELS = ['x4p', 'x5g', 'x5n', 'x5pg', 'x6n', 'x6p', 'vt2'];

export class KuraError extends Error {
  constructor(code, message, extra = {}) {
    super(message);
    this.name = 'KuraError';
    this.code = code;
    this.suggestedLevel = extra.suggestedLevel ?? null;
    this.issues = extra.issues ?? [];
  }
}

const HERE = path.dirname(fileURLToPath(import.meta.url));
const EXE = process.platform === 'win32' ? '.exe' : '';
const PLATFORM_PACKAGE = `kura-pdf-${process.platform}-${process.arch}`;
const requireFromHere = createRequire(import.meta.url);
let resolved;

export function binaryPath() {
  if (resolved !== undefined) return resolved;
  const fromEnv = process.env.KURA_BIN;
  if (fromEnv) {
    resolved = fromEnv;
    return resolved;
  }
  const sibling = path.join(HERE, '..', PLATFORM_PACKAGE, 'bin', `kura${EXE}`);
  if (fs.existsSync(sibling)) {
    resolved = sibling;
    return resolved;
  }
  try {
    const dir = path.dirname(requireFromHere.resolve(`${PLATFORM_PACKAGE}/package.json`));
    const candidate = path.join(dir, 'bin', `kura${EXE}`);
    resolved = fs.existsSync(candidate) ? candidate : null;
  } catch {
    resolved = null;
  }
  return resolved;
}

function requireBinary() {
  const bin = binaryPath();
  if (bin) return bin;
  throw new KuraError('ENGINE_MISSING',
    `no kura binary for ${process.platform}-${process.arch}: use a platform with a native package (darwin-arm64, linux-x64, win32-x64), set KURA_BIN to a binary you built, or switch to the kura-pdf-wasm package`);
}

const BOOL_FLAGS = {
  ua: '--ua',
  allowVisualRisk: '--allow-visual-risk',
  rasterizePages: '--rasterize-pages',
  outlineFonts: '--outline-fonts',
  analyze: '--analyze',
  embedSource: '--embed-source',
  ocr: '--ocr',
};
const TEXT_FLAGS = {
  lang: '--lang',
  password: '--password',
  rasterDpi: '--raster-dpi',
  imageMaxPpi: '--image-max-ppi',
  attachXmlName: '--attach-xml-name',
  facturxProfile: '--facturx-profile',
  embedSourceName: '--embed-source-name',
  outputCondition: '--output-condition',
  outputConditionInfo: '--output-condition-info',
  registry: '--registry',
  vtRecords: '--vt-records',
  ocrEngine: '--ocr-engine',
  fontFolder: '--font-folder',
};
const FILE_FLAGS = {
  attachXml: ['--attach-xml', 'attachment.xml'],
  destProfile: ['--dest-profile', 'dest.icc'],
  defaultRgb: ['--default-rgb', 'rgb.icc'],
  defaultCmyk: ['--default-cmyk', 'cmyk.icc'],
  defaultGray: ['--default-gray', 'gray.icc'],
  profile: ['--profile', 'profile.txt'],
};

function toBytes(input, what) {
  if (input instanceof Uint8Array) return input;
  if (input instanceof ArrayBuffer) return new Uint8Array(input);
  if (ArrayBuffer.isView(input)) return new Uint8Array(input.buffer, input.byteOffset, input.byteLength);
  throw new KuraError('BAD_INPUT', `${what} must be a Uint8Array or ArrayBuffer`);
}

function fileContent(value, what) {
  if (typeof value === 'string') return value;
  return toBytes(value, what);
}

function buildArgs(options, dir) {
  const args = [];
  const o = options ?? {};
  for (const [key, flag] of Object.entries(BOOL_FLAGS)) {
    if (o[key]) args.push(flag);
  }
  for (const [key, flag] of Object.entries(TEXT_FLAGS)) {
    if (o[key] === undefined || o[key] === null || o[key] === '') continue;
    args.push(flag, String(o[key]));
  }
  for (const [key, [flag, name]] of Object.entries(FILE_FLAGS)) {
    if (o[key] === undefined || o[key] === null) continue;
    const p = path.join(dir, name);
    fs.writeFileSync(p, fileContent(o[key], `options.${key}`));
    args.push(flag, p);
  }
  if (o.sign) {
    const s = o.sign;
    let p12 = s.p12;
    if (typeof p12 !== 'string') {
      p12 = path.join(dir, 'signer.p12');
      fs.writeFileSync(p12, toBytes(s.p12, 'options.sign.p12'));
    }
    args.push('--sign', p12);
    if (s.password) args.push('--sign-password', String(s.password));
    if (s.name) args.push('--sign-name', String(s.name));
    if (s.reason) args.push('--sign-reason', String(s.reason));
    if (s.location) args.push('--sign-location', String(s.location));
  }
  return args;
}

function run(bin, args, cwd, timeoutMs) {
  return new Promise((resolve) => {
    const env = { ...process.env };
    if (timeoutMs) env.PDFA_TIMEOUT = String(Math.max(1, Math.ceil(timeoutMs / 1000)));
    let child;
    try {
      child = spawn(bin, args, { cwd, env, stdio: ['ignore', 'pipe', 'pipe'] });
    } catch (e) {
      resolve({ code: -1, out: '', err: e.message, timedOut: false });
      return;
    }
    let out = '';
    let err = '';
    let timedOut = false;
    const timer = timeoutMs ? setTimeout(() => { timedOut = true; child.kill('SIGKILL'); }, timeoutMs + 5000) : null;
    child.stdout.on('data', (d) => { out += d; });
    child.stderr.on('data', (d) => { if (err.length < 65536) err += d; });
    child.on('error', (e) => { if (timer) clearTimeout(timer); resolve({ code: -1, out, err: e.message, timedOut }); });
    child.on('close', (code) => { if (timer) clearTimeout(timer); resolve({ code, out, err, timedOut }); });
  });
}

function parseReport(out) {
  const line = out.split('\n').find((l) => l.startsWith('{'));
  if (!line) return null;
  try {
    return JSON.parse(line);
  } catch {
    return null;
  }
}

function plainIssues(list) {
  return (list ?? []).map((i) => ({ code: i.code, detail: i.detail, fixed: !!i.fixed }));
}

function plainAnalysis(list) {
  return (list ?? []).map((a) => ({ code: a.code, detail: a.detail }));
}

function engineFailure(r, bin) {
  if (r.timedOut || r.code === 3) throw new KuraError('TIMEOUT', 'the engine exceeded the time limit');
  const report = parseReport(r.out);
  if (r.code === 2 && report) {
    throw new KuraError(report.errorCode || 'ENGINE_ERROR', report.error || 'the engine rejected the document', {
      suggestedLevel: report.suggestedLevel || null,
      issues: plainIssues(report.issues),
    });
  }
  const firstLine = r.err.split('\n').find((l) => l.trim()) ?? '';
  if (r.code === 64) throw new KuraError('BAD_OPTION', firstLine || 'the engine rejected the options');
  if (r.code === -1) throw new KuraError('ENGINE_MISSING', `cannot run ${bin}: ${firstLine}`);
  throw new KuraError('ENGINE_ERROR', firstLine || `the engine exited with status ${r.code}`);
}

async function withTemp(fn) {
  const dir = await fs.promises.mkdtemp(path.join(os.tmpdir(), 'kura-pdf-'));
  try {
    return await fn(dir);
  } finally {
    await fs.promises.rm(dir, { recursive: true, force: true }).catch(() => {});
  }
}

function inputName(bytes) {
  const jpeg = bytes.length >= 3 && bytes[0] === 0xff && bytes[1] === 0xd8 && bytes[2] === 0xff;
  return jpeg ? 'in.jpg' : 'in.pdf';
}

function checkLevel(level, allowed) {
  const l = String(level);
  if (!allowed.includes(l)) {
    throw new KuraError('BAD_LEVEL', `unknown level ${l}; expected one of ${allowed.join(', ')}`);
  }
  return l;
}

export async function convert(input, level = '2b', options = {}) {
  const bytes = toBytes(input, 'input');
  const l = checkLevel(level, LEVELS);
  const bin = requireBinary();
  return withTemp(async (dir) => {
    const src = path.join(dir, inputName(bytes));
    const dst = path.join(dir, 'out.pdf');
    await fs.promises.writeFile(src, bytes);
    const args = ['--level', l, ...buildArgs(options, dir), src, dst];
    const r = await run(bin, args, dir, options?.timeoutMs);
    if (r.code !== 0) engineFailure(r, bin);
    const report = parseReport(r.out) ?? {};
    const pdf = new Uint8Array(await fs.promises.readFile(dst));
    return {
      pdf,
      level: report.level ?? l,
      engine: report.engine ?? '',
      issues: plainIssues(report.issues),
      analysis: plainAnalysis(report.analysis),
    };
  });
}

export async function check(input, level = '2b', options = {}) {
  const bytes = toBytes(input, 'input');
  const l = checkLevel(level, [...LEVELS, ...CHECK_ONLY_LEVELS]);
  const bin = requireBinary();
  return withTemp(async (dir) => {
    const src = path.join(dir, inputName(bytes));
    await fs.promises.writeFile(src, bytes);
    const args = ['--check', '--level', l, ...buildArgs(options, dir), src];
    const r = await run(bin, args, dir, options?.timeoutMs);
    if (r.code !== 0 && r.code !== 1) engineFailure(r, bin);
    const report = parseReport(r.out);
    if (!report) engineFailure({ ...r, code: -2 }, bin);
    return {
      compliant: !!report.compliant,
      findings: Number(report.findings ?? 0),
      level: report.level ?? l,
      engine: report.engine ?? '',
      issues: plainIssues(report.issues),
      analysis: plainAnalysis(report.analysis),
    };
  });
}

export async function verifyPassword(input, password = '') {
  const bytes = toBytes(input, 'input');
  const bin = requireBinary();
  return withTemp(async (dir) => {
    const src = path.join(dir, 'in.pdf');
    await fs.promises.writeFile(src, bytes);
    const args = ['--verify-password'];
    if (password) args.push('--password', String(password));
    args.push(src);
    const r = await run(bin, args, dir);
    if (r.code === 0) return true;
    if (r.code === 1 || r.code === 2) return false;
    engineFailure(r, bin);
    return false;
  });
}

export async function version() {
  const bin = requireBinary();
  const r = await run(bin, ['--version'], os.tmpdir());
  if (r.code !== 0) engineFailure(r, bin);
  return r.out.trim().replace(' (kura)', '');
}

export async function load() {
  requireBinary();
}
