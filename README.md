# libsonare

[![License](https://img.shields.io/badge/license-Apache--2.0-blue)](https://github.com/libraz/libsonare/blob/master/LICENSE)
[![C++17](https://img.shields.io/badge/C%2B%2B-17-blue?logo=c%2B%2B)](https://en.cppreference.com/w/cpp/17)
[![Platform](https://img.shields.io/badge/platform-Linux%20%7C%20macOS%20%7C%20WebAssembly-lightgrey)](https://github.com/libraz/libsonare)

C++17 audio analysis library for music information retrieval. WASM-ready, Eigen3-based signal processing.

## Why libsonare?

**libsonare** provides comprehensive music analysis capabilities:

- **BPM Detection**: Accurate tempo estimation using tempogram and autocorrelation
- **Key Detection**: Krumhansl-Schmuckler algorithm with enhanced key profiles
- **Beat Tracking**: Dynamic programming-based beat detection
- **Chord Recognition**: Template matching with 84 chord types
- **Section Detection**: Structural segmentation using novelty curves
- **Audio Effects**: HPSS, time stretch, pitch shift

All features work in both native C++ and WebAssembly environments. While libsonare is primarily designed for offline analysis, its core DSP modules can be used for low-latency audio processing in WebAssembly.

## Features

| Category | Capabilities |
|----------|-------------|
| **Core** | STFT/iSTFT, Griffin-Lim, Resampling (r8brain) |
| **Features** | Mel Spectrogram, MFCC, Chroma, Spectral Features |
| **Effects** | HPSS, Time Stretch, Pitch Shift, Normalize |
| **Analysis** | BPM, Key, Beats, Chords, Sections, Timbre, Dynamics |
| **Platform** | Linux, macOS, WebAssembly |
| **Audio I/O** | WAV, MP3 (dr_libs, minimp3) |

## Quick Start

### Native (C++)

```cpp
#include <sonare/sonare.h>

// Load audio file
auto audio = sonare::Audio::from_file("music.mp3");

// Simple analysis
float bpm = sonare::quick::detect_bpm(audio.data(), audio.size(), audio.sample_rate());
auto key = sonare::quick::detect_key(audio.data(), audio.size(), audio.sample_rate());

std::cout << "BPM: " << bpm << std::endl;
std::cout << "Key: " << key.to_string() << std::endl;  // e.g., "C major"

// Full analysis
sonare::MusicAnalyzer analyzer(audio);
auto result = analyzer.analyze();

std::cout << "Beats: " << result.beats.size() << std::endl;
std::cout << "Sections: " << result.sections.size() << std::endl;
```

### WebAssembly (JavaScript/TypeScript)

```typescript
import { Sonare } from '@libraz/sonare';

const sonare = await Sonare.create();

// Decode audio with Web Audio API
const audioCtx = new AudioContext();
const response = await fetch('music.mp3');
const audioBuffer = await audioCtx.decodeAudioData(await response.arrayBuffer());
const samples = audioBuffer.getChannelData(0);

// Analyze
const bpm = sonare.detectBpm(samples, audioBuffer.sampleRate);
const key = sonare.detectKey(samples, audioBuffer.sampleRate);

console.log(`BPM: ${bpm}`);
console.log(`Key: ${key.root} ${key.mode}`);  // e.g., "C Major"
```

## Installation

### Requirements

**System:**
- C++17 compatible compiler (GCC 8+, Clang 8+, MSVC 2019+)
- CMake 3.16+
- Eigen3 (system package or FetchContent)

**For WebAssembly:**
- Emscripten SDK (emsdk)

### Build from Source

```bash
git clone https://github.com/libraz/libsonare.git
cd libsonare

# Native build
make build
make test   # Run tests (317 tests)

# WebAssembly build
source /path/to/emsdk/emsdk_env.sh
emcmake cmake -B build-wasm -DBUILD_WASM=ON -DCMAKE_BUILD_TYPE=Release
cmake --build build-wasm --parallel

# Output: dist/sonare.js (34KB), dist/sonare.wasm (228KB)
```

### npm Package

```bash
npm install @libraz/sonare
# or
yarn add @libraz/sonare
```

## API Reference

### Quick API (Simple Functions)

```cpp
namespace sonare::quick {
  // BPM detection
  float detect_bpm(const float* samples, size_t length, int sample_rate);

  // Key detection
  Key detect_key(const float* samples, size_t length, int sample_rate);

  // Beat detection
  std::vector<float> detect_beats(const float* samples, size_t length, int sample_rate);

  // Onset detection
  std::vector<float> detect_onsets(const float* samples, size_t length, int sample_rate);

  // Full analysis
  AnalysisResult analyze(const float* samples, size_t length, int sample_rate);
}
```

### MusicAnalyzer (Facade)

```cpp
class MusicAnalyzer {
public:
  explicit MusicAnalyzer(const Audio& audio);

  // Individual analyzers (lazy initialization)
  BpmAnalyzer& bpm_analyzer();
  KeyAnalyzer& key_analyzer();
  BeatAnalyzer& beat_analyzer();
  ChordAnalyzer& chord_analyzer();
  SectionAnalyzer& section_analyzer();
  // ...

  // Quick access
  float bpm() const;
  Key key() const;
  std::vector<Beat> beats() const;
  std::vector<Chord> chords() const;
  std::vector<Section> sections() const;

  // Full analysis
  AnalysisResult analyze() const;
};
```

### C API

```c
#include <sonare_c.h>

SonareAudio* audio = NULL;
sonare_audio_from_file("music.mp3", &audio);

float bpm;
sonare_detect_bpm(audio, &bpm, NULL);

sonare_audio_free(audio);
```

## Architecture

```
src/
├── util/           # Basic types, exceptions, math utilities
├── core/           # FFT, STFT, Window, Audio I/O, Resample
├── filters/        # Mel, Chroma, DCT, IIR filterbanks
├── feature/        # MelSpectrogram, MFCC, Chroma, Spectral
├── effects/        # HPSS, Time Stretch, Pitch Shift
├── analysis/       # BPM, Key, Beat, Chord, Section analyzers
├── quick.h/cpp     # Simple function API
├── sonare.h        # Unified header
├── sonare_c.h/cpp  # C API
└── wasm/           # Embind bindings
```

### Dependency Levels

| Level | Modules | Dependencies |
|-------|---------|--------------|
| 0 | util/ | None |
| 1 | core/convert, core/window | util/ |
| 2 | core/fft | util/, KissFFT |
| 3 | core/spectrum | core/fft, core/window |
| 4 | filters/, feature/ | core/ |
| 5 | effects/ | core/, feature/ |
| 6 | analysis/ | feature/, effects/ |

## Documentation

### Guides
- [Installation Guide](docs/en/installation.md) - Build from source
- [Examples](docs/en/examples.md) - Usage examples
- [WASM Guide](docs/en/wasm.md) - WebAssembly usage

### API Reference
- [C++ API Reference](docs/en/cpp-api.md) - Complete C++ API documentation
- [JavaScript/TypeScript API Reference](docs/en/js-api.md) - Complete JS/TS API documentation
- [CLI Reference](docs/en/cli-reference.md) - Command-line interface documentation

### Technical Notes
- [Architecture](docs/en/architecture.md) - Internal architecture and design
- [librosa Compatibility](docs/en/librosa-compatibility.md) - librosa comparison and migration guide

## Comparison with librosa

libsonare provides similar functionality to Python's librosa, optimized for C++ and WebAssembly:

| Feature | librosa | libsonare |
|---------|---------|-----------|
| STFT/iSTFT | Yes | Yes |
| Mel Spectrogram | Yes | Yes |
| MFCC | Yes | Yes |
| Chroma | Yes | Yes |
| HPSS | Yes | Yes |
| Beat Tracking | Yes | Yes |
| Tempo Detection | Yes | Yes |
| Key Detection | No (use madmom) | Yes |
| Chord Detection | No | Yes |
| Section Detection | No | Yes |
| WebAssembly | No | Yes |

## Third-Party Libraries

| Library | Purpose |
|---------|---------|
| [KissFFT](https://github.com/mborgerding/kissfft) | FFT operations |
| [Eigen3](https://eigen.tuxfamily.org/) | Matrix operations |
| [dr_libs](https://github.com/mackron/dr_libs) | WAV decoding |
| [minimp3](https://github.com/lieff/minimp3) | MP3 decoding |
| [r8brain](https://github.com/avaneev/r8brain-free-src) | Resampling |
| [Catch2](https://github.com/catchorg/Catch2) | Testing |

## License

[Apache License 2.0](LICENSE)

## Contributing

Contributions are welcome! Please see [CONTRIBUTING.md](CONTRIBUTING.md) for guidelines.

## Author

- libraz <libraz@libraz.net>
