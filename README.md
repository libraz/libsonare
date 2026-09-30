# libsonare

**libsonare turns audio into data and data back into audio.** Load a song and get its BPM, key, chords, and structure; master and mix it to broadcast loudness; turn MIDI into sound with built-in instruments; or build a whole DAW on top — the same engine in C++, Python, Node.js, and the browser. The C++ core has zero runtime dependencies; the Python package depends on NumPy, and there is no GPL/AGPL code and no model weights.

📖 **[Documentation](https://libsonare.libraz.net)** &nbsp;·&nbsp; 🎧 **[Browser-local demos](https://libsonare.libraz.net/demos)** &nbsp;·&nbsp; **[Getting started](https://libsonare.libraz.net/docs/getting-started)**

## sonare studio

**[sonare studio](https://sonare-studio.libraz.net)** is a browser-based DAW built entirely on the libsonare WASM engine: multi-track sequencing, piano roll, score engraving, mixer, mastering, and WAV/MP3/MIDI/MusicXML export, all client-side. It is a hosted live demo that exercises the whole engine end to end, not a production product (source not public).

## What's inside

- **Analysis** — BPM, key, chords, beats, sections, pitch, loudness (EBU R128), and room acoustics; defaults match librosa where they overlap, and CI checks them against librosa reference values. [Analysis](https://libsonare.libraz.net/docs/analysis)
- **Stems and transcription** — split a mix into stems with `decomposeStems`, and transcribe audio to MIDI with `transcribe`. [Source separation](https://libsonare.libraz.net/docs/analysis#source-separation) · [CLI `transcribe`](https://libsonare.libraz.net/docs/cli-examples)
- **Mastering** — 91 distinct named DSP processors (EQ, dynamics, multiband, stereo, saturation, repair, maximizer, reference matching), or 73 with `BUILD_FX=OFF`, plus five restoration presets (vinyl, tape hiss, field recording, voice memo, shellac 78). [Mastering processors](https://libsonare.libraz.net/docs/mastering-processors) · [Mastering assistant](https://libsonare.libraz.net/docs/mastering-assistant)
- **Mixing and routing** — channel strips, buses, sends, bus-to-bus routing, track- or bus-keyed sidechains, scene presets, and an optional assistant that suggests a mixer scene. [Mixing](https://libsonare.libraz.net/docs/mixing) · [Realtime engine](https://libsonare.libraz.net/docs/realtime-engine) · [Mixing assistant](https://libsonare.libraz.net/docs/mixing-assistant)
- **Editing and creative FX** — time stretch, pitch shift, pitch correction, voice change, reverbs, modulation, delay, and amp sim. [Editing DSP](https://libsonare.libraz.net/docs/editing-dsp) · [Spectral editing](https://libsonare.libraz.net/docs/spectral-editing)
- **Note editing and takes** — edit notes of monophonic and polyphonic takes, align a take to a reference (`alignTakeToReference`), and tune it to a MIDI melody (`tune-to-midi`). [MIDI editing](https://libsonare.libraz.net/docs/project-editing-midi) · [Takes](https://libsonare.libraz.net/docs/recording-and-takes)
- **Room acoustics** — synthesize, estimate, and morph room impulse responses. [Acoustic analysis](https://libsonare.libraz.net/docs/acoustic-analysis)
- **Built-in instruments** — a NativeSynth with 17 synthesis engines and a GM/GS fallback covering all 128 programs, so MIDI never renders silent. The acoustic piano is calibrated; the other physical-model voices are still being tuned and will change in 1.8.x patch releases. [NativeSynth](https://libsonare.libraz.net/docs/native-synth) · [Physical models](https://libsonare.libraz.net/docs/physical-models)
- **GS and SoundFont** — GS SysEx reception, two realisations of the GS insertion effects (modern and classic), and a GS-compatible 16-part SF2 player for host-supplied SoundFonts. [GM/GS](https://libsonare.libraz.net/docs/gm-gs) · [SoundFont player](https://libsonare.libraz.net/docs/soundfont-player)
- **Headless DAW runtime** — projects with audio and MIDI tracks, takes, warp, MIDI 1.0/2.0 sequencing, SMF I/O, and offline bounce. [Project editing](https://libsonare.libraz.net/docs/project-editing)
- **Realtime engine** — allocation-free playback, streaming, live MIDI 1.0/2.0 input, lock-free automation, and recording, in the browser through an AudioWorklet too. [Realtime engine](https://libsonare.libraz.net/docs/realtime-engine) · [MIDI input](https://libsonare.libraz.net/docs/midi-input)
- **Playback renderer** — channel conversion, loudness matching, bass management, and HRTF binaural rendering for headphones and speakers. [Playback](https://libsonare.libraz.net/docs/playback)
- **C++ package** — the C++ library installs as a CMake package (`find_package(sonare)`). [C++ API](https://libsonare.libraz.net/docs/cpp-api)

## Installation

```bash
npm install @libraz/libsonare   # JavaScript / TypeScript (WASM, takes Float32Array)
pip install libsonare            # Python (WAV/MP3; other formats need an FFmpeg build)
```

[`@libraz/libsonare-native`](bindings/node/) is not published to npm; clone this repository and use it as a local dependency. See [Installation](https://libsonare.libraz.net/docs/installation) for formats, FFmpeg, and the runtime choice.

## Quick start

### JavaScript / TypeScript (WASM)

```typescript
import { Audio, init } from '@libraz/libsonare';

await init();

const bytes = new Uint8Array(await file.arrayBuffer());
const audio = await Audio.fromMemoryWithBrowserFallback(bytes);
const result = audio.analyze(); // BPM, key, chords, sections, ...
console.log(result.key.name);
```

→ [JavaScript API](https://libsonare.libraz.net/docs/js-api) · [Browser / WASM](https://libsonare.libraz.net/docs/wasm)

### Python

```python
import libsonare

audio = libsonare.Audio.from_file("song.mp3")
print(f"BPM: {audio.detect_bpm()}, Key: {audio.detect_key()}")

result = audio.mastering(target_lufs=-14.0, ceiling_db=-1.0)
print(f"{result.input_lufs:.1f} LUFS → {result.output_lufs:.1f} LUFS")
```

The `sonare` command-line tool ships with the Python package; the native CLI is `sonare-cli`. → [Python API](https://libsonare.libraz.net/docs/python-api) · [CLI](https://libsonare.libraz.net/docs/cli)

### C++

```cmake
find_package(sonare REQUIRED)
target_link_libraries(app PRIVATE sonare::sonare)
```

```cpp
#include "sonare.h"

auto audio = sonare::Audio::from_file("music.mp3");
auto result = sonare::MusicAnalyzer(audio).analyze();
std::cout << "BPM: " << result.bpm
          << ", Key: " << result.key.to_string() << std::endl;
```

→ [C++ API](https://libsonare.libraz.net/docs/cpp-api)

## Build from source

```bash
make build && make test   # native
make wasm                  # WebAssembly
make release               # optimized native build
```

Build options (`BUILD_MASTERING`, `BUILD_MIXING`, `BUILD_MIXING_ASSISTANT`, FFmpeg) are covered in [Architecture](https://libsonare.libraz.net/docs/architecture).

## Documentation

Full docs and browser-local demos live at **[libsonare.libraz.net](https://libsonare.libraz.net)**.

- **Learn** — [Introduction](https://libsonare.libraz.net/docs/introduction) · [Getting started](https://libsonare.libraz.net/docs/getting-started) · [Installation](https://libsonare.libraz.net/docs/installation) · [Examples](https://libsonare.libraz.net/docs/examples)
- **API by runtime** — [Browser / WASM](https://libsonare.libraz.net/docs/wasm) · [JavaScript](https://libsonare.libraz.net/docs/js-api) · [Python](https://libsonare.libraz.net/docs/python-api) · [Node.js native](https://libsonare.libraz.net/docs/native-bindings) · [C++](https://libsonare.libraz.net/docs/cpp-api) · [CLI](https://libsonare.libraz.net/docs/cli)
- **Details** — [Architecture](https://libsonare.libraz.net/docs/architecture) · [librosa compatibility](https://libsonare.libraz.net/docs/librosa-compatibility) · [Benchmarks](https://libsonare.libraz.net/docs/benchmarks) · [Glossary](https://libsonare.libraz.net/docs/glossary)

Every runtime calls the same C++17 DSP core, but each API surface is hand-written and not identical; see [Binding parity](https://libsonare.libraz.net/docs/binding-parity) and the generated [runtime capability matrix](tools/parity/surface-coverage.md).

## Non-goals

libsonare is the headless engine, not an application. It does not include a UI or DAW workflow, VST/CLAP plugin hosting, a cross-platform real-time I/O abstraction, bundled sample data, or deep-learning models. Windows is not supported; use Linux, macOS, WebAssembly, or WSL2. Note-level composition belongs to the separate [`@libraz/libcantus`](https://github.com/libraz/libcantus). See [Architecture](https://libsonare.libraz.net/docs/architecture#non-goals) for the rationale.

## License

[Apache-2.0](LICENSE)
