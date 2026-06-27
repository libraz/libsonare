# libsonare

[![CI](https://img.shields.io/github/actions/workflow/status/libraz/libsonare/ci.yml?branch=main&label=CI)](https://github.com/libraz/libsonare/actions)
[![npm](https://img.shields.io/npm/v/@libraz/libsonare)](https://www.npmjs.com/package/@libraz/libsonare)
[![PyPI](https://img.shields.io/pypi/v/libsonare)](https://pypi.org/project/libsonare/)
[![codecov](https://codecov.io/gh/libraz/libsonare/branch/main/graph/badge.svg)](https://codecov.io/gh/libraz/libsonare)
[![License](https://img.shields.io/badge/license-Apache--2.0-blue)](https://github.com/libraz/libsonare/blob/main/LICENSE)
[![C++17](https://img.shields.io/badge/C%2B%2B-17-blue?logo=c%2B%2B)](https://en.cppreference.com/w/cpp/17)
[![Platform](https://img.shields.io/badge/platform-Linux%20%7C%20macOS%20%7C%20WebAssembly-lightgrey)](https://github.com/libraz/libsonare)
[![Docs](https://img.shields.io/badge/docs-libsonare.libraz.net-2563eb)](https://libsonare.libraz.net)

**From analysis to arrangement: a dependency-free audio engine for C++, Python,
Node.js, and the browser — librosa-compatible analysis, broadcast-grade
mastering and mixing, built-in instruments, and a realtime headless-DAW runtime,
all under one Apache-2.0 license.**

One C++ codebase powers native and WebAssembly: the same DSP that analyzes a
song, masters it, plays it back, and renders its MIDI through built-in
instruments runs identically in C++ and in the browser (WASM + AudioWorklet).
No runtime dependencies, no Python at runtime, no GPL/AGPL, no model weights.

📖 **[Documentation](https://libsonare.libraz.net)** &nbsp;·&nbsp; 🎧 **[Browser-local demos](https://libsonare.libraz.net/demos)** &nbsp;·&nbsp; **[Getting started](https://libsonare.libraz.net/docs/getting-started)**

## What's inside

- **Analysis (librosa-compatible)** — BPM, key, chords (HMM smoothing,
  inversions, key-context), beat/downbeat, time signature, sections, timbre,
  dynamics, pitch (YIN/pYIN), tempogram/PLP, NNLS chroma, EBU R128 loudness, and
  room acoustics (blind or IR-based RT60/EDT/C50/C80/D50). Defaults match librosa
  and are validated against librosa reference values in CI.
- **Mastering** — 76 named DSP processors (EQ, dynamics, multiband, stereo,
  saturation, repair, maximizer, reference matching) built against published
  references: ITU-R BS.1770-4 loudness and true-peak limiting, Linkwitz-Riley
  crossovers, Vicanek matched-Z biquads, ADAA-antialiased clippers, a Dempwolf
  12AX7 tube model, and polyphase FIR oversampling. Repair is classical DSP, not
  DNN separation.
- **Mixing & routing** — a real-time-safe channel-strip / bus model
  (denormal-guarded, lock-free parameter changes, plugin-delay compensation) with
  pan modes, sends, FX buses, metering, scene presets, and offline rendering.
- **Editing & creative FX** — time stretch / pitch shift, pitch correction, note
  stretch, voice change, five reverb engines, modulation effects, stereo delay,
  guitar amp sim, and ducking.
- **Room acoustics** — synthesize a room impulse response from shoebox geometry,
  blindly estimate an equivalent room from a recording, or morph a recording's
  reverberation toward a target room. Dependency-free and deterministic.
- **Built-in instruments** — a patch-driven NativeSynth (7 synthesis engines, mod
  matrix, named presets) with a data-free GM fallback covering all 128 programs +
  drums, so MIDI never renders silent. Add a host-supplied SoundFont and the
  GS-compatible 16-part SF2 player takes over, falling back per program.
- **Headless DAW runtime** — author projects with audio & MIDI tracks/clips
  (split/trim/move with undo/redo), takes and comp lanes, per-clip warp, MIDI
  1.0/2.0 sequencing, SMF and MIDI 2.0 Clip File I/O, deterministic byte-stable
  JSON, and offline bounce through the built-in instruments.
- **Realtime engine** — a sample-accurate, allocation-free playback engine:
  transport, clip playback with warp, paged streaming for huge clips, live MIDI
  input, lock-free automation, and capture/recording. The same engine runs in the
  browser through an AudioWorklet.

See the [documentation](https://libsonare.libraz.net) for the full API of every
feature, runtime, and processor.

## Installation

```bash
npm install @libraz/libsonare   # JavaScript / TypeScript (WASM, takes Float32Array)
pip install libsonare            # Python (WAV/MP3 — see "Audio formats" for M4A/AAC etc.)
```

For Node.js with native file decoding, build
[`@libraz/libsonare-native`](bindings/node/) from source (it auto-detects FFmpeg
via pkg-config). See the [installation guide](https://libsonare.libraz.net/docs/installation)
for native builds and FFmpeg options.

## Quick start

The snippets below cover the headline capabilities; the docs site has the full,
per-runtime API.

### JavaScript / TypeScript (WASM)

`@libraz/libsonare` accepts decoded `Float32Array` samples (use the Web Audio API
or a JS decoder to obtain them).

```typescript
import { init, analyze, detectKey, masterAudio } from '@libraz/libsonare';

await init();

const result = analyze(samples, sampleRate);   // BPM, key, chords, sections, ...
const key = detectKey(samples, sampleRate);     // { name: "C major", confidence: 0.95 }

// One-shot mastering with a named preset.
const mastered = masterAudio(samples, sampleRate, 'aiMusic', { 'loudness.targetLufs': -13 });
```

→ [JavaScript API](https://libsonare.libraz.net/docs/js-api) · [Browser / WASM](https://libsonare.libraz.net/docs/wasm)

### Python

`pip install libsonare` ships a WAV/MP3-only wheel. For other formats, pre-convert
with `ffmpeg` or rebuild with FFmpeg linked (see [Audio formats](#audio-formats)).

```python
import libsonare

audio = libsonare.Audio.from_file("song.mp3")
print(f"BPM: {audio.detect_bpm()}, Key: {audio.detect_key()}")

result = audio.mastering(target_lufs=-14.0, ceiling_db=-1.0)
print(f"{result.input_lufs:.1f} LUFS → {result.output_lufs:.1f} LUFS")
```

→ [Python API](https://libsonare.libraz.net/docs/python-api) · [CLI](https://libsonare.libraz.net/docs/cli)

The `sonare` command-line tool ships with the Python package
(`sonare analyze song.mp3`, `sonare mastering …`, `sonare project …`).

### C++

```cpp
#include "sonare.h"            // analysis + features + effects
#include "mastering/master.h"  // mastering chain & processors

auto audio = sonare::Audio::from_file("music.mp3");
auto result = sonare::MusicAnalyzer(audio).analyze();
std::cout << "BPM: " << result.bpm
          << ", Key: " << result.key.to_string() << std::endl;
```

→ [C++ API](https://libsonare.libraz.net/docs/cpp-api)

## Audio formats

| Format | Default¹ | With FFmpeg² | WASM (`@libraz/libsonare`) |
|--------|----------|--------------|----------------------------|
| WAV (PCM 16/24/32, float32) | ✅ | ✅ | n/a (samples in) |
| MP3 | ✅ | ✅ | n/a |
| M4A / AAC / FLAC / OGG / Opus / WMA / … | ❌ (clear error) | ✅ | n/a (use Web Audio API) |

¹ **Default**: the PyPI wheel and source builds without FFmpeg dev libs. Wheels
are pinned to this mode so installation never depends on your `libavformat`.

² **With FFmpeg**: a source build with FFmpeg linked — CMake auto-detects via
pkg-config (`-DSONARE_WITH_FFMPEG=AUTO`). Python: `SONARE_FFMPEG=1 pip install
libsonare --no-binary libsonare`; Node native: `SONARE_FFMPEG=1 yarn build`.

WASM bundles no file decoder by design — pass `Float32Array` samples from the Web
Audio API or another JS decoder.

## Build from source

```bash
make build && make test   # native; auto-detects FFmpeg, mastering + mixing on by default
make wasm                  # WebAssembly
make release               # optimized native build
```

Trim the binary with `-DBUILD_MASTERING=OFF` / `-DBUILD_MIXING=OFF` for
analysis-only builds. Optional, experimental, off-by-default macOS host backends
(CoreAudio / CoreMIDI / AU host) cover device I/O and AU hosting for source
builds; they ship in no published package. See the
[architecture docs](https://libsonare.libraz.net/docs/architecture) for build
options.

## Documentation

Full docs and browser-local demos live at
**[libsonare.libraz.net](https://libsonare.libraz.net)** ([demos](https://libsonare.libraz.net/demos)).

- **Learn** — [Introduction](https://libsonare.libraz.net/docs/introduction) · [Getting started](https://libsonare.libraz.net/docs/getting-started) · [Installation](https://libsonare.libraz.net/docs/installation) · [Examples](https://libsonare.libraz.net/docs/examples)
- **API by runtime** — [Browser / WASM](https://libsonare.libraz.net/docs/wasm) · [JavaScript](https://libsonare.libraz.net/docs/js-api) · [Python](https://libsonare.libraz.net/docs/python-api) · [Node.js native](https://libsonare.libraz.net/docs/native-bindings) · [C++](https://libsonare.libraz.net/docs/cpp-api) · [CLI](https://libsonare.libraz.net/docs/cli)
- **By task** — [Mastering processors](https://libsonare.libraz.net/docs/mastering-processors) · [Mixing](https://libsonare.libraz.net/docs/mixing) · [Editing DSP](https://libsonare.libraz.net/docs/editing-dsp) · [Realtime & streaming](https://libsonare.libraz.net/docs/realtime-streaming) · [Room acoustics](https://libsonare.libraz.net/docs/acoustic-analysis)
- **Details** — [Architecture](https://libsonare.libraz.net/docs/architecture) · [librosa compatibility](https://libsonare.libraz.net/docs/librosa-compatibility) · [Benchmarks](https://libsonare.libraz.net/docs/benchmarks) · [Glossary](https://libsonare.libraz.net/docs/glossary)

## Non-goals

libsonare is the headless engine, not an application. It intentionally does not
include a UI or DAW workflow, third-party plugin hosting (VST/CLAP), a
cross-platform real-time I/O abstraction, bundled sample data, or deep-learning
models. Callers own the audio callback and the UI; the experimental macOS
backends are the only (opt-in, unpublished) exception to the I/O boundary. These
limits keep the library dependency-free and Apache-2.0 pure. See
[Non-goals](https://libsonare.libraz.net/docs/architecture) for the rationale.

## License

[Apache-2.0](LICENSE)
