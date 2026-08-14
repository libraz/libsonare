# libsonare

[![PyPI](https://img.shields.io/pypi/v/libsonare)](https://pypi.org/project/libsonare/)
[![npm](https://img.shields.io/npm/v/@libraz/libsonare)](https://www.npmjs.com/package/@libraz/libsonare)
[![License](https://img.shields.io/badge/license-Apache--2.0-blue)](https://github.com/libraz/libsonare/blob/main/LICENSE)
[![Docs](https://img.shields.io/badge/docs-libsonare.libraz.net-2563eb)](https://libsonare.libraz.net)

**Turn audio into data and back, from Python.** Analyze songs (BPM, key, chords,
loudness), master and mix to broadcast loudness, and render MIDI through built-in
instruments — a fast C++ core with NumPy as its only dependency.

Mastering ships 88 named DSP processors implemented against published references
(ITU-R BS.1770-4 true-peak limiting, Linkwitz-Riley crossovers, Vicanek matched-Z
biquads, ADAA-antialiased saturation); analysis defaults match librosa where the
two overlap (validated against generated librosa reference values in CI).
Apache-2.0, no model weights.

📖 **Full API reference, guides, and CLI docs: [libsonare.libraz.net](https://libsonare.libraz.net)**

## Installation

```bash
pip install libsonare
```

Supported platforms: Linux (x86_64, aarch64), macOS (Apple Silicon).

## Quick Start

`Audio` is the recommended entry point: it decodes files and caches samples. The
top-level `libsonare.detect_*` / `libsonare.analyze` functions are thin wrappers
for one-shot calls on a numpy array.

```python
import libsonare

audio = libsonare.Audio.from_file("song.mp3")  # or "song.wav"
result = audio.analyze()  # BPM + key + time signature + beats
print(f"BPM: {result.bpm:.1f}  Key: {result.key.root.name} {result.key.mode.name}")

# Master toward a target loudness with a named preset
mastered = libsonare.master_audio(
    audio.data, sample_rate=audio.sample_rate, preset_name="streaming",
)
print(mastered.output_lufs, mastered.applied_gain_db)
```

Analyze a numpy array directly (mono float32; downmix stereo first):

```python
import numpy as np

samples = np.asarray(my_mono_float32_signal, dtype=np.float32)
bpm = libsonare.detect_bpm(samples, sample_rate=22050)
key = libsonare.detect_key(samples, sample_rate=22050)  # Key(root, mode, confidence)
```

Render a MIDI arrangement through a built-in instrument with the headless
`Project` (a context manager):

```python
with libsonare.Project() as project:
    project.set_sample_rate(48000)
    _, clip_id = project.add_midi_clip(0.0, 4.0)
    project.set_midi_events(clip_id, [
        libsonare.Project.midi_note_on(0.0, 0, 0, 60, 100),  # ppq, group, channel, note, velocity
        libsonare.Project.midi_note_off(2.0, 0, 0, 60),
    ])
    audio = project.bounce_with_synth_instrument("saw-lead", num_channels=2)
```

## Capabilities

Every area below has runnable examples and the full API in the
[documentation](https://libsonare.libraz.net/docs/python-api). The functional and
`Audio`-method forms return identical results; `Audio` caches decoded samples and
is preferred when doing more than one computation on the same signal.

- **Analysis** — BPM, key (+ candidates), chords, downbeats, sections, melody, tuning; pitch (YIN / pYIN), timbre, and the full spectral feature set (STFT, mel, MFCC, chroma, CQT/VQT, spectral contrast); metering (`metering_*`, `waveform_peaks`). → [Python API](https://libsonare.libraz.net/docs/python-api)
- **Mastering** — 88 named DSP processors, the configurable `mastering_chain`, 25 named presets via `master_audio`, dynamics / repair specialist functions, and reference-matching. → [Mastering processors](https://libsonare.libraz.net/docs/mastering-processors)
- **Mixing** — offline `mix_stereo` and the block-based `Mixer` with scene presets. → [Mixing](https://libsonare.libraz.net/docs/mixing)
- **Editing DSP** — time-stretch, pitch-shift, HPSS (+ residual), phase vocoder, normalize, trim, remix. → [Editing DSP](https://libsonare.libraz.net/docs/editing-dsp)
- **Room acoustics** — blind RT60 / EDT, impulse-response clarity metrics, `estimate_room`, `synthesize_rir`, `room_morph`. → [Room acoustics](https://libsonare.libraz.net/docs/acoustic-analysis)
- **Realtime & streaming** — `RealtimeEngine` (transport / MIDI / render / capture), `StreamAnalyzer`, `StreamingMasteringChain`, `RealtimeVoiceChanger`. → [Realtime & streaming](https://libsonare.libraz.net/docs/realtime-streaming)
- **Instruments & synthesis** — built-in oscillator synth, patch-driven NativeSynth (15 synthesis engines, incl. physically-modeled piano / strings / winds — being tuned over time), and a GS-compatible SoundFont (SF2) player. → [Python API](https://libsonare.libraz.net/docs/python-api)
- **Headless DAW** — `Project` arrangement model: audio / MIDI tracks and clips, undo/redo, SMF / MIDI 2.0 Clip File I/O, deterministic JSON, offline `bounce`. → [Python API](https://libsonare.libraz.net/docs/python-api)
- **Conversions** — Hz / mel / MIDI / note, frames / time, resample.

Native return-code failures, including native input/parameter validation, raise
`libsonare.SonareError` (a `RuntimeError` subclass carrying a numeric `.code`).
Python-side preflight validation of empty / NaN / Inf buffers and bad shapes
raises `ValueError`.

## CLI

The `sonare` command exposes the analysis, mastering, mixing, effects, and
project surfaces. A few representative commands:

```bash
sonare analyze song.mp3                              # BPM + key summary
sonare bpm song.mp3 --json                           # {"bpm": 161.0}
sonare master song.wav -o mastered.wav --preset pop  # preset mastering
sonare voice-change vocal.wav -o out.wav --preset bright-idol
sonare project bounce --in project.json -o out.wav --synth saw-lead
```

Run `sonare --help` (or `sonare <command> --help`), or see the
[CLI reference](https://libsonare.libraz.net/docs/cli) for the full command list.

## Realtime voice changer preset schemas

The wheel includes JSON Schema documents for third-party voice changer presets.
Use `importlib.resources` to obtain them instead of copying a schema into an
application:

```python
from importlib.resources import files

preset_schema = files("libsonare").joinpath(
    "schemas/realtime-voice-changer-preset.schema.json"
)
```

Validate data against this schema before saving it, then call
`validate_realtime_voice_changer_preset_json()` before applying it. The runtime
check is authoritative and also rejects malformed JSON such as duplicate keys.

## Supported audio formats

| Format | Default build | With FFmpeg support |
|--------|---------------|---------------------|
| WAV (PCM 16/24/32, float32) | yes | yes |
| MP3 | yes | yes |
| M4A / AAC / FLAC / OGG / Opus / WMA / ... | no | yes |

The PyPI wheels are pinned to **`SONARE_WITH_FFMPEG=OFF`** so the distributed
wheel never silently links against the build host's FFmpeg. To enable
FFmpeg-backed decoding, build from source with `SONARE_FFMPEG=1` (see the
[installation guide](https://libsonare.libraz.net/docs/installation)); this links
against the system FFmpeg shared libraries (LGPL by default), so install them
first (`brew install ffmpeg`, or `apt install libavformat-dev libavcodec-dev
libavutil-dev libswresample-dev`).

## Input format expectations

| API | dtype | shape | range |
|-----|-------|-------|-------|
| `Audio.from_buffer(samples, sample_rate=...)` | float32 (float64 also accepted) | 1D mono | nominally `[-1.0, 1.0]` |
| `Audio.from_memory(data)` | `bytes` of an encoded WAV / MP3 / (FFmpeg) file | — | — |
| `Audio.from_file(path)` | path to an encoded audio file | — | — |
| `libsonare.detect_bpm(samples, sample_rate=...)` etc. | float32 (float64 also accepted) | 1D mono | nominally `[-1.0, 1.0]` |

Stereo input passed as samples is **not** downmixed automatically — downmix
yourself (e.g. `samples.mean(axis=1, dtype=np.float32)`). File loaders downmix to
mono internally.

## librosa-compatible defaults

| Parameter | Default |
|-----------|---------|
| Sample rate | 22050 Hz |
| `n_fft` | 2048 |
| `hop_length` | 512 |
| `n_mels` | 128 |
| `fmin` / `fmax` | 0.0 / `sr/2` |

## Documentation

Full API reference and guides live at
**[libsonare.libraz.net](https://libsonare.libraz.net)**
([getting started](https://libsonare.libraz.net/docs/getting-started) ·
[Python API](https://libsonare.libraz.net/docs/python-api) ·
[CLI](https://libsonare.libraz.net/docs/cli)).

## Also available

```bash
npm install @libraz/libsonare  # JavaScript / TypeScript (WASM)
```

## License

Apache-2.0
