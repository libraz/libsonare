# libsonare (Node native)

[![CI](https://img.shields.io/github/actions/workflow/status/libraz/libsonare/ci.yml?branch=main&label=CI)](https://github.com/libraz/libsonare/actions)
[![Node](https://img.shields.io/badge/node-%3E%3D22-brightgreen)](https://github.com/libraz/libsonare/tree/main/bindings/node)
[![License](https://img.shields.io/badge/license-Apache--2.0-blue)](https://github.com/libraz/libsonare/blob/main/LICENSE)
[![Docs](https://img.shields.io/badge/docs-libsonare.libraz.net-2563eb)](https://libsonare.libraz.net)
[![PyPI](https://img.shields.io/pypi/v/libsonare?label=PyPI)](https://pypi.org/project/libsonare/)

**Turn audio into data and back, natively in Node.js.** Analyze songs (BPM, key,
chords, loudness), master and mix to broadcast loudness, and render MIDI through
built-in instruments — a native N-API addon on the libsonare C++ core. Mastering
ships 88 named DSP processors implemented against published references (ITU-R
BS.1770-4 true-peak limiting, Linkwitz-Riley crossovers, Vicanek matched-Z
biquads, ADAA-antialiased saturation); analysis defaults match librosa where the
two overlap. Apache-2.0, no model weights.

Unlike the WebAssembly package (`@libraz/libsonare`), this binding decodes audio
files directly from disk or memory (WAV / MP3 out of the box, plus
M4A / AAC / FLAC / OGG / Opus when built with FFmpeg). The analysis, mastering,
mixing, and editing APIs match the C, Python, CLI, and WASM surfaces.

📖 **Full API reference and guides: [libsonare.libraz.net](https://libsonare.libraz.net)**

## Installation

This binding has no npm package. Clone the repository and consume it as a local /
workspace dependency; npm cannot install `@libraz/libsonare-native`. `yarn build`
runs `cmake-js compile` then `tsc`, auto-detecting FFmpeg via pkg-config.

```bash
git clone https://github.com/libraz/libsonare
cd libsonare/bindings/node
yarn install
yarn build
```

For a browser-friendly, npm-published runtime use the WebAssembly package
[`@libraz/libsonare`](https://www.npmjs.com/package/@libraz/libsonare); for Python
use [`libsonare`](https://pypi.org/project/libsonare/).

The native build honours `SONARE_FFMPEG`: `auto` (default, detect via pkg-config),
`1` (require FFmpeg — fail if dev libs are missing), `0` (disable). FFmpeg dev
libraries are needed for `=1` (`brew install ffmpeg`, or
`apt install libavformat-dev libavcodec-dev libavutil-dev libswresample-dev`).

## Quick Start

`Audio` is the recommended entry point: it decodes files and caches samples.
Top-level one-shot functions accept a request object (recommended) or positional
arguments.

```typescript
import { Audio, masterAudio } from '@libraz/libsonare-native';

// Decode straight from disk (WAV/MP3 always; more with FFmpeg)
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

Render a MIDI arrangement through a built-in instrument with the headless
`Project`. Call `destroy()` to release the native handle.

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

## Mixing assistant

`suggestMixScene` takes a set of tracks and returns a starting point for a mixer
scene — per-strip input trim and fader, pan and width, corrective EQ, dynamics,
bus structure and sends — alongside the measurements it was derived from and a
written explanation of every decision.

**The assistant suggests; it does not apply.** It never processes or emits
audio: what comes back is parameters and prose. Applying them is a separate,
explicit step, so nothing changes behind the user's back.

```typescript
import { suggestMixScene } from '@libraz/libsonare-native';

const tracks = [
  { id: 'kick', name: 'Kick', left: kickSamples },
  { id: 'bass', name: 'Bass', left: bassSamples },
  { id: 'vox', name: 'Lead Vox', left: voxLeft, right: voxRight },
];

const result = suggestMixScene({
  tracks,
  sampleRate: 48000,
  options: { targetTrackLufs: -18, eqMaxCutDb: 3 },
});

for (const line of result.explanation) console.log(line);
for (const track of result.tracks) {
  console.log(track.stripId, track.source, track.sourceConfidence);
}
```

Nothing has been applied at this point. Realizing the suggestion is the second
step, and it is the caller's own:

```typescript
import { Mixer } from '@libraz/libsonare-native';

const mixer = Mixer.fromSceneJson(JSON.stringify(result.scene), 48000, 512);
try {
  const out = mixer.processStereo(leftBlocks, rightBlocks); // { left, right, sampleRate }
} finally {
  mixer.destroy();
}
```

`suggestMixSceneJson` takes the same request and returns only the scene, already
serialized in the form `Mixer.fromSceneJson` reads, for a caller that goes
straight to applying it.

### Reading the explanation

`explanation` is a list of English declarative sentences in the order the
changes were applied — structure, then gain, then balance, then EQ, then
dynamics, then stereo image. Reading it top to bottom retraces how the scene was
built, one decision at a time. It is empty when every decision domain is
switched off, or when no track survived exclusion.

### What it does not do

- **Suggestion, not automation.** No audio is processed and no audio is
  returned. Whether to apply the scene is the user's decision.
- **Classification is confidence-scored.** Each track resolves to a source class
  with a confidence in `[0, 1]`, and a track the rules cannot place stays
  `'unknown'`. An unknown track keeps a neutral strip routed straight to master;
  no class-driven EQ, dynamics or placement is suggested for it. A track name is
  read only as a hint and can never select a class on its own.
- **Degenerate audio is not an error; a malformed call is.** A track that is
  silent, shorter than the minimum measurable duration (0.4 s), or without
  energy in the analysis bands comes back with `usable: false` and an
  `exclusionReason`, and gets no suggestion; an empty track list yields an empty
  scene. A malformed *call* is rejected instead — a missing or duplicate track
  id, a non-positive `sampleRate`, or a `right` channel whose length differs
  from `left` throws.
- **Genre- and material-dependent.** The relative levels and placements follow
  common recording practice, not a universal correct answer. Expect to treat the
  result as a first pass on unusual material.
- **Offline only.** The pipeline allocates, runs an STFT per track and evaluates
  every track pair; it must not be called from a realtime audio thread.

### Options

Every field is optional, and an omitted field keeps the core default rather than
being sent as an explicit value.

| Option               | Default | Meaning                                                        |
| -------------------- | ------- | -------------------------------------------------------------- |
| `targetTrackLufs`    | `-18`   | Absolute integrated-loudness target each track is staged towards, in LUFS |
| `suggestionStrength` | `1`     | Overall strength in `[0, 1]`, scaling every level-like decision; `0` suggests nothing |
| `eqMaxCutDb`         | `4`     | Largest cut a single suggested EQ band may apply, in dB          |
| `mixBusHeadroomDbtp` | `-6`    | Headroom the summed mix is left with on the master bus, in dBTP  |
| `enableStructure`    | `true`  | Evaluate bus structure, routing and sends                        |
| `enableGain`         | `true`  | Evaluate per-track gain staging                                  |
| `enableBalance`      | `true`  | Evaluate fader balance between tracks                            |
| `enableEq`           | `true`  | Evaluate corrective EQ                                           |
| `enableDynamics`     | `true`  | Evaluate dynamics processing                                     |
| `enableImage`        | `true`  | Evaluate stereo placement and width                              |
| `enableHighPass`     | `false` | Suggest a high-pass on tracks measured to carry residue below their register |
| `nFft`               | `2048`  | Shared analysis FFT size for every track                         |
| `hopLength`          | `512`   | Shared analysis hop length, in samples                           |

A disabled domain is not evaluated at all rather than evaluated and discarded.

The assistant is an optional addition on top of the mixer, not a required part
of it: `-DBUILD_MIXING_ASSISTANT=OFF` (default `ON`) removes the subsystem
entirely and leaves the rest of the library unchanged.

## Supported audio formats

| Format                                     | Default build | With FFmpeg support |
| ------------------------------------------ | ------------- | ------------------- |
| WAV (PCM 16/24/32, float32)                | yes           | yes                 |
| MP3                                        | yes           | yes                 |
| M4A / AAC / FLAC / OGG / Opus / WMA / ...  | no            | yes                 |

`Audio.fromFile()` reports a clear error for unsupported formats. Check at runtime
whether the loaded binding was compiled against FFmpeg with `hasFfmpegSupport()`.
`Audio.fromMemory` accepts a Node `Buffer` or `Uint8Array` (zero-copy); stereo
files are downmixed to mono on load.

## Capabilities

Every area below has runnable examples and the full API in the
[documentation](https://libsonare.libraz.net/docs/native-bindings).

- **Analysis** — BPM, key (+ candidates), chords, downbeats, sections, melody, tuning; pitch (YIN / pYIN), timbre, and the full spectral feature set (STFT, mel, MFCC, chroma, CQT/VQT, spectral contrast); metering (true-peak, LUFS, correlation, vectorscope, waveform peaks). → [Python/JS API](https://libsonare.libraz.net/docs/native-bindings)
- **Mastering** — 88 named DSP processors, the configurable `masteringChain`, 25 named presets via `masterAudio`, and reference-matching. → [Mastering processors](https://libsonare.libraz.net/docs/mastering-processors)
- **Mixing** — offline `mixStereo` and the block-based `Mixer` with scene presets. → [Mixing](https://libsonare.libraz.net/docs/mixing)
- **Mixing assistant** — `suggestMixScene` analyzes a set of tracks and suggests a mixer scene with a written explanation; it suggests only, and applying the scene is a separate step. → [Mixing assistant](#mixing-assistant)
- **Editing DSP** — time-stretch, pitch-shift, HPSS (+ residual), phase vocoder, normalize, trim, remix. → [Editing DSP](https://libsonare.libraz.net/docs/editing-dsp)
- **Room acoustics** — blind RT60 / EDT, impulse-response clarity metrics, RIR synthesis, room estimation and morphing. → [Room acoustics](https://libsonare.libraz.net/docs/acoustic-analysis)
- **Realtime & streaming** — `RealtimeEngine` (transport / MIDI / render / capture), `StreamingMasteringChain`, `RealtimeVoiceChanger`. → [Realtime & streaming](https://libsonare.libraz.net/docs/realtime-streaming)
- **Instruments & synthesis** — built-in oscillator synth, patch-driven NativeSynth (15 synthesis engines, incl. physically-modeled piano / strings / winds — being tuned over time), and a GS-compatible SoundFont (SF2) player. → [API](https://libsonare.libraz.net/docs/native-bindings)
- **Headless DAW** — `Project` arrangement model: audio / MIDI tracks and clips, undo/redo, clip warp, SMF / MIDI 2.0 Clip File I/O, deterministic JSON, offline `bounce`. → [API](https://libsonare.libraz.net/docs/native-bindings)
- **Conversions** — Hz / mel / MIDI / note, frames / time, resample.

C-ABI failures throw a `SonareError` (`name` `'SonareError'`) carrying a numeric
`code` (an `ErrorCode` value) and its `codeName`. Narrow with the `isSonareError`
type guard or with `instanceof SonareError`; both accept the same values.
`SonareError` is exported as a runtime class here and in `@libraz/libsonare`, so
a module shared between the two packages can use either form. Its `instanceof`
is brand-based rather than prototype-based, so an error that lost its prototype
crossing a worker or `structuredClone` boundary still narrows.

## Documentation

The canonical JSON Schema documents for realtime voice changer presets live in
the repository's [`schemas/`](../../schemas/) directory. This local-only native
binding is not published; applications should consume the schema subpaths from
the published WASM package instead.

Full API reference and guides live at
**[libsonare.libraz.net](https://libsonare.libraz.net)**
([getting started](https://libsonare.libraz.net/docs/getting-started) ·
[Node.js native API](https://libsonare.libraz.net/docs/native-bindings)).

## Also available

```bash
npm install @libraz/libsonare   # JavaScript / TypeScript (WASM, takes Float32Array)
pip install libsonare           # Python bindings with CLI
```

## License

Apache-2.0
