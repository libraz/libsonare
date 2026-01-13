# librosa Compatibility Guide

This document describes how libsonare functions correspond to Python's librosa library, including parameter mappings, algorithm differences, and verification methodology.

## Overview

libsonare aims to provide functionality similar to [librosa](https://librosa.org/) while being optimized for C++ and WebAssembly environments. Most core features use the same algorithms as librosa with compatible default parameters.

## Feature Comparison

### Supported Features

| librosa | libsonare | Notes |
|---------|-----------|-------|
| `librosa.load()` | `Audio::from_file()` | WAV, MP3 support |
| `librosa.resample()` | `resample()` | Uses r8brain |
| `librosa.stft()` | `Spectrogram::compute()` | Full compatibility |
| `librosa.istft()` | `Spectrogram::to_audio()` | OLA reconstruction |
| `librosa.griffinlim()` | `griffin_lim()` | Momentum-based |
| `librosa.feature.melspectrogram()` | `MelSpectrogram::compute()` | Slaney normalization |
| `librosa.feature.mfcc()` | `MelSpectrogram::mfcc()` | DCT-II, liftering |
| `librosa.feature.chroma_stft()` | `Chroma::compute()` | STFT-based |
| `librosa.onset.onset_strength()` | `compute_onset_strength()` | Spectral flux |
| `librosa.beat.beat_track()` | `BeatAnalyzer` | DP-based |
| `librosa.beat.tempo()` | `BpmAnalyzer` | Tempogram |
| `librosa.effects.hpss()` | `hpss()` | Median filtering |
| `librosa.effects.time_stretch()` | `time_stretch()` | Phase vocoder |
| `librosa.effects.pitch_shift()` | `pitch_shift()` | WSOLA-like |

### Features Not in librosa

| libsonare | Description |
|-----------|-------------|
| `KeyAnalyzer` | Musical key detection (Krumhansl-Schmuckler) |
| `ChordAnalyzer` | Chord recognition (template matching) |
| `SectionAnalyzer` | Song structure analysis |
| `TimbreAnalyzer` | Timbre characteristics |
| `DynamicsAnalyzer` | Loudness and dynamics |
| `RhythmAnalyzer` | Time signature, groove |
| `MelodyAnalyzer` | Pitch/melody tracking |

---

## Detailed Function Mapping

### STFT (Short-Time Fourier Transform)

#### librosa

```python
import librosa
import numpy as np

y, sr = librosa.load('audio.wav', sr=22050)

# STFT
S = librosa.stft(
    y,
    n_fft=2048,
    hop_length=512,
    win_length=None,  # defaults to n_fft
    window='hann',
    center=True,
    pad_mode='constant'
)

# Result shape: (1 + n_fft/2, n_frames)
# Complex values
```

#### libsonare

```cpp
#include <sonare/sonare.h>

auto audio = sonare::Audio::from_file("audio.wav");
// Resample to 22050 if needed
audio = sonare::resample(audio, 22050);

sonare::StftConfig config;
config.n_fft = 2048;
config.hop_length = 512;
config.win_length = 0;  // defaults to n_fft
config.window = sonare::WindowType::Hann;
config.center = true;

auto spec = sonare::Spectrogram::compute(audio, config);

// Result: n_bins() x n_frames()
// Access complex values via complex_view()
```

**Compatibility Notes:**
- Default parameters are identical
- Padding mode is always 'constant' (zero-padding)
- Window functions: Hann, Hamming, Blackman supported
- Complex output format is compatible

---

### Mel Spectrogram

#### librosa

```python
import librosa

# Mel spectrogram
mel = librosa.feature.melspectrogram(
    y=y,
    sr=sr,
    n_fft=2048,
    hop_length=512,
    n_mels=128,
    fmin=0.0,
    fmax=None,  # sr/2
    htk=False,  # Slaney formula
    norm='slaney'
)

# Convert to dB
mel_db = librosa.power_to_db(mel, ref=np.max)
```

#### libsonare

```cpp
sonare::MelConfig config;
config.n_mels = 128;
config.fmin = 0.0f;
config.fmax = 0.0f;  // sr/2
config.stft.n_fft = 2048;
config.stft.hop_length = 512;

auto mel = sonare::MelSpectrogram::compute(audio, config);

// Convert to dB
auto mel_db = mel.to_db(1.0f, 1e-10f);
```

**Compatibility Notes:**
- Slaney Mel scale is default (same as librosa)
- HTK Mel scale available via `hz_to_mel_htk()`
- Slaney normalization applied by default
- dB conversion uses same formula: `10 * log10(max(power, amin) / ref)`

---

### MFCC

#### librosa

```python
mfcc = librosa.feature.mfcc(
    y=y,
    sr=sr,
    n_mfcc=13,
    n_fft=2048,
    hop_length=512,
    n_mels=128,
    fmin=0.0,
    fmax=None,
    lifter=0
)

# Delta features
mfcc_delta = librosa.feature.delta(mfcc)
```

#### libsonare

```cpp
auto mel = sonare::MelSpectrogram::compute(audio, config);

// MFCC (DCT-II, ortho normalization)
auto mfcc = mel.mfcc(13, 0.0f);  // n_mfcc=13, lifter=0

// Delta features
auto mfcc_delta = sonare::MelSpectrogram::delta(mfcc, 13, mel.n_frames());
```

**Compatibility Notes:**
- DCT-II with orthogonal normalization (same as librosa)
- Liftering coefficient supported
- Delta computation uses same window-based method

---

### Chroma

#### librosa

```python
chroma = librosa.feature.chroma_stft(
    y=y,
    sr=sr,
    n_fft=2048,
    hop_length=512,
    n_chroma=12,
    tuning=0.0
)
```

#### libsonare

```cpp
sonare::ChromaConfig config;
config.n_chroma = 12;
config.tuning = 0.0f;
config.stft.n_fft = 2048;
config.stft.hop_length = 512;

auto chroma = sonare::Chroma::compute(audio, config);
```

**Compatibility Notes:**
- STFT-based chroma (not CQT)
- Tuning offset in cents
- Output normalized per frame

---

### HPSS

#### librosa

```python
y_harm, y_perc = librosa.effects.hpss(
    y,
    kernel_size=31,
    power=2.0,
    margin=1.0
)
```

#### libsonare

```cpp
sonare::HpssConfig config;
config.kernel_size_harmonic = 31;
config.kernel_size_percussive = 31;
config.power = 2.0f;
config.margin_harmonic = 1.0f;
config.margin_percussive = 1.0f;

auto result = sonare::hpss(audio, config);
// result.harmonic
// result.percussive
```

**Compatibility Notes:**
- Median filtering algorithm is identical
- Soft masking with power parameter
- Separate kernel sizes for H and P (librosa uses same for both by default)

---

### Beat Tracking

#### librosa

```python
tempo, beats = librosa.beat.beat_track(
    y=y,
    sr=sr,
    hop_length=512,
    start_bpm=120.0,
    tightness=100
)

beat_times = librosa.frames_to_time(beats, sr=sr, hop_length=512)
```

#### libsonare

```cpp
sonare::BeatConfig config;
config.start_bpm = 120.0f;
config.tightness = 100.0f;

sonare::BeatAnalyzer analyzer(audio, config);

float bpm = analyzer.bpm();
auto beat_times = analyzer.beat_times();  // Already in seconds
```

**Compatibility Notes:**
- Dynamic programming beat tracker
- Start BPM prior
- Tightness parameter controls tempo consistency
- Returns times directly (no frame-to-time conversion needed)

---

### Tempo Detection

#### librosa

```python
tempo, _ = librosa.beat.beat_track(y=y, sr=sr)
# or
tempo = librosa.feature.tempo(
    y=y,
    sr=sr,
    hop_length=512,
    start_bpm=120
)
```

#### libsonare

```cpp
sonare::BpmConfig config;
config.bpm_min = 60.0f;
config.bpm_max = 200.0f;
config.start_bpm = 120.0f;

sonare::BpmAnalyzer analyzer(audio, config);

float bpm = analyzer.bpm();
float confidence = analyzer.confidence();
auto candidates = analyzer.bpm_candidates();
```

**Compatibility Notes:**
- Tempogram-based algorithm
- Start BPM influences candidate selection
- Returns confidence score
- Multiple BPM candidates available

---

## Default Parameters

### librosa Defaults (Reference)

| Parameter | librosa Default | libsonare Default |
|-----------|-----------------|-------------------|
| `sr` | 22050 | User-provided |
| `n_fft` | 2048 | 2048 |
| `hop_length` | 512 | 512 |
| `win_length` | n_fft | n_fft (0) |
| `window` | 'hann' | Hann |
| `center` | True | true |
| `n_mels` | 128 | 128 |
| `fmin` | 0.0 | 0.0 |
| `fmax` | sr/2 | sr/2 (0.0) |
| `n_mfcc` | 20 | 13 |
| `n_chroma` | 12 | 12 |

---

## Mel Scale Formulas

### Slaney (librosa default, libsonare default)

```
For f < 1000 Hz (linear region):
    mel = 3 * f / 200

For f >= 1000 Hz (log region):
    mel = 15 + 27 * log10(f / 1000) / log10(6.4)
```

### HTK

```
mel = 2595 * log10(1 + f / 700)
```

libsonare provides both:
```cpp
float mel_slaney = sonare::hz_to_mel(hz);
float mel_htk = sonare::hz_to_mel_htk(hz);
```

---

## Verification Methodology

### Test Data Generation

Reference values are generated from librosa:

```python
# scripts/generate_reference.py
import librosa
import numpy as np
import json

def generate_reference(audio_path):
    y, sr = librosa.load(audio_path, sr=22050)

    results = {
        'mel_spectrogram': {
            'mean': librosa.feature.melspectrogram(y=y, sr=sr).mean(axis=1).tolist(),
            'shape': librosa.feature.melspectrogram(y=y, sr=sr).shape
        },
        'mfcc': {
            'mean': librosa.feature.mfcc(y=y, sr=sr, n_mfcc=13).mean(axis=1).tolist()
        },
        'chroma': {
            'mean': librosa.feature.chroma_stft(y=y, sr=sr).mean(axis=1).tolist()
        },
        'tempo': float(librosa.beat.beat_track(y=y, sr=sr)[0]),
        'onset_strength': {
            'mean': float(librosa.onset.onset_strength(y=y, sr=sr).mean()),
            'max': float(librosa.onset.onset_strength(y=y, sr=sr).max())
        }
    }

    return results

# Generate for test files
for audio_file in glob.glob('tests/testdata/*.wav'):
    ref = generate_reference(audio_file)
    with open(f'{audio_file}.reference.json', 'w') as f:
        json.dump(ref, f, indent=2)
```

### C++ Comparison Tests

```cpp
TEST_CASE("Mel spectrogram matches librosa", "[librosa]") {
  auto audio = sonare::Audio::from_file("tests/testdata/sine_440hz.wav");
  audio = sonare::resample(audio, 22050);

  sonare::MelConfig config;
  auto mel = sonare::MelSpectrogram::compute(audio, config);

  // Load reference values
  auto reference = load_json("tests/testdata/sine_440hz.wav.reference.json");

  // Compare mean values per mel band (allow 1% tolerance)
  auto mel_mean = compute_mean_per_band(mel);
  for (int i = 0; i < mel.n_mels(); ++i) {
    REQUIRE_THAT(mel_mean[i],
                 Catch::Matchers::WithinRel(reference["mel_spectrogram"]["mean"][i], 0.01f));
  }
}
```

### Tolerance Guidelines

| Feature | Tolerance | Notes |
|---------|-----------|-------|
| STFT magnitude | < 1e-6 | Floating point precision |
| Mel spectrogram | < 1% relative | Filterbank implementation differences |
| MFCC | < 2% relative | DCT normalization variations |
| Chroma | < 5% relative | Pitch mapping differences |
| BPM | ±2 BPM | Algorithm differences acceptable |
| Beat times | ±50ms | Phase alignment |

---

## Known Differences

### 1. Resampling Algorithm

- **librosa**: Uses `resampy` (Kaiser best)
- **libsonare**: Uses `r8brain-free` (24-bit quality)

Impact: Slight differences in high-frequency content after resampling. For most MIR tasks, the difference is negligible.

### 2. CQT (Constant-Q Transform)

- **librosa**: Full CQT implementation
- **libsonare**: STFT-based chroma only

If you need CQT-based chroma, use librosa for preprocessing or implement CQT separately.

### 3. Mel Filterbank Edges

- **librosa**: Uses floor rounding for bin edges
- **libsonare**: Uses nearest rounding

Impact: 1-2 bin difference in some edge cases. Minimal effect on downstream features.

### 4. Window Normalization

- **librosa**: Normalizes window for COLA
- **libsonare**: Uses raw window values

Impact: iSTFT reconstruction may have slight amplitude differences. Use `normalize()` to correct.

---

## Migration Guide

### From Python + librosa to C++

**Before (Python):**
```python
import librosa

y, sr = librosa.load('audio.mp3', sr=22050)
tempo, beats = librosa.beat.beat_track(y=y, sr=sr)
print(f"BPM: {tempo}")
```

**After (C++):**
```cpp
#include <sonare/sonare.h>

auto audio = sonare::Audio::from_file("audio.mp3");
audio = sonare::resample(audio, 22050);

sonare::BpmAnalyzer analyzer(audio);
std::cout << "BPM: " << analyzer.bpm() << "\n";
```

### From Python to JavaScript

**Before (Python):**
```python
import librosa

y, sr = librosa.load('audio.mp3')
chroma = librosa.feature.chroma_stft(y=y, sr=sr)
key = detect_key_from_chroma(chroma)  # Custom function
```

**After (JavaScript):**
```typescript
import { init, detectKey } from '@libraz/sonare';

await init();

// Get samples from AudioContext
const audioBuffer = await audioCtx.decodeAudioData(arrayBuffer);
const samples = audioBuffer.getChannelData(0);

const key = detectKey(samples, audioBuffer.sampleRate);
console.log(`Key: ${key.name}`);  // Built-in key detection!
```

---

## Reference Data Location

Reference data for compatibility testing:

```
tests/
├── librosa/
│   ├── reference/
│   │   ├── mel_reference.json
│   │   ├── mfcc_reference.json
│   │   ├── chroma_reference.json
│   │   ├── tempo_reference.json
│   │   └── NOTICE.md
│   └── compare_librosa_test.cpp
└── testdata/
    ├── sine_440hz.wav
    ├── c_major_chord.wav
    └── 120bpm_drums.wav
```

---

## Performance Comparison

| Operation | librosa (Python) | libsonare (C++) | Speedup |
|-----------|------------------|-----------------|---------|
| STFT (3min audio) | ~500ms | ~50ms | ~10x |
| Mel spectrogram | ~600ms | ~60ms | ~10x |
| MFCC | ~700ms | ~70ms | ~10x |
| Beat tracking | ~2s | ~200ms | ~10x |
| Full analysis | ~5s | ~500ms | ~10x |

*Benchmarked on Intel Core i7, 3-minute audio at 22050 Hz*

WebAssembly performance is approximately 2-3x slower than native C++, but still faster than Python.
