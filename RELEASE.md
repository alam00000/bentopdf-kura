# Releasing

One command:

```bash
scripts/release.sh patch     # or minor, major, or an explicit X.Y.Z
```

The script refuses to run unless you are on a clean, up-to-date `main`. It
bumps the version in the three places that must agree: `KURA_VERSION` in
`pdfa-engine/core/include/kura/kura.h`, `kEngineVersion` in
`pdfa-engine/core/include/pdfa/pdfa.hh`, and every `package.json`. It then runs
`make check`, commits `chore: release vX.Y.Z`, tags, and pushes.
`DRY_RUN=1 scripts/release.sh patch` shows what would happen without changing
anything.

CI does the rest, in order:

1. `release.yml` builds, on real runners:
   - `kura-<tag>-linux-x64.tar.gz`: the self-contained `kura` binary and
     `libkura.a` with PDFium linked in
   - `kura-<tag>-windows-x64.zip`: `kura.exe` and `kura.lib`
   - `kura-<tag>-macos-arm64.tar.gz`: `kura` and `libkura.a`
   - `kura-<tag>-wasm.tar.gz`: `kura.js` and `kura.wasm`, the WebAssembly engine
   - `kura-<tag>-headers.tar.gz`: `kura.h`
   - the docker image, pushed to `ghcr.io/alam00000/bentopdf-kura` as `<tag>`
     and `latest`
2. It publishes a GitHub Release with a `SHA256SUMS` file covering every
   artifact. Linux and the WebAssembly module are required; the Windows and
   macOS jobs may fail without blocking the release and ship in the next one.
3. `npm-publish.yml` starts when `release.yml` finishes successfully. It takes
   the WebAssembly tarball from the release, assembles `kura-pdf`, checks that
   the package version matches the tag, and publishes with provenance.

   It runs as its own workflow rather than a job inside `release.yml` on
   purpose: npm's trusted publishing validates the OIDC claim against the
   calling workflow, so the publisher configured on npm must be
   `npm-publish.yml`. To republish a tag whose package did not reach npm, run
   the `npm-publish` workflow manually with that tag.

## Verifying a release

```bash
gh release download v1.2.0 --pattern 'SHA256SUMS' --pattern '*linux-x64*'
sha256sum -c SHA256SUMS --ignore-missing
```

Every native binary is built from the pinned PDFium checkout on a GitHub-hosted
runner; nothing on a maintainer's machine goes into a release.
