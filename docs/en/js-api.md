# JavaScript/TypeScript API Reference

Complete API reference for libsonare JavaScript/TypeScript interface.

## Table of Contents

- [Installation](#installation)
- [Initialization](#initialization)
- [Core Functions](#core-functions)
- [Types and Interfaces](#types-and-interfaces)
- [Enumerations](#enumerations)
- [Error Handling](#error-handling)
- [Advanced Usage](#advanced-usage)

---

## Installation

### npm/yarn

```bash
npm install @libraz/sonare
# or
yarn add @libraz/sonare
```

### Import

```typescript
// ES Modules (recommended)
import {
  init,
  detectBpm,
  detectKey,
  detectBeats,
  detectOnsets,
  analyze,
  analyzeWithProgress,
  version
} from '@libraz/sonare';

// CommonJS
const sonare = require('@libraz/sonare');
```

---

## Initialization

Before using any analysis functions, you must initialize the WASM module.

### `init(options?)`

Initializes the WASM module. Must be called before any analysis functions.

```typescript
async function init(options?: InitOptions): Promise<void>

interface InitOptions {
  /**
   * Custom function to locate WASM files.
   * Useful for custom CDN or bundler configurations.
   */
  locateFile?: (path: string, prefix: string) => string;
}
```

**Example:**

```typescript
import { init, detectBpm } from '@libraz/sonare';

// Basic initialization
await init();

// With custom file location
await init({
  locateFile: (path, prefix) => {
    return `/custom/wasm/path/${path}`;
  }
});

// Now you can use analysis functions
const bpm = detectBpm(samples, sampleRate);
```

### `isInitialized()`

Check if the module is initialized.

```typescript
function isInitialized(): boolean
```

### `version()`

Get the library version.

```typescript
function version(): string  // e.g., "1.0.0"
```

---

## Core Functions

### `detectBpm(samples, sampleRate)`

Detect BPM (tempo) from audio samples.

```typescript
function detectBpm(samples: Float32Array, sampleRate: number): number
```

**Parameters:**
- `samples` - Mono audio samples (Float32Array, range -1.0 to 1.0)
- `sampleRate` - Sample rate in Hz (e.g., 44100, 48000)

**Returns:** Detected BPM as a number (typically 60-200)

**Accuracy:** ±2 BPM

**Example:**

```typescript
import { init, detectBpm } from '@libraz/sonare';

await init();

// Get samples from AudioBuffer (Web Audio API)
const audioCtx = new AudioContext();
const response = await fetch('song.mp3');
const arrayBuffer = await response.arrayBuffer();
const audioBuffer = await audioCtx.decodeAudioData(arrayBuffer);

const samples = audioBuffer.getChannelData(0);  // Mono
const sampleRate = audioBuffer.sampleRate;

const bpm = detectBpm(samples, sampleRate);
console.log(`BPM: ${bpm}`);  // e.g., "BPM: 120"
```

---

### `detectKey(samples, sampleRate)`

Detect musical key from audio samples.

```typescript
function detectKey(samples: Float32Array, sampleRate: number): Key
```

**Parameters:**
- `samples` - Mono audio samples
- `sampleRate` - Sample rate in Hz

**Returns:** [`Key`](#key) object with root, mode, confidence, and name

**Example:**

```typescript
import { init, detectKey, PitchClass, Mode } from '@libraz/sonare';

await init();

const key = detectKey(samples, sampleRate);

console.log(`Key: ${key.name}`);           // "C major"
console.log(`Short: ${key.shortName}`);    // "C"
console.log(`Root: ${key.root}`);          // 0 (PitchClass.C)
console.log(`Mode: ${key.mode}`);          // 0 (Mode.Major)
console.log(`Confidence: ${(key.confidence * 100).toFixed(1)}%`);

// Check specific keys
if (key.root === PitchClass.C && key.mode === Mode.Major) {
  console.log('Song is in C major!');
}
```

---

### `detectBeats(samples, sampleRate)`

Detect beat times from audio samples.

```typescript
function detectBeats(samples: Float32Array, sampleRate: number): Float32Array
```

**Parameters:**
- `samples` - Mono audio samples
- `sampleRate` - Sample rate in Hz

**Returns:** Float32Array of beat times in seconds

**Example:**

```typescript
import { init, detectBeats } from '@libraz/sonare';

await init();

const beats = detectBeats(samples, sampleRate);

console.log(`Found ${beats.length} beats`);

// Print first 10 beat times
for (let i = 0; i < Math.min(10, beats.length); i++) {
  console.log(`Beat ${i + 1}: ${beats[i].toFixed(3)}s`);
}

// Calculate average BPM from beats
if (beats.length > 1) {
  const intervals = [];
  for (let i = 1; i < beats.length; i++) {
    intervals.push(beats[i] - beats[i - 1]);
  }
  const avgInterval = intervals.reduce((a, b) => a + b) / intervals.length;
  const calculatedBpm = 60 / avgInterval;
  console.log(`Calculated BPM: ${calculatedBpm.toFixed(1)}`);
}
```

---

### `detectOnsets(samples, sampleRate)`

Detect onset times (note attacks) from audio samples.

```typescript
function detectOnsets(samples: Float32Array, sampleRate: number): Float32Array
```

**Parameters:**
- `samples` - Mono audio samples
- `sampleRate` - Sample rate in Hz

**Returns:** Float32Array of onset times in seconds

**Example:**

```typescript
import { init, detectOnsets } from '@libraz/sonare';

await init();

const onsets = detectOnsets(samples, sampleRate);

console.log(`Found ${onsets.length} onsets`);

// Useful for:
// - Note segmentation
// - Rhythm analysis
// - Audio-to-MIDI conversion
```

---

### `analyze(samples, sampleRate)`

Perform complete music analysis.

```typescript
function analyze(samples: Float32Array, sampleRate: number): AnalysisResult
```

**Parameters:**
- `samples` - Mono audio samples
- `sampleRate` - Sample rate in Hz

**Returns:** [`AnalysisResult`](#analysisresult) with BPM, key, beats, chords, sections, etc.

**Example:**

```typescript
import { init, analyze } from '@libraz/sonare';

await init();

const result = analyze(samples, sampleRate);

console.log('=== Music Analysis ===');
console.log(`BPM: ${result.bpm} (confidence: ${(result.bpmConfidence * 100).toFixed(0)}%)`);
console.log(`Key: ${result.key.name}`);
console.log(`Time Signature: ${result.timeSignature.numerator}/${result.timeSignature.denominator}`);

console.log(`\nBeats: ${result.beats.length}`);

console.log('\nChords:');
for (const chord of result.chords) {
  console.log(`  ${chord.name} [${chord.start.toFixed(2)}s - ${chord.end.toFixed(2)}s]`);
}

console.log('\nSections:');
for (const section of result.sections) {
  console.log(`  ${section.name} [${section.start.toFixed(2)}s - ${section.end.toFixed(2)}s]`);
}

console.log('\nTimbre:');
console.log(`  Brightness: ${result.timbre.brightness.toFixed(2)}`);
console.log(`  Warmth: ${result.timbre.warmth.toFixed(2)}`);

console.log(`\nForm: ${result.form}`);  // e.g., "IABABCO"
```

---

### `analyzeWithProgress(samples, sampleRate, onProgress)`

Perform complete music analysis with progress reporting.

```typescript
function analyzeWithProgress(
  samples: Float32Array,
  sampleRate: number,
  onProgress: ProgressCallback
): AnalysisResult

type ProgressCallback = (progress: number, stage: string) => void;
```

**Parameters:**
- `samples` - Mono audio samples
- `sampleRate` - Sample rate in Hz
- `onProgress` - Callback receiving progress (0.0-1.0) and stage name

**Progress Stages:**
| Stage | Description | Progress Range |
|-------|-------------|----------------|
| `"bpm"` | BPM detection | 0.0 - 0.15 |
| `"key"` | Key detection | 0.15 - 0.25 |
| `"beats"` | Beat tracking | 0.25 - 0.45 |
| `"chords"` | Chord recognition | 0.45 - 0.65 |
| `"sections"` | Section detection | 0.65 - 0.85 |
| `"timbre"` | Timbre analysis | 0.85 - 0.95 |
| `"dynamics"` | Dynamics analysis | 0.95 - 1.0 |

**Example:**

```typescript
import { init, analyzeWithProgress } from '@libraz/sonare';

await init();

const result = analyzeWithProgress(samples, sampleRate, (progress, stage) => {
  const percent = Math.round(progress * 100);
  console.log(`${stage}: ${percent}%`);

  // Update UI progress bar
  updateProgressBar(percent, stage);
});

console.log(`Analysis complete: BPM=${result.bpm}`);
```

**React Example:**

```tsx
import { useState } from 'react';
import { init, analyzeWithProgress, AnalysisResult } from '@libraz/sonare';

function AudioAnalyzer() {
  const [progress, setProgress] = useState(0);
  const [stage, setStage] = useState('');
  const [result, setResult] = useState<AnalysisResult | null>(null);

  const handleAnalyze = async (samples: Float32Array, sampleRate: number) => {
    await init();

    const analysisResult = analyzeWithProgress(samples, sampleRate, (p, s) => {
      setProgress(p);
      setStage(s);
    });

    setResult(analysisResult);
  };

  return (
    <div>
      {stage && (
        <div>
          <div>{stage}: {Math.round(progress * 100)}%</div>
          <progress value={progress} max={1} />
        </div>
      )}
      {result && <div>BPM: {result.bpm}</div>}
    </div>
  );
}
```

---

## Types and Interfaces

### Key

Detected musical key.

```typescript
interface Key {
  /** Pitch class (0-11, C=0, B=11) */
  root: PitchClass;

  /** Mode (Major=0, Minor=1) */
  mode: Mode;

  /** Detection confidence (0.0 to 1.0) */
  confidence: number;

  /** Full name (e.g., "C major", "A minor") */
  name: string;

  /** Short name (e.g., "C", "Am") */
  shortName: string;
}
```

### Beat

Detected beat.

```typescript
interface Beat {
  /** Time in seconds */
  time: number;

  /** Beat strength (0.0 to 1.0) */
  strength: number;
}
```

### Chord

Detected chord.

```typescript
interface Chord {
  /** Root pitch class */
  root: PitchClass;

  /** Chord quality */
  quality: ChordQuality;

  /** Start time in seconds */
  start: number;

  /** End time in seconds */
  end: number;

  /** Detection confidence (0.0 to 1.0) */
  confidence: number;

  /** Chord name (e.g., "C", "Am", "G7") */
  name: string;
}
```

### Section

Detected song section.

```typescript
interface Section {
  /** Section type */
  type: SectionType;

  /** Start time in seconds */
  start: number;

  /** End time in seconds */
  end: number;

  /** Relative energy level (0.0 to 1.0) */
  energyLevel: number;

  /** Detection confidence (0.0 to 1.0) */
  confidence: number;

  /** Section name (e.g., "Intro", "Verse 1", "Chorus") */
  name: string;
}
```

### TimeSignature

Detected time signature.

```typescript
interface TimeSignature {
  /** Beats per measure (e.g., 4) */
  numerator: number;

  /** Beat unit (e.g., 4 for quarter note) */
  denominator: number;

  /** Detection confidence (0.0 to 1.0) */
  confidence: number;
}
```

### Timbre

Timbre characteristics.

```typescript
interface Timbre {
  /** High-frequency content (0.0 to 1.0) */
  brightness: number;

  /** Low-frequency emphasis (0.0 to 1.0) */
  warmth: number;

  /** Spectral density (0.0 to 1.0) */
  density: number;

  /** Dissonance/inharmonicity (0.0 to 1.0) */
  roughness: number;

  /** Spectral complexity (0.0 to 1.0) */
  complexity: number;
}
```

### Dynamics

Dynamics characteristics.

```typescript
interface Dynamics {
  /** Peak to RMS dynamic range in dB */
  dynamicRangeDb: number;

  /** Short-term loudness variation in dB */
  loudnessRangeDb: number;

  /** Peak to RMS ratio */
  crestFactor: number;

  /** True if audio appears heavily compressed */
  isCompressed: boolean;
}
```

### RhythmFeatures

Rhythm characteristics.

```typescript
interface RhythmFeatures {
  /** Amount of syncopation (0.0 to 1.0) */
  syncopation: number;

  /** Groove type ("straight", "shuffle", "swing") */
  grooveType: string;

  /** Rhythmic regularity (0.0 to 1.0) */
  patternRegularity: number;
}
```

### AnalysisResult

Complete analysis result.

```typescript
interface AnalysisResult {
  /** Detected BPM */
  bpm: number;

  /** BPM detection confidence (0.0 to 1.0) */
  bpmConfidence: number;

  /** Detected musical key */
  key: Key;

  /** Detected time signature */
  timeSignature: TimeSignature;

  /** Array of detected beats */
  beats: Beat[];

  /** Array of detected chords */
  chords: Chord[];

  /** Array of detected sections */
  sections: Section[];

  /** Overall timbre characteristics */
  timbre: Timbre;

  /** Dynamics characteristics */
  dynamics: Dynamics;

  /** Rhythm features */
  rhythm: RhythmFeatures;

  /** Song form string (e.g., "IABABCO") */
  form: string;
}
```

---

## Enumerations

### PitchClass

Pitch class (chromatic scale degrees).

```typescript
const PitchClass = {
  C: 0,
  Cs: 1,   // C#/Db
  D: 2,
  Ds: 3,   // D#/Eb
  E: 4,
  F: 5,
  Fs: 6,   // F#/Gb
  G: 7,
  Gs: 8,   // G#/Ab
  A: 9,
  As: 10,  // A#/Bb
  B: 11
} as const;

type PitchClass = (typeof PitchClass)[keyof typeof PitchClass];
```

**Helper function to get note name:**

```typescript
function pitchClassName(pc: PitchClass): string {
  const names = ['C', 'C#', 'D', 'D#', 'E', 'F', 'F#', 'G', 'G#', 'A', 'A#', 'B'];
  return names[pc];
}
```

### Mode

Musical mode (major/minor).

```typescript
const Mode = {
  Major: 0,
  Minor: 1
} as const;

type Mode = (typeof Mode)[keyof typeof Mode];
```

### ChordQuality

Chord quality types.

```typescript
const ChordQuality = {
  Major: 0,       // C, D, E...
  Minor: 1,       // Cm, Dm, Em...
  Diminished: 2,  // Cdim, Ddim...
  Augmented: 3,   // Caug, Daug...
  Dominant7: 4,   // C7, D7...
  Major7: 5,      // Cmaj7, Dmaj7...
  Minor7: 6,      // Cm7, Dm7...
  Sus2: 7,        // Csus2, Dsus2...
  Sus4: 8         // Csus4, Dsus4...
} as const;

type ChordQuality = (typeof ChordQuality)[keyof typeof ChordQuality];
```

### SectionType

Song section types.

```typescript
const SectionType = {
  Intro: 0,
  Verse: 1,
  PreChorus: 2,
  Chorus: 3,
  Bridge: 4,
  Instrumental: 5,
  Outro: 6
} as const;

type SectionType = (typeof SectionType)[keyof typeof SectionType];
```

---

## Error Handling

All analysis functions throw errors if the module is not initialized.

```typescript
import { init, detectBpm, isInitialized } from '@libraz/sonare';

// Check initialization
if (!isInitialized()) {
  await init();
}

// Safe usage with try-catch
try {
  const bpm = detectBpm(samples, sampleRate);
  console.log(`BPM: ${bpm}`);
} catch (error) {
  if (error.message.includes('not initialized')) {
    console.error('Module not initialized. Call init() first.');
  } else {
    console.error('Analysis error:', error);
  }
}
```

### Common Errors

| Error | Cause | Solution |
|-------|-------|----------|
| "Module not initialized" | Called analysis function before `init()` | Call `await init()` first |
| "Invalid sample rate" | Sample rate <= 0 | Use valid sample rate (e.g., 44100) |
| "Empty samples" | Empty Float32Array | Provide valid audio samples |

---

## Advanced Usage

### Processing Audio Files

```typescript
import { init, analyze } from '@libraz/sonare';

async function analyzeAudioFile(url: string): Promise<AnalysisResult> {
  await init();

  const audioCtx = new AudioContext();
  const response = await fetch(url);
  const arrayBuffer = await response.arrayBuffer();
  const audioBuffer = await audioCtx.decodeAudioData(arrayBuffer);

  // Convert to mono if stereo
  let samples: Float32Array;
  if (audioBuffer.numberOfChannels === 2) {
    const left = audioBuffer.getChannelData(0);
    const right = audioBuffer.getChannelData(1);
    samples = new Float32Array(left.length);
    for (let i = 0; i < left.length; i++) {
      samples[i] = (left[i] + right[i]) / 2;
    }
  } else {
    samples = audioBuffer.getChannelData(0);
  }

  return analyze(samples, audioBuffer.sampleRate);
}
```

### Web Worker Usage

Offload analysis to a Web Worker to avoid blocking the main thread.

**worker.ts:**

```typescript
import { init, analyze, AnalysisResult } from '@libraz/sonare';

let initialized = false;

self.onmessage = async (e: MessageEvent) => {
  const { samples, sampleRate } = e.data;

  if (!initialized) {
    await init();
    initialized = true;
  }

  try {
    const result = analyze(samples, sampleRate);
    self.postMessage({ success: true, result });
  } catch (error) {
    self.postMessage({ success: false, error: error.message });
  }
};
```

**main.ts:**

```typescript
const worker = new Worker(new URL('./worker.ts', import.meta.url), {
  type: 'module'
});

function analyzeInWorker(samples: Float32Array, sampleRate: number): Promise<AnalysisResult> {
  return new Promise((resolve, reject) => {
    worker.onmessage = (e) => {
      if (e.data.success) {
        resolve(e.data.result);
      } else {
        reject(new Error(e.data.error));
      }
    };

    worker.postMessage({ samples, sampleRate });
  });
}
```

### Analyzing Audio Segments

```typescript
import { init, detectKey, detectBpm } from '@libraz/sonare';

async function analyzeSegment(
  samples: Float32Array,
  sampleRate: number,
  startSec: number,
  endSec: number
) {
  await init();

  const startSample = Math.floor(startSec * sampleRate);
  const endSample = Math.floor(endSec * sampleRate);

  const segment = samples.slice(startSample, endSample);

  return {
    bpm: detectBpm(segment, sampleRate),
    key: detectKey(segment, sampleRate)
  };
}

// Analyze the chorus (60-90 seconds)
const chorusAnalysis = await analyzeSegment(samples, sampleRate, 60, 90);
```

### Downsampling for Faster Analysis

For BPM detection, downsampling to 22050 Hz is often sufficient and faster.

```typescript
function downsample(samples: Float32Array, srcRate: number, dstRate: number): Float32Array {
  if (srcRate === dstRate) return samples;

  const ratio = srcRate / dstRate;
  const newLength = Math.floor(samples.length / ratio);
  const result = new Float32Array(newLength);

  for (let i = 0; i < newLength; i++) {
    result[i] = samples[Math.floor(i * ratio)];
  }

  return result;
}

// Fast BPM detection
const downsampled = downsample(samples, 48000, 22050);
const bpm = detectBpm(downsampled, 22050);
```

### TypeScript Strict Mode

The library is fully typed for TypeScript strict mode.

```typescript
import {
  init,
  analyze,
  AnalysisResult,
  Key,
  Chord,
  Section,
  PitchClass,
  Mode,
  ChordQuality,
  SectionType
} from '@libraz/sonare';

async function getChordProgression(
  samples: Float32Array,
  sampleRate: number
): Promise<string[]> {
  await init();

  const result: AnalysisResult = analyze(samples, sampleRate);

  return result.chords.map((chord: Chord) => chord.name);
}
```

---

## Bundle Size

| File | Size | Gzipped |
|------|------|---------|
| `sonare.js` | ~34 KB | ~12 KB |
| `sonare.wasm` | ~228 KB | ~80 KB |
| **Total** | ~262 KB | ~92 KB |

---

## Browser Support

| Browser | Minimum Version |
|---------|-----------------|
| Chrome | 57+ |
| Firefox | 52+ |
| Safari | 11+ |
| Edge | 16+ |

Requirements:
- WebAssembly support
- ES2017+ (async/await)
- Web Audio API (for audio decoding)
