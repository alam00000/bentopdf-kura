import createModule from './kura.js';

let modPromise = null;

function engine() {
  if (!modPromise) modPromise = createModule();
  return modPromise;
}

function plainIssues(list) {
  return (list || []).map((i) => ({ code: i.code, detail: i.detail, fixed: !!i.fixed }));
}

function plainAnalysis(list) {
  return (list || []).map((a) => ({ code: a.code, detail: a.detail }));
}

self.onmessage = async (e) => {
  const { id, bytes, level, opts, verify, password } = e.data;
  if (verify) {
    try {
      const mod = await engine();
      self.postMessage({ id, ok: true, valid: mod.verifyPassword(new Uint8Array(bytes), password || '') });
    } catch (err) {
      self.postMessage({ id, ok: false, valid: false, error: String(err && err.message || err) });
    }
    return;
  }
  try {
    const mod = await engine();
    const r = mod.convert(new Uint8Array(bytes), level, opts || {});
    const base = {
      id,
      ok: r.ok,
      level: r.level,
      engine: r.engine,
      issues: plainIssues(r.issues),
      analysis: plainAnalysis(r.analysis),
    };
    if (!r.ok) {
      self.postMessage({
        ...base,
        errorCode: r.errorCode || 'ERROR',
        error: r.error || 'conversion failed',
        suggestedLevel: r.suggestedLevel || null,
      });
    } else if (r.mode === 'check') {
      self.postMessage({ ...base, mode: 'check', compliant: r.compliant, findings: r.findings });
    } else {
      const pdf = r.pdf.buffer.slice(r.pdf.byteOffset, r.pdf.byteOffset + r.pdf.byteLength);
      self.postMessage({ ...base, pdf }, [pdf]);
    }
  } catch (err) {
    self.postMessage({ id, ok: false, errorCode: 'ENGINE', error: String(err && err.message || err) });
  }
};

engine().then(
  (mod) => self.postMessage({ ready: true, version: mod.version() }),
  (err) => self.postMessage({ ready: false, error: String(err && err.message || err) }),
);
