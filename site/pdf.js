export function looksEncrypted(bytes) {
  const v = new Uint8Array(bytes);
  const pat = [0x2f, 0x45, 0x6e, 0x63, 0x72, 0x79, 0x70, 0x74];
  outer: for (let i = 0; i + pat.length <= v.length; i++) {
    for (let k = 0; k < pat.length; k++) if (v[i + k] !== pat[k]) continue outer;
    const next = v[i + pat.length];
    const letter = next !== undefined && ((next >= 0x41 && next <= 0x5a) || (next >= 0x61 && next <= 0x7a));
    if (!letter) return true;
  }
  return false;
}
