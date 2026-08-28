import { readFileSync, readdirSync } from 'node:fs';

const header = readFileSync('pdfa-engine/core/include/kura/kura.h', 'utf8').match(/KURA_VERSION "(.*)"/)[1];
const engine = readFileSync('pdfa-engine/core/include/pdfa/pdfa.hh', 'utf8').match(/kEngineVersion = "(.*)"/)[1];
const seen = [['kura.h', header], ['pdfa.hh', engine]];
const root = JSON.parse(readFileSync('package.json', 'utf8'));
seen.push(['package.json', root.version]);
for (const dir of readdirSync('packages/npm')) {
  const pkg = JSON.parse(readFileSync(`packages/npm/${dir}/package.json`, 'utf8'));
  seen.push([pkg.name, pkg.version]);
  for (const [dep, pin] of Object.entries(pkg.optionalDependencies ?? {})) seen.push([`${pkg.name} -> ${dep}`, pin]);
}
const versions = new Set(seen.map(([, v]) => v));
if (versions.size !== 1) {
  console.error(`version drift: ${seen.map(([n, v]) => `${n} ${v}`).join(', ')}`);
  process.exit(1);
}
console.log(`version ${header} across ${seen.length} places`);
