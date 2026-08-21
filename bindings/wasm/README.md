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
weights. 88 named mastering DSP processors implemented against published
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

For BPM/key/chord detection, feature extraction, and metering without the
mastering, mixing, or realtime-engine APIs, import the smaller analysis entry:

```typescript
import { detectBpm, init } from '@libraz/libsonare/analysis';
```

With emsdk 5.0.2, the analysis binary is 0.91 MiB raw / 368 KiB gzip; the full
entry is 3.88 MiB raw / 1.31 MiB gzip. The analysis entry deliberately has no
`masterAudio`, `mixStereo`, `Project`, `Mixer`, or `RealtimeEngine` export.

## Quick Start

`init()` loads the WASM module once; every API is available afterwards. Top-level
one-shot functions accept a request object (recommended) or positional arguments.

> **Audio input:** start with `Audio.fromMemoryWithBrowserFallback(bytes)`. It
> decodes WAV/MP3 in WASM, then uses the browser's `decodeAudioData` for other
> browser-supported formats. Pass a decoded mono `Float32Array` only when one is
> already available.

> **Platform constraints:** the WebAssembly build is single-threaded (analysis
> runs to completion on the calling thread — there is no non-blocking variant)
> and has no host filesystem access. Drive long-running calls from a Web Worker
> to keep the UI responsive.

```typescript
import { Audio, init } from '@libraz/libsonare';

await init();

const bytes = new Uint8Array(await file.arrayBuffer());
const audio = await Audio.fromMemoryWithBrowserFallback(bytes);
const { bpm, key } = audio.analyze();
console.log(`BPM: ${bpm}  Key: ${key.name}`);
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

### Using already-decoded audio

Use `Float32Array` directly when another API already decoded the audio:

```typescript
const audio = Audio.fromBuffer(decoded.getChannelData(0), decoded.sampleRate);
const { bpm, key } = audio.analyze();
```

### Offline Worker for longer audio

For audio longer than roughly 30 seconds, use `OfflineWorkerClient` to keep
analysis or preset mastering off the UI thread. The published `./worker`
subpath is resolved automatically. It intentionally exposes only one-shot
value APIs (`analyze`, BPM/key/chord detection, and `masterAudio`): native
handles such as `Project`, `Mixer`, and realtime engines stay in their owning
JavaScript realm.

```typescript
import { OfflineWorkerClient } from '@libraz/libsonare';

const offline = new OfflineWorkerClient();
const task = offline.analyze(
  { samples, sampleRate },
  {
    onProgress: ({ progress, stage }) => updateProgress(progress, stage),
    // copy: true, // retain `samples`; the default transfers and detaches it
  },
);

cancelButton.onclick = () => task.cancel();
try {
  const result = await task;
  console.log(result.bpm, result.key.name);
} finally {
  offline.dispose();
}
```

By default the input `Float32Array` is transferred, so its buffer is detached
on the calling thread. Pass `{ copy: true }` when it must remain usable. Prompt
cancellation of a running synchronous WASM call uses `SharedArrayBuffer`; serve
the page with cross-origin isolation (COOP/COEP) when a cancel button must take
effect immediately. `workerUrl` lets a host point the client at a separately
hosted copy of `@libraz/libsonare/worker`.

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

### Realtime voice changer preset schemas

The published package includes the JSON Schema documents for third-party voice
changer presets. Resolve them through the package exports rather than copying a
schema from the repository:

```text
@libraz/libsonare/schemas/realtime-voice-changer-preset.schema.json
@libraz/libsonare/schemas/realtime-voice-changer-preset-pack.schema.json
```

Validate data against the schema before saving it, then pass the JSON text to
`validateRealtimeVoiceChangerPresetJson()` before applying it. The runtime check
is authoritative and also rejects malformed JSON such as duplicate keys.

### Bounded-memory OPFS clip streaming

For long raw float32 clips stored in OPFS, `attachOpfsClipStream` supplies only
the current playback window to WASM. It primes the first page, then fetches page
misses on the main thread and evicts pages outside the configured read-ahead /
retain-behind window. The AudioWorklet path uses the same helper: the worklet
posts a bounded batch of misses, and it outputs silence until a page arrives.

```typescript
import { attachOpfsClipStream } from '@libraz/libsonare';
import { SonareEngine } from '@libraz/libsonare/worklet';

const engine = await SonareEngine.create(audioContext);
const stream = await attachOpfsClipStream(engine, {
  path: 'takes/lead.f32',
  clipId: 42,
  numChannels: 2,
  numSamples: 48_000 * 600,
  pageFrames: 16_384,
});

// `clipId` must equal the explicit id supplied here.
engine.addClip(trackId, stream.provider, 0, { id: 42 });

// Close the returned binding after removing the clip (or when the host closes).
stream.binding.close();
```

The bounded-memory guarantee applies only to an OPFS/page-provider source.
Passing a `Float32Array[]` to `addClip` keeps that full array in the JavaScript
heap, so it is appropriate for short clips but does not make long clips bounded.

### Cue bus on a second AudioWorklet output

Per-track PFL/AFL monitoring normally folds the cue into the program output.
Pass `cueOutput` and the node gains a second output carrying the cue alone, so
it can be routed to headphones or a separate device while the program mix stays
untouched.

```typescript
import { SonareEngine } from '@libraz/libsonare/worklet';

const engine = await SonareEngine.create(audioContext, { cueOutput: true });
engine.setTrackMonitorMode(trackId, 'pfl');

engine.node.connect(audioContext.destination, 0); // program
engine.node.connect(cueDestination, 1); // cue
```

Without `cueOutput` the node keeps a single output and the folded mix, sample
for sample. Off the worklet, the same split is available on the zero-copy path
as `prepareMonitorChannels` / `getMonitorChannelBuffer` /
`processPreparedWithMonitor`, and as `processWithMonitor` for a copy-in call.

### Mastering preview inside the worklet

`StreamingMasteringChain` is exported from `@libraz/libsonare/worklet`, so a live
preview can run in the render realm instead of round-tripping audio to the main
thread. Build and `prepare()` it from a message handler — `prepare()` allocates
and must not run inside `process()`. An enabled `loudness` stage needs the
offline-measured `loudnessStaticGainDb`, since whole-signal integrated LUFS
cannot be measured block by block. The chain is a host-side stage: it is outside
the engine's own delay compensation, so aligning it against other engine outputs
is the caller's job.

## Mixing assistant

`suggestMixScene` takes a set of tracks and returns a starting point for a mixer
scene — per-strip input trim and fader, pan and width, corrective EQ, dynamics,
bus structure and sends — alongside the measurements it was derived from and a
written explanation of every decision.

**The assistant suggests; it does not apply.** It never processes or emits
audio: what comes back is parameters and prose. Applying them is a separate,
explicit step, so nothing changes behind the user's back.

```typescript
import { init, suggestMixScene } from '@libraz/libsonare';

await init();

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
import { Mixer } from '@libraz/libsonare';

const mixer = Mixer.fromSceneJson(JSON.stringify(result.scene), 48000, 512);
try {
  const out = mixer.processStereo(leftBlocks, rightBlocks); // { left, right, sampleRate }
} finally {
  mixer.delete();
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
  scene. A malformed *call* is rejected instead — a missing or non-string track
  id, a `left` that is not a `Float32Array`, a `right` whose length differs from
  `left`, or a non-positive `sampleRate` throws.
- **Genre- and material-dependent.** The relative levels and placements follow
  common recording practice, not a universal correct answer. Expect to treat the
  result as a first pass on unusual material.
- **Offline only.** The pipeline allocates, runs an STFT per track and evaluates
  every track pair; it must not be called from an AudioWorklet or any other
  realtime audio callback. The WASM build is single-threaded, so drive it from a
  Web Worker to keep the UI responsive on a large session.

### Options

Every field is optional, and an omitted field keeps the core default rather than
being sent as an explicit value. `sampleRate` itself defaults to `48000`.

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
| `nFft`               | `2048`  | Shared analysis FFT size for every track                         |
| `hopLength`          | `512`   | Shared analysis hop length, in samples                           |

A disabled domain is not evaluated at all rather than evaluated and discarded.

The assistant is an optional addition on top of the mixer, not a required part
of it: source builds can drop the subsystem entirely with
`-DBUILD_MIXING_ASSISTANT=OFF` (default `ON`), which leaves the rest of the
library unchanged. It is not part of the smaller `@libraz/libsonare/analysis`
entry.

## Capabilities

Every area below has runnable examples and the full API in the
[documentation](https://libsonare.libraz.net/docs/wasm).

- **Analysis** — BPM, key (+ candidates), chords, downbeats, sections, melody, tuning; pitch (YIN / pYIN), timbre, and the full spectral feature set (STFT, mel, MFCC, chroma, CQT/VQT, spectral contrast); metering (true-peak, LUFS, correlation, vectorscope, waveform peaks). → [API](https://libsonare.libraz.net/docs/wasm)
- **Mastering** — 88 named DSP processors, the configurable `masteringChain`, 25 named presets via `masterAudio`, and reference-matching. → [Mastering processors](https://libsonare.libraz.net/docs/mastering-processors)
- **Mixing** — offline `mixStereo` and the block-based `Mixer` with scene presets. → [Mixing](https://libsonare.libraz.net/docs/mixing)
- **Mixing assistant** — `suggestMixScene` analyzes a set of tracks and suggests a mixer scene with a written explanation; it suggests only, and applying the scene is a separate step. → [Mixing assistant](#mixing-assistant)
- **Editing DSP** — time-stretch, pitch-shift, HPSS (+ residual), phase vocoder, normalize, trim, remix. → [Editing DSP](https://libsonare.libraz.net/docs/editing-dsp)
- **Room acoustics** — blind RT60 / EDT, impulse-response clarity metrics, RIR synthesis, room estimation and morphing. → [Room acoustics](https://libsonare.libraz.net/docs/acoustic-analysis)
- **Realtime & streaming** — `RealtimeEngine` (transport / MIDI / render, bounded-memory clip streaming), `StreamingMasteringChain` / `StreamingEqualizer` / `StreamingRetune`, `RealtimeVoiceChanger`, and the AudioWorklet bridge. → [Realtime & streaming](https://libsonare.libraz.net/docs/realtime-streaming)
- **Instruments & synthesis** — built-in oscillator synth, patch-driven NativeSynth (15 synthesis engines, incl. physically-modeled piano / strings / winds — being tuned over time), and a GS-compatible SoundFont (SF2) player. → [API](https://libsonare.libraz.net/docs/wasm)
- **Headless DAW** — `Project` arrangement model: audio / MIDI tracks and clips, undo/redo, clip warp, SMF / MIDI 2.0 Clip File I/O, deterministic JSON, offline `bounce`. → [API](https://libsonare.libraz.net/docs/wasm)
- **Conversions** — Hz / mel / MIDI / note, frames / time, resample.

Native failures throw a `SonareError` carrying a numeric `code` (an `ErrorCode`
value) and its `codeName`. Narrow with the `isSonareError` type guard or with
`instanceof SonareError`; both accept the same values. `SonareError` is exported
as a runtime class here and in the Node binding, so a module shared between the
two packages can use either form. Its `instanceof` is brand-based rather than
prototype-based, so an error that arrives from the analysis worker as a
structured clone — with its prototype gone — still narrows.

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
