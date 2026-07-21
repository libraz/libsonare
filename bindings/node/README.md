# libsonare (Node native)

[![CI](https://img.shields.io/github/actions/workflow/status/libraz/libsonare/ci.yml?branch=main&label=CI)](https://github.com/libraz/libsonare/actions)
[![Node](https://img.shields.io/badge/node-%3E%3D22-brightgreen)](https://github.com/libraz/libsonare/tree/main/bindings/node)
[![License](https://img.shields.io/badge/license-Apache--2.0-blue)](https://github.com/libraz/libsonare/blob/main/LICENSE)
[![Docs](https://img.shields.io/badge/docs-libsonare.libraz.net-2563eb)](https://libsonare.libraz.net)
[![PyPI](https://img.shields.io/pypi/v/libsonare?label=PyPI)](https://pypi.org/project/libsonare/)

**Turn audio into data and back, natively in Node.js.** Analyze songs (BPM, key,
chords, loudness), master and mix to broadcast loudness, and render MIDI through
built-in instruments — a native N-API addon on the libsonare C++ core. Mastering
ships 76 named DSP processors implemented against published references (ITU-R
BS.1770-4 true-peak limiting, Linkwitz-Riley crossovers, Vicanek matched-Z
biquads, ADAA-antialiased saturation); analysis defaults match librosa where the
two overlap. Apache-2.0, no model weights.

Unlike the WebAssembly package (`@libraz/libsonare`), this binding decodes audio
files directly from disk or memory (WAV / MP3 out of the box, plus
M4A / AAC / FLAC / OGG / Opus when built with FFmpeg). The analysis, mastering,
mixing, and editing APIs match the C, Python, CLI, and WASM surfaces.

📖 **Full API reference and guides: [libsonare.libraz.net](https://libsonare.libraz.net)**

## Installation

This binding is distributed as source (it is **not published to npm**); build it
from the repository and consume it as a local / workspace dependency. `yarn build`
runs `cmake-js compile` then `tsc`, auto-detecting FFmpeg via pkg-config.

```bash
git clone https://github.com/libraz/libsonare
cd libsonare/bindings/node
yarn install
yarn build
```

For a browser-friendly, npm-published runtime use the WebAssembly package
[`@libraz/libsonare`](https://www.npmjs.com/package/@libraz/libsonare); for Python
use [`libsonare`](https://pypi.org/project/libsonare/).

The native build honours `SONARE_FFMPEG`: `auto` (default, detect via pkg-config),
`1` (require FFmpeg — fail if dev libs are missing), `0` (disable). FFmpeg dev
libraries are needed for `=1` (`brew install ffmpeg`, or
`apt install libavformat-dev libavcodec-dev libavutil-dev libswresample-dev`).

## Quick Start

`Audio` is the recommended entry point: it decodes files and caches samples.
Top-level one-shot functions accept a request object (recommended) or positional
arguments.

```typescript
import { Audio, masterAudio } from '@libraz/libsonare-native';

// Decode straight from disk (WAV/MP3 always; more with FFmpeg)
const audio = Audio.fromFile('song.mp3');
const result = audio.analyze();               // BPM + key + time signature + beats
console.log(`BPM: ${result.bpm.toFixed(1)}  Key: ${result.key.name}`);

// Master toward a target loudness with a named preset
const mastered = masterAudio({
  samples: audio.getData(),
  sampleRate: audio.getSampleRate(),
  preset: 'streaming',
});
console.log(mastered.outputLufs, mastered.appliedGainDb);
```

Render a MIDI arrangement through a built-in instrument with the headless
`Project`. Call `destroy()` to release the native handle.

```typescript
import { Project } from '@libraz/libsonare-native';

const project = Project.create();
const { clipId } = project.addMidiClip(0, 4);
project.setMidiEvents(clipId, [
  Project.midiNoteOn(0, 0, 0, 60, 100),       // ppq, group, channel, note, velocity
  Project.midiNoteOff(1, 0, 0, 60),
]);
const audio = project.bounceWithSynthInstrument('saw-lead', { numChannels: 2 });
project.destroy();
```

## Supported audio formats

| Format                                     | Default build | With FFmpeg support |
| ------------------------------------------ | ------------- | ------------------- |
| WAV (PCM 16/24/32, float32)                | yes           | yes                 |
| MP3                                        | yes           | yes                 |
| M4A / AAC / FLAC / OGG / Opus / WMA / ...  | no            | yes                 |

`Audio.fromFile()` reports a clear error for unsupported formats. Check at runtime
whether the loaded binding was compiled against FFmpeg with `hasFfmpegSupport()`.
`Audio.fromMemory` accepts a Node `Buffer` or `Uint8Array` (zero-copy); stereo
files are downmixed to mono on load.

## Capabilities

Every area below has runnable examples and the full API in the
[documentation](https://libsonare.libraz.net/docs/native-bindings).

- **Analysis** — BPM, key (+ candidates), chords, downbeats, sections, melody, tuning; pitch (YIN / pYIN), timbre, and the full spectral feature set (STFT, mel, MFCC, chroma, CQT/VQT, spectral contrast); metering (true-peak, LUFS, correlation, vectorscope, waveform peaks). → [Python/JS API](https://libsonare.libraz.net/docs/native-bindings)
- **Mastering** — 76 named DSP processors, the configurable `masteringChain`, 25 named presets via `masterAudio`, and reference-matching. → [Mastering processors](https://libsonare.libraz.net/docs/mastering-processors)
- **Mixing** — offline `mixStereo` and the block-based `Mixer` with scene presets. → [Mixing](https://libsonare.libraz.net/docs/mixing)
- **Editing DSP** — time-stretch, pitch-shift, HPSS (+ residual), phase vocoder, normalize, trim, remix. → [Editing DSP](https://libsonare.libraz.net/docs/editing-dsp)
- **Room acoustics** — blind RT60 / EDT, impulse-response clarity metrics, RIR synthesis, room estimation and morphing. → [Room acoustics](https://libsonare.libraz.net/docs/acoustic-analysis)
- **Realtime & streaming** — `RealtimeEngine` (transport / MIDI / render / capture), `StreamingMasteringChain`, `RealtimeVoiceChanger`. → [Realtime & streaming](https://libsonare.libraz.net/docs/realtime-streaming)
- **Instruments & synthesis** — built-in oscillator synth, patch-driven NativeSynth (15 synthesis engines, incl. physically-modeled piano / strings / winds — being tuned over time), and a GS-compatible SoundFont (SF2) player. → [API](https://libsonare.libraz.net/docs/native-bindings)
- **Headless DAW** — `Project` arrangement model: audio / MIDI tracks and clips, undo/redo, clip warp, SMF / MIDI 2.0 Clip File I/O, deterministic JSON, offline `bounce`. → [API](https://libsonare.libraz.net/docs/native-bindings)
- **Conversions** — Hz / mel / MIDI / note, frames / time, resample.

C-ABI failures throw a `SonareError` (`name` `'SonareError'`) carrying a numeric
`code` (an `ErrorCode` value) and its `codeName`; narrow with the `isSonareError`
type guard.

## Documentation

Full API reference and guides live at
**[libsonare.libraz.net](https://libsonare.libraz.net)**
([getting started](https://libsonare.libraz.net/docs/getting-started) ·
[Node.js native API](https://libsonare.libraz.net/docs/native-bindings)).

## Also available

```bash
npm install @libraz/libsonare   # JavaScript / TypeScript (WASM, takes Float32Array)
pip install libsonare           # Python bindings with CLI
```

## License

Apache-2.0
