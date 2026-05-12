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

## Features

- **Detection**: BPM, key, beats, onsets, chords, sections
- **Effects**: HPSS, time stretch, pitch shift, normalize, trim
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
