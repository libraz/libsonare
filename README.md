# libsonare

[![CI](https://img.shields.io/github/actions/workflow/status/libraz/libsonare/ci.yml?branch=main&label=CI)](https://github.com/libraz/libsonare/actions)
[![npm](https://img.shields.io/npm/v/@libraz/libsonare)](https://www.npmjs.com/package/@libraz/libsonare)
[![PyPI](https://img.shields.io/pypi/v/libsonare)](https://pypi.org/project/libsonare/)
[![codecov](https://codecov.io/gh/libraz/libsonare/branch/main/graph/badge.svg)](https://codecov.io/gh/libraz/libsonare)
[![License](https://img.shields.io/badge/license-Apache--2.0-blue)](https://github.com/libraz/libsonare/blob/main/LICENSE)
[![C++17](https://img.shields.io/badge/C%2B%2B-17-blue?logo=c%2B%2B)](https://en.cppreference.com/w/cpp/17)
[![Platform](https://img.shields.io/badge/platform-Linux%20%7C%20macOS%20%7C%20WebAssembly-lightgrey)](https://github.com/libraz/libsonare)

**Audio analysis + commercial-grade mastering DSP for C++, Python, and browsers.**
Apache-2.0, dependency-free, runs anywhere — including the browser via WebAssembly.

- **Analysis** — BPM, key, chord, beat, section, timbre, dynamics, pitch
  (librosa-compatible defaults; tens of times faster than librosa/Python).
- **Mastering** — EQ, dynamics, multiband, stereo, saturation, repair, maximizer,
  reference matching. 90+ DSP modules built around published standards
  (ITU-R BS.1770-4 loudness/true-peak, Vicanek biquads, ADAA nonlinearities,
  Lemire sliding max, polyphase FIR oversampling).
- **One permissive license** — Apache-2.0 across the entire stack. No LGPL/GPL
  surface, no proprietary algorithms, no SaaS dependencies.

## Installation

```bash
npm install @libraz/libsonare         # JavaScript / TypeScript (WASM, takes Float32Array)
pip install libsonare                  # Python (WAV/MP3 — see "Supported audio formats" for M4A/AAC)
```

For Node.js with native file decoding, build
[`@libraz/libsonare-native`](bindings/node/) from source:

```bash
cd bindings/node
yarn install
yarn build  # auto-detects FFmpeg via pkg-config (WAV/MP3 if absent, +M4A/AAC/FLAC/OGG if present)
```

To force a specific mode:

```bash
SONARE_FFMPEG=0 yarn build  # explicitly disable FFmpeg
SONARE_FFMPEG=1 yarn build  # require FFmpeg (fails if dev libs missing)
```

## Quick Start

### JavaScript / TypeScript (WASM)

`@libraz/libsonare` accepts decoded `Float32Array` samples — use the Web Audio
API or a JS decoder to obtain them. Mastering DSP is included in the default WASM build.

**Analysis**

```typescript
import { init, detectBpm, detectKey, analyze } from '@libraz/libsonare';

await init();

const bpm = detectBpm(samples, sampleRate);
const key = detectKey(samples, sampleRate);  // { name: "C major", confidence: 0.95 }
const result = analyze(samples, sampleRate);
```

**Mastering**

```typescript
import {
  init,
  masteringChain,
  masteringChainStereo,
  masteringPairAnalyze,
  masteringPairProcess,
  masteringPairProcessorNames,
  masteringProcess,
  masteringProcessorNames,
} from '@libraz/libsonare';

await init();

const mastered = masteringChain(samples, sampleRate, {
  eq: { tiltDb: 1.0 },
  dynamics: { compressor: { thresholdDb: -24, ratio: 1.5 } },
  saturation: { tape: { driveDb: 1.0, saturation: 0.2 } },
  loudness: { targetLufs: -14, ceilingDb: -1, truePeakOversample: 4 },
});

const stereo = masteringChainStereo(left, right, sampleRate, {
  stereo: { imager: { width: 1.1 }, monoMaker: { amount: 0.2 } },
  loudness: { targetLufs: -14, ceilingDb: -1, truePeakOversample: 4 },
});

// Apply a single named processor
const compressed = masteringProcess('dynamics.compressor', samples, sampleRate, {
  thresholdDb: -24,
  ratio: 1.5,
});

// Reference-based mastering
const matched = masteringPairProcess('match.abCrossfade', source, reference, sampleRate, {
  mix: 0.25,
});
const loudnessJson = masteringPairAnalyze(
  'match.referenceLoudness', source, reference, sampleRate,
);

// Discover available processors
masteringProcessorNames();     // ['dynamics.compressor', 'eq.parametric', ...]
masteringPairProcessorNames(); // ['match.abCrossfade', ...]
```

### Python

`pip install libsonare` ships a **WAV/MP3-only wheel** (matching librosa / pydub /
soundfile conventions). For M4A/AAC/FLAC/OGG either pre-convert with external
`ffmpeg`, or rebuild from source with FFmpeg linked:

```bash
SONARE_FFMPEG=1 pip install libsonare --no-binary libsonare
# requires system FFmpeg dev libs: brew install ffmpeg / apt install libavformat-dev libavcodec-dev libavutil-dev libswresample-dev
```

```python
import libsonare

audio = libsonare.Audio.from_file("song.mp3")
print(f"BPM: {audio.detect_bpm()}, Key: {audio.detect_key()}")

# Mastering chain — returns MasteringResult(samples, sample_rate,
# input_lufs, output_lufs, applied_gain_db, latency_samples)
result = audio.mastering(target_lufs=-14.0, ceiling_db=-1.0)
print(f"{result.input_lufs:.1f} LUFS → {result.output_lufs:.1f} LUFS "
      f"(gain {result.applied_gain_db:+.2f} dB)")

# Single processor / reference matching
compressed = libsonare.mastering_process(
    "dynamics.compressor", samples, sample_rate=44100,
    params={"thresholdDb": -24, "ratio": 1.5},
)
loudness_json = libsonare.mastering_pair_analyze(
    "match.referenceLoudness", source, reference, sample_rate=44100,
)

# Discover available processors
libsonare.mastering_processor_names()       # ['dynamics.compressor', ...]
libsonare.mastering_pair_processor_names()  # ['match.abCrossfade', ...]
```

### Python CLI

```bash
pip install libsonare

# Analysis
sonare analyze song.mp3
# > Estimated BPM : 161.00 BPM  (conf 75.0%)
# > Estimated Key : C major  (conf 100.0%)

sonare bpm song.mp3 --json

# Mastering
sonare mastering song.wav -o mastered.wav --target-lufs -14
sonare mastering-processor song.wav --processor dynamics.compressor \
    --params thresholdDb=-24,ratio=1.5
sonare mastering-pair-analyze source.wav --reference reference.wav \
    --analysis match.referenceLoudness
```

### C++

```cpp
#include <sonare/sonare.h>           // analysis + features + effects
#include <sonare/mastering/master.h> // mastering chain & processors

auto audio = sonare::Audio::from_file("music.mp3");
auto result = sonare::MusicAnalyzer(audio).analyze();
std::cout << "BPM: " << result.bpm
          << ", Key: " << result.key.to_string() << std::endl;
```

## Features

### Analysis (librosa-compatible)

| Music              | Spectral / Pitch     | Streaming           |
|--------------------|----------------------|---------------------|
| BPM / Tempo        | STFT / iSTFT         | Real-time analyzer  |
| Key Detection      | Mel Spectrogram      | Incremental BPM     |
| Beat Tracking      | MFCC                 | Incremental key     |
| Chord Recognition  | Chroma               | Onset events        |
| Section Detection  | CQT / VQT            |                     |
| Timbre / Dynamics  | Spectral Features    |                     |
| Pitch (YIN / pYIN) | Onset Detection      |                     |

### Mastering (90+ DSP modules)

| Dynamics                  | EQ                        | Multiband / Stereo            |
|---------------------------|---------------------------|-------------------------------|
| Compressor                | Parametric / Graphic      | Multiband comp / EQ / limiter |
| Limiter / Brickwall       | Linear / Minimum phase    | Stereo imager / M-S           |
| Expander / Gate           | Dynamic EQ                | Haas / phase align            |
| De-esser                  | Pultec / API style        | Mono maker / compat           |
| Transient shaper          | Tilt / shelving           |                               |

| Saturation / Repair               | Maximizer / Match                       | Building blocks            |
|-----------------------------------|-----------------------------------------|----------------------------|
| Tape / Tube / Transformer         | True-peak limiter (ITU-R BS.1770-4)     | Polyphase FIR oversampler  |
| Exciter / Bitcrusher              | Loudness optimizer (LUFS target)        | ADAA nonlinearities        |
| Declick / Declip / Decrackle      | Adaptive release                        | Vicanek biquad design      |
| Denoise / Dereverb / Dehum        | Reference EQ / loudness / spectrum      | Partitioned convolver      |

Mastering is built by default (`BUILD_MASTERING=ON`). Disable with
`cmake -DBUILD_MASTERING=OFF` to ship analysis-only builds.

## Performance

Dramatically faster than Python-based alternatives. Parallelized analysis with
automatic CPU detection, optimized HPSS with multi-threaded median filter.
Mastering processors use ITU-spec polyphase oversampling, antiderivative
anti-aliasing (ADAA), and SIMD-friendly Eigen GEMM for hot paths.

See [Benchmarks](https://libsonare.libraz.net/docs/benchmarks) for detailed comparisons.

## librosa Compatibility

Default parameters match librosa:
- Sample rate: 22050 Hz
- n_fft: 2048, hop_length: 512, n_mels: 128
- fmin: 0.0, fmax: sr/2

## Supported audio formats

| Format | Default¹ | With FFmpeg² | WASM (`@libraz/libsonare`) |
|--------|----------|--------------|----------------------------|
| WAV (PCM 16/24/32, float32) | ✅ | ✅ | n/a (samples in) |
| MP3 | ✅ | ✅ | n/a |
| M4A / AAC / FLAC / OGG / Opus / WMA / ... | ❌ (clear error message) | ✅ | n/a (use Web Audio API) |

¹ **Default**: PyPI wheel (`pip install libsonare`) and source builds where FFmpeg
dev libs are not present. PyPI wheels are deterministically pinned to this mode
so installation never depends on the user's `libavformat`.

² **With FFmpeg**: source build with FFmpeg linked. CMake auto-detects via
pkg-config (`-DSONARE_WITH_FFMPEG=AUTO`, the default for `make build`), and you
can force on/off with `-DSONARE_WITH_FFMPEG=ON`/`OFF`. Python equivalent:
`SONARE_FFMPEG=1 pip install libsonare --no-binary libsonare`. Node native:
`SONARE_FFMPEG=1 yarn build`.

WASM does not bundle a file decoder by design; pass `Float32Array` samples obtained from
the Web Audio API or another JS decoder.

## Build from Source

```bash
# Native (auto-detects FFmpeg; mastering on by default)
make build && make test

# Analysis-only (smaller binary)
cmake -B build -DBUILD_MASTERING=OFF && cmake --build build

# WebAssembly (mastering included)
make wasm

# Release (optimized)
make release
```

## Documentation

- [Getting Started](https://libsonare.libraz.net/docs/getting-started)
- [JavaScript API](https://libsonare.libraz.net/docs/js-api)
- [Python API](https://libsonare.libraz.net/docs/python-api)
- [C++ API](https://libsonare.libraz.net/docs/cpp-api)
- [WebAssembly Guide](https://libsonare.libraz.net/docs/wasm)

## License

[Apache-2.0](LICENSE)
