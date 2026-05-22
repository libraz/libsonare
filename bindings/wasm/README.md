# libsonare

[![CI](https://img.shields.io/github/actions/workflow/status/libraz/libsonare/ci.yml?branch=main&label=CI)](https://github.com/libraz/libsonare/actions)
[![npm](https://img.shields.io/npm/v/@libraz/libsonare)](https://www.npmjs.com/package/@libraz/libsonare)
[![PyPI](https://img.shields.io/pypi/v/libsonare)](https://pypi.org/project/libsonare/)
[![License](https://img.shields.io/github/license/libraz/libsonare)](https://github.com/libraz/libsonare/blob/main/LICENSE)

Fast, dependency-free audio analysis library for browser and Node.js via WebAssembly.

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

// Audio class API
const audio = Audio.fromBuffer(samples, sampleRate);
console.log(`BPM: ${audio.detectBpm()}`);
console.log(`Key: ${audio.detectKey().name}`);
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

masteringPresetNames(); // ['pop', 'edm', 'acoustic', 'hipHop', 'aiMusic', 'speech']

const result = masterAudio('aiMusic', samples, sampleRate, {
  // optional flat overrides applied on top of the preset
  'loudness.targetLufs': -13,
});
console.log(result.outputLufs, result.appliedGainDb);
```

### Progress callback

```typescript
import { init, masteringChain } from '@libraz/libsonare';

await init();

masteringChain(samples, sampleRate,
  { 'dynamics.compressor.thresholdDb': -24 },
  (progress, stage) => console.log(`${stage}: ${(progress * 100).toFixed(0)}%`),
);
```

The same `(progress, stage) => void` callback is accepted as the final argument
of `masterAudio`.

### Streaming mastering chain

`StreamingMasteringChain` processes interleaved blocks in place and only
supports modules whose state depends solely on the sample rate. It cannot
include `repair.denoise` or `loudness` (those require offline / look-ahead
analysis).

```typescript
import { init, StreamingMasteringChain } from '@libraz/libsonare';

await init();

const chain = new StreamingMasteringChain({
  'eq.tilt.tiltDb': 0.5,
  'dynamics.compressor.thresholdDb': -20,
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
