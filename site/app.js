import { icon, mountIcons } from './icons/phosphor.js';
import { createEngine } from './engine.js';
import { looksEncrypted } from './pdf.js';
import { $, el, withIcon, fmtBytes, setStatus, describeLevel, wireDropzone, wireFileInput, blockWindowDrops, download } from './ui.js';

mountIcons();

let ready = false;
let running = false;
let invoiceXml = null;
let nextId = 0;
const files = [];

const engine = createEngine((m) => {
  ready = m.ready;
  if (ready) {
    $('ver').textContent = m.version;
    setStatus('');
  } else {
    setStatus(`Engine failed to load: ${m.error}`, true);
  }
  refresh();
});

async function addFiles(list) {
  for (const f of list) {
    if (!/\.(pdf|jpe?g)$/i.test(f.name)) continue;
    const bytes = await f.arrayBuffer();
    files.push({
      id: ++nextId, file: f, bytes, status: 'pending',
      encrypted: looksEncrypted(bytes), password: '', verified: false, pwError: '',
    });
  }
  renderAll();
  refresh();
}

async function unlock(it, input, button) {
  it.password = input.value;
  if (!it.password) return;
  button.disabled = true;
  button.lastChild.textContent = 'Checking…';
  it.verified = await engine.verifyPassword(it.bytes.slice(0), it.password);
  it.pwError = it.verified ? '' : 'Wrong password';
  renderAll();
}

function unlockRow(it) {
  const wrap = el('div', 'tool-file-pw');
  const input = el('input');
  input.type = 'password';
  input.placeholder = 'password';
  input.autocomplete = 'off';
  input.value = it.password || '';
  input.addEventListener('input', () => { it.password = input.value; });
  const button = withIcon('button', 'btn-ghost', 'lock-key', 'Unlock');
  button.type = 'button';
  button.onclick = () => unlock(it, input, button);
  input.addEventListener('keydown', (e) => { if (e.key === 'Enter') unlock(it, input, button); });
  wrap.appendChild(input);
  wrap.appendChild(button);
  if (it.pwError) wrap.appendChild(el('span', 'pw-err', it.pwError));
  return wrap;
}

function renderAll() {
  const ul = $('fileList');
  ul.replaceChildren();
  files.forEach((it, i) => ul.appendChild(renderRow(it, i)));
  const any = files.length > 0;
  ul.classList.toggle('hidden', !any);
  $('drop').classList.toggle('hidden', any);
  $('addStrip').classList.toggle('hidden', !any);
}

function renderRow(it, i) {
  const row = el('li', 'tool-file-row');
  row.appendChild(el('span', 'tool-file-num', String(i + 1)));
  const meta = el('div', 'tool-file-meta');
  meta.appendChild(el('div', 'tool-file-name', it.file.name));
  const sub = el('div', 'tool-file-sub');
  sub.appendChild(el('span', null, fmtBytes(it.file.size)));
  if (it.outSize) {
    sub.appendChild(el('span', 'dot', '·'));
    sub.appendChild(el('span', null, `${fmtBytes(it.outSize)} as PDF/${describeLevel(it.level)}${it.ua ? ' + UA' : ''}`));
  }
  if (it.ms) { sub.appendChild(el('span', 'dot', '·')); sub.appendChild(el('span', null, `${it.ms} ms`)); }
  if (it.encrypted) {
    sub.appendChild(el('span', 'dot', '·'));
    const badge = withIcon('span', it.verified ? 'lock-badge unlocked' : 'lock-badge', it.verified ? 'lock-open' : 'lock', it.verified ? 'Unlocked' : 'Locked');
    if (it.verified) {
      badge.title = 'Change password';
      badge.onclick = () => { it.verified = false; renderAll(); };
    }
    sub.appendChild(badge);
  }
  meta.appendChild(sub);
  const status = el('div', 'tool-file-status');
  const detailsToggle = (label) => {
    const toggle = el('button', 'link-btn');
    toggle.appendChild(el('span', null, label));
    toggle.appendChild(icon(it.open ? 'caret-up' : 'caret-down'));
    toggle.onclick = () => { it.open = !it.open; renderAll(); };
    return toggle;
  };
  if (it.status === 'working') {
    status.appendChild(withIcon('span', 'busy', 'hourglass-medium', 'Converting…'));
  } else if (it.status === 'done') {
    status.appendChild(withIcon('span', 'ok', 'check-circle', `${it.issues.length} change(s)`));
    status.appendChild(detailsToggle(it.open ? 'hide details' : 'show details'));
  } else if (it.status === 'checked') {
    status.appendChild(withIcon('span', it.compliant ? 'ok' : 'warn', it.compliant ? 'check-circle' : 'warning-circle',
      it.compliant ? `already conforms to PDF/${describeLevel(it.level)}` : `${it.findings} finding(s)`));
    if (!it.compliant) status.appendChild(detailsToggle(it.open ? 'hide' : 'show'));
  } else if (it.status === 'error' && it.errorCode === 'PASSWORD_REQUIRED' && it.encrypted && !it.verified) {
    status.appendChild(withIcon('span', 'warn', 'lock', 'Needs its password'));
  } else if (it.status === 'error') {
    status.appendChild(withIcon('span', 'err', 'x-circle', `${it.errorCode}: ${it.error}`));
    if (it.suggest) {
      const b = el('button', 'link-btn', `try PDF/${describeLevel(it.suggest)}`);
      b.onclick = () => convertOne(it, it.suggest);
      status.appendChild(b);
    }
  }
  meta.appendChild(status);
  if (it.encrypted && !it.verified && !running) meta.appendChild(unlockRow(it));
  let list = null;
  if (it.open && it.issues && it.issues.length) {
    list = el('ul', 'issues');
    for (const is of it.issues) {
      const li = el('li');
      li.appendChild(el('code', null, is.code));
      li.appendChild(el('span', null, is.detail));
      list.appendChild(li);
    }
  }
  row.appendChild(meta);
  const actions = el('div', 'tool-file-actions');
  if (it.status === 'done') {
    const dl = withIcon('button', 'btn', 'download-simple', 'Download');
    dl.onclick = () => download(it.blob, it.outName);
    actions.appendChild(dl);
  } else if (it.status === 'checked' && !it.compliant) {
    const c = withIcon('button', 'btn-ghost', 'arrow-right', 'Convert');
    c.onclick = () => { $('checkOnly').checked = false; syncSwitches(); convertOne(it); };
    actions.appendChild(c);
  }
  const x = el('button', 'icon-x');
  x.appendChild(icon('x'));
  x.title = 'Remove';
  x.onclick = () => { files.splice(files.indexOf(it), 1); renderAll(); refresh(); };
  actions.appendChild(x);
  row.appendChild(actions);
  if (list) row.appendChild(list);
  return row;
}

function currentOptions(bytes, name, it) {
  const opts = {};
  const pw = it.password || $('password').value;
  if (pw) opts.password = pw;
  if ($('ua').checked && !$('ua').disabled) opts.ua = true;
  if ($('visualRisk').checked) opts.allowVisualRisk = true;
  if ($('checkOnly').checked) opts.check = true;
  if ($('rasterize').checked) opts.rasterizePages = true;
  opts.rasterDpi = parseInt($('dpi').value, 10);
  if ($('embedSource').checked && !$('embedSource').disabled) {
    opts.embedSource = new Uint8Array(bytes.slice(0));
    opts.embedSourceName = name;
    opts.embedSourceMime = /\.jpe?g$/i.test(name) ? 'image/jpeg' : 'application/pdf';
  }
  if (invoiceXml) opts.attachXml = invoiceXml;
  return opts;
}

async function convertOne(it, levelOverride) {
  const level = levelOverride || $('level').value;
  const ua = $('ua').checked && !$('ua').disabled;
  it.status = 'working'; it.suggest = null; it.open = false;
  renderAll();
  const bytes = it.bytes.slice(0);
  const opts = currentOptions(bytes, it.file.name, it);
  const checkOnly = !!opts.check;
  const started = performance.now();
  const r = await engine.run(bytes, level, opts);
  it.ms = Math.round(performance.now() - started);
  it.level = level; it.ua = ua;
  if (r.ok && checkOnly) {
    it.status = 'checked';
    it.compliant = r.compliant;
    it.findings = r.findings;
    it.issues = r.issues;
  } else if (r.ok) {
    it.status = 'done';
    it.issues = r.issues;
    it.blob = new Blob([r.pdf], { type: 'application/pdf' });
    it.outSize = it.blob.size;
    it.outName = it.file.name.replace(/\.(pdf|jpe?g)$/i, '') + `.${level}${ua ? '-ua' : ''}.pdf`;
  } else {
    it.status = 'error';
    it.errorCode = r.errorCode;
    it.error = r.error;
    it.suggest = r.suggestedLevel;
    it.issues = r.issues || [];
    if (r.errorCode === 'PASSWORD_REQUIRED') {
      it.encrypted = true;
      it.verified = false;
      it.pwError = opts.password ? 'Wrong password' : 'Password required';
    }
  }
  renderAll();
}

async function runAll() {
  if (running || !ready) return;
  running = true;
  refresh();
  setStatus('');
  for (const it of files) {
    if (it.status !== 'done') await convertOne(it);
  }
  running = false;
  renderAll();
  refresh();
  const done = files.filter((f) => f.status === 'done');
  if (done.length > 1) {
    const all = withIcon('button', 'btn-ghost', 'download-simple', `Download all (${done.length})`);
    all.onclick = () => done.forEach((it, k) => setTimeout(() => download(it.blob, it.outName), k * 350));
    $('status').replaceChildren(all);
  }
}

function refresh() {
  $('run').disabled = !ready || running || files.length === 0;
  $('clearBtn').disabled = running || files.length === 0;
  $('run').textContent = $('checkOnly').checked ? 'Check' : 'Convert';
  if (ready && !running) {
    const done = files.filter((f) => f.status === 'done').length;
    const err = files.filter((f) => f.status === 'error').length;
    if (files.length && (done || err)) setStatus(`${done} of ${files.length} converted${err ? `, ${err} rejected` : ''}`);
  }
}

function syncSwitches() {
  const v = $('level').value;
  const isA = /^[1-4]/.test(v);
  $('ua').disabled = !isA;
  if (!isA) $('ua').checked = false;
  $('uaLabel').textContent = isA ? (/^4/.test(v) ? 'Accessibility (PDF/UA-2)' : 'Accessibility (PDF/UA-1)') : 'Accessibility (PDF/A only)';
  const canAttach = /^(3|4f|4e|e1)/.test(v);
  $('embedSource').disabled = !canAttach;
  if (!canAttach) $('embedSource').checked = false;
  for (const s of document.querySelectorAll('.switch')) {
    const input = s.querySelector('input');
    s.classList.toggle('off', !input.checked);
  }
  refresh();
}

$('level').addEventListener('change', syncSwitches);
for (const s of document.querySelectorAll('.switch input')) s.addEventListener('change', syncSwitches);
syncSwitches();

$('invxml').addEventListener('change', async (e) => {
  const f = e.target.files && e.target.files[0];
  if (!f) return;
  invoiceXml = new Uint8Array(await f.arrayBuffer());
  $('invName').textContent = f.name;
  $('invClear').classList.remove('hidden');
  if (!/^(3b|3u|3a|4f)$/.test($('level').value)) { $('level').value = '3b'; syncSwitches(); }
});
$('invClear').addEventListener('click', () => {
  invoiceXml = null;
  $('invxml').value = '';
  $('invName').textContent = 'none';
  $('invClear').classList.add('hidden');
});

$('run').addEventListener('click', runAll);
$('clearBtn').addEventListener('click', () => { files.length = 0; renderAll(); setStatus(''); refresh(); });
$('addBtn').addEventListener('click', () => $('file').click());
wireFileInput($('file'), addFiles);
wireDropzone($('drop'), $('file'), addFiles);
wireDropzone($('addStrip'), $('file'), addFiles);
blockWindowDrops();
