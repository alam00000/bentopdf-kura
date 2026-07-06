import createModule from './pdfa.js';

let modPromise = null;

function engine() {
  if (!modPromise) modPromise = createModule();
  return modPromise;
}

self.onmessage = async (e) => {
  const { id, bytes, level, opts } = e.data;
  try {
    const mod = await engine();
    const t0 = performance.now();
    const r = mod.convert(new Uint8Array(bytes), level, opts || {});
    const ms = Math.round(performance.now() - t0);
    if (r.ok) {
      const pdf = r.pdf.slice().buffer;
      self.postMessage(
        { id, ok: true, pdf, ms, issues: r.issues.length, engine: r.engine },
        [pdf]
      );
    } else {
      self.postMessage({
        id, ok: false, ms,
        errorCode: r.errorCode || 'ERROR',
        error: r.error || 'conversion failed',
        suggestedLevel: r.suggestedLevel || null,
      });
    }
  } catch (err) {
    self.postMessage({ id, ok: false, errorCode: 'ENGINE', error: String(err) });
  }
};
