# libsonare (Node native)

[![npm](https://img.shields.io/npm/v/@libraz/libsonare-native)](https://www.npmjs.com/package/@libraz/libsonare-native)
[![License](https://img.shields.io/badge/license-Apache--2.0-blue)](https://github.com/libraz/libsonare/blob/main/LICENSE)

Fast audio analysis and mastering DSP for Node.js, exposed as a native N-API
addon built on the libsonare C++ core.

Unlike the WebAssembly package (`@libraz/libsonare`), this binding can decode
audio files directly from disk or memory (WAV / MP3 out of the box, plus
M4A / AAC / FLAC / OGG / Opus when built with FFmpeg). Both the analysis and
mastering APIs are exposed, matching the C, Python, CLI, and WASM surfaces.

## Installation

```bash
yarn add @libraz/libsonare-native
```

Or build locally from the repository:

```bash
git clone https://github.com/libraz/libsonare
cd libsonare/bindings/node
yarn install
yarn build  # auto-detects FFmpeg via pkg-config
```

`yarn build` runs `cmake-js compile` followed by `tsc`. The native build
honours `SONARE_FFMPEG`:

```bash
SONARE_FFMPEG=auto yarn build  # default: detect via pkg-config
SONARE_FFMPEG=1    yarn build  # require FFmpeg (fail if dev libs missing)
SONARE_FFMPEG=0    yarn build  # explicitly disable FFmpeg
```

System FFmpeg dev libraries are needed for the `=1` mode
(`brew install ffmpeg` on macOS,
`apt install libavformat-dev libavcodec-dev libavutil-dev libswresample-dev`
on Debian/Ubuntu).

## Quick Start

`Audio` is the recommended entry point. The top-level `detectBpm` /
`detectKey` / `analyze` functions are thin wrappers around `Audio` for
one-shot calls and are kept for convenience.

### Analysis

```typescript
import { Audio, analyze, detectBpm, detectKey } from '@libraz/libsonare-native';

const audio = Audio.fromFile('song.mp3');     // or 'song.wav'
console.log(`BPM: ${audio.detectBpm().toFixed(1)}`);
console.log(`Key: ${audio.detectKey().name}`);

const result = audio.analyze();               // BPM + key + time signature + beats
console.log(`BPM: ${result.bpm.toFixed(1)}  Key: ${result.key.name}`);

// Or call the standalone functions on Float32Array samples
const samples: Float32Array = audio.getData();
const bpm = detectBpm(samples, audio.getSampleRate());
const key = detectKey(samples, audio.getSampleRate());
```

### Mastering

```typescript
import {
  Audio,
  mastering,
  masteringPairAnalyze,
  masteringPairProcess,
  masteringPairProcessorNames,
  masteringProcess,
  masteringProcessStereo,
  masteringProcessorNames,
  masteringStereoAnalyze,
} from '@libraz/libsonare-native';

const audio = Audio.fromFile('song.wav');
const sampleRate = audio.getSampleRate();
const samples = audio.getData();

// Full mastering chain (loudness optimizer toward a target LUFS / true-peak ceiling).
// Returns MasteringResult(samples, sampleRate, inputLufs, outputLufs,
// appliedGainDb, latencySamples).
const mastered = mastering(samples, sampleRate, -14.0, -1.0, 4);
console.log(
  `${mastered.inputLufs.toFixed(1)} LUFS -> ${mastered.outputLufs.toFixed(1)} LUFS ` +
    `(gain ${mastered.appliedGainDb.toFixed(2)} dB)`,
);

// Audio class shortcut
const masteredViaAudio = audio.mastering(-14.0, -1.0, 4);

// Apply a single named processor
const compressed = masteringProcess('dynamics.compressor', samples, sampleRate, {
  thresholdDb: -24,
  ratio: 1.5,
});

// Stereo processor
const widened = masteringProcessStereo('stereo.imager', left, right, sampleRate, {
  width: 1.1,
});

// Reference-based mastering (source + reference)
const matched = masteringPairProcess('match.abCrossfade', source, reference, sampleRate, {
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

#### Discover available processors

The native addon exposes the same named-processor registry as the C / Python /
WASM bindings. Use these to enumerate what is available at runtime:

```typescript
import {
  masteringPairAnalysisNames,
  masteringPairProcessorNames,
  masteringProcessorNames,
  masteringStereoAnalysisNames,
} from '@libraz/libsonare-native';

masteringProcessorNames();        // e.g. ['dynamics.compressor', 'eq.parametric', ...]
masteringPairProcessorNames();    // e.g. ['match.abCrossfade', ...]
masteringPairAnalysisNames();     // e.g. ['match.referenceLoudness', ...]
masteringStereoAnalysisNames();   // e.g. ['stereo.monoCompatCheck', ...]
```

### Audio class

`Audio` caches the decoded samples and is the only way to load files, so it is
the recommended entry point when you do more than a single computation on the
same signal.

```typescript
import { Audio } from '@libraz/libsonare-native';

// From a file on disk (WAV/MP3 always; M4A/AAC/FLAC/OGG when built with FFmpeg)
const fromFile = Audio.fromFile('song.mp3');

// From an in-memory encoded buffer (Node Buffer or Uint8Array)
import { readFile } from 'node:fs/promises';
const bytes = await readFile('song.mp3');
const fromMemory = Audio.fromMemory(bytes);

// From decoded mono Float32Array samples
const fromBuffer = Audio.fromBuffer(samples, 22050);

console.log(fromFile.getSampleRate(), fromFile.getDuration(), fromFile.getLength());

// Mastering / effects / features are available as methods too
const stretched = fromFile.timeStretch(0.9);
const shifted = fromFile.pitchShift(2);              // +2 semitones
const { harmonic, percussive } = fromFile.hpss();
const mel = fromFile.melSpectrogram();
```

`Audio.fromMemory` accepts either a Node `Buffer` or a `Uint8Array`; both are
zero-copy into the native decoder. Stereo files are automatically downmixed to
mono on load.

## Supported audio formats

| Format                                     | Default build | With FFmpeg support |
| ------------------------------------------ | ------------- | ------------------- |
| WAV (PCM 16/24/32, float32)                | yes           | yes                 |
| MP3                                        | yes           | yes                 |
| M4A / AAC / FLAC / OGG / Opus / WMA / ...  | no            | yes                 |

If `Audio.fromFile()` is given an unsupported format you will see a clear
error such as:

```
Error: Unsupported audio format: '.m4a'. Supported: WAV, MP3.
Rebuild with -DSONARE_WITH_FFMPEG=ON for M4A/AAC/FLAC/OGG,
or convert via: ffmpeg -i "song.m4a" output.wav
```

To check at runtime whether the loaded binding was compiled against FFmpeg:

```typescript
import { hasFfmpegSupport } from '@libraz/libsonare-native';
console.log(hasFfmpegSupport()); // true if M4A/AAC/FLAC/OGG/Opus are available
```

`SONARE_FFMPEG=auto` (the default) detects FFmpeg via pkg-config at build
time: if FFmpeg dev libraries are present they are enabled, otherwise the
addon falls back to WAV/MP3 only. Set `SONARE_FFMPEG=1` to require FFmpeg
(build fails if dev libs are missing) or `SONARE_FFMPEG=0` to force it off so
the binary never links libavformat.

## Also available

```bash
npm install @libraz/libsonare   # JavaScript / TypeScript (WASM, takes Float32Array)
pip install libsonare           # Python bindings with CLI
```

## License

Apache-2.0
