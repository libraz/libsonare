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
npm install @libraz/libsonare   # JavaScript / TypeScript (WASM)
pip install libsonare            # Python
```

## Quick Start

### JavaScript / TypeScript

```typescript
import { init, detectBpm, detectKey, analyze } from '@libraz/libsonare';

await init();

const bpm = detectBpm(samples, sampleRate);
const key = detectKey(samples, sampleRate);  // { name: "C major", confidence: 0.95 }
const result = analyze(samples, sampleRate);
```

### Python

```python
import libsonare

bpm = libsonare.detect_bpm(samples, sample_rate=22050)
key = libsonare.detect_key(samples, sample_rate=22050)
result = libsonare.analyze(samples, sample_rate=22050)

# Or use the Audio class
audio = libsonare.Audio.from_file("song.mp3")
print(f"BPM: {audio.detect_bpm()}, Key: {audio.detect_key()}")
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

## Build from Source

```bash
# Native (C++ library + CLI)
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
