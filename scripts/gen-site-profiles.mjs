import { readFile, writeFile } from 'node:fs/promises';
import * as path from 'node:path';
import { fileURLToPath } from 'node:url';

const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const LIB = path.join(ROOT, 'pdfa-engine', 'profiles');

const CURATED = [
  ['Before press', [
    ['press', 'Prepress basics'], ['press', 'Digital print quality'],
    ['press', 'Sheetfed offset, CMYK: check'], ['press', 'Sheetfed offset with spot colours: check'],
    ['press', 'Web offset, heatset: check'], ['press', 'Newspaper, coldset web: check'],
    ['press', 'Digital print, toner: check'], ['press', 'Large format: check'], ['press', 'Packaging with spot colours: check'],
    ['press', 'Sheetfed offset, CMYK: check and fix'],
  ]],
  ['Ghent Workgroup 2022', [
    ['gwg', 'GWG 2022 sheetfed offset, CMYK: check'], ['gwg', 'GWG 2022 sheetfed offset with spot colours: check'],
    ['gwg', 'GWG 2022 heatset web offset: check'], ['gwg', 'GWG 2022 digital print: check'], ['gwg', 'GWG 2022 large format: check'],
    ['gwg', 'GWG 2022 packaging with spot colours: check'], ['gwg', 'GWG 2022 sheetfed offset, CMYK: check and fix'],
  ]],
  ['Online publishing', [
    ['online', 'Online publishing: check'], ['online', 'Online publishing: check and fix'], ['online', 'Email attachment: check'],
  ]],
  ['Reports', [
    ['report', 'Report hairlines'], ['report', 'Report small text'], ['report', 'Report rich black'], ['report', 'Report white objects'],
    ['report', 'Report invisible text'], ['report', 'Report low resolution images'], ['report', 'Report high resolution images'],
    ['report', 'Report image formats'], ['report', 'Report spot colours'], ['report', 'Report overprint'], ['report', 'Report transparency'],
    ['report', 'Report fonts'], ['report', 'Report page geometry'], ['report', 'Report ink coverage'], ['report', 'Report everything'],
  ]],
  ['Fixes', [
    ['actions', 'Prepress cleanup'], ['actions', 'Thicken hairlines to 0.25 pt'], ['actions', 'Black text to overprint'],
    ['actions', 'All white objects to knockout'], ['pages', 'Set trim box from crop box'], ['pages', 'Add 3 mm bleed box'],
    ['actions', 'Flatten layers'], ['actions', 'Make invisible text visible'], ['pages', 'Scale pages to A4'], ['pages', 'Scale pages to US Letter'],
  ]],
];

const LEVELS = [
  ['Archive (PDF/A)', [
    { name: 'Convert to PDF/A-1b', description: 'Convert the file to the PDF/A-1b archival standard.', level: '1b' },
    { name: 'Convert to PDF/A-2b', description: 'Convert the file to the PDF/A-2b archival standard, the usual choice.', level: '2b' },
    { name: 'Convert to PDF/A-2u', description: 'Convert to PDF/A-2u, with every character mapped to Unicode.', level: '2u' },
    { name: 'Convert to PDF/A-3b', description: 'Convert to PDF/A-3b, which may carry attachments of any kind.', level: '3b' },
    { name: 'Convert to PDF/A-4', description: 'Convert to PDF/A-4, the PDF 2.0 archival standard.', level: '4' },
  ]],
  ['Accessibility (PDF/UA)', [
    { name: 'Convert to PDF/A-2a + PDF/UA-1', description: 'Convert with full tagging and accessibility identification.', level: '2a', ua: true },
    { name: 'Convert to PDF/A-4 + PDF/UA-2', description: 'Convert to the PDF 2.0 archival standard with PDF/UA-2 accessibility.', level: '4', ua: true },
  ]],
  ['Print standards (PDF/X)', [
    { name: 'Convert to PDF/X-1a', description: 'Convert to the blind-exchange print standard (CMYK only, no transparency).', level: 'x1a' },
    { name: 'Convert to PDF/X-3', description: 'Convert to PDF/X-3, colour-managed print exchange.', level: 'x3' },
    { name: 'Convert to PDF/X-4', description: 'Convert to PDF/X-4, the modern print standard with live transparency.', level: 'x4' },
    { name: 'Convert to PDF/X-6', description: 'Convert to PDF/X-6, the PDF 2.0 print standard.', level: 'x6' },
  ]],
  ['Engineering and variable data', [
    { name: 'Convert to PDF/E-1', description: 'Convert to the engineering document standard.', level: 'e1' },
    { name: 'Convert to PDF/VT-1', description: 'Convert to PDF/VT-1 for variable-data print.', level: 'vt1' },
  ]],
];

function slug(name) {
  return name.toLowerCase().replace(/[^a-z0-9]+/g, '-').replace(/^-|-$/g, '').replace(/-+/g, '-');
}

const categories = [];
let n = 0;
for (const [title, items] of CURATED) {
  const profiles = [];
  for (const [folder, name] of items) {
    const file = path.join(LIB, folder, slug(name) + '.json');
    const json = JSON.parse(await readFile(file, 'utf8'));
    profiles.push({ name: json.name, description: json.description, json });
    n++;
  }
  categories.push({ name: title, profiles });
}
for (const [title, items] of LEVELS) {
  categories.push({ name: title, profiles: items });
  n += items.length;
}
const out = `export const PROFILES = ${JSON.stringify({ categories }, null, 1)};\n`;
await writeFile(path.join(ROOT, 'site', 'profiles.js'), out);
console.log(`site/profiles.js: ${categories.length} categories, ${n} entries`);
