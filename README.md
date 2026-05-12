# libsonare

[![CI](https://img.shields.io/github/actions/workflow/status/libraz/libsonare/ci.yml?branch=main&label=CI)](https://github.com/libraz/libsonare/actions)
[![npm](https://img.shields.io/npm/v/@libraz/libsonare)](https://www.npmjs.com/package/@libraz/libsonare)
[![PyPI](https://img.shields.io/pypi/v/libsonare)](https://pypi.org/project/libsonare/)
[![codecov](https://codecov.io/gh/libraz/libsonare/branch/main/graph/badge.svg)](https://codecov.io/gh/libraz/libsonare)
[![License](https://img.shields.io/badge/license-Apache--2.0-blue)](https://github.com/libraz/libsonare/blob/main/LICENSE)
[![C++17](https://img.shields.io/badge/C%2B%2B-17-blue?logo=c%2B%2B)](https://en.cppreference.com/w/cpp/17)
[![Platform](https://img.shields.io/badge/platform-Linux%20%7C%20macOS%20%7C%20WebAssembly-lightgrey)](https://github.com/libraz/libsonare)

**librosa-like audio analysis for C++, Python, and browsers.** Fast, dependency-free, runs anywhere.

Tens of times faster than librosa/Python.

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
API or a JS decoder to obtain them.

```typescript
import { init, detectBpm, detectKey, analyze } from '@libraz/libsonare';

await init();

const bpm = detectBpm(samples, sampleRate);
const key = detectKey(samples, sampleRate);  // { name: "C major", confidence: 0.95 }
const result = analyze(samples, sampleRate);
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

# Recommended: Audio class for file input
audio = libsonare.Audio.from_file("song.mp3")
print(f"BPM: {audio.detect_bpm()}, Key: {audio.detect_key()}")

# Or pass numpy / list samples to the functional API
bpm = libsonare.detect_bpm(samples, sample_rate=22050)
key = libsonare.detect_key(samples, sample_rate=22050)
result = libsonare.analyze(samples, sample_rate=22050)
```

### Python CLI

```bash
pip install libsonare

sonare analyze song.mp3
# > Estimated BPM : 161.00 BPM  (conf 75.0%)
# > Estimated Key : C major  (conf 100.0%)

sonare bpm song.mp3 --json
# {"bpm": 161.0}
```

### C++

```cpp
#include <sonare/sonare.h>

auto audio = sonare::Audio::from_file("music.mp3");
auto result = sonare::MusicAnalyzer(audio).analyze();
std::cout << "BPM: " << result.bpm << ", Key: " << result.key.to_string() << std::endl;
```

## Features

| Analysis | DSP | Effects |
|----------|-----|---------|
| BPM / Tempo | STFT / iSTFT | HPSS |
| Key Detection | Mel Spectrogram | Time Stretch |
| Beat Tracking | MFCC | Pitch Shift |
| Chord Recognition | Chroma | Normalize / Trim |
| Section Detection | CQT / VQT | |
| Timbre / Dynamics | Spectral Features | |
| Pitch Tracking (YIN/pYIN) | Onset Detection | |
| Real-time Streaming | Resample | |

## Performance

Dramatically faster than Python-based alternatives. Parallelized analysis with automatic CPU detection, optimized HPSS with multi-threaded median filter.

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
# Native (auto-detects FFmpeg; pass -DSONARE_WITH_FFMPEG=ON to require, =OFF to disable)
make build && make test

# WebAssembly
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
