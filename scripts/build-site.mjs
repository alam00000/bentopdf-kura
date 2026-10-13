import { createHash } from 'node:crypto';
import { execFileSync } from 'node:child_process';
import { cpSync, existsSync, mkdirSync, readFileSync, readdirSync, rmSync, writeFileSync } from 'node:fs';
import * as path from 'node:path';
import { fileURLToPath } from 'node:url';

const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const SITE = path.join(ROOT, 'site');
const OUT = path.join(ROOT, 'dist-site');
const ASSET_FILES = ['app.js', 'preflight.js', 'ui.js', 'pdf.js', 'profiles.js', 'engine.js', 'worker.js', 'app.css'];
const PAGES = ['index.html', 'preflight.html'];

for (const f of ['kura.js', 'kura.wasm']) {
  if (!existsSync(path.join(SITE, f))) {
    throw new Error(`site/${f} not found; run make wasm && make site-sync first`);
  }
}

execFileSync(process.execPath, [path.join(ROOT, 'scripts', 'gen-site-profiles.mjs')], { stdio: 'ignore' });

const hash = (files) => {
  const h = createHash('sha256');
  for (const f of files) h.update(readFileSync(f));
  return h.digest('hex').slice(0, 10);
};

const fonts = readdirSync(path.join(SITE, 'fonts')).filter((f) => f.endsWith('.woff2')).sort();
const ENGINE = hash([path.join(SITE, 'kura.wasm')]);
const ASSETS = hash([
  ...ASSET_FILES.map((f) => path.join(SITE, f)),
  path.join(SITE, 'icons', 'phosphor.js'),
  ...fonts.map((f) => path.join(SITE, 'fonts', f)),
]);
const A = path.join(OUT, 'a', ASSETS);

rmSync(OUT, { recursive: true, force: true });
mkdirSync(path.join(OUT, 'engine'), { recursive: true });
mkdirSync(path.join(A, 'fonts'), { recursive: true });
mkdirSync(path.join(A, 'icons'), { recursive: true });

for (const f of [...PAGES, 'logo.svg']) cpSync(path.join(SITE, f), path.join(OUT, f));
for (const f of ASSET_FILES) cpSync(path.join(SITE, f), path.join(A, f));
for (const f of [...fonts, 'OFL.txt']) cpSync(path.join(SITE, 'fonts', f), path.join(A, 'fonts', f));
for (const f of ['phosphor.js', 'LICENSE.txt']) cpSync(path.join(SITE, 'icons', f), path.join(A, 'icons', f));
cpSync(path.join(SITE, 'kura.wasm'), path.join(OUT, 'engine', `kura.${ENGINE}.wasm`));

const rewrite = (file, pairs) => {
  let s = readFileSync(file, 'utf8');
  for (const [from, to] of pairs) s = s.replaceAll(from, to);
  writeFileSync(file, s);
};

writeFileSync(
  path.join(OUT, 'engine', `kura.${ENGINE}.js`),
  readFileSync(path.join(SITE, 'kura.js'), 'utf8').replaceAll('kura.wasm', `kura.${ENGINE}.wasm`)
);
rewrite(path.join(A, 'worker.js'), [["'./kura.js'", `'../../engine/kura.${ENGINE}.js'`]]);
for (const page of PAGES) {
  rewrite(path.join(OUT, page), [
    ['href="./app.css"', `href="./a/${ASSETS}/app.css"`],
    ['src="./app.js"', `src="./a/${ASSETS}/app.js"`],
    ['src="./preflight.js"', `src="./a/${ASSETS}/preflight.js"`],
  ]);
}

const unrewritten = [
  ...PAGES.filter((p) => /"\.\/(app\.css|app\.js|preflight\.js)"/.test(readFileSync(path.join(OUT, p), 'utf8'))),
  ...ASSET_FILES.filter((f) => /'\.\/kura\.js'|'kura\.wasm'/.test(readFileSync(path.join(A, f), 'utf8'))),
];
if (unrewritten.length) throw new Error(`unrewritten asset reference in ${unrewritten.join(', ')}`);

execFileSync('npx', ['vitepress', 'build', path.join(ROOT, 'docs')], { cwd: ROOT, stdio: 'inherit' });
cpSync(path.join(ROOT, 'docs', '.vitepress', 'dist'), path.join(OUT, 'docs'), { recursive: true });

console.log(`assembled ${OUT} (assets a/${ASSETS}, engine kura.${ENGINE})`);
