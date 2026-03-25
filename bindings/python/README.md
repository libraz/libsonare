# libsonare

Fast audio analysis library for Python — librosa-like API, 80x faster.

Built on a C++ core with zero Python dependencies.

## Installation

```bash
pip install libsonare
```

## Quick Start

```python
import libsonare

# Detect BPM
bpm = libsonare.detect_bpm(samples, sample_rate=22050)

# Detect musical key
key = libsonare.detect_key(samples, sample_rate=22050)
print(f"{key.root.name} {key.mode.name}")  # e.g. "C MAJOR"

# Full analysis
result = libsonare.analyze(samples, sample_rate=22050)
print(f"BPM: {result.bpm}, Key: {result.key}")

# Audio class (convenience wrapper)
audio = libsonare.Audio.from_file("song.mp3")
print(f"BPM: {audio.detect_bpm()}")
print(f"Key: {audio.detect_key()}")
```

## Features

- **Detection**: BPM, key, beats, onsets, chords
- **Effects**: HPSS, pitch shift, time stretch, normalize, trim
- **Features**: STFT, mel spectrogram, MFCC, chroma, spectral features, pitch tracking
- **Conversions**: Hz/mel/MIDI/note, frames/time

## librosa Compatibility

Default parameters match librosa:
- Sample rate: 22050 Hz
- n_fft: 2048
- hop_length: 512
- n_mels: 128

## License

Apache-2.0
