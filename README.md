# libsonare

[![CI](https://img.shields.io/github/actions/workflow/status/libraz/libsonare/ci.yml?branch=main&label=CI)](https://github.com/libraz/libsonare/actions)
[![npm](https://img.shields.io/npm/v/@libraz/libsonare)](https://www.npmjs.com/package/@libraz/libsonare)
[![PyPI](https://img.shields.io/pypi/v/libsonare)](https://pypi.org/project/libsonare/)
[![codecov](https://codecov.io/gh/libraz/libsonare/branch/main/graph/badge.svg)](https://codecov.io/gh/libraz/libsonare)
[![License](https://img.shields.io/badge/license-Apache--2.0-blue)](https://github.com/libraz/libsonare/blob/main/LICENSE)
[![C++17](https://img.shields.io/badge/C%2B%2B-17-blue?logo=c%2B%2B)](https://en.cppreference.com/w/cpp/17)
[![Platform](https://img.shields.io/badge/platform-Linux%20%7C%20macOS%20%7C%20WebAssembly-lightgrey)](https://github.com/libraz/libsonare)
[![Docs](https://img.shields.io/badge/docs-libsonare.libraz.net-2563eb)](https://libsonare.libraz.net)

**libsonare turns audio into data and data back into audio.** Load a song and get
its BPM, key, chords, and structure; master and mix it to broadcast loudness; turn
MIDI into sound with built-in instruments; or build a whole DAW on top — the same
engine in C++, Python, Node.js, and the browser, with zero runtime dependencies,
no Python at runtime, and no GPL/AGPL or model weights.

**Reach for it when you need to:**

- **Analyze audio** — BPM, key, chords, sections, loudness — without pulling in a heavy Python/ML stack.
- **Master or mix to spec** — broadcast-grade loudness and true-peak control, in-process or fully in the browser.
- **Turn MIDI into sound** — built-in instruments cover all 128 GM programs + drums, no SoundFont required.
- **Ship one audio engine** — the same C++ DSP runs natively and in the browser (WASM + AudioWorklet), with identical results.

📖 **[Documentation](https://libsonare.libraz.net)** &nbsp;·&nbsp; 🎧 **[Browser-local demos](https://libsonare.libraz.net/demos)** &nbsp;·&nbsp; **[Getting started](https://libsonare.libraz.net/docs/getting-started)**

## What can you build with it?

**[sonare studio](https://sonare-studio.libraz.net)** is a full browser-based
DAW built entirely on the libsonare WASM engine — multi-track sequencing, a
piano roll, score engraving, a mixer, mastering, and WAV/MP3/MIDI/MusicXML
export, all running client-side. It shows how far one Apache-2.0 engine reaches,
from analysis to a playable, exportable arrangement.

It's a hosted live demo that drives the whole engine end-to-end as an
integration test bed, not a production product (source not public). Try it in
the browser to see what libsonare can power.

## What's inside

- **Analysis** — BPM, key, chords (HMM smoothing, inversions, key-context),
  beat/downbeat, time signature, sections, timbre, dynamics, pitch (YIN/pYIN),
  tempogram/PLP, NNLS chroma, EBU R128 loudness, and room acoustics (blind or
  IR-based RT60/EDT/C50/C80/D50). Where it overlaps librosa, defaults match and
  are validated against librosa reference values in CI — so results port over
  without surprises.
- **Mastering** — 88 distinct named DSP processors (EQ, dynamics, multiband,
  stereo, saturation, repair, maximizer, reference matching), or 71 with
  `BUILD_FX=OFF` (the creative streaming effects are omitted), built against
  published
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
- **Built-in instruments** — a patch-driven NativeSynth with 15 synthesis engines
  (subtractive, FM, additive plus physically-modeled piano, bowed strings, reeds,
  brass, flute, pipe organ, plucked strings, voice, free reed, and percussion), a
  mod matrix, and named presets,
  backed by a data-free GM fallback covering all 128 programs + drums, so MIDI
  never renders silent. Add a host-supplied SoundFont and the GS-compatible
  16-part SF2 player takes over, falling back per program. The physical-model
  voices are usable today and being refined over time as tuning continues.
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

[`@libraz/libsonare-native`](bindings/node/) is not published to npm. Clone this
repository and use it as a local dependency; its README covers the native build
and FFmpeg options.

## Quick start

The snippets below cover the headline capabilities; the docs site has the full,
per-runtime API.

> **Which runtime, and does it decode files for you?** WASM decodes WAV/MP3 and
> falls back to the browser codec stack for other browser-supported formats;
> Python and the Node native addon read files directly. →
> [Choose your runtime](https://libsonare.libraz.net/docs/getting-started#choose-your-runtime)

### JavaScript / TypeScript (WASM)

Start with encoded audio bytes. `Audio.fromMemoryWithBrowserFallback()` uses the
built-in WAV/MP3 decoder first, then `AudioContext.decodeAudioData()` for formats
such as M4A, AAC, FLAC, and OGG that the browser supports.

Top-level one-shot APIs use a request object as their documented form. Positional
calls remain supported for compatibility; stateful methods and small scalar helpers
keep their natural forms.

```typescript
import { Audio, init } from '@libraz/libsonare';

await init();

const bytes = new Uint8Array(await file.arrayBuffer());
const audio = await Audio.fromMemoryWithBrowserFallback(bytes);
const result = audio.analyze(); // BPM, key, chords, sections, ...
console.log(result.key.name);

// If samples are already decoded, pass a Float32Array directly instead.
const decoded = Audio.fromBuffer(samples, sampleRate);
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
The separately released native CLI is named `sonare-cli`, so both commands can
coexist on `PATH` without changing their behavior.

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

### Instruments & MIDI

Turn a MIDI arrangement into audio with the built-in instruments — no SoundFont
required. Build a `Project`, add notes, and bounce it through a NativeSynth preset.

```python
import libsonare

with libsonare.Project() as project:
    project.set_sample_rate(48000)
    _, clip_id = project.add_midi_clip(0.0, 4.0)             # start, length (quarter notes)
    project.set_midi_events(clip_id, [
        libsonare.Project.midi_note_on(0.0, 0, 0, 60, 100),  # ppq, group, channel, note, velocity
        libsonare.Project.midi_note_off(2.0, 0, 0, 60),
    ])
    audio = project.bounce_with_synth_instrument("e-piano", num_channels=2)  # → float32 audio
```

The same `Project` / bounce API is available in every runtime; add a host-supplied
SoundFont to render through the GS-compatible SF2 player instead.

→ [Python API](https://libsonare.libraz.net/docs/python-api) · [Documentation](https://libsonare.libraz.net)

## Audio formats

| Format | Default¹ | With FFmpeg² | WASM (`@libraz/libsonare`) |
|--------|----------|--------------|----------------------------|
| WAV (PCM 16/24/32, float32) | ✅ | ✅ | built-in decoder |
| MP3 | ✅ | ✅ | built-in decoder |
| M4A / AAC / FLAC / OGG / Opus / WMA / … | ❌ (clear error) | ✅ | browser codec fallback where supported |

¹ **Default**: the PyPI wheel and source builds without FFmpeg dev libs. Wheels
are pinned to this mode so installation never depends on your `libavformat`.

² **With FFmpeg**: a source build with FFmpeg linked — CMake auto-detects via
pkg-config (`-DSONARE_WITH_FFMPEG=AUTO`). Python: `SONARE_FFMPEG=1 pip install
libsonare --no-binary libsonare`; Node native: `SONARE_FFMPEG=1 yarn build`.

The WASM bundle intentionally excludes large codec libraries. Its small built-in
WAV/MP3 decoder and browser `decodeAudioData()` fallback cover the normal browser
entry point; pass a `Float32Array` when samples are already decoded.

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
models. Windows is not supported; use Linux, macOS, WebAssembly, or WSL2. Callers
own the audio callback and the UI; the experimental macOS backends are the only
(opt-in, unpublished) exception to the I/O boundary. These limits keep the library
dependency-free and Apache-2.0 pure. See
[Non-goals](https://libsonare.libraz.net/docs/architecture) for the rationale.

## License

[Apache-2.0](LICENSE)
