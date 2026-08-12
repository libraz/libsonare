# Benchmarks

Reproducible performance comparison between **libsonare** and **librosa** on
a synthetic 73-second audio fixture (44100 Hz stereo).

## Layout

```
benchmarks/
├── CMakeLists.txt                    # builds all bench binaries below
├── bench_cpp.cpp                     # main librosa-comparison benchmark (→ sonare_bench)
├── filterbank_bench.cpp              # mel/chroma filter application micro-bench
├── spectrum_power_bench.cpp          # |X|² spectrogram micro-bench
├── transcendental_bench.cpp          # log/exp/pow scalar-vs-vectorised
├── log10_bench.cpp                   # log10 hot-path variants
├── streaming_mel_chroma_bench.cpp    # streaming feature pipeline
├── mastering_isp_bench.cpp           # intersample-peak detector (BUILD_MASTERING)
├── mastering_support_bench.cpp       # shared mastering DSP utilities (BUILD_MASTERING)
├── mastering_stereo_bench.cpp        # stereo width / M-S processors (BUILD_MASTERING)
├── pyproject.toml                    # rye-managed env (librosa 0.11.0 pinned)
├── generate_audio.py                 # synthesises the fixture + its ground truth
├── run_bench.py                      # times librosa + merges C++ results into results.json
├── measure_accuracy.py               # scores the analyzers against the ground truth
├── fixtures/                         # generated audio + ground truth (gitignored)
├── results_cpp.json                  # C++-side measurements (from sonare_bench)
└── results.json                      # merged librosa + libsonare numbers used by the homepage
```

## Methodology

All per-feature numbers are measured **standalone from raw audio** — every call
rebuilds whatever intermediate it needs (STFT, Mel, etc.) from the resampled
samples. Both sides see the same workload:

- **libsonare**: timed with `chrono::steady_clock` inside `sonare_bench` so no
  FFI marshalling is in the measurement.
- **librosa**: timed with `time.perf_counter` around the Python call.

`Full analyze` measures the entire `analyze()` pipeline (BPM, key, beats,
chords, sections, timbre, dynamics, rhythm, melody). The "librosa equivalent"
is the separate librosa-based [bpm-detector](https://github.com/libraz/bpm-detector)
project run with `--comprehensive`, which the script invokes automatically if
`bpm-detector` is on `PATH`.

## Run it

```bash
# from libsonare repo root

# 1. build the C++ bench binary
cmake -B build-bench -DCMAKE_BUILD_TYPE=Release \
                     -DBUILD_BENCH=ON -DBUILD_TESTING=OFF -DBUILD_CLI=OFF
cmake --build build-bench -j

# 2. set up the rye env and generate the test fixture
rye sync --pyproject benchmarks/pyproject.toml
rye run --pyproject benchmarks/pyproject.toml python benchmarks/generate_audio.py

# 3. run the C++ side
./build-bench/bin/sonare_bench \
    benchmarks/fixtures/bench_73s_44100.wav \
    benchmarks/results_cpp.json

# 4. time librosa and write the merged results.json
rye run --pyproject benchmarks/pyproject.toml python benchmarks/run_bench.py
```

`results.json` is the source of truth for the homepage `benchmarks.md`. Re-run
on different hardware and the relative gaps stay stable; absolute times scale
with the machine.

### Do not publish a run from a busy machine

`sonare_bench` records the one-minute load average before and after the run and
warns when the machine was already busy. The threaded paths take every core
they can get, so a contended machine inflates HPSS, pYIN and the full pipeline
several-fold while every other line still looks plausible. The recorded
`load_average_before` / `load_average_after` in `results_cpp.json` is what makes
a published number checkable rather than merely asserted.

## Accuracy

Timings say how fast an answer arrives, not whether it is right. The fixture is
synthesised from an explicit tempo, beat grid, chord progression and key, and
`generate_audio.py` writes that description to
`fixtures/bench_73s_44100.groundtruth.json` — derived from the same constants
that drive the synthesis, so it cannot drift from the audio.

```bash
# needs a CLI build (BUILD_CLI=ON)
rye run --pyproject benchmarks/pyproject.toml python benchmarks/generate_audio.py
python3 benchmarks/measure_accuracy.py --cli build-release/bin/sonare
```

Scored: tempo error (with the MIREX 4% convention), beat F-measure at the
standard ±70 ms tolerance, frame-wise chord agreement on root and on
root+quality, and key with the relative major/minor counted separately.

This is a floor, not a benchmark. Synthetic audio has no performance timing,
no timbral ambiguity and no production; a good score means the analyzers
recover a signal built to be recoverable. Accuracy on real recordings needs an
annotated corpus this repository does not ship.

## WebAssembly

The same benchmark builds for WASM, so the browser cost of the threaded paths
is a measured number rather than a caveat:

```bash
# from libsonare repo root — the build directory must stay outside
# bindings/wasm, whose package.json sets "type": "module". Emscripten emits
# CommonJS, so a bench built under that package is parsed as ESM and dies on
# its first require().
emcmake cmake -S . -B build-wasm-bench -DBUILD_WASM=ON -DBUILD_BENCH=ON \
              -DBUILD_TESTING=OFF -DBUILD_CLI=OFF -DCMAKE_BUILD_TYPE=Release
cmake --build build-wasm-bench --target sonare_bench
node build-wasm-bench/bin/sonare_bench.js benchmarks/fixtures/bench_73s_44100.wav
```

Path-based decoding is compiled out of the WASM build, so that target reads the
fixture into memory and decodes from there. The ingest is outside every timed
region and both builds measure the same samples.

The WASM build reports one hardware thread and no load average, so it prints no
busy-machine warning. Check the load yourself before trusting a WASM run.

## Micro-benchmarks

The additional `*_bench.cpp` binaries are standalone micro-benchmarks for
hot-path DSP work (no librosa comparison, no `results.json` integration). They
print timings to stdout. Build with `BUILD_BENCH=ON`; mastering benches also
require `BUILD_MASTERING=ON`. Run individually, e.g.:

```bash
./build-bench/bin/sonare_filterbank_bench
./build-bench/bin/sonare_spectrum_power_bench
./build-bench/bin/sonare_mastering_isp_bench   # needs -DBUILD_MASTERING=ON
./build-bench/bin/sonare_eq_bench              # needs -DBUILD_MASTERING=ON
```

The mastering ISP bench is also runnable in WASM via
`cd bindings/wasm && yarn bench:wasm:isp` (builds and executes under Node).

## Optional: bpm-detector full-pipeline comparison

The script auto-detects `bpm-detector` on `PATH` and times it on the same
fixture. To enable:

```bash
rye run --pyproject benchmarks/pyproject.toml python -m ensurepip --upgrade
rye run --pyproject benchmarks/pyproject.toml python -m pip install \
    --no-deps -e ../bpm-detector
rye run --pyproject benchmarks/pyproject.toml python -m pip install \
    psutil tqdm colorama scikit-learn matplotlib seaborn pandas resampy
```

(adjust the path to where you have bpm-detector checked out)

## Notes

- Hardware-dependent: M5 Max numbers are the published reference.
- The synthetic fixture is deterministic but minimal — real music will exercise
  more code paths (e.g. richer chord detection); absolute timings drift a little
  but the relative gaps are robust.
- WASM is single-threaded, so the HPSS / pYIN speedups shrink there. Build the
  WASM bench above rather than guessing at how much.
