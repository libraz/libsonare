# libsonare

[![CI](https://img.shields.io/github/actions/workflow/status/libraz/libsonare/ci.yml?branch=main&label=CI)](https://github.com/libraz/libsonare/actions)
[![codecov](https://codecov.io/gh/libraz/libsonare/branch/main/graph/badge.svg)](https://codecov.io/gh/libraz/libsonare)
[![License](https://img.shields.io/badge/license-Apache--2.0-blue)](https://github.com/libraz/libsonare/blob/main/LICENSE)
[![C++17](https://img.shields.io/badge/C%2B%2B-17-blue?logo=c%2B%2B)](https://en.cppreference.com/w/cpp/17)
[![Platform](https://img.shields.io/badge/platform-Linux%20%7C%20macOS%20%7C%20WebAssembly-lightgrey)](https://github.com/libraz/libsonare)

**librosa-like audio analysis for C++ and browsers.** Fast, dependency-free, runs anywhere.

## Use Cases

- **Love librosa, need native speed?** → C++ with Eigen3 vectorization
- **Audio analysis in the browser?** → WebAssembly build (262KB)
- **Building a music app?** → BPM, key, chords, beats, sections in one library

## Quick Start

### JavaScript / TypeScript

```bash
npm install @libraz/sonare  # Coming soon (currently in beta)
```

```typescript
import { Sonare } from '@libraz/sonare';

const sonare = await Sonare.create();
const bpm = sonare.detectBpm(samples, sampleRate);
const key = sonare.detectKey(samples, sampleRate);  // { root: "C", mode: "Major" }
const beats = sonare.detectBeats(samples, sampleRate);
```

### C++

```cpp
#include <sonare/sonare.h>

auto audio = sonare::Audio::from_file("music.mp3");
float bpm = sonare::quick::detect_bpm(audio.data(), audio.size(), audio.sample_rate());
auto key = sonare::quick::detect_key(audio.data(), audio.size(), audio.sample_rate());
```

## Features

| Analysis | DSP | Effects |
|----------|-----|---------|
| BPM / Tempo | STFT / iSTFT | HPSS |
| Key Detection | Mel Spectrogram | Time Stretch |
| Beat Tracking | MFCC | Pitch Shift |
| Chord Recognition | Chroma | Normalize |
| Section Detection | CQT / VQT | |

## librosa Compatibility

libsonare implements librosa-compatible algorithms with identical default parameters. Migration is straightforward.

```python
# librosa
S = librosa.feature.melspectrogram(y=y, sr=sr, n_mels=128, fmax=8000)
```

```cpp
// libsonare
auto mel = sonare::MelSpectrogram(sr, 2048, 512, 128, 0, 8000);
auto S = mel.compute(audio);
```

See [librosa Compatibility Guide](https://libsonare.libraz.net/docs/librosa-compatibility) for details.

## Build

```bash
# Native
make build && make test

# WebAssembly (requires: source /path/to/emsdk/emsdk_env.sh)
make wasm
```

## Documentation

- [Getting Started](https://libsonare.libraz.net/docs/getting-started)
- [JavaScript API](https://libsonare.libraz.net/docs/js-api)
- [C++ API](https://libsonare.libraz.net/docs/cpp-api)
- [Examples](https://libsonare.libraz.net/docs/examples)
- [WebAssembly Guide](https://libsonare.libraz.net/docs/wasm)

## License

[Apache-2.0](LICENSE)
