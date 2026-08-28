# Fuzzing

Kura parses untrusted input, so it is fuzzed continuously. Five harnesses under `pdfa-engine/fuzz/` each drive one untrusted input:

| Harness | Input |
|---|---|
| `fuzz_convert` | the PDF byte stream, across every conformance level |
| `fuzz_profile` | preflight profiles, both the JSON and XML dialects |
| `fuzz_invoice` | e-invoice XML attached at PDF/A-3 |
| `fuzz_icc` | a caller-supplied destination ICC profile |
| `fuzz_password` | the open password plus the encrypted document |

`pdfa-engine/fuzz/gen_seeds.py` generates the starting inputs, and dictionaries under `.clusterfuzzlite/` give the mutator the PDF, profile and invoice keywords up front.

## Where it runs

- **Every push**, in `ci.yml`: every seed is replayed through every harness under AddressSanitizer and UndefinedBehaviorSanitizer, and `fuzz_convert` runs coverage-guided for sixty seconds.
- **ClusterFuzzLite**, in the repository's own GitHub Actions: pull requests are fuzzed for ten minutes per sanitizer on the code they touch, a batch runs nightly for an hour, and a weekly job reports which lines the corpus reaches. The build definition and dictionaries are under `.clusterfuzzlite/`, and the corpus it grows is kept in the repository's own storage, so nothing depends on an outside service.

A crash found by either is reported with the reproducing input; the fix adds that input to the seeds so it stays fixed.

## Running locally

Without a fuzzing engine, the replay binaries run any file through a harness under the sanitizers:

```bash
cmake -S pdfa-engine -B build-fuzz -DPDFA_BUILD_FUZZ=ON -DPDFA_WITH_PDFIUM=OFF \
  -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_CXX_FLAGS="-fsanitize=address,undefined -g -O1" \
  -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=address,undefined"
cmake --build build-fuzz --target fuzz_replay_convert
python3 pdfa-engine/fuzz/gen_seeds.py seeds
./build-fuzz/fuzz/fuzz_replay_convert seeds/*.pdf
```

With clang's libFuzzer, `make fuzz` builds `fuzz_convert` and runs it for a minute. For a longer run:

```bash
pdfa-engine/build-fuzz/fuzz/fuzz_convert seeds -dict=.clusterfuzzlite/fuzz_convert.dict -max_total_time=3600
```

## Testing the ClusterFuzzLite build

The exact container build that CI runs can be reproduced with Google's helper script, which builds every dependency from source inside the same base image:

```bash
git clone --depth 1 https://github.com/google/oss-fuzz.git fuzz-helper
cd fuzz-helper
python3 infra/helper.py build_image --external <path-to-this-repo>
python3 infra/helper.py build_fuzzers --external --sanitizer address <path-to-this-repo>
python3 infra/helper.py check_build --external <path-to-this-repo>
python3 infra/helper.py run_fuzzer --external <path-to-this-repo> fuzz_convert -- -max_total_time=60
```

On Apple silicon the base image is x86_64 only, so export `DOCKER_DEFAULT_PLATFORM=linux/amd64` first and expect emulation to be slow. `build.sh` caches each dependency under `$WORK/deps-<sanitizer>`, so repeated local builds skip straight to the engine.

`address` and `undefined` are enabled; MemorySanitizer is deliberately off until the first two run clean for a while, because it reports uninitialised reads only when every dependency is instrumented and a single uninstrumented library floods it with false positives.
