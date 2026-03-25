# libsonare

[![PyPI](https://img.shields.io/pypi/v/libsonare)](https://pypi.org/project/libsonare/)
[![npm](https://img.shields.io/npm/v/@libraz/libsonare)](https://www.npmjs.com/package/@libraz/libsonare)
[![License](https://img.shields.io/badge/license-Apache--2.0-blue)](https://github.com/libraz/libsonare/blob/main/LICENSE)

Fast audio analysis library for Python -- librosa-like API, **tens of times faster**.

Built on a C++ core with zero Python dependencies.

## Installation

```bash
pip install libsonare
```

Supported platforms: Linux (x86_64, aarch64), macOS (Apple Silicon).

## Quick Start

```python
import libsonare

# From raw samples
bpm = libsonare.detect_bpm(samples, sample_rate=22050)
key = libsonare.detect_key(samples, sample_rate=22050)
print(f"{key.root.name} {key.mode.name}")  # e.g. "C MAJOR"

# Full analysis
result = libsonare.analyze(samples, sample_rate=22050)
print(f"BPM: {result.bpm}, Key: {result.key}")

# Audio class (load files directly)
audio = libsonare.Audio.from_file("song.mp3")
print(f"BPM: {audio.detect_bpm()}")
print(f"Key: {audio.detect_key()}")
print(f"Mel: {audio.mel_spectrogram().n_mels}x{audio.mel_spectrogram().n_frames}")
```

## CLI

```bash
sonare analyze song.mp3
# > Estimated BPM : 161.00 BPM  (conf 75.0%)
# > Estimated Key : C major  (conf 100.0%)

sonare bpm song.mp3 --json       # {"bpm": 161.0}
sonare key song.mp3              # Key: C major (confidence: 100.0%)
sonare spectral song.mp3         # Spectral features table
sonare pitch song.mp3            # Pitch tracking (pYIN)
sonare mel song.mp3              # Mel spectrogram shape
sonare chroma song.mp3           # Chromagram with visualization
```

## Features

- **Detection**: BPM, key, beats, onsets
- **Analysis**: Full music analysis (BPM + key + time signature + beats)
- **Effects**: HPSS, pitch shift, time stretch, normalize, trim
- **Features**: STFT, mel spectrogram, MFCC, chroma, spectral features, pitch tracking (YIN/pYIN)
- **Conversions**: Hz/mel/MIDI/note, frames/time
- **I/O**: Load WAV/MP3 files, resample

## librosa Compatibility

Default parameters match librosa:
- Sample rate: 22050 Hz
- n_fft: 2048
- hop_length: 512
- n_mels: 128

## Also available

```bash
npm install @libraz/libsonare  # JavaScript/TypeScript (WASM)
```

## License

Apache-2.0
