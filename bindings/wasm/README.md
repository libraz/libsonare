# libsonare

[![CI](https://img.shields.io/github/actions/workflow/status/libraz/libsonare/ci.yml?branch=main&label=CI)](https://github.com/libraz/libsonare/actions)
[![npm](https://img.shields.io/npm/v/@libraz/libsonare)](https://www.npmjs.com/package/@libraz/libsonare)
[![npm downloads](https://img.shields.io/npm/dm/@libraz/libsonare)](https://www.npmjs.com/package/@libraz/libsonare)
[![types](https://img.shields.io/npm/types/@libraz/libsonare)](https://www.npmjs.com/package/@libraz/libsonare)
[![License](https://img.shields.io/github/license/libraz/libsonare)](https://github.com/libraz/libsonare/blob/main/LICENSE)
[![PyPI](https://img.shields.io/pypi/v/libsonare?label=PyPI)](https://pypi.org/project/libsonare/)

A dependency-free audio DSP toolkit for browser and Node.js via WebAssembly —
librosa-compatible analysis plus broadcast-grade mastering, mixing, and editing.
The same C++ processors run client-side in the browser: 66 named mastering DSP
processors implemented against published references (ITU-R BS.1770-4 true-peak
limiting, Linkwitz-Riley crossovers, Vicanek matched-Z biquads, ADAA-antialiased
saturation), with analysis defaults matching librosa — Apache-2.0, no Python,
no model weights.

> **Audio input:** This package expects already-decoded `Float32Array` mono
> samples (it does not bundle a file decoder). Use the Web Audio API in the
> browser or `node:wasi` / a JS audio decoder in Node to obtain samples.
> If you need to read WAV/MP3/M4A files directly in Node, use the native
> N-API package [`@libraz/libsonare-native`](https://github.com/libraz/libsonare/tree/main/bindings/node) instead.

> **Platform constraints:** the WebAssembly build is single-threaded (analysis
> runs to completion on the calling thread — there is no non-blocking variant),
> has no host filesystem access, and expects pre-decoded `Float32Array` sample
> buffers. Drive long-running calls from a Web Worker to keep the UI responsive.

## Installation

```bash
npm install @libraz/libsonare
```

## Usage

```typescript
import { init, detectBpm, detectKey, analyze, Audio } from '@libraz/libsonare';

await init();

// Function API
const bpm = detectBpm(samples, sampleRate);
const key = detectKey(samples, sampleRate);
const result = analyze(samples, sampleRate);
console.log(`BPM: ${result.bpm}, Key: ${result.key.name}`);

// Advanced key options are opt-in; defaults preserve existing behavior.
const keyWithOptions = detectKey(samples, sampleRate, {
  useHpss: true,
  loudnessWeighted: true,
  highPassHz: 80,
  nFft: 4096,
  hopLength: 512,
});

// Audio class API
const audio = Audio.fromBuffer(samples, sampleRate);
console.log(`BPM: ${audio.detectBpm()}`);
console.log(`Key: ${audio.detectKey().name}`);
const audioKeyWithOptions = audio.detectKey({ useHpss: true, highPassHz: 80 });
```

### Pitch, timbre, and spectral APIs

Pitch tracking keeps unvoiced `f0` frames as `NaN` by default. Pass
`fillNa: true` when downstream code needs finite values and should treat
unvoiced frames as `0`. Timbre analysis returns aggregate metrics plus
`timbreOverTime`.

```typescript
import {
  analyzeTimbre,
  decompose,
  ebur128LoudnessRange,
  estimateTuning,
  hpssWithResidual,
  init,
  lufsInterleaved,
  nnFilter,
  phaseVocoder,
  pitchPyin,
  pitchTuning,
  pitchYin,
  polyFeatures,
  remix,
  spectralContrast,
  zeroCrossings,
} from '@libraz/libsonare';

await init();

const yin = pitchYin(samples, sampleRate, 2048, 512, 65, 2093, 0.3, true);
const pyin = pitchPyin(samples, sampleRate, 2048, 512, 65, 2093, 0.3, true);

const timbre = analyzeTimbre(samples, sampleRate);
console.log(timbre.brightness, timbre.timbreOverTime[0]?.brightness);

const contrast = spectralContrast(samples, sampleRate); // Matrix2d result
const poly = polyFeatures(samples, sampleRate);         // Matrix2d result
const crossings = zeroCrossings(samples);               // Int32Array
const tuning = estimateTuning(samples, sampleRate);
const offset = pitchTuning(yin.f0);

const { w, h } = decompose(spectrogram, nFeatures, nFrames, 8);
const filtered = nnFilter(spectrogram, nFeatures, nFrames);
const remixed = remix(samples, Int32Array.from([0, sampleRate, sampleRate, 2 * sampleRate]));
const stretched = phaseVocoder(samples, 1.5, sampleRate);
const hpss = hpssWithResidual(samples, sampleRate);

const multi = lufsInterleaved(interleaved, 2, sampleRate);
const lra = ebur128LoudnessRange(samples, sampleRate);
```

### Room acoustics

Use `detectAcoustic` for blind RT60/EDT estimation from ordinary audio.
Use `analyzeImpulseResponse` when you have a measured impulse response and need
clarity metrics (`c50`, `c80`, `d50`). Blind mode returns `NaN` for clarity
metrics because they are not reliable without an impulse response.

```typescript
import { init, analyzeImpulseResponse, detectAcoustic } from '@libraz/libsonare';

await init();

const blind = detectAcoustic(samples, sampleRate, 6, 24, 30.0, 10.0);
const room = analyzeImpulseResponse(irSamples, sampleRate);
console.log(blind.rt60, room.c50);
```

Acoustic simulation adds `synthesizeRir` (synthesize a shoebox-room impulse
response from geometry), `estimateRoom` (recover an equivalent room from a
recording or IR), and `roomMorph` (creatively re-reverberate audio toward a
target room — not dereverberation).

```typescript
import { init, synthesizeRir, estimateRoom, roomMorph } from '@libraz/libsonare';

await init();

const { rir, hasError } = synthesizeRir({
  lengthM: 6,
  widthM: 4,
  heightM: 3,
  absorption: 0.2,
  sampleRate: 48000,
});

const room = estimateRoom(samples, 48000); // { volume, length, width, height, ... }
const morphed = roomMorph(samples, sampleRate, { lengthM: 12, widthM: 9, wet: 0.5 });
```

### Error handling

Native (C++) failures are thrown as a `SonareError` carrying a numeric `code`
(an `ErrorCode` value) and its canonical `codeName`, so you can branch on the
cause instead of matching message text. Use the `isSonareError` type guard in a
`catch`:

```typescript
import { init, analyze, ErrorCode, isSonareError } from '@libraz/libsonare';

await init();

try {
  const result = analyze(samples, sampleRate);
} catch (error) {
  if (isSonareError(error)) {
    console.error(`${error.codeName} (${error.code}): ${error.message}`);
    if (error.code === ErrorCode.InvalidParameter) {
      // recover...
    }
  } else {
    throw error;
  }
}
```

### Decoding files in the browser

```typescript
import { init, analyze } from '@libraz/libsonare';

await init();

const arrayBuffer = await fetch('song.m4a').then((r) => r.arrayBuffer());
const audioCtx = new AudioContext();
const decoded = await audioCtx.decodeAudioData(arrayBuffer);
// Mono downmix for libsonare:
const samples = decoded.getChannelData(0);
const result = analyze(samples, decoded.sampleRate);
```

Web Audio's `decodeAudioData` handles whatever codecs the browser ships with
(WAV/MP3/M4A/AAC/Opus/FLAC on most modern browsers).

### Browser (CDN)

```html
<script type="module">
  import { init, analyze } from 'https://esm.sh/@libraz/libsonare';

  await init();
  // ... use with Web Audio API
</script>
```

### Bundlers (Vite, webpack, Next.js, etc.)

If your bundler doesn't automatically resolve the `.wasm` file, specify its path:

```typescript
import wasmUrl from '@libraz/libsonare/wasm?url'; // Vite
import { init } from '@libraz/libsonare';

// `locateFile(path, prefix)` is called by the Emscripten loader to resolve the
// `.wasm` file; return your bundler-provided URL for it.
await init({ locateFile: (path) => (path.endsWith('.wasm') ? wasmUrl : path) });
```

### Real-time Streaming

```typescript
import { init, StreamAnalyzer } from '@libraz/libsonare';

await init();

const analyzer = new StreamAnalyzer({ sampleRate: 44100 });

// In audio processing callback
analyzer.process(audioChunk);

const stats = analyzer.stats();
console.log(`BPM: ${stats.estimate.bpm}, Key: ${stats.estimate.key}`);
```

### Mastering (WASM)

The npm package ships mastering DSP in the default WebAssembly build. Pass
decoded `Float32Array` samples directly:

```typescript
import { init, masteringChain, masteringChainStereo } from '@libraz/libsonare';

await init();

const mastered = masteringChain(samples, sampleRate, {
  eq: { tiltDb: 1.0 },
  dynamics: { compressor: { thresholdDb: -24, ratio: 1.5 } },
  saturation: { tape: { driveDb: 1.0, saturation: 0.2 } },
  loudness: { targetLufs: -14, ceilingDb: -1, truePeakOversample: 4 },
});

const stereo = masteringChainStereo(left, right, sampleRate, {
  stereo: { imager: { width: 1.1 }, monoMaker: { amount: 0.2 } },
  loudness: { targetLufs: -14, ceilingDb: -1, truePeakOversample: 4 },
});
```

Named mastering processors use the same names and behavior as the native,
Python, C, and CLI APIs:

```typescript
import {
  masteringPairAnalyze,
  masteringPairProcess,
  masteringPairProcessorNames,
  masteringProcess,
  masteringProcessStereo,
  masteringProcessorNames,
  masteringStereoAnalyze,
} from '@libraz/libsonare';

const names = masteringProcessorNames(); // e.g. "dynamics.compressor"
const compressed = masteringProcess('dynamics.compressor', samples, sampleRate, {
  thresholdDb: -24,
  ratio: 1.5,
});

const widened = masteringProcessStereo('stereo.imager', left, right, sampleRate, {
  width: 1.1,
});

const pairNames = masteringPairProcessorNames(); // e.g. "match.abCrossfade"
const crossfaded = masteringPairProcess('match.abCrossfade', source, reference, sampleRate, {
  mix: 0.25,
});

const loudnessJson = masteringPairAnalyze(
  'match.referenceLoudness',
  source,
  reference,
  sampleRate,
);
const monoCompatJson = masteringStereoAnalyze(
  'stereo.monoCompatCheck',
  left,
  right,
  sampleRate,
);
```

### Mastering presets

```typescript
import { init, masterAudio, masteringPresetNames } from '@libraz/libsonare';

await init();

masteringPresetNames(); // ['pop', 'edm', 'acoustic', 'hipHop', 'aiMusic', 'speech', 'streaming', 'youtube', 'broadcast', 'podcast', 'audiobook', 'cinema', 'jpop', 'ambient', 'lofi', 'classical', 'drumAndBass', 'techno', 'metal', 'trap', 'rnb', 'jazz', 'kpop', 'trance', 'gameOst']

const result = masterAudio(samples, sampleRate, 'aiMusic', {
  // optional flat overrides applied on top of the preset (dot notation)
  'loudness.targetLufs': -13,
});
console.log(result.outputLufs, result.appliedGainDb);
```

### Mixing

```typescript
import { init, Mixer, mixStereo, mixingScenePresetJson } from '@libraz/libsonare';

await init();

const sceneJson = mixingScenePresetJson('vocalReverbSend');
const offline = mixStereo([vocalL, musicL], [vocalR, musicR], sampleRate, {
  inputTrimDb: [3, 0],
  faderDb: [-3, -12],
  pan: [0, -0.2],
  width: [1, 0.9],
});

const mixer = Mixer.fromSceneJson(sceneJson, sampleRate, 512);
const block = mixer.processStereo([vocalBlockL, returnBlockL], [vocalBlockR, returnBlockR]);
console.log(offline.meters[0].maxTruePeakDb, block.left.length);

const outL = new Float32Array(512);
const outR = new Float32Array(512);
mixer.processStereoInto([vocalBlockL, returnBlockL], [vocalBlockR, returnBlockR], outL, outR);

const realtime = mixer.createRealtimeBuffer();
realtime.leftInputs[0].set(vocalBlockL);
realtime.rightInputs[0].set(vocalBlockR);
realtime.leftInputs[1].set(returnBlockL);
realtime.rightInputs[1].set(returnBlockR);
realtime.process();
console.log(realtime.outLeft[0], realtime.outRight[0]);
mixer.delete();
```

### Headless DAW project

`Project` is a headless arrangement model: audio & MIDI tracks and clips, MIDI
sequencing, SMF / MIDI 2.0 Clip File I/O, deterministic JSON save/load, and an
offline `bounce`. Every mutation routes through an undoable history, and musical
positions are PPQ (quarter notes). Call `delete()` (or wrap in `try/finally`) to
release the WASM object — the embind handle is not garbage-collected.

```typescript
import { init, Project } from '@libraz/libsonare';

await init();

const project = new Project();
try {
  project.setSampleRate(48000);

  const { clipId } = project.addMidiClip(0, 4);          // { trackId, clipId }
  project.setMidiEvents(clipId, [
    Project.midiNoteOn(0, 0, 0, 60, 100),                // ppq, group, channel, note, velocity
    Project.midiNoteOff(1, 0, 0, 60),
  ]);

  const json = project.toJson();                         // deterministic, byte-stable within a build
  const smf = project.exportSmf();                       // Uint8Array — Standard MIDI File
  const midi2 = project.exportClipFile();                // Uint8Array — MIDI 2.0 Clip File (lossless)

  const { hasTimeline, diagnostics } = project.compile();
  const audio = project.bounce({ numChannels: 2 });      // interleaved Float32Array
} finally {
  project.delete();
}
```

#### Clip warp

A clip can be time-warped during an offline `bounce`. `setClipWarpMode` selects
the playback mode (`ProjectWarpMode`: `'off'` | `'repitch'` | `'tempo-sync'`),
`setClipWarpRef` binds it to a warp map, and `setWarpMap` registers a first-class
warp map (anchors mapping warp-timeline samples to source samples).

```typescript
project.setWarpMap({
  id: 1,
  name: 'main',
  anchors: [
    { warpSample: 0, sourceSample: 0 },
    { warpSample: 48000, sourceSample: 24000 },
  ],
});
project.setClipWarpRef(clipId, 1);
project.setClipWarpMode(clipId, 'tempo-sync');
```

> Warp is an offline `Project.bounce` feature only. Realtime warp playback is
> **not** available in `RealtimeEngine`.

### Instruments and synthesis

MIDI tracks bounce silently unless an instrument is bound. `Project` offers
three instrument backends, each as a `bounceWith…` variant that takes a binding
(or array of bindings) plus the usual `ProjectBounceOptions`:

- `bounceWithBuiltinInstrument(binding?, options?)` — simple built-in oscillator
  synth (`BuiltinSynthConfig` / `BuiltinSynthBinding`: waveform + ADSR + gain).
- `bounceWithSynthInstrument(patchOrName?, options?)` — patch-driven NativeSynth
  (`SynthPatch`, or a preset-name string like `'saw-lead'`).
- `bounceWithSf2Instrument(config?, options?)` — GS-compatible SoundFont player
  (`Sf2InstrumentConfig`), fed by `loadSoundFont()`.

Discover NativeSynth presets with `synthPresetNames()` and fetch one as an
editable patch with `synthPresetPatch(name)`.

```typescript
import { init, Project, synthPresetNames, synthPresetPatch } from '@libraz/libsonare';

await init();

const project = new Project();
try {
  const { clipId } = project.addMidiClip(0, 4);
  project.setMidiEvents(clipId, [
    Project.midiNoteOn(0, 0, 0, 60, 100),
    Project.midiNoteOff(1, 0, 0, 60),
  ]);

  // Built-in oscillator synth.
  const a = project.bounceWithBuiltinInstrument({ waveform: 'saw' }, { numChannels: 2 });

  // NativeSynth from a named preset, tweaked.
  const patch = synthPresetPatch(synthPresetNames()[0]);
  patch.cutoffHz = 4000;
  const b = project.bounceWithSynthInstrument(patch, { numChannels: 2 });

  // SoundFont player (requires loadSoundFont first).
  project.loadSoundFont(sf2Bytes);
  const c = project.bounceWithSf2Instrument({ gain: 0.6 }, { numChannels: 2 });
} finally {
  project.delete();
}
```

### Real-time engine

`RealtimeEngine` is a control-thread-driven transport + render engine: it plays
a clip/automation timeline, hosts MIDI instruments, accepts live MIDI, and
renders blocks (or bounces offline). Bind it to an AudioWorklet for browser
playback — see the [AudioWorklet bridge](#audioworklet-bridge) below; the engine
is the offline/headless half, the worklet is the audio-thread half.

```typescript
import { init, RealtimeEngine } from '@libraz/libsonare';

await init();

const engine = new RealtimeEngine(48000, 128); // sampleRate, maxBlockSize
try {
  engine.setSynthInstrument('saw-lead', 0);     // patch (or name), destinationId
  engine.play();
  engine.pushMidiNoteOn(0, 0, 0, 60, 100);      // destination, group, channel, note, velocity
  engine.pushMidiNoteOff(0, 0, 0, 60);

  const blockL = new Float32Array(128);
  const blockR = new Float32Array(128);
  const out = engine.process([blockL, blockR]); // Float32Array[] per channel
  const telemetry = engine.drainTelemetry();
} finally {
  engine.destroy();
}
```

Capabilities:

- **Transport**: `play` / `stop` / `seekSample` / `seekPpq` / `setTempo` /
  `setTimeSignature` / `setLoop`, plus `getTransportState`.
- **Instruments**: `setBuiltinInstrument` / `setSynthInstrument` /
  `setSf2Instrument` (+ `loadSoundFont`) per MIDI destination id.
- **Live MIDI**: `pushMidiNoteOn` / `pushMidiNoteOff` / `pushMidiCc` /
  `pushMidiPanic`, and `bindMidiCc(channel, controller, paramId, options?)` to
  map a CC to an automation parameter.
- **Process / bounce**: `process` (real-time blocks), the allocation-free
  `prepareChannels` + `getChannelBuffer` + `processPrepared` worklet path,
  `renderOffline`, `bounceOffline`, `freezeOffline`.
- **Clip page providers**: `createClipPageProvider` + `supplyClipPage` for
  streaming large clip audio in pages; pair with the OPFS helpers
  (`createOpfsClipPageProvider`).
- **Telemetry**: `drainTelemetry` / `drainMeterTelemetry`. Inspect runtime
  capabilities (ABI compatibility, SharedArrayBuffer/Atomics) via
  `engineCapabilities()`.

### Real-time voice changer

`RealtimeVoiceChanger` runs a block-by-block voice transformation chain (retune,
formant shaping, EQ, gate, compressor). Construct it from a preset id (see
`realtimeVoiceChangerPresetNames()`) or a full config object, then process blocks.

```typescript
import { init, RealtimeVoiceChanger, voiceChangeRealtime } from '@libraz/libsonare';

await init();

const changer = new RealtimeVoiceChanger('bright-idol');
try {
  changer.prepare(48000, 128, 1); // sampleRate, maxBlockSize, channels
  const out = changer.processMono(block);
} finally {
  changer.delete();
}

// Whole-buffer convenience wrapper (constructs/prepares/disposes internally).
const processed = voiceChangeRealtime(samples, { preset: 'deep-narrator', sampleRate: 48000 });
```

For a simple offline pitch + formant shift without the full chain, use
`voiceChange(samples, sampleRate, { pitchSemitones: -2, formantFactor: 1.1 })`.
Inspect a preset with `realtimeVoiceChangerPresetJson(name)`.

### AudioWorklet bridge

The package exposes an optional worklet entry that uses the same `sonare.wasm`
as the offline API. The bridge processes fixed 128-sample render quanta and
treats each AudioWorklet input as one stereo mixer strip.

```typescript
// worklet.ts, loaded with audioContext.audioWorklet.addModule(...)
import { init, mixingScenePresetJson } from '@libraz/libsonare';
import { registerSonareWorkletProcessor } from '@libraz/libsonare/worklet';

await init();
registerSonareWorkletProcessor();
```

```typescript
// main thread
import { mixingScenePresetJson } from '@libraz/libsonare';

await audioContext.audioWorklet.addModule('/worklet.js');
const sceneJson = mixingScenePresetJson('vocalReverbSend');
const node = new AudioWorkletNode(audioContext, 'sonare-worklet-processor', {
  numberOfInputs: 2,
  numberOfOutputs: 1,
  outputChannelCount: [2],
  processorOptions: {
    sceneJson,
    sampleRate: audioContext.sampleRate,
    blockSize: 128,
    spectrumIntervalFrames: 2048,
    spectrumBands: 16,
  },
});

node.port.postMessage({
  type: 'scheduleInsertAutomation',
  stripIndex: 0,
  insertIndex: 0,
  paramId: 0,
  samplePos: 0,
  value: 0,
  curve: 'linear',
});

node.port.onmessage = (event) => {
  if (event.data?.type === 'meter') {
    console.log(event.data.peakDbL, event.data.rmsDbL, event.data.correlation);
  } else if (event.data?.type === 'spectrum') {
    console.log(event.data.frame, event.data.bands);
  }
};
```

For cross-origin-isolated pages, meters and spectrum snapshots can use optional
SharedArrayBuffer rings instead of per-message `postMessage`:

```typescript
import {
  createSonareMeterRingBuffer,
  createSonareSpectrumRingBuffer,
  readSonareMeterRingBuffer,
  readSonareSpectrumRingBuffer,
} from '@libraz/libsonare/worklet';

const meterRing = createSonareMeterRingBuffer(128);
const spectrumRing = createSonareSpectrumRingBuffer(64, 16);
const node = new AudioWorkletNode(audioContext, 'sonare-worklet-processor', {
  numberOfInputs: 2,
  numberOfOutputs: 1,
  outputChannelCount: [2],
  processorOptions: {
    sceneJson,
    sampleRate: audioContext.sampleRate,
    blockSize: 128,
    meterSharedBuffer: meterRing.sharedBuffer,
    spectrumIntervalFrames: 2048,
    spectrumSharedBuffer: spectrumRing.sharedBuffer,
  },
});

let nextMeterRead = 0;
let nextSpectrumRead = 0;
function readMeters() {
  const result = readSonareMeterRingBuffer(meterRing, nextMeterRead);
  nextMeterRead = result.nextReadIndex;
  for (const meter of result.meters) {
    console.log(meter.frame, meter.peakDbL, meter.rmsDbL);
  }
  const spectra = readSonareSpectrumRingBuffer(spectrumRing, nextSpectrumRead);
  nextSpectrumRead = spectra.nextReadIndex;
  for (const spectrum of spectra.spectra) {
    console.log(spectrum.frame, spectrum.bands);
  }
}
```

### Progress callback

`masteringChainWithProgress` (and its stereo variant) is `masteringChain` with
an extra `(progress, stage) => void` callback invoked after each enabled stage:

```typescript
import { init, masteringChainWithProgress } from '@libraz/libsonare';

await init();

masteringChainWithProgress(
  samples,
  sampleRate,
  { dynamics: { compressor: { thresholdDb: -24 } } },
  (progress, stage) => console.log(`${stage}: ${(progress * 100).toFixed(0)}%`),
);
```

### Streaming mastering chain

`StreamingMasteringChain` processes blocks while maintaining per-stage state
across calls. It only supports modules whose state depends solely on the
sample rate — it cannot include `repair.denoise` or `loudness` (those require
offline / look-ahead analysis) and throws at construction if they are enabled.

```typescript
import { init, StreamingMasteringChain } from '@libraz/libsonare';

await init();

const chain = new StreamingMasteringChain({
  eq: { tiltDb: 0.5 },
  dynamics: { compressor: { thresholdDb: -20 } },
});
chain.prepare(48000, 512, 2);

// Mono block
const monoBlock = new Float32Array(512);
const processedMono = chain.processMono(monoBlock);

// Stereo block (separate L/R Float32Arrays)
const left = new Float32Array(512);
const right = new Float32Array(512);
const { left: outL, right: outR } = chain.processStereo(left, right);

chain.reset();
chain.delete(); // release WASM memory
```

### Streaming equalizer and retune

`StreamingEqualizer` wraps the unified `EqualizerProcessor` (up to 24 bands,
RBJ/Vicanek biquads, dynamic EQ, linear-phase FIR, mid/side, auto-gain) with
state maintained across calls. `StreamingRetune` is a block-by-block mono voice
retune / pitch shifter.

```typescript
import { init, StreamingEqualizer, StreamingRetune } from '@libraz/libsonare';

await init();

const eq = new StreamingEqualizer({ sampleRate: 48000, maxBlockSize: 512 });
try {
  eq.setBand(0, { type: 'HighShelf', frequencyHz: 8000, gainDb: 6, enabled: true });
  const { left: eqL, right: eqR } = eq.processStereo(left, right);
} finally {
  eq.delete();
}

const retune = new StreamingRetune({ semitones: 2, mix: 1.0 });
try {
  retune.prepare(48000, 512); // sampleRate, maxBlockSize
  const shifted = retune.processMono(monoBlock);
} finally {
  retune.delete();
}
```

## Features

- **Detection**: `detectBeats`, `detectOnsets`, `detectDownbeats`, `detectChords`, `detectKey`, `detectKeyCandidates`, `chordFunctionalAnalysis`, sections
- **Analysis**: `analyze`, `analyzeWithProgress`, `analyzeBpm`, `analyzeRhythm`, `analyzeDynamics`, `analyzeTimbre`; `hasFfmpegSupport` capability check
- **Effects**: HPSS, HPSS with residual, time stretch, phase vocoder, pitch shift, normalize, trim, remix
- **Mastering**: EQ, compressor, tape/exciter, air band, stereo imaging,
  true-peak limiting, loudness optimization
- **Features**: STFT, mel spectrogram, MFCC, chroma, CQT/VQT, spectral contrast, poly features, zero crossings
- **Pitch**: YIN, pYIN algorithms with optional `fillNa`
- **Decomposition & loudness**: NMF decomposition, nearest-neighbour filtering, multichannel LUFS, EBU R128 LRA
- **Streaming**: Real-time analysis with progressive estimates; streaming mastering chain, equalizer, and retune
- **Instruments**: built-in synth, patch-driven NativeSynth, SoundFont (SF2) player — bound to `Project` bounces or the `RealtimeEngine`
- **Real-time**: `RealtimeEngine` transport/MIDI/render, `RealtimeVoiceChanger`, AudioWorklet bridge
- **Room acoustics**: blind RT60/EDT, impulse-response clarity metrics, RIR synthesis, room estimation, room morphing
- **Headless DAW**: `Project` arrangement model — audio/MIDI tracks & clips, undo/redo, MIDI sequencing, clip warp, SMF / MIDI 2.0 Clip File I/O, deterministic JSON, offline `bounce`
- **Conversions**: Hz/mel/MIDI/note, frames/time, resample

## Also available

```bash
pip install libsonare  # Python bindings with CLI
```

## License

[Apache License 2.0](https://github.com/libraz/libsonare/blob/main/LICENSE)
