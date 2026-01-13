# WebAssembly Guide

## Overview

libsonare can be compiled to WebAssembly, enabling audio analysis directly in web browsers without server-side processing.

## Installation

### npm/yarn

```bash
npm install @libraz/sonare
# or
yarn add @libraz/sonare
```

### CDN

```html
<script type="module">
  import createSonare from 'https://unpkg.com/@libraz/sonare/dist/sonare.js';
</script>
```

## Basic Usage

### ES Modules

```typescript
import { Sonare } from '@libraz/sonare';

async function analyzeAudio() {
  const sonare = await Sonare.create();

  // Get audio data from AudioContext
  const audioCtx = new AudioContext();
  const response = await fetch('music.mp3');
  const arrayBuffer = await response.arrayBuffer();
  const audioBuffer = await audioCtx.decodeAudioData(arrayBuffer);

  // Get mono samples
  const samples = audioBuffer.getChannelData(0);
  const sampleRate = audioBuffer.sampleRate;

  // Detect BPM
  const bpm = sonare.detectBpm(samples, sampleRate);
  console.log(`BPM: ${bpm}`);

  // Detect key
  const key = sonare.detectKey(samples, sampleRate);
  console.log(`Key: ${key.root} ${key.mode}`);
}
```

### CommonJS

```javascript
const { Sonare } = require('@libraz/sonare');

async function main() {
  const sonare = await Sonare.create();
  // ... same as above
}
```

## TypeScript Types

```typescript
interface Key {
  root: PitchClass;   // 0-11 (C=0, B=11)
  mode: Mode;         // 0=Major, 1=Minor
  confidence: number; // 0.0-1.0
}

enum PitchClass {
  C = 0, Cs = 1, D = 2, Ds = 3, E = 4, F = 5,
  Fs = 6, G = 7, Gs = 8, A = 9, As = 10, B = 11
}

enum Mode {
  Major = 0,
  Minor = 1
}

interface AnalysisResult {
  bpm: number;
  key: Key;
  timeSignature: TimeSignature;
  beats: Beat[];
  chords: Chord[];
  sections: Section[];
}
```

## API Reference

### Sonare Class

```typescript
class Sonare {
  // Initialize
  static async create(): Promise<Sonare>;

  // BPM detection
  detectBpm(samples: Float32Array, sampleRate: number): number;

  // Key detection
  detectKey(samples: Float32Array, sampleRate: number): Key;

  // Beat detection
  detectBeats(samples: Float32Array, sampleRate: number): number[];

  // Onset detection
  detectOnsets(samples: Float32Array, sampleRate: number): number[];

  // Full analysis
  analyze(samples: Float32Array, sampleRate: number): AnalysisResult;

  // Version info
  version(): string;
}
```

## Working with Audio Files

### File Input

```typescript
async function analyzeFile(file: File) {
  const sonare = await Sonare.create();
  const audioCtx = new AudioContext();

  const arrayBuffer = await file.arrayBuffer();
  const audioBuffer = await audioCtx.decodeAudioData(arrayBuffer);
  const samples = audioBuffer.getChannelData(0);

  const bpm = sonare.detectBpm(samples, audioBuffer.sampleRate);
  return bpm;
}

// Usage with file input
const input = document.querySelector('input[type="file"]');
input.addEventListener('change', async (e) => {
  const file = e.target.files[0];
  const bpm = await analyzeFile(file);
  console.log(`BPM: ${bpm}`);
});
```

### Microphone Input

```typescript
async function analyzeFromMicrophone() {
  const sonare = await Sonare.create();
  const audioCtx = new AudioContext();

  const stream = await navigator.mediaDevices.getUserMedia({ audio: true });
  const source = audioCtx.createMediaStreamSource(stream);
  const analyser = audioCtx.createScriptProcessor(4096, 1, 1);

  const samples: number[] = [];

  analyser.onaudioprocess = (e) => {
    const input = e.inputBuffer.getChannelData(0);
    samples.push(...input);

    // Analyze every 5 seconds of audio
    if (samples.length >= audioCtx.sampleRate * 5) {
      const float32 = new Float32Array(samples);
      const bpm = sonare.detectBpm(float32, audioCtx.sampleRate);
      console.log(`Current BPM: ${bpm}`);
      samples.length = 0; // Clear buffer
    }
  };

  source.connect(analyser);
  analyser.connect(audioCtx.destination);
}
```

## Performance Tips

### Resampling for Faster Analysis

For BPM detection, you can downsample to 22050Hz for faster processing:

```typescript
async function fastBpmDetection(samples: Float32Array, sampleRate: number) {
  const sonare = await Sonare.create();

  // Downsample to 22050Hz if needed
  let processedSamples = samples;
  let processedSampleRate = sampleRate;

  if (sampleRate > 22050) {
    // Simple downsampling (use proper resampling in production)
    const ratio = sampleRate / 22050;
    const newLength = Math.floor(samples.length / ratio);
    processedSamples = new Float32Array(newLength);
    for (let i = 0; i < newLength; i++) {
      processedSamples[i] = samples[Math.floor(i * ratio)];
    }
    processedSampleRate = 22050;
  }

  return sonare.detectBpm(processedSamples, processedSampleRate);
}
```

### Web Workers

For heavy analysis, use Web Workers to avoid blocking the main thread:

```typescript
// worker.ts
import { Sonare } from '@libraz/sonare';

let sonare: Sonare;

self.onmessage = async (e) => {
  if (!sonare) {
    sonare = await Sonare.create();
  }

  const { samples, sampleRate, action } = e.data;

  switch (action) {
    case 'bpm':
      const bpm = sonare.detectBpm(samples, sampleRate);
      self.postMessage({ type: 'bpm', result: bpm });
      break;
    case 'analyze':
      const result = sonare.analyze(samples, sampleRate);
      self.postMessage({ type: 'analyze', result });
      break;
  }
};
```

```typescript
// main.ts
const worker = new Worker(new URL('./worker.ts', import.meta.url));

worker.onmessage = (e) => {
  console.log('Result:', e.data);
};

// Send audio for analysis
worker.postMessage({
  action: 'analyze',
  samples: audioBuffer.getChannelData(0),
  sampleRate: audioBuffer.sampleRate
});
```

## Memory Management

### Automatic Cleanup

The Sonare wrapper automatically manages memory. However, for large audio files, you may want to explicitly manage the audio data:

```typescript
async function analyzeWithCleanup(url: string) {
  const sonare = await Sonare.create();
  const audioCtx = new AudioContext();

  const response = await fetch(url);
  const arrayBuffer = await response.arrayBuffer();
  const audioBuffer = await audioCtx.decodeAudioData(arrayBuffer);

  // Get samples (this creates a copy)
  const samples = audioBuffer.getChannelData(0);

  // Analyze
  const result = sonare.analyze(samples, audioBuffer.sampleRate);

  // AudioBuffer will be garbage collected
  // Float32Array (samples) will also be garbage collected

  return result;
}
```

## Browser Compatibility

| Browser | Support |
|---------|---------|
| Chrome | 57+ |
| Firefox | 52+ |
| Safari | 11+ |
| Edge | 16+ |

Requirements:
- WebAssembly support
- Web Audio API
- ES2017+ (async/await)

## Bundle Size

- `sonare.js`: ~34KB (gzipped: ~12KB)
- `sonare.wasm`: ~228KB (gzipped: ~80KB)

Total: ~262KB (~92KB gzipped)

## Troubleshooting

### "Out of memory" Error

For very long audio files, increase the WASM memory:

```javascript
// This is handled internally, but you can monitor memory usage
console.log('Memory usage:', performance.memory?.usedJSHeapSize);
```

### AudioContext Not Allowed

Modern browsers require user interaction before creating AudioContext:

```typescript
document.addEventListener('click', async () => {
  const audioCtx = new AudioContext();
  await audioCtx.resume();
  // Now you can use audioCtx
});
```

### Cross-Origin Issues

When loading audio from other domains, ensure CORS headers are set:

```typescript
const response = await fetch(url, {
  mode: 'cors',
  credentials: 'omit'
});
```
