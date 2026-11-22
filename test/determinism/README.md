# Cross-SIMD determinism acceptance test

## What it checks

`check_determinism.py` encodes a fixed stereo sample matrix with two
**independently configured** encoder builds and enforces:

1. **Bit-exact output (pinned SHA-256).** Every produced AAC stream has a
   SHA-256 recorded in `manifest.json`; the two builds must also emit
   byte-identical files. A pinned hash catches any silent algorithm change.
2. **Warning-free decode and L/R energy side preservation.** Every stream is
   decoded by `aac-dec` and the run must be completely silent (zero status,
   nothing on stdout/stderr). The decoded left/right energy ratio is pinned in
   the manifest and must stay within `RATIO_TOL` (0.10) and on the same side
   (left-louder / right-louder / balanced) as the input — this is the check
   that an MS/IS decision can never silently swap the channels.

The encoded decisions themselves use the fixed tie-breaking rules documented
in `cpp/documentation/numerical-determinism.md`.

## Inputs

The WAV matrix is generated deterministically by
`artifacts/tools/gen_samples.py` (seeded RNG, 16-bit PCM, 44.1 kHz stereo) and
checked in under `artifacts/samples/`. Regenerate with:

```
python3 -P artifacts/tools/gen_samples.py
```

## Running

Build two trees with different SIMD/optimization configurations, for example
an aggressive native build and a generic-C debug build:

```
cmake -B build-primary -DBUILD_PROGRAMS=ON \
      -DCMAKE_C_FLAGS="-O3 -march=native" -DCMAKE_CXX_FLAGS="-O3 -march=native"
cmake -B build-generic -DBUILD_PROGRAMS=ON -DCMAKE_BUILD_TYPE=Debug \
      -DCMAKE_C_FLAGS="-O0" -DCMAKE_CXX_FLAGS="-O0"
cmake --build build-primary
cmake --build build-generic

python3 test/determinism/check_determinism.py \
        --build-a build-primary --build-b build-generic
```

With CMake the same test is registered as CTest:

```
cmake -B build-ctest -DBUILD_PROGRAMS=ON \
      -DFDK_TEST_BUILD_A=$PWD/build-primary \
      -DFDK_TEST_BUILD_B=$PWD/build-generic
cmake --build build-ctest
ctest --test-dir build-ctest --output-on-failure
```

Python needs NumPy and SciPy (`scipy.io.wavfile`).

## Updating the manifest

`--update` rewrites `manifest.json` from the current code. Do this **only**
for a deliberately reviewed algorithm change; a drift without an `--update`
commit is a test failure. Review the diff of the pinned values and confirm the
L/R energy sides did not move.
