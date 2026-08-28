import { PROFILES } from './profiles.js';
import { icon, mountIcons } from './icons/phosphor.js';
import { createEngine } from './engine.js';
import { looksEncrypted } from './pdf.js';
import { $, el, fmtBytes, setStatus, describeLevel, wireDropzone, wireFileInput, blockWindowDrops, download } from './ui.js';

mountIcons();

const DEFAULT_PROFILE = 'Prepress basics';
let ready = false;
let busy = false;
let queued = false;
let hasResults = false;
let fileBytes = null;
let fileName = '';
let selected = null;
let encrypted = false;
let verified = false;
let password = '';
let pwError = '';

const engine = createEngine((m) => {
  ready = m.ready;
  if (ready) { $('ver').textContent = m.version; setStatus(''); }
  else setStatus(`Engine failed to load: ${m.error}`, true);
  updateButtons();
  if (ready && fileBytes) run(false);
});

function kindOf(p) {
  if (p.level) return 'convert';
  if (p.json && p.json.fixes && p.json.fixes.length) return 'fix';
  return 'check';
}

function primaryLabel(p) {
  if (!p) return null;
  if (p.level) return `Convert to PDF/${describeLevel(p.level)}${p.ua ? ' + UA' : ''}`;
  if (kindOf(p) === 'fix') return 'Fix and download';
  return null;
}

function select(p, row) {
  for (const r of document.querySelectorAll('.profile-row')) {
    const on = r === row;
    r.classList.toggle('sel', on);
    r.setAttribute('aria-checked', on ? 'true' : 'false');
  }
  row.closest('details').open = true;
  selected = p;
  $('cardName').textContent = p.name;
  $('cardKind').textContent = kindOf(p);
  $('cardDesc').textContent = p.description || '';
  updateButtons();
  if (fileBytes) run(false);
}

function renderProfiles() {
  const host = $('profiles');
  host.replaceChildren();
  let defaultRow = null;
  let defaultProfile = null;
  PROFILES.categories.forEach((cat, ci) => {
    const d = el('details');
    const s = el('summary');
    s.appendChild(icon('caret-right'));
    s.appendChild(el('span', null, cat.name));
    s.appendChild(el('span', 'count', String(cat.profiles.length)));
    d.appendChild(s);
    for (const p of cat.profiles) {
      const row = el('div', 'profile-row');
      row.setAttribute('role', 'radio');
      row.setAttribute('aria-checked', 'false');
      row.tabIndex = 0;
      row.title = p.description || '';
      row.appendChild(el('span', null, p.name));
      row.appendChild(el('span', 'kind', kindOf(p)));
      row.onclick = () => select(p, row);
      row.onkeydown = (e) => { if (e.key === 'Enter' || e.key === ' ') { e.preventDefault(); select(p, row); } };
      d.appendChild(row);
      if (p.name === DEFAULT_PROFILE || (!defaultRow && ci === 0)) { defaultRow = row; defaultProfile = p; }
    }
    host.appendChild(d);
  });
  if (defaultRow) select(defaultProfile, defaultRow);
}

function renderLock() {
  const badge = $('lockBadge');
  badge.classList.toggle('hidden', !encrypted);
  badge.classList.toggle('unlocked', verified);
  badge.replaceChildren();
  if (encrypted) {
    badge.appendChild(icon(verified ? 'lock-open' : 'lock'));
    badge.appendChild(el('span', null, verified ? 'Unlocked' : 'Locked'));
    badge.title = verified ? 'Change password' : '';
  }
  $('unlock').classList.toggle('hidden', !(encrypted && !verified));
  $('pwErr').textContent = pwError;
  $('unlockBtn').disabled = false;
  $('unlockLabel').textContent = 'Unlock';
}

async function unlock() {
  password = $('pw').value;
  if (!password || !ready) return;
  $('unlockBtn').disabled = true;
  $('unlockLabel').textContent = 'Checking…';
  verified = await engine.verifyPassword(fileBytes.slice(0), password);
  pwError = verified ? '' : 'Wrong password';
  renderLock();
  updateButtons();
  if (verified) run(false);
}

function updateButtons() {
  const locked = encrypted && !verified;
  const can = ready && !busy && fileBytes && selected && !locked;
  $('analyzeBtn').disabled = !can;
  $('analyzeLabel').textContent = hasResults ? 'Analyze again' : 'Analyze';
  const label = primaryLabel(selected);
  $('fixBtn').classList.toggle('hidden', !label);
  $('fixHelp').classList.toggle('hidden', !!label);
  if (label) { $('fixLabel').textContent = label; $('fixBtn').disabled = !can; }
  if (!ready || busy) return;
  if (locked) setStatus('This file is password protected. Unlock it to analyze it.');
  else if (fileBytes && selected) setStatus(`${fileName} · ${selected.name}`);
  else if (selected) setStatus(`Drop a PDF on the left to check it against ${selected.name}.`);
  else setStatus('Everything runs in your browser. No file leaves this machine.');
}

function setFile(f) {
  f.arrayBuffer().then((buf) => {
    fileBytes = buf;
    fileName = f.name;
    hasResults = false;
    $('fileName').textContent = f.name;
    $('fileSize').textContent = fmtBytes(f.size);
    $('filechip').classList.remove('hidden');
    $('drop').classList.add('hidden');
    $('results').classList.add('hidden');
    encrypted = looksEncrypted(buf);
    verified = false;
    password = '';
    pwError = '';
    $('pw').value = '';
    renderLock();
    updateButtons();
    if (!encrypted) run(false);
  });
}

function clearFile() {
  fileBytes = null; fileName = ''; hasResults = false;
  encrypted = false; verified = false; password = ''; pwError = '';
  renderLock();
  $('filechip').classList.add('hidden');
  $('drop').classList.remove('hidden');
  $('results').classList.add('hidden');
  updateButtons();
}

function verdict(cls, name, text) {
  const v = el('div', `verdict ${cls}`);
  v.appendChild(icon(name, 'mark'));
  v.appendChild(el('span', null, text));
  return v;
}

function hit(host, sevLabel, sevClass, text) {
  const h = el('div', 'hit');
  h.appendChild(el('span', `sev ${sevClass}`, sevLabel));
  h.appendChild(el('span', null, text));
  host.appendChild(h);
}

async function run(fix) {
  if (!fileBytes || !selected || !ready) return;
  if (busy) { queued = true; return; }
  busy = true;
  const profile = selected;
  updateButtons();
  setStatus(fix ? `${primaryLabel(profile)}…` : `Analyzing with ${profile.name}…`);
  const level = profile.level || '2b';
  const opts = {};
  if (!fix) { opts.check = true; opts.analyze = true; }
  if (profile.ua) opts.ua = true;
  if (profile.json) opts.profile = JSON.stringify(profile.json);
  if (password) opts.password = password;
  const started = performance.now();
  const m = await engine.run(fileBytes.slice(0), level, opts);
  const ms = Math.round(performance.now() - started);
  busy = false;
  if (queued) { queued = false; return run(false); }
  hasResults = true;
  updateButtons();
  const R = $('results');
  R.replaceChildren();
  R.classList.remove('hidden');
  if (!m.ok && m.errorCode === 'PASSWORD_REQUIRED') {
    R.classList.add('hidden');
    encrypted = true;
    verified = false;
    pwError = password ? 'Wrong password' : 'Password required';
    renderLock();
    updateButtons();
    return;
  }
  if (!m.ok) {
    R.appendChild(verdict('err', 'x-circle', `${m.errorCode || 'Error'}: ${m.error || 'processing failed'}`));
    if (m.suggestedLevel) R.appendChild(el('p', 'tool-field-help', `The document would be accepted at level ${m.suggestedLevel}.`));
    setStatus('Rejected.', true);
    return;
  }
  const analysis = m.analysis || [];
  const hits = analysis.filter((a) => a.code === 'PROFILE_HIT');
  const unsupported = analysis.filter((a) => a.code === 'PROFILE_RULE_UNSUPPORTED');
  const list = el('div', 'hits');
  if (fix) {
    const steps = m.issues || [];
    R.appendChild(verdict('ok', 'check-circle', `${profile.name} · ${steps.length} step(s) applied in ${ms} ms`));
    for (const i of steps) hit(list, 'fixed', 'fixed', i.detail);
    R.appendChild(list);
    if (m.pdf) {
      const name = fileName.replace(/\.pdf$/i, '') + (profile.level ? `.${profile.level}.pdf` : '-fixed.pdf');
      download(new Blob([m.pdf], { type: 'application/pdf' }), name);
      setStatus(`Download started: ${name}`);
    }
    return;
  }
  const bad = hits.some((h) => h.detail.startsWith('Error'));
  const warn = hits.some((h) => h.detail.startsWith('Warning'));
  const cls = hits.length === 0 ? 'ok' : bad ? 'err' : warn ? 'warn' : 'ok';
  const name = hits.length === 0 ? 'check-circle' : bad ? 'x-circle' : warn ? 'warning-circle' : 'info';
  R.appendChild(verdict(cls, name, hits.length === 0
    ? `No problems found · ${profile.name} · ${ms} ms`
    : `${hits.length} finding(s) · ${profile.name} · ${ms} ms`));
  for (const h of hits) {
    const sev = h.detail.split(':')[0];
    const known = ['Error', 'Warning', 'Info'].includes(sev);
    hit(list, known ? sev : 'Info', known ? sev.toLowerCase() : 'info', known ? h.detail.slice(sev.length + 2) : h.detail);
  }
  for (const u of unsupported) hit(list, 'note', 'note', u.detail);
  if (profile.level && m.findings !== undefined) {
    hit(list, m.compliant ? 'ok' : 'note', m.compliant ? 'fixed' : 'note',
      m.compliant ? `Already conforms to PDF/${describeLevel(level)}` : `${m.findings} conformance finding(s); "${primaryLabel(profile)}" produces a conforming file`);
    for (const i of m.issues || []) hit(list, 'note', 'note', `${i.code}: ${i.detail}`);
  }
  R.appendChild(list);
  setStatus(hits.length === 0 ? `Clean against ${profile.name}.` : `${hits.length} finding(s) against ${profile.name}.`);
}

$('analyzeBtn').addEventListener('click', () => run(false));
$('fixBtn').addEventListener('click', () => run(true));
$('fileClear').addEventListener('click', clearFile);
$('unlockBtn').addEventListener('click', unlock);
$('pw').addEventListener('keydown', (e) => { if (e.key === 'Enter') unlock(); });
$('lockBadge').addEventListener('click', () => { if (verified) { verified = false; renderLock(); updateButtons(); } });
wireFileInput($('file'), (list) => { if (list[0]) setFile(list[0]); });
wireDropzone($('drop'), $('file'), (list) => { if (list[0]) setFile(list[0]); });
blockWindowDrops();

renderProfiles();
updateButtons();
