# Security

Kura parses untrusted input: PDF files, embedded fonts, ICC profiles, JPEG and
JPEG 2000 streams, preflight profiles and e-invoice XML. Parsing bugs in this
class of software are security bugs, and we treat them that way.

## What the engine does about it

- Every recursion into document structure is depth-bounded and cycle-checked,
  and every decoded image is capped in pixels and components. The caps are
  named in one place, `pdfa-engine/core/src/limits.hh`.
- The C API never lets an exception cross the boundary; every failure comes back
  as an error code on the result.
- The CLI runs under a wall-clock watchdog (`PDFA_TIMEOUT`, default 120 seconds
  per document) and exits with status 3 when it fires, so a pathological file
  cannot hold a batch job forever.
- The engine is built and tested under AddressSanitizer and
  UndefinedBehaviorSanitizer on every push, and five libFuzzer harnesses cover
  the PDF input, preflight profiles, invoice XML, ICC profiles and passwords.
  Continuous fuzzing runs through ClusterFuzzLite in this repository's own
  CI; the build definition and dictionaries live under `.clusterfuzzlite/`.
- The WebAssembly build runs inside the browser's sandbox; a document never
  leaves the machine it was opened on.

If you run Kura on untrusted input in production, keep the watchdog on and add
a memory limit at the process level; the engine bounds its own work, but the
operating system is the right place to bound the process.

## Reporting a vulnerability

Report vulnerabilities privately to contact@bentopdf.com, or through GitHub's
private vulnerability reporting: the Security tab of this repository, then
"Report a vulnerability". Please do not open a public issue.

Include, if you can:

- the input file that triggers it, or a generator for it
- the command line or API call used
- what happens: crash, hang, memory error, wrong output, information disclosure
- the version or commit you tested

You should hear back within a few days. If a fix is warranted we will agree a
disclosure date with you; the default is 90 days from the report or the release
of a fix, whichever comes first.

## Scope

In scope:

- memory-safety errors reachable from a document, font, ICC profile, preflight
  profile or invoice payload
- unbounded CPU or memory consumption from a bounded input
- output that discloses process memory
- conversions that silently destroy or alter document content
- checks that report a non-conforming file as conforming

Out of scope:

- crashes only reachable by an API caller who violates the documented contract,
  such as passing a null buffer with a non-zero length
- resource use proportional to a genuinely large input
- issues in third-party libraries, unless Kura's use of them is what makes the
  issue reachable; please report those upstream as well

## Supported versions

Fixes land on the default branch and in the next release. Older releases are not
patched.
