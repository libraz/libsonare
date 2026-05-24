# libsonare (Node native)

[![CI](https://img.shields.io/github/actions/workflow/status/libraz/libsonare/ci.yml?branch=main&label=CI)](https://github.com/libraz/libsonare/actions)
[![npm](https://img.shields.io/npm/v/@libraz/libsonare-native)](https://www.npmjs.com/package/@libraz/libsonare-native)
[![npm downloads](https://img.shields.io/npm/dm/@libraz/libsonare-native)](https://www.npmjs.com/package/@libraz/libsonare-native)
[![types](https://img.shields.io/npm/types/@libraz/libsonare-native)](https://www.npmjs.com/package/@libraz/libsonare-native)
[![Node](https://img.shields.io/node/v/@libraz/libsonare-native)](https://www.npmjs.com/package/@libraz/libsonare-native)
[![License](https://img.shields.io/badge/license-Apache--2.0-blue)](https://github.com/libraz/libsonare/blob/main/LICENSE)
[![PyPI](https://img.shields.io/pypi/v/libsonare?label=PyPI)](https://pypi.org/project/libsonare/)

Audio analysis and mastering DSP for Node.js, exposed as a native N-API
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

// Advanced key options are opt-in; defaults preserve existing behavior.
const keyWithOptions = detectKey(samples, audio.getSampleRate(), {
  useHpss: true,
  loudnessWeighted: true,
  highPassHz: 80,
  nFft: 4096,
  hopLength: 512,
});
const audioKeyWithOptions = audio.detectKey({ useHpss: true, highPassHz: 80 });
```

### Room acoustics

Use `detectAcoustic` for blind RT60/EDT estimation from ordinary audio.
Use `analyzeImpulseResponse` when you have a measured impulse response and need
clarity metrics (`c50`, `c80`, `d50`). Blind mode returns `NaN` for clarity
metrics because they are not reliable without an impulse response.

```typescript
import { Audio, analyzeImpulseResponse, detectAcoustic } from '@libraz/libsonare-native';

const audio = Audio.fromFile('recording.wav');
const blind = audio.detectAcoustic(6, 24, 30.0, 10.0);
console.log(blind.rt60, blind.edt, blind.isBlind);

const ir = Audio.fromFile('room_ir.wav');
const params = analyzeImpulseResponse(ir.getData(), ir.getSampleRate());
console.log(params.rt60, params.c50, params.c80, params.d50);
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

### Mastering chain

`masteringChain` runs the full configurable mastering pipeline (EQ, dynamics,
saturation, repair, stereo, loudness, ...). The Node binding uses **flat
dot-notation keys** for the config object — addressing any module parameter
directly — and accepts an optional `onProgress(progress, stage)` callback that
is invoked after each stage (progress in `[0, 1]`).

```typescript
import { masteringChain, masteringChainStereo } from '@libraz/libsonare-native';

const mastered = masteringChain(samples, sampleRate, {
  'eq.tilt.tiltDb': 0.5,
  'dynamics.compressor.thresholdDb': -24,
  'dynamics.compressor.ratio': 1.5,
  'dynamics.transientShaper.attackGainDb': 2.0,
  'repair.declick.enabled': true,
  'loudness.targetLufs': -14,
  'loudness.ceilingDb': -1,
});

const stereo = masteringChainStereo(left, right, sampleRate, {
  'stereo.imager.width': 1.1,
  'loudness.targetLufs': -14,
}, (progress, stage) => {
  console.log(`[${(progress * 100).toFixed(0)}%] ${stage}`);
});
```

`MasteringChainResult` contains the rendered samples (`samples` for mono,
`left`/`right` for stereo) plus loudness telemetry
(`inputLufs`, `outputLufs`, `appliedGainDb`, `latencySamples`).

### Mastering presets

Named presets ship sensible defaults for common targets. `masterAudio` applies
a preset and lets you override any individual parameter with the same flat
dot-notation keys as `masteringChain`.

```typescript
import { masterAudio, masteringPresetNames } from '@libraz/libsonare-native';

masteringPresetNames(); // ['pop', 'edm', 'acoustic', 'hipHop', 'aiMusic', 'speech', 'streaming', 'youtube', 'broadcast', 'podcast', 'audiobook', 'cinema', 'jpop', 'ambient', 'lofi', 'classical']

const result = masterAudio(samples, sampleRate, 'aiMusic', {
  'loudness.targetLufs': -13,
  'dynamics.multibandComp.enabled': true,
});

// Audio class shortcut
const audio = Audio.fromFile('song.wav');
const popMastered = audio.masterAudio('pop');
```

### Streaming mastering chain

`StreamingMasteringChain` runs the same pipeline block-by-block for real-time
or chunked workflows. The constructor takes the same flat dot-notation config
as `masteringChain`; non-streamable stages (`repair.denoise`, `loudness`) cause
the constructor to throw, so omit those keys for streaming use.

```typescript
import { StreamingMasteringChain } from '@libraz/libsonare-native';

const chain = new StreamingMasteringChain({
  'eq.tilt.tiltDb': 0.5,
  'dynamics.compressor.thresholdDb': -20,
  'dynamics.transientShaper.attackGainDb': 1.5,
});

chain.prepare(48000, 512, 1);   // sampleRate, maxBlockSize, numChannels
console.log(chain.stageNames());
console.log(`latency = ${chain.latencySamples()} samples`);

const out = chain.processMono(new Float32Array(512));
// Or for stereo:
chain.prepare(48000, 512, 2);
const { left: outL, right: outR } = chain.processStereo(left, right);

chain.reset();
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
