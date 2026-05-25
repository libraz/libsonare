# libsonare

[![PyPI](https://img.shields.io/pypi/v/libsonare)](https://pypi.org/project/libsonare/)
[![npm](https://img.shields.io/npm/v/@libraz/libsonare)](https://www.npmjs.com/package/@libraz/libsonare)
[![License](https://img.shields.io/badge/license-Apache--2.0-blue)](https://github.com/libraz/libsonare/blob/main/LICENSE)

Audio analysis library for Python.

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

# Advanced key options are opt-in; defaults preserve existing behavior.
key_with_options = audio.detect_key(
    use_hpss=True,
    loudness_weighted=True,
    high_pass_hz=80.0,
)

result = audio.analyze()  # BPM + key + time signature + beats
print(f"BPM: {result.bpm:.1f}  Key: {result.key.root.name} {result.key.mode.name}")
```

### Room acoustics

Use `detect_acoustic` for blind RT60/EDT estimation from ordinary audio.
Use `analyze_impulse_response` when you have a measured impulse response and
need clarity metrics (`c50`, `c80`, `d50`). Blind mode returns `nan` for
clarity metrics because they are not reliable without an impulse response.

```python
import libsonare

audio = libsonare.Audio.from_file("recording.wav")
blind = audio.detect_acoustic(
    n_octave_bands=6,
    n_third_octave_subbands=24,
    min_decay_db=30.0,
    noise_floor_margin_db=10.0,
)
print(blind.rt60, blind.edt, blind.is_blind)

ir = libsonare.Audio.from_file("room_ir.wav")
params = ir.analyze_impulse_response()
print(params.rt60, params.c50, params.c80, params.d50)
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

### Mastering chain

`mastering_chain` runs the full configurable mastering pipeline (EQ, dynamics,
saturation, repair, stereo, loudness, ...). The Python binding accepts **flat
dot-notation keys** for the config dict — addressing any module parameter
directly — and an optional `on_progress(progress, stage)` callback that is
invoked after each stage (progress in ``[0, 1]``).

```python
import libsonare

result = libsonare.mastering_chain(
    samples,
    sample_rate=sr,
    config={
        "eq.tilt.tiltDb": 0.5,
        "dynamics.compressor.thresholdDb": -24.0,
        "dynamics.compressor.ratio": 1.5,
        "dynamics.transientShaper.attackGainDb": 2.0,
        "repair.declick.enabled": True,
        "loudness.targetLufs": -14.0,
        "loudness.ceilingDb": -1.0,
    },
    on_progress=lambda p, stage: print(f"[{p * 100:5.1f}%] {stage}"),
)

stereo_result = libsonare.mastering_chain_stereo(
    left, right, sample_rate=sr,
    config={"stereo.imager.width": 1.1, "loudness.targetLufs": -14.0},
)
```

`MasteringChainResult` exposes the rendered samples plus loudness telemetry
(`input_lufs`, `output_lufs`, `applied_gain_db`, `latency_samples`).

### Mastering presets

Named presets ship sensible defaults for common targets. `master_audio`
applies a preset and lets you override any individual parameter using the
same flat dot-notation keys as `mastering_chain`.

```python
import libsonare

libsonare.mastering_preset_names()
# -> ['pop', 'edm', 'acoustic', 'hipHop', 'aiMusic', 'speech', 'streaming', 'youtube', 'broadcast', 'podcast', 'audiobook', 'cinema', 'jpop', 'ambient', 'lofi', 'classical', 'drumAndBass', 'techno', 'metal', 'trap', 'rnb', 'jazz', 'kpop', 'trance', 'gameOst']

result = libsonare.master_audio(
    samples,
    sample_rate=sr,
    preset="aiMusic",
    overrides={
        "loudness.targetLufs": -13.0,
        "dynamics.multibandComp.enabled": True,
    },
)

# Audio shortcut
audio = libsonare.Audio.from_file("song.wav")
pop_mastered = audio.master_audio("pop")
```

### Mixing

```python
import libsonare

libsonare.mixing_scene_preset_names()
# -> ['vocalReverbSend', ...]
scene_json = libsonare.mixing_scene_preset_json("vocalReverbSend")

offline = libsonare.mix_stereo(
    [(vocal_l, vocal_r), (music_l, music_r)],
    sample_rate=sr,
    input_trim_db=[3.0, 0.0],
    fader_db=[-3.0, -12.0],
    pan=[0.0, -0.2],
    width=[1.0, 0.9],
)

mixer = libsonare.Mixer.from_scene_json(scene_json, sample_rate=sr, block_size=512)
try:
    block_l, block_r = mixer.process_stereo(
        [vocal_block_l, return_block_l],
        [vocal_block_r, return_block_r],
    )
finally:
    mixer.close()
```

### Streaming mastering chain

`StreamingMasteringChain` runs the same pipeline block-by-block for real-time
or chunked workflows. The constructor takes the same flat dot-notation config
as `mastering_chain`; non-streamable stages (`repair.denoise`, `loudness`)
cause the constructor to raise, so omit those keys for streaming use. The
object is also a context manager.

```python
import libsonare

with libsonare.StreamingMasteringChain({
    "eq.tilt.tiltDb": 0.5,
    "dynamics.compressor.thresholdDb": -20.0,
    "dynamics.transientShaper.attackGainDb": 1.5,
}) as chain:
    chain.prepare(sample_rate=48000, max_block_size=512, num_channels=1)
    print(chain.stage_names())
    print(f"latency = {chain.latency_samples()} samples")

    out_block = chain.process_mono([0.0] * 512)
    # Stereo:
    # chain.prepare(48000, 512, 2)
    # out_l, out_r = chain.process_stereo(left_block, right_block)
    chain.reset()
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
key_hpss = libsonare.detect_key(
    samples,
    sample_rate=22050,
    n_fft=4096,
    hop_length=512,
    use_hpss=True,
    loudness_weighted=True,
    high_pass_hz=80.0,
)
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
