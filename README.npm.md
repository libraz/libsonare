# libsonare

[![CI](https://img.shields.io/github/actions/workflow/status/libraz/libsonare/ci.yml?branch=main&label=CI)](https://github.com/libraz/libsonare/actions)
[![npm](https://img.shields.io/npm/v/@libraz/libsonare)](https://www.npmjs.com/package/@libraz/libsonare)
[![codecov](https://codecov.io/gh/libraz/libsonare/branch/main/graph/badge.svg)](https://codecov.io/gh/libraz/libsonare)
[![License](https://img.shields.io/github/license/libraz/libsonare)](https://github.com/libraz/libsonare/blob/main/LICENSE)
[![C++17](https://img.shields.io/badge/C%2B%2B-17-blue?logo=c%2B%2B)](https://en.cppreference.com/w/cpp/17)

Fast, dependency-free audio analysis library for browser and Node.js via WebAssembly. A librosa-like API for music information retrieval.

## Usage

```typescript
import { init, detectBpm, detectKey, analyze } from '@libraz/libsonare';

await init();

// Quick analysis
const result = await analyze(audioBuffer, sampleRate);
console.log(`BPM: ${result.bpm}, Key: ${result.key.tonic} ${result.key.mode}`);

// Individual features
const bpm = detectBpm(audioBuffer, sampleRate);
const key = detectKey(audioBuffer, sampleRate);
```

### Browser (CDN)

```html
<script type="module">
  import { init, analyze } from 'https://esm.sh/@libraz/libsonare';

  await init();
  // ... use with Web Audio API
</script>
```

### Bundlers (webpack, Vite, Next.js, etc.)

If your bundler doesn't automatically resolve the `.wasm` file, specify its path:

```typescript
import wasmUrl from '@libraz/libsonare/wasm?url'; // Vite
import { init } from '@libraz/libsonare';

await init({ wasmPath: wasmUrl });
```

## Features

- BPM / tempo detection
- Key detection (major/minor)
- Beat tracking
- Chord recognition
- Onset detection
- Mel spectrogram, MFCC, Chroma
- Spectral features (centroid, bandwidth, rolloff, flatness)
- Pitch detection (YIN, pYIN)
- HPSS (harmonic-percussive separation)
- Time stretching & pitch shifting
- Real-time streaming analysis

## License

[Apache License 2.0](https://github.com/libraz/libsonare/blob/main/LICENSE)
