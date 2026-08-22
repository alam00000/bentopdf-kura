import { readFileSync, writeFileSync } from 'fs';
import createKuraModule from './kura.js';

const [, , inputPath, outputPath, level = '2b', password = ''] = process.argv;
if (!inputPath || !outputPath) {
  console.error('usage: node example.mjs input.pdf output.pdf [level] [password]');
  process.exit(1);
}

const mod = await createKuraModule();
const input = new Uint8Array(readFileSync(inputPath));
const res = mod.convert(input, level, { password: password });

if (!res.ok) {
  console.error(`conversion failed: ${res.errorCode}: ${res.error}`);
  process.exit(2);
}
for (const issue of res.issues) {
  console.log(`${issue.fixed ? 'fixed' : 'note '} ${issue.code}: ${issue.detail}`);
}
writeFileSync(outputPath, Buffer.from(res.pdf));
console.log(`wrote ${outputPath} (${res.pdf.length} bytes) as PDF/A-${res.level}`);
