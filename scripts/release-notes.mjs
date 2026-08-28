import { readFileSync } from 'node:fs';

const [version, image, tag] = process.argv.slice(2);
if (!version) {
  console.error('usage: node scripts/release-notes.mjs <version> [image] [tag]');
  process.exit(64);
}
const log = readFileSync(new URL('../CHANGELOG.md', import.meta.url), 'utf8');
const lines = log.split('\n');
const start = lines.findIndex((l) => l.startsWith(`## [${version}]`));
let body = 'See CHANGELOG.md.';
if (start >= 0) {
  let end = lines.length;
  for (let i = start + 1; i < lines.length; i++) {
    if (lines[i].startsWith('## [')) { end = i; break; }
  }
  body = lines.slice(start + 1, end).join('\n').trim();
}
let out = `${body}\n\nEvery archive is listed in \`SHA256SUMS\`.`;
if (image && tag) out += ` The Docker image is \`ghcr.io/${image}:${tag}\`.`;
process.stdout.write(`${out}\n`);
