# libsonare

[![PyPI](https://img.shields.io/pypi/v/libsonare)](https://pypi.org/project/libsonare/)
[![npm](https://img.shields.io/npm/v/@libraz/libsonare)](https://www.npmjs.com/package/@libraz/libsonare)
[![License](https://img.shields.io/badge/license-Apache--2.0-blue)](https://github.com/libraz/libsonare/blob/main/LICENSE)

Fast audio analysis library for Python.

Built on a C++ core with zero Python dependencies.

## Installation

```bash
pip install libsonare
```

Supported platforms: Linux (x86_64, aarch64), macOS (Apple Silicon).

## Quick Start

`Audio` is the recommended entry point. The top-level `libsonare.detect_*` /
`libsonare.analyze` functions are thin wrappers around `Audio` for
one-shot calls and are kept for convenience.

### Load from a file

```python
import libsonare

audio = libsonare.Audio.from_file("song.mp3")  # or "song.wav"
print(f"BPM: {audio.detect_bpm():.1f}")
print(f"Key: {audio.detect_key().root.name} {audio.detect_key().mode.name}")

result = audio.analyze()  # BPM + key + time signature + beats
print(f"BPM: {result.bpm:.1f}  Key: {result.key.root.name} {result.key.mode.name}")
```

### Load from a numpy array / in-memory samples

`from_buffer` accepts any 1D float sequence. With numpy, use a **mono float32**
array (stereo must be downmixed first, e.g. `samples.mean(axis=1)`).

```python
import numpy as np
import libsonare

sr = 22050
samples = np.asarray(my_mono_float32_signal, dtype=np.float32)

audio = libsonare.Audio.from_buffer(samples, sample_rate=sr)
bpm = audio.detect_bpm()

# Or call the function directly (equivalent shortcut)
bpm = libsonare.detect_bpm(samples, sample_rate=sr)
```

### Mastering

The Python binding exposes the same mastering processors as the C, CLI, Node,
and WASM APIs.

```python
import libsonare

names = libsonare.mastering_processor_names()  # e.g. "dynamics.compressor"

compressed = libsonare.mastering_process(
    "dynamics.compressor",
    samples,
    sample_rate=sr,
    params={"thresholdDb": -24.0, "ratio": 1.5},
)

widened = libsonare.mastering_process_stereo(
    "stereo.imager",
    left,
    right,
    sample_rate=sr,
    params={"width": 1.1},
)

pair_names = libsonare.mastering_pair_processor_names()  # e.g. "match.abCrossfade"
crossfaded = libsonare.mastering_pair_process(
    "match.abCrossfade",
    source,
    reference,
    sample_rate=sr,
    params={"mix": 0.25},
)

loudness_json = libsonare.mastering_pair_analyze(
    "match.referenceLoudness",
    source,
    reference,
    sample_rate=sr,
)
mono_compat_json = libsonare.mastering_stereo_analyze(
    "stereo.monoCompatCheck",
    left,
    right,
    sample_rate=sr,
)
```

### Library version

```python
import libsonare
libsonare.__version__   # e.g. "1.0.4"  (preferred — matches importlib.metadata)
libsonare.version()     # native library version string
```

## Supported audio formats

| Format | Default build | With FFmpeg support |
|--------|---------------|---------------------|
| WAV (PCM 16/24/32, float32) | yes | yes |
| MP3 | yes | yes |
| M4A / AAC / FLAC / OGG / Opus / WMA / ... | no | yes |

If `Audio.from_file()` is given an unsupported format you will see a clear
error such as:

```
RuntimeError: Unsupported audio format: '.m4a'. Supported: WAV, MP3.
Rebuild with -DSONARE_WITH_FFMPEG=ON for M4A/AAC/FLAC/OGG,
or convert via: ffmpeg -i "song.m4a" output.wav
```

The PyPI wheels are pinned to **`SONARE_WITH_FFMPEG=OFF`** so the distributed
wheel never silently links against the build host's FFmpeg. From-source builds
default to **AUTO detection** via pkg-config: if FFmpeg dev libraries are
present they are enabled, otherwise WAV/MP3 only.

To enable FFmpeg-backed decoding when building from source:

```bash
git clone https://github.com/libraz/libsonare
cd libsonare
SONARE_FFMPEG=1 bash bindings/python/build_wheel.sh   # require FFmpeg
# or
SONARE_FFMPEG=AUTO bash bindings/python/build_wheel.sh # detect via pkg-config
pip install bindings/python/dist/*.whl
```

This links against the system FFmpeg shared libraries (LGPL by default), so
ensure they are installed (e.g. `brew install ffmpeg`,
`apt install libavformat-dev libavcodec-dev libavutil-dev libswresample-dev`).

## Function vs `Audio` method

Both APIs return the same results. Use whichever is more convenient:

```python
# Functional (good for ad-hoc numpy work)
bpm = libsonare.detect_bpm(samples, sample_rate=22050)        # -> float
key = libsonare.detect_key(samples, sample_rate=22050)        # -> Key(root, mode, confidence)
result = libsonare.analyze(samples, sample_rate=22050)        # -> AnalysisResult

# Audio class (recommended for files, multiple operations on the same audio)
audio = libsonare.Audio.from_file("song.wav")
bpm = audio.detect_bpm()
key = audio.detect_key()
result = audio.analyze()
```

`Audio` caches the decoded samples and is the only way to load files, so it is
the recommended entry point when you do more than a single computation on the
same signal.

## Input format expectations

| API | dtype | shape | range |
|-----|-------|-------|-------|
| `Audio.from_buffer(samples, sample_rate=...)` | float32 (float64 also accepted) | 1D mono | nominally `[-1.0, 1.0]` |
| `Audio.from_memory(data)` | `bytes` of an encoded WAV / MP3 / (FFmpeg) file | — | — |
| `Audio.from_file(path)` | path to an encoded audio file | — | — |
| `libsonare.detect_bpm(samples, sample_rate=...)` etc. | float32 (float64 also accepted) | 1D mono | nominally `[-1.0, 1.0]` |

Stereo input is **not** downmixed automatically when passed as samples —
downmix yourself (e.g. `samples.mean(axis=1, dtype=np.float32)`). File loaders
downmix to mono internally.

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

sonare mastering song.wav -o mastered.wav --target-lufs -14
sonare mastering-processor song.wav --processor dynamics.compressor --params thresholdDb=-24,ratio=1.5
sonare mastering-pair-processor source.wav --reference reference.wav --processor match.abCrossfade
sonare mastering-pair-analyze source.wav --reference reference.wav --analysis match.referenceLoudness
sonare mastering-stereo-analyze left.wav --reference right.wav --analysis stereo.monoCompatCheck
```

## Features

- **Detection**: BPM (`float`), key (`Key(root, mode, confidence)`), beats (`list[float]` seconds), onsets (`list[float]` seconds)
- **Analysis**: Full music analysis (`AnalysisResult` with BPM, key, time signature, beat times)
- **Effects**: HPSS, pitch shift, time stretch, normalize, trim
- **Features**: STFT, mel spectrogram, MFCC, chroma, spectral features, pitch tracking (YIN / pYIN with voicing probabilities)
- **Conversions**: Hz / mel / MIDI / note, frames / time
- **I/O**: Load WAV / MP3 (and M4A/AAC/FLAC/OGG when built with FFmpeg), resample

## librosa-compatible defaults

| Parameter | Default |
|-----------|---------|
| Sample rate | 22050 Hz |
| `n_fft` | 2048 |
| `hop_length` | 512 |
| `n_mels` | 128 |
| `fmin` / `fmax` | 0.0 / `sr/2` |

## Also available

```bash
npm install @libraz/libsonare  # JavaScript / TypeScript (WASM)
```

## License

Apache-2.0
