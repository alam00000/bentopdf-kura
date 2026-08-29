#!/usr/bin/env node
import { readFile, writeFile, mkdir, readdir, stat } from 'node:fs/promises';
import * as path from 'node:path';
import { convert, check, version, KuraError, LEVELS, CHECK_ONLY_LEVELS } from '../index.js';

const KNOWN_LEVELS = new Set([...LEVELS, ...CHECK_ONLY_LEVELS]);

const EXIT_OK = 0;
const EXIT_FINDINGS = 1;
const EXIT_REJECTED = 2;
const EXIT_USAGE = 64;

const USAGE = `usage: kura --level {1b,1a,2b,2u,2a,3b,3u,3a,4,4f,4e,x1a,x3,x4,x6,e1,vt1,vt3} [--ua] [--lang <tag>] (check only: x4p,x5g,x5n,x5pg,x6n,x6p,vt2) [--output-condition <name>] [--output-condition-info <text>] [--registry <url>] [--vt-records <ranges>] [--allow-visual-risk] [--password <pw>] <input.pdf> [output.pdf]
       kura --check --level <level> [options] <input.pdf>
       kura --einvoice <invoice.xml> [--level 3b|3u|3a] <input.pdf> [output.pdf]
       kura --level <level> --batch [-r] [-d <dir>] [-s <suffix>] [-w] <folder>

exit status: 0 ok, 1 check found findings, 2 input rejected, 64 usage error`;

const STRING_FLAGS = {
  '--lang': 'lang',
  '--password': 'password',
  '--output-condition': 'outputCondition',
  '--output-condition-info': 'outputConditionInfo',
  '--registry': 'registry',
  '--vt-records': 'vtRecords',
  '--attach-xml-name': 'attachXmlName',
  '--facturx-profile': 'facturxProfile',
  '--embed-source-name': 'embedSourceName',
};
const BOOL_FLAGS = {
  '--ua': 'ua',
  '--allow-visual-risk': 'allowVisualRisk',
  '--rasterize-pages': 'rasterizePages',
  '--outline-fonts': 'outlineFonts',
  '--analyze': 'analyze',
};
const FILE_FLAGS = {
  '--dest-profile': 'destProfile',
  '--default-rgb': 'defaultRgb',
  '--default-cmyk': 'defaultCmyk',
  '--default-gray': 'defaultGray',
  '--attach-xml': 'attachXml',
  '--einvoice': 'attachXml',
};
const NATIVE_ONLY = ['--sign', '--sign-password', '--sign-name', '--sign-reason', '--sign-location', '--ocr', '--ocr-engine', '--font-folder', '--substitute'];

function usage() {
  console.error(USAGE);
  return EXIT_USAGE;
}

function defaultOutput(input, level, ua) {
  const m = input.match(/\.(pdf|jpe?g)$/i);
  const base = m ? input.slice(0, -m[0].length) : input;
  return `${base}.${level}${ua ? '-ua' : ''}.pdf`;
}

function report(file, r, opts, level, output) {
  const out = { file, ok: r.ok };
  if (output) out.output = output;
  if (r.level) out.level = r.level;
  else if (level) out.level = level;
  if (r.engine) out.engine = r.engine;
  if (r.errorCode) { out.errorCode = r.errorCode; out.error = r.error; }
  if (r.suggestedLevel) out.suggestedLevel = r.suggestedLevel;
  if (opts.check) { out.mode = 'check'; out.compliant = r.compliant; out.findings = r.findings; }
  out.issues = r.issues ?? [];
  if (opts.analyze || opts.profile) out.analysis = r.analysis ?? [];
  return JSON.stringify(out);
}

async function runOne(input, output, level, opts, embedSource) {
  let data;
  try {
    data = new Uint8Array(await readFile(input));
  } catch {
    console.error(`cannot open ${input}`);
    return EXIT_REJECTED;
  }
  const options = { ...opts };
  if (embedSource) {
    options.embedSource = data;
    options.embedSourceName ??= path.basename(input);
    options.embedSourceMime = data[0] === 0xff && data[1] === 0xd8 ? 'image/jpeg' : 'application/pdf';
  }
  let engineName;
  try {
    if (opts.check) {
      const r = await check(data, level, options);
      console.log(report(input, { ok: true, ...r }, opts, level));
      return r.compliant ? EXIT_OK : EXIT_FINDINGS;
    }
    const r = await convert(data, level, options);
    engineName = r.engine;
    try {
      await writeFile(output, r.pdf);
    } catch {
      console.error(`cannot write ${output}`);
      return EXIT_REJECTED;
    }
    console.log(report(input, { ok: true, ...r }, opts, level, output));
    return EXIT_OK;
  } catch (e) {
    if (!(e instanceof KuraError)) throw e;
    console.log(report(input, {
      ok: false, errorCode: e.code, error: e.message, suggestedLevel: e.suggestedLevel,
      issues: e.issues, engine: engineName,
    }, opts, level));
    return EXIT_REJECTED;
  }
}

async function listPdfs(dir, recursive) {
  const out = [];
  for (const entry of await readdir(dir, { withFileTypes: true })) {
    const p = path.join(dir, entry.name);
    if (entry.isDirectory()) {
      if (recursive) out.push(...await listPdfs(p, true));
    } else if (/\.pdf$/i.test(entry.name)) {
      out.push(p);
    }
  }
  return out.sort();
}

async function main(argv) {
  const opts = {};
  let level = null;
  let input = null;
  let output = null;
  let einvoice = false;
  let embedSource = false;
  const batch = { active: false, recursive: false, overwrite: false, outDir: '', suffix: '' };

  for (let i = 0; i < argv.length; i++) {
    const arg = argv[i];
    const next = () => (i + 1 < argv.length ? argv[++i] : null);
    if (arg === '--version' || arg === '-v') { console.log(`${await version()} (kura-pdf-wasm)`); return EXIT_OK; }
    if (arg === '--help' || arg === '-h') { console.log(USAGE); return EXIT_OK; }
    if (arg === '--level') {
      level = next();
      if (!level || !KNOWN_LEVELS.has(level)) return usage();
    } else if (arg === '--check') {
      opts.check = true;
    } else if (arg in BOOL_FLAGS) {
      opts[BOOL_FLAGS[arg]] = true;
    } else if (arg in STRING_FLAGS) {
      const v = next();
      if (v === null) return usage();
      opts[STRING_FLAGS[arg]] = v;
    } else if (arg in FILE_FLAGS) {
      const p = next();
      if (p === null) return usage();
      try {
        opts[FILE_FLAGS[arg]] = new Uint8Array(await readFile(p));
      } catch {
        console.error(`cannot open ${p}`);
        return EXIT_USAGE;
      }
      if (arg === '--einvoice') einvoice = true;
    } else if (arg === '--profile') {
      const p = next();
      if (p === null) return usage();
      try {
        opts.profile = await readFile(p, 'utf8');
      } catch {
        console.error(`cannot open ${p}`);
        return EXIT_USAGE;
      }
    } else if (arg === '--raster-dpi') {
      const v = Number(next());
      if (!(v >= 24 && v <= 1200)) { console.error('--raster-dpi must be between 24 and 1200'); return EXIT_USAGE; }
      opts.rasterDpi = v;
    } else if (arg === '--embed-source') {
      embedSource = true;
    } else if (arg === '--batch') {
      batch.active = true;
    } else if (arg === '--recursive' || arg === '-r') {
      batch.active = true; batch.recursive = true;
    } else if (arg === '--out-dir' || arg === '-d') {
      batch.outDir = next() ?? '';
      batch.active = true;
    } else if (arg === '--suffix' || arg === '-s') {
      batch.suffix = next() ?? '';
    } else if (arg === '--overwrite' || arg === '-w') {
      batch.overwrite = true;
    } else if (NATIVE_ONLY.includes(arg)) {
      console.error(`${arg} needs the native kura binary; it is not available in the WebAssembly build`);
      return EXIT_USAGE;
    } else if (arg.startsWith('-')) {
      return usage();
    } else if (input === null) {
      input = arg;
    } else if (output === null) {
      output = arg;
    } else {
      return usage();
    }
  }

  if (einvoice && !level) level = '3b';
  if (!level || input === null) return usage();
  if (CHECK_ONLY_LEVELS.includes(level) && !opts.check) {
    console.error(`${level} is a check-only flavour; use --check`);
    return EXIT_USAGE;
  }
  if (batch.active) {
    if (output !== null) return usage();
    if (!opts.check && !batch.outDir && !batch.suffix && !batch.overwrite) batch.suffix = '_pdfa';
  } else if (opts.check) {
    if (output !== null) return usage();
  } else if (output === null) {
    output = defaultOutput(input, level, !!opts.ua);
  }

  if (!batch.active) return runOne(input, output, level, opts, embedSource);

  let inputs;
  try {
    inputs = (await stat(input)).isDirectory() ? await listPdfs(input, batch.recursive) : [input];
  } catch {
    console.error(`cannot open ${input}`);
    return EXIT_REJECTED;
  }
  if (inputs.length === 0) {
    console.error(`no PDF files found under ${input}`);
    return EXIT_FINDINGS;
  }
  let failed = 0;
  let nonCompliant = 0;
  console.log('[');
  for (let i = 0; i < inputs.length; i++) {
    const src = inputs[i];
    let dst = '';
    if (!opts.check) {
      const dir = batch.outDir || path.dirname(src);
      await mkdir(dir, { recursive: true });
      dst = path.join(dir, path.basename(src, path.extname(src)) + batch.suffix + '.pdf');
      if (path.resolve(dst) === path.resolve(src) && !batch.overwrite) {
        console.error(`refusing to overwrite input ${dst}; use --suffix or -w`);
        failed++;
        continue;
      }
    }
    const rc = await runOne(src, dst, level, opts, embedSource);
    if (rc === EXIT_REJECTED) failed++;
    if (rc === EXIT_FINDINGS) nonCompliant++;
    console.log(i + 1 < inputs.length ? ',' : '');
  }
  console.log(']');
  console.error(`batch: ${inputs.length} file(s), ${failed} rejected, ${nonCompliant} non-compliant`);
  return failed ? EXIT_REJECTED : nonCompliant ? EXIT_FINDINGS : EXIT_OK;
}

main(process.argv.slice(2)).then(
  (code) => process.exit(code),
  (err) => { console.error(err && err.stack || String(err)); process.exit(EXIT_REJECTED); },
);
