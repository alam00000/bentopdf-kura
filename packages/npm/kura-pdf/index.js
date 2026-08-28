import createModule from './engine/kura.js';

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

const BYTE_OPTIONS = ['attachXml', 'embedSource', 'destProfile', 'defaultRgb', 'defaultCmyk', 'defaultGray'];

let pending = null;

function engine() {
  pending ??= createModule().catch((err) => {
    pending = null;
    throw new KuraError('ENGINE_LOAD', `the WebAssembly module failed to load: ${describe(err)}`);
  });
  return pending;
}

function describe(err) {
  return err instanceof Error ? err.message : String(err);
}

function toBytes(input, what) {
  if (input instanceof Uint8Array) return input;
  if (input instanceof ArrayBuffer) return new Uint8Array(input);
  if (ArrayBuffer.isView(input)) return new Uint8Array(input.buffer, input.byteOffset, input.byteLength);
  throw new KuraError('BAD_INPUT', `${what} must be a Uint8Array or ArrayBuffer`);
}

function prepare(options, check) {
  const out = {};
  for (const [key, value] of Object.entries(options ?? {})) {
    if (value === undefined || value === null) continue;
    out[key] = BYTE_OPTIONS.includes(key) ? toBytes(value, `options.${key}`) : value;
  }
  if (check) out.check = true;
  return out;
}

function plainIssues(list) {
  return (list ?? []).map((i) => ({ code: i.code, detail: i.detail, fixed: !!i.fixed }));
}

function plainAnalysis(list) {
  return (list ?? []).map((a) => ({ code: a.code, detail: a.detail }));
}

function raise(r) {
  throw new KuraError(r.errorCode || 'ENGINE_ERROR', r.error || 'the engine rejected the document', {
    suggestedLevel: r.suggestedLevel || null,
    issues: plainIssues(r.issues),
  });
}

export async function convert(input, level = '2b', options = {}) {
  const bytes = toBytes(input, 'input');
  const mod = await engine();
  const r = mod.convert(bytes, String(level), prepare(options, false));
  if (!r.ok) raise(r);
  return {
    pdf: r.pdf,
    level: r.level,
    engine: r.engine,
    issues: plainIssues(r.issues),
    analysis: plainAnalysis(r.analysis),
  };
}

export async function check(input, level = '2b', options = {}) {
  const bytes = toBytes(input, 'input');
  const mod = await engine();
  const r = mod.convert(bytes, String(level), prepare(options, true));
  if (!r.ok) raise(r);
  return {
    compliant: !!r.compliant,
    findings: Number(r.findings ?? 0),
    level: r.level,
    engine: r.engine,
    issues: plainIssues(r.issues),
    analysis: plainAnalysis(r.analysis),
  };
}

export async function verifyPassword(input, password = '') {
  const mod = await engine();
  const bytes = toBytes(input, 'input');
  return mod.verifyPassword(bytes, String(password));
}

export async function version() {
  const mod = await engine();
  return mod.version();
}

export async function load() {
  await engine();
}
