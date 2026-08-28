# Security

Kura parses untrusted input: PDF files, embedded fonts, ICC profiles, JPEG and JPEG 2000 streams, preflight profiles and e-invoice XML. Parsing bugs in this class of software are security bugs, and the project treats them that way.

## What the engine does about it

- Every recursion into document structure is depth-bounded and cycle-checked, and every decoded image is capped in pixels and components. The caps are named in one header, `pdfa-engine/core/src/limits.hh`.
- The C API never lets an exception cross the boundary.
- The CLI runs under a wall-clock watchdog, `PDFA_TIMEOUT`, default 120 seconds per document, and exits with status 3 when it fires.
- Every push builds and tests the engine under AddressSanitizer and UndefinedBehaviorSanitizer, and five libFuzzer harnesses cover the PDF input, preflight profiles, invoice XML, ICC profiles and passwords. Continuous fuzzing runs through ClusterFuzzLite in the repository's own CI; see [Fuzzing](/fuzzing).
- The WebAssembly build runs inside the browser's sandbox; a document never leaves the machine it was opened on.

## Running it on untrusted input

Keep the watchdog on. Put the process under a memory limit; the engine bounds its own work, but the operating system is the right place to bound the process. For a server, run the native binary in a subprocess rather than the npm module in-process, so a crash on hostile input takes down the worker and not your service.

## Reporting a vulnerability

Report privately to [contact@bentopdf.com](mailto:contact@bentopdf.com), or through GitHub's private vulnerability reporting on the repository's Security tab. Please do not open a public issue. The full policy, including scope and disclosure timelines, is in [SECURITY.md](https://github.com/alam00000/bentopdf-kura/blob/main/SECURITY.md).
