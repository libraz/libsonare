# libsonare

[![CI](https://img.shields.io/github/actions/workflow/status/libraz/libsonare/ci.yml?branch=main&label=CI)](https://github.com/libraz/libsonare/actions)
[![npm](https://img.shields.io/npm/v/@libraz/libsonare)](https://www.npmjs.com/package/@libraz/libsonare)
[![npm downloads](https://img.shields.io/npm/dm/@libraz/libsonare)](https://www.npmjs.com/package/@libraz/libsonare)
[![types](https://img.shields.io/npm/types/@libraz/libsonare)](https://www.npmjs.com/package/@libraz/libsonare)
[![License](https://img.shields.io/github/license/libraz/libsonare)](https://github.com/libraz/libsonare/blob/main/LICENSE)
[![Docs](https://img.shields.io/badge/docs-libsonare.libraz.net-2563eb)](https://libsonare.libraz.net)
[![PyPI](https://img.shields.io/pypi/v/libsonare?label=PyPI)](https://pypi.org/project/libsonare/)

**Turn audio into data and back — entirely in the browser.** Analyze songs (BPM, key, chords, loudness), master and mix to broadcast loudness, and render MIDI through built-in instruments, all client-side via WebAssembly — the same C++ engine that runs natively, with zero dependencies and no model weights. 94 named mastering DSP processors implemented against published references; analysis defaults match librosa where the two overlap.

**Guides and the full API reference: [libsonare.libraz.net](https://libsonare.libraz.net)**. Try it without installing anything: the [live demos](https://libsonare.libraz.net/demos), or [sonare studio](https://sonare-studio.libraz.net), a browser DAW whose audio runs on this engine.

## Installation

```bash
npm install @libraz/libsonare
```

For BPM / key / chord detection, feature extraction and metering without the mastering, mixing or realtime-engine APIs, import the smaller analysis entry: `import { detectBpm, init } from '@libraz/libsonare/analysis'`.

## Quick Start

`init()` loads the WASM module once. Start from `Audio.fromMemoryWithBrowserFallback(bytes)`, which decodes WAV / MP3 in WASM and uses the browser's `decodeAudioData` for other formats. A multi-channel file folds to mono there with the ITU-R BS.775 rule; `decodeChannels(bytes)` keeps every channel and `downmix(channels, targetLayout)` applies the same fold on demand. The build is single-threaded, so drive long calls from a Web Worker.

```typescript
import { Audio, init } from '@libraz/libsonare';

await init();

const bytes = new Uint8Array(await file.arrayBuffer());
const audio = await Audio.fromMemoryWithBrowserFallback(bytes);
const { bpm, key } = audio.analyze();
console.log(`BPM: ${bpm}  Key: ${key.name}`);
```

Render a MIDI arrangement through a built-in instrument with the headless `Project`. The embind handle is not garbage-collected — call `delete()` when done.

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

## Capabilities

- **Analysis** — BPM, key, chords, downbeats, sections, melody, tuning, pitch, timbre, spectral features and metering. → [Analysis](https://libsonare.libraz.net/docs/wasm)
- **Stem decomposition and transcription** — `decomposeStems` splits a mix into stems, and `transcribe` turns audio into MIDI events. → [Analysis](https://libsonare.libraz.net/docs/js-api-analysis)
- **Mastering** — 94 named DSP processors, the configurable `masteringChain`, 30 named presets via `masterAudio`, and reference-matching. → [Mastering processors](https://libsonare.libraz.net/docs/mastering-processors)
- **Restoration** — five repair-only presets, `vinyl`, `tapeHiss`, `fieldRecording`, `voiceMemo` and `shellac78`. → [Mastering processors](https://libsonare.libraz.net/docs/mastering-processors)
- **Mixing** — offline `mixStereo`, the block-based `Mixer` with scene presets, and `suggestMixScene`, which suggests a mixer scene with a written explanation without applying it; `sampleRate` is **required** and must be an integer within `[8000, 384000]`. → [Mixing](https://libsonare.libraz.net/docs/mixing), [Mixing assistant](https://libsonare.libraz.net/docs/mixing-assistant)
- **Editing DSP** — time-stretch, pitch-shift, HPSS, phase vocoder, normalize, trim, remix. → [Editing DSP](https://libsonare.libraz.net/docs/editing-dsp)
- **Note editing and takes** — edit the notes of monophonic takes (`extractNotes`) and polyphonic material (`analyzePolyphonic`), align a take to a reference with `alignTakeToReference`, and build tuning targets from a MIDI melody with `noteTargetsFromSmf`, and edit vocal takes offline with undo/redo through `createVocalEditSession`. → [Project editing](https://libsonare.libraz.net/docs/project-editing-midi), [Recording and takes](https://libsonare.libraz.net/docs/recording-and-takes)
- **Room acoustics** — blind RT60, impulse-response metrics, RIR synthesis, room estimation and morphing. → [Room acoustics](https://libsonare.libraz.net/docs/acoustic-analysis)
- **Realtime and streaming** — `RealtimeEngine` with MIDI 1.0 / 2.0 input, bus-to-bus routing with sends, sidechain keys (`setBusSidechain`), `StreamingMasteringChain`, `RealtimeVoiceChanger` and the AudioWorklet bridge. → [Realtime engine](https://libsonare.libraz.net/docs/realtime-engine), [Streaming](https://libsonare.libraz.net/docs/wasm-streaming)
- **Instruments and synthesis** — built-in synth, the patch-driven NativeSynth, and a GS-compatible SoundFont (SF2) player that receives GS SysEx and offers two GS insertion-effect realisations (`gsEfxRealization`), with a selectable rig per part (`setPartRig`). The acoustic piano is calibrated; the other physical-model voices are still being tuned and will change in 1.8.x patch releases. → [GM and GS](https://libsonare.libraz.net/docs/gm-gs), [Native synth](https://libsonare.libraz.net/docs/native-synth)
- **Headless DAW** — `Project` with audio and MIDI tracks, undo/redo, SMF and MIDI 2.0 Clip File I/O, and offline `bounce`; `transcribeToClip` writes a transcription into a clip, and `compileTimeline` turns a project into a timeline a stopped `RealtimeEngine` plays. → [Project editing](https://libsonare.libraz.net/docs/project-editing)
- **Playback** — `PlaybackRenderer` renders PCM up to 7.1 to speakers or HRTF binaural headphones. → [Playback](https://libsonare.libraz.net/docs/playback)

## Advanced

- Offline Worker for long audio (`OfflineWorkerClient`) → [Web Worker usage](https://libsonare.libraz.net/docs/wasm-advanced)
- Loading the `.wasm` file with `locateFile` → [JavaScript API](https://libsonare.libraz.net/docs/js-api)
- Realtime voice changer preset schemas → [Realtime voice changer](https://libsonare.libraz.net/docs/realtime-voice-changer)
- Bounded-memory OPFS clip streaming, the cue bus and mastering preview in the worklet → [Realtime and streaming](https://libsonare.libraz.net/docs/realtime-streaming)

## Documentation

[Getting started](https://libsonare.libraz.net/docs/getting-started) · [Browser / WASM API](https://libsonare.libraz.net/docs/wasm) · [Examples](https://libsonare.libraz.net/docs/examples) · [Binding parity](https://libsonare.libraz.net/docs/binding-parity) · [Demos](https://libsonare.libraz.net/demos).

## Also available

```bash
pip install libsonare  # Python bindings with CLI
```

The native Node.js N-API binding (reads files from disk) lives at [`bindings/node`](https://github.com/libraz/libsonare/tree/main/bindings/node).

## License

[Apache License 2.0](https://github.com/libraz/libsonare/blob/main/LICENSE)
