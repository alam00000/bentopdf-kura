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

1. `release.yml` builds, on real runners, each archive carrying `LICENSE`,
   `NOTICE.md` and `include/kura/kura.h`:
   - `kura-<tag>-linux-x64.tar.gz`: the self-contained `kura` binary,
     `libkura.a`, `libkura_raster.a` and `libpdfium.a`
   - `kura-<tag>-windows-x64.zip`: `kura.exe` and the matching `.lib` files
   - `kura-<tag>-macos-arm64.tar.gz`: `kura` and the matching `.a` files
   - `kura-<tag>-wasm.tar.gz`: `kura.js` and `kura.wasm`, the WebAssembly engine
   - the docker image, built from `docker/Dockerfile` and pushed to
     `ghcr.io/alam00000/bentopdf-kura` as `<tag>` and `latest`
2. It publishes a GitHub Release with the changelog section for that version
   as its notes and a `SHA256SUMS` file covering every archive. Linux and the
   WebAssembly module are required; the Windows and macOS jobs may fail without
   blocking the release and ship in the next one.
3. `npm-publish.yml` starts when `release.yml` finishes successfully. It takes
   the WebAssembly tarball from the release, assembles `kura-pdf`, checks that
   the package version matches the tag, runs the smoke test, publishes with
   provenance, and waits until the registry serves the new version. A tag whose
   version is already on the registry is skipped, so the workflow can be re-run
   safely; the manual run has a `force` switch for the rare case where that is
   wrong.

   It runs as its own workflow rather than a job inside `release.yml` on
   purpose: npm's trusted publishing validates the OIDC claim against the
   calling workflow, so the publisher configured on npm must be
   `npm-publish.yml`. To republish a tag whose package did not reach npm, run
   the `npm-publish` workflow manually with that tag.

## npm authentication

Publishing uses npm's trusted publishing: no token is stored anywhere. The
package's settings on npmjs.com list a trusted publisher of type GitHub
Actions with owner `alam00000`, repository `bentopdf-kura`, workflow
`npm-publish.yml` and the `npm publish` action allowed; the workflow requests
an OIDC token with `id-token: write` and npm 11.5 or newer exchanges it for a
short-lived publish credential.

npm only offers that setting for a package that already exists, so the very
first version of a new package (`kura-pdf` 1.1.0) is published once from a
maintainer's machine with `npm publish` and the account's second factor; every
version after that comes from CI. If an `NPM_TOKEN` repository secret exists
the workflow uses it instead, which is only meant for that bootstrap and
should be deleted afterwards.

## Verifying a release

```bash
gh release download v1.1.0 --pattern 'SHA256SUMS' --pattern '*linux-x64*'
sha256sum -c SHA256SUMS --ignore-missing      # shasum -a 256 -c on macOS
```

The npm package can be checked the same way: `npm view kura-pdf dist.integrity`
prints the hash the registry serves, and `npm pack kura-pdf@<version>` fetches
the exact tarball for inspection.

Every native binary is built from the pinned PDFium checkout on a GitHub-hosted
runner; nothing on a maintainer's machine goes into a release.
