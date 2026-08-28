import { icon } from './icons/phosphor.js';

export const $ = (id) => document.getElementById(id);
export const fmtBytes = (n) => n < 1000 ? `${n} B` : n < 1e6 ? `${(n / 1000).toFixed(1)} KB` : `${(n / 1e6).toFixed(2)} MB`;

export function el(tag, cls, text) {
  const n = document.createElement(tag);
  if (cls) n.className = cls;
  if (text !== undefined) n.textContent = text;
  return n;
}

export function withIcon(tag, cls, name, text) {
  const n = el(tag, cls);
  n.appendChild(icon(name));
  n.appendChild(el('span', null, text));
  return n;
}

export function setStatus(text, isError) {
  const s = $('status');
  s.textContent = text;
  s.className = isError ? 'err' : '';
}

export function describeLevel(v) {
  if (/^[1-4]/.test(v)) return `A-${v}`;
  if (/^x/.test(v)) return `X-${v.slice(1)}`;
  if (/^e/.test(v)) return `E-${v.slice(1)}`;
  if (/^vt/.test(v)) return `VT-${v.slice(2)}`;
  return v;
}

export function wireDropzone(zone, input, onFiles) {
  zone.addEventListener('click', () => input.click());
  zone.addEventListener('keydown', (e) => { if (e.key === 'Enter' || e.key === ' ') { e.preventDefault(); input.click(); } });
  for (const ev of ['dragover', 'dragenter']) zone.addEventListener(ev, (e) => { e.preventDefault(); zone.classList.add('over'); });
  for (const ev of ['dragleave', 'drop']) zone.addEventListener(ev, (e) => { e.preventDefault(); zone.classList.remove('over'); });
  zone.addEventListener('drop', (e) => onFiles(e.dataTransfer.files));
}

export function wireFileInput(input, onFiles) {
  input.addEventListener('change', (e) => { onFiles(e.target.files); e.target.value = ''; });
}

export function blockWindowDrops() {
  window.addEventListener('dragover', (e) => e.preventDefault());
  window.addEventListener('drop', (e) => e.preventDefault());
}

export function download(blob, name) {
  const a = document.createElement('a');
  a.href = URL.createObjectURL(blob);
  a.download = name;
  a.click();
  setTimeout(() => URL.revokeObjectURL(a.href), 4000);
}
