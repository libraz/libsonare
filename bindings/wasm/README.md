# libsonare

[![CI](https://img.shields.io/github/actions/workflow/status/libraz/libsonare/ci.yml?branch=main&label=CI)](https://github.com/libraz/libsonare/actions)
[![npm](https://img.shields.io/npm/v/@libraz/libsonare)](https://www.npmjs.com/package/@libraz/libsonare)
[![npm downloads](https://img.shields.io/npm/dm/@libraz/libsonare)](https://www.npmjs.com/package/@libraz/libsonare)
[![types](https://img.shields.io/npm/types/@libraz/libsonare)](https://www.npmjs.com/package/@libraz/libsonare)
[![License](https://img.shields.io/github/license/libraz/libsonare)](https://github.com/libraz/libsonare/blob/main/LICENSE)
[![Docs](https://img.shields.io/badge/docs-libsonare.libraz.net-2563eb)](https://libsonare.libraz.net)
[![PyPI](https://img.shields.io/pypi/v/libsonare?label=PyPI)](https://pypi.org/project/libsonare/)

**Turn audio into data and back — entirely in the browser.** Analyze songs
(BPM, key, chords, loudness), master and mix to broadcast loudness, and render
MIDI through built-in instruments, all client-side via WebAssembly — the same
C++ engine that runs natively, with zero dependencies and no Python or model
weights. 76 named mastering DSP processors implemented against published
references (ITU-R BS.1770-4 true-peak limiting, Linkwitz-Riley crossovers,
Vicanek matched-Z biquads, ADAA-antialiased saturation); analysis defaults match
librosa where the two overlap.

## Try it in the browser

Everything runs client-side — no server, nothing uploaded.

- 🎧 **[Live demos](https://libsonare.libraz.net/demos)** — analyze a song (BPM / key / chords), master to a target loudness, mix, and render MIDI through the built-in instruments, all in the page.
- 🎛️ **[sonare studio](https://sonare-studio.libraz.net)** — a full browser DAW (multi-track sequencing, piano roll, mixer, mastering, WAV / MP3 / MIDI / MusicXML export) built entirely on this WASM engine. It shows how far one Apache-2.0 engine reaches, from analysis to a playable, exportable arrangement.
- 📖 **[Documentation & getting started](https://libsonare.libraz.net/docs/getting-started)**

## Installation

```bash
npm install @libraz/libsonare
```

## Quick Start

`init()` loads the WASM module once; every API is available afterwards. Top-level
one-shot functions accept a request object (recommended) or positional arguments.

> **Audio input:** analysis works on decoded `Float32Array` mono samples.
> `Audio.fromMemory` decodes WAV from an in-memory buffer, but the WebAssembly
> build bundles no decoder for compressed formats — use the Web Audio API
> (`decodeAudioData`) for MP3 / M4A / AAC / Opus / FLAC, or the native N-API
> package [`@libraz/libsonare-native`](https://github.com/libraz/libsonare/tree/main/bindings/node)
> to read files from disk.

> **Platform constraints:** the WebAssembly build is single-threaded (analysis
> runs to completion on the calling thread — there is no non-blocking variant)
> and has no host filesystem access. Drive long-running calls from a Web Worker
> to keep the UI responsive.

```typescript
import { init, analyze, masterAudio } from '@libraz/libsonare';

await init();

// Analyze decoded Float32Array mono samples
const { bpm, key } = analyze({ samples, sampleRate });
console.log(`BPM: ${bpm}  Key: ${key.name}`);

// Master toward a target loudness with a named preset
const mastered = masterAudio({ samples, sampleRate, preset: 'streaming' });
console.log(mastered.outputLufs, mastered.appliedGainDb);
```

Render a MIDI arrangement through a built-in instrument with the headless
`Project`. The embind handle is not garbage-collected — call `delete()` when done.

```typescript
import { init, Project } from '@libraz/libsonare';

await init();

const project = new Project();
try {
  const { clipId } = project.addMidiClip(0, 4);
  project.setMidiEvents(clipId, [
    Project.midiNoteOn(0, 0, 0, 60, 100), // ppq, group, channel, note, velocity
    Project.midiNoteOff(1, 0, 0, 60),
  ]);
  const audio = project.bounceWithSynthInstrument('saw-lead', { numChannels: 2 });
} finally {
  project.delete();
}
```

### Decoding audio in the browser

The WASM build takes decoded samples, so decode with the Web Audio API first:

```typescript
const buf = await fetch('song.m4a').then((r) => r.arrayBuffer());
const decoded = await new AudioContext().decodeAudioData(buf);
const { bpm, key } = analyze({ samples: decoded.getChannelData(0), sampleRate: decoded.sampleRate });
```

### Loading the `.wasm` file

Bundlers that don't auto-resolve the `.wasm` asset need its URL. Pass a
`locateFile` resolver to `init()`:

```typescript
import wasmUrl from '@libraz/libsonare/wasm?url'; // Vite; adapt per bundler

await init({ locateFile: (path) => (path.endsWith('.wasm') ? wasmUrl : path) });
```

From a CDN, `import { init } from 'https://esm.sh/@libraz/libsonare'` resolves the
`.wasm` automatically. See the
[getting-started guide](https://libsonare.libraz.net/docs/getting-started) for
per-bundler setup and the AudioWorklet bridge.

## Capabilities

Every area below has runnable examples and the full API in the
[documentation](https://libsonare.libraz.net/docs/wasm).

- **Analysis** — BPM, key (+ candidates), chords, downbeats, sections, melody, tuning; pitch (YIN / pYIN), timbre, and the full spectral feature set (STFT, mel, MFCC, chroma, CQT/VQT, spectral contrast); metering (true-peak, LUFS, correlation, vectorscope, waveform peaks). → [API](https://libsonare.libraz.net/docs/wasm)
- **Mastering** — 76 named DSP processors, the configurable `masteringChain`, 25 named presets via `masterAudio`, and reference-matching. → [Mastering processors](https://libsonare.libraz.net/docs/mastering-processors)
- **Mixing** — offline `mixStereo` and the block-based `Mixer` with scene presets. → [Mixing](https://libsonare.libraz.net/docs/mixing)
- **Editing DSP** — time-stretch, pitch-shift, HPSS (+ residual), phase vocoder, normalize, trim, remix. → [Editing DSP](https://libsonare.libraz.net/docs/editing-dsp)
- **Room acoustics** — blind RT60 / EDT, impulse-response clarity metrics, RIR synthesis, room estimation and morphing. → [Room acoustics](https://libsonare.libraz.net/docs/acoustic-analysis)
- **Realtime & streaming** — `RealtimeEngine` (transport / MIDI / render, bounded-memory clip streaming), `StreamingMasteringChain` / `StreamingEqualizer` / `StreamingRetune`, `RealtimeVoiceChanger`, and the AudioWorklet bridge. → [Realtime & streaming](https://libsonare.libraz.net/docs/realtime-streaming)
- **Instruments & synthesis** — built-in oscillator synth, patch-driven NativeSynth (15 synthesis engines, incl. physically-modeled piano / strings / winds — being tuned over time), and a GS-compatible SoundFont (SF2) player. → [API](https://libsonare.libraz.net/docs/wasm)
- **Headless DAW** — `Project` arrangement model: audio / MIDI tracks and clips, undo/redo, clip warp, SMF / MIDI 2.0 Clip File I/O, deterministic JSON, offline `bounce`. → [API](https://libsonare.libraz.net/docs/wasm)
- **Conversions** — Hz / mel / MIDI / note, frames / time, resample.

Native failures throw a `SonareError` carrying a numeric `code` (an `ErrorCode`
value) and its `codeName`; narrow with the `isSonareError` type guard.

## Documentation

Full API reference, guides, and browser-local demos live at
**[libsonare.libraz.net](https://libsonare.libraz.net)**
([getting started](https://libsonare.libraz.net/docs/getting-started) ·
[browser / WASM API](https://libsonare.libraz.net/docs/wasm) ·
[demos](https://libsonare.libraz.net/demos)).

## Also available

```bash
pip install libsonare  # Python bindings with CLI
```

The native Node.js N-API binding (reads files from disk) lives at
[`bindings/node`](https://github.com/libraz/libsonare/tree/main/bindings/node).

## License

[Apache License 2.0](https://github.com/libraz/libsonare/blob/main/LICENSE)
