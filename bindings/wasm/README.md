# libsonare

[![CI](https://img.shields.io/github/actions/workflow/status/libraz/libsonare/ci.yml?branch=main&label=CI)](https://github.com/libraz/libsonare/actions)
[![npm](https://img.shields.io/npm/v/@libraz/libsonare)](https://www.npmjs.com/package/@libraz/libsonare)
[![npm downloads](https://img.shields.io/npm/dm/@libraz/libsonare)](https://www.npmjs.com/package/@libraz/libsonare)
[![types](https://img.shields.io/npm/types/@libraz/libsonare)](https://www.npmjs.com/package/@libraz/libsonare)
[![License](https://img.shields.io/github/license/libraz/libsonare)](https://github.com/libraz/libsonare/blob/main/LICENSE)
[![PyPI](https://img.shields.io/pypi/v/libsonare?label=PyPI)](https://pypi.org/project/libsonare/)

A dependency-free audio DSP toolkit for browser and Node.js via WebAssembly —
librosa-compatible analysis plus broadcast-grade mastering, mixing, and editing.
The same C++ processors run client-side in the browser: 77 named mastering DSP
processors implemented against published references (ITU-R BS.1770-4 true-peak
limiting, Linkwitz-Riley crossovers, Vicanek matched-Z biquads, ADAA-antialiased
saturation), with analysis defaults matching librosa — Apache-2.0, no Python,
no model weights.

> **Audio input:** This package expects already-decoded `Float32Array` mono
> samples (it does not bundle a file decoder). Use the Web Audio API in the
> browser or `node:wasi` / a JS audio decoder in Node to obtain samples.
> If you need to read WAV/MP3/M4A files directly in Node, use the native
> N-API package [`@libraz/libsonare-native`](https://github.com/libraz/libsonare/tree/main/bindings/node) instead.

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

await init({ wasmPath: wasmUrl });
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

## Features

- **Detection**: BPM, key, beats, onsets, chords, sections
- **Effects**: HPSS, time stretch, pitch shift, normalize, trim
- **Mastering**: EQ, compressor, tape/exciter, air band, stereo imaging,
  true-peak limiting, loudness optimization
- **Features**: STFT, mel spectrogram, MFCC, chroma, CQT/VQT, spectral features
- **Pitch**: YIN, pYIN algorithms
- **Streaming**: Real-time analysis with progressive estimates
- **Conversions**: Hz/mel/MIDI/note, frames/time, resample

## Also available

```bash
pip install libsonare  # Python bindings with CLI
```

## License

[Apache License 2.0](https://github.com/libraz/libsonare/blob/main/LICENSE)
