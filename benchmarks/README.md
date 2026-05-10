# Benchmarks

Reproducible performance comparison between **libsonare** and **librosa** on
a synthetic 73-second audio fixture (44100 Hz stereo).

## Layout

```
benchmarks/
├── CMakeLists.txt      # builds sonare_bench (C++ chrono timing)
├── bench_cpp.cpp       # C++ benchmark binary
├── pyproject.toml      # rye-managed env (librosa 0.11.0 pinned)
├── generate_audio.py   # synthesises fixtures/bench_73s_44100.wav
├── run_bench.py        # times librosa + merges C++ results into results.json
├── fixtures/           # generated audio (gitignored)
├── results_cpp.json    # C++-side measurements (from sonare_bench)
└── results.json        # merged librosa + libsonare numbers used by the homepage
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
- WASM is single-threaded, so the HPSS / pYIN speedups shrink there. The
  full-pipeline win persists.
