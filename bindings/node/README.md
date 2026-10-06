# libsonare (Node native)

[![CI](https://img.shields.io/github/actions/workflow/status/libraz/libsonare/ci.yml?branch=main&label=CI)](https://github.com/libraz/libsonare/actions)
[![Node](https://img.shields.io/badge/node-%3E%3D22-brightgreen)](https://github.com/libraz/libsonare/tree/main/bindings/node)
[![License](https://img.shields.io/badge/license-Apache--2.0-blue)](https://github.com/libraz/libsonare/blob/main/LICENSE)
[![Docs](https://img.shields.io/badge/docs-libsonare.libraz.net-2563eb)](https://libsonare.libraz.net)
[![PyPI](https://img.shields.io/pypi/v/libsonare?label=PyPI)](https://pypi.org/project/libsonare/)

**Turn audio into data and back, natively in Node.js.** Analyze songs (BPM, key, chords, loudness), master and mix to broadcast loudness, and render MIDI through built-in instruments — a native N-API addon on the libsonare C++ core. Mastering ships 94 named DSP processors implemented against published references; analysis defaults match librosa where the two overlap. Apache-2.0, no model weights.

Unlike the WebAssembly package (`@libraz/libsonare`), this binding decodes audio files directly from disk or memory (WAV / MP3 out of the box, plus M4A / AAC / FLAC / OGG / Opus when built with FFmpeg).

**Guides and the full API reference: [libsonare.libraz.net](https://libsonare.libraz.net)**

## Installation

This binding has no npm package. Clone the repository and consume it as a local / workspace dependency; npm cannot install `@libraz/libsonare-native`. `yarn build` runs `cmake-js compile` then `tsc`, auto-detecting FFmpeg via pkg-config.

```bash
git clone https://github.com/libraz/libsonare
cd libsonare/bindings/node
yarn install
yarn build
```

`SONARE_FFMPEG` controls FFmpeg support at build time: `auto` (default), `1` (require it) or `0` (disable). See the [installation guide](https://libsonare.libraz.net/docs/installation) for the FFmpeg development libraries.

## Quick Start

```typescript
import { Audio, masterAudio } from '@libraz/libsonare-native';

const audio = Audio.fromFile('song.mp3');
const result = audio.analyze();               // BPM + key + time signature + beats
console.log(`BPM: ${result.bpm.toFixed(1)}  Key: ${result.key.name}`);

// Master toward a target loudness with a named preset
const mastered = masterAudio({
  samples: audio.getData(),
  sampleRate: audio.getSampleRate(),
  preset: 'streaming',
});
console.log(mastered.outputLufs, mastered.appliedGainDb);
```

Render a MIDI arrangement through a built-in instrument with the headless `Project`. Call `destroy()` to release the native handle.

```typescript
import { Project } from '@libraz/libsonare-native';

const project = Project.create();
const { clipId } = project.addMidiClip(0, 4);
project.setMidiEvents(clipId, [
  Project.midiNoteOn(0, 0, 0, 60, 100),       // ppq, group, channel, note, velocity
  Project.midiNoteOff(1, 0, 0, 60),
]);
const audio = project.bounceWithSynthInstrument('saw-lead', { numChannels: 2 });
project.destroy();
```

## Capabilities

- **Analysis** — BPM, key, chords, downbeats, sections, melody, tuning, pitch, timbre, spectral features and metering. → [Analysis](https://libsonare.libraz.net/docs/node-api-analysis)
- **Stem decomposition and transcription** — `decomposeStems` splits a mix into stems, and `transcribe` turns audio into MIDI events. → [Analysis](https://libsonare.libraz.net/docs/node-api-analysis)
- **Mastering** — 94 named DSP processors, the configurable `masteringChain`, 30 named presets via `masterAudio`, and reference-matching. → [Mastering processors](https://libsonare.libraz.net/docs/mastering-processors)
- **Restoration** — five repair-only presets, `vinyl`, `tapeHiss`, `fieldRecording`, `voiceMemo` and `shellac78`. → [Mastering processors](https://libsonare.libraz.net/docs/mastering-processors)
- **Mixing** — offline `mixStereo`, the block-based `Mixer` with scene presets, and `suggestMixScene`, which suggests a mixer scene with a written explanation without applying it; `sampleRate` is **required** and must be an integer within `[8000, 384000]`. → [Mixing](https://libsonare.libraz.net/docs/mixing), [Mixing assistant](https://libsonare.libraz.net/docs/mixing-assistant)
- **Editing DSP** — time-stretch, pitch-shift, HPSS, phase vocoder, normalize, trim, remix. → [Editing DSP](https://libsonare.libraz.net/docs/editing-dsp)
- **Note editing and takes** — edit the notes of monophonic takes (`extractNotes`) and polyphonic material (`analyzePolyphonic`), align a take to a reference with `alignTakeToReference`, and build tuning targets from a MIDI melody with `noteTargetsFromSmf`, and edit vocal takes offline with undo/redo through `createVocalEditSession`. → [Project editing](https://libsonare.libraz.net/docs/project-editing-midi), [Recording and takes](https://libsonare.libraz.net/docs/recording-and-takes)
- **Room acoustics** — blind RT60, impulse-response metrics, RIR synthesis, room estimation and morphing. → [Room acoustics](https://libsonare.libraz.net/docs/acoustic-analysis)
- **Realtime and streaming** — `RealtimeEngine` with MIDI 1.0 / 2.0 input, bus-to-bus routing with sends, sidechain keys (`setBusSidechain`), `StreamingMasteringChain` and `RealtimeVoiceChanger`. → [Realtime engine](https://libsonare.libraz.net/docs/realtime-engine)
- **Instruments and synthesis** — built-in synth, the patch-driven NativeSynth, and a GS-compatible SoundFont (SF2) player that receives GS SysEx and offers two GS insertion-effect realisations (`gsEfxRealization`), with a selectable rig per part (`setPartRig`). The acoustic piano is calibrated; the other physical-model voices are still being tuned and will change in 1.8.x patch releases. → [GM and GS](https://libsonare.libraz.net/docs/gm-gs), [Native synth](https://libsonare.libraz.net/docs/native-synth)
- **Headless DAW** — `Project` with audio and MIDI tracks, undo/redo, SMF and MIDI 2.0 Clip File I/O, and offline `bounce`; `transcribeToClip` writes a transcription into a clip, and `compileTimeline` turns a project into a timeline a stopped `RealtimeEngine` plays. → [Project editing](https://libsonare.libraz.net/docs/project-editing)
- **Playback** — `PlaybackRenderer` renders PCM up to 7.1 to speakers or HRTF binaural headphones. → [Playback](https://libsonare.libraz.net/docs/playback)

The whole surface is listed in the [Node.js API reference](https://libsonare.libraz.net/docs/node-api).

## Supported audio formats

| Format                                     | Default build | With FFmpeg support |
| ------------------------------------------ | ------------- | ------------------- |
| WAV (PCM 16/24/32, float32)                | yes           | yes                 |
| MP3                                        | yes           | yes                 |
| M4A / AAC / FLAC / OGG / Opus / WMA / ...  | no            | yes                 |

`hasFfmpegSupport()` reports whether the loaded binding was built with FFmpeg.

## Documentation

[Getting started](https://libsonare.libraz.net/docs/getting-started) · [Node.js API](https://libsonare.libraz.net/docs/node-api) · [Examples](https://libsonare.libraz.net/docs/examples) · [Binding parity](https://libsonare.libraz.net/docs/binding-parity). Realtime voice changer preset schemas live in the repository's [`schemas/`](../../schemas/) directory; this binding is not published, so applications should take them from the published WASM package.

## Also available

```bash
npm install @libraz/libsonare   # JavaScript / TypeScript (WASM, takes Float32Array)
pip install libsonare           # Python bindings with CLI
```

## License

Apache-2.0
