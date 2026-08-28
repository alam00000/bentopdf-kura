#!/usr/bin/env node
import { spawnSync } from 'node:child_process';
import { binaryPath } from '../index.js';

const bin = binaryPath();
if (!bin) {
  console.error(`kura-pdf: no engine binary for ${process.platform}-${process.arch}; use a platform with a native package (darwin-arm64, linux-x64, win32-x64), set KURA_BIN to a binary you built, or install kura-pdf-wasm for the kura-wasm command`);
  process.exit(64);
}
const r = spawnSync(bin, process.argv.slice(2), { stdio: 'inherit' });
if (r.error) {
  console.error(`kura-pdf: cannot run ${bin}: ${r.error.message}`);
  process.exit(64);
}
process.exit(r.status ?? 1);
