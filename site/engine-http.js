const BOOL_KEYS = ['ua', 'allowVisualRisk', 'rasterizePages', 'outlineFonts', 'analyze', 'embedSource'];
const TEXT_KEYS = ['lang', 'rasterDpi', 'imageMaxPpi', 'embedSourceName', 'attachXmlName', 'facturxProfile',
  'outputCondition', 'outputConditionInfo', 'registry', 'vtRecords'];

function fromBase64(b64) {
  const bin = atob(b64);
  const out = new Uint8Array(bin.length);
  for (let i = 0; i < bin.length; i++) out[i] = bin.charCodeAt(i);
  return out;
}

export function createEngine(onState) {
  fetch('/healthz')
    .then((r) => r.json())
    .then((h) => onState(h.ok ? { ready: true, version: h.version || '' } : { ready: false, error: 'service unavailable' }))
    .catch((e) => onState({ ready: false, error: e.message || 'service unavailable' }));
  return {
    async verifyPassword(bytes, password) {
      try {
        const headers = password ? { 'X-Password': password } : {};
        const res = await fetch('/api/verify-password', { method: 'POST', headers, body: bytes });
        if (!res.ok) return false;
        return (await res.json()).valid === true;
      } catch {
        return false;
      }
    },
    async run(bytes, level, opts) {
      const o = opts || {};
      const q = new URLSearchParams({ level });
      for (const k of BOOL_KEYS) if (o[k]) q.set(k, 'true');
      for (const k of TEXT_KEYS) if (o[k] !== undefined && o[k] !== null && o[k] !== '') q.set(k, String(o[k]));
      const fd = new FormData();
      fd.append('file', new Blob([bytes]), 'input');
      if (o.profile) fd.append('profile', o.profile);
      if (o.attachXml) fd.append('xml', new Blob([o.attachXml]), 'invoice.xml');
      const headers = {};
      if (o.password) headers['X-Password'] = o.password;
      const check = !!o.check;
      const url = check ? `/api/check?${q}` : `/api/convert?report=json&${q}`;
      let res;
      try {
        res = await fetch(url, { method: 'POST', headers, body: fd });
      } catch (e) {
        return { ok: false, errorCode: 'NETWORK', error: e.message || 'request failed', issues: [] };
      }
      let m;
      try {
        m = await res.json();
      } catch {
        return { ok: false, errorCode: `HTTP_${res.status}`, error: res.statusText || 'unexpected response', issues: [] };
      }
      if (!m.ok) return { ...m, issues: m.issues || [], analysis: m.analysis || [] };
      if (check) return { ...m, mode: 'check', issues: m.issues || [], analysis: m.analysis || [] };
      return { ...m, issues: m.issues || [], analysis: m.analysis || [], pdf: fromBase64(m.pdf) };
    },
  };
}
