### Description

Please include a summary of the change and which issue is fixed, with the
motivation and context.

Fixes # (issue)

### Type of change

Please delete options that are not relevant.

- [ ] Bug fix (non-breaking change which fixes an issue)
- [ ] New feature (non-breaking change which adds functionality)
- [ ] Breaking change (fix or feature that would cause existing behaviour to change)
- [ ] This change requires a documentation update

### How has this been tested?

- [ ] `make check` passes
- [ ] For a bug fix: the reproducing PDF is attached to the linked issue
- [ ] For a parser or decoder change: the matching fuzz harness ran for a while without findings
- [ ] For an engine change: the WebAssembly module was rebuilt (`make wasm`) so the demo and npm package match

### Checklist

- [ ] I have signed the CLA (the bot will prompt on your first PR)
- [ ] The C ABI (`pdfa-engine/core/include/kura/kura.h`) is unchanged, or the change is additive
- [ ] No comments were added to code; the reasoning is in the commit message
- [ ] Behaviour changes are described in `CHANGELOG.md`
