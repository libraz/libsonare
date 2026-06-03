# libsonare

[![CI](https://img.shields.io/github/actions/workflow/status/libraz/libsonare/ci.yml?branch=main&label=CI)](https://github.com/libraz/libsonare/actions)
[![npm](https://img.shields.io/npm/v/@libraz/libsonare)](https://www.npmjs.com/package/@libraz/libsonare)
[![PyPI](https://img.shields.io/pypi/v/libsonare)](https://pypi.org/project/libsonare/)
[![codecov](https://codecov.io/gh/libraz/libsonare/branch/main/graph/badge.svg)](https://codecov.io/gh/libraz/libsonare)
[![License](https://img.shields.io/badge/license-Apache--2.0-blue)](https://github.com/libraz/libsonare/blob/main/LICENSE)
[![C++17](https://img.shields.io/badge/C%2B%2B-17-blue?logo=c%2B%2B)](https://en.cppreference.com/w/cpp/17)
[![Platform](https://img.shields.io/badge/platform-Linux%20%7C%20macOS%20%7C%20WebAssembly-lightgrey)](https://github.com/libraz/libsonare)
[![Docs](https://img.shields.io/badge/docs-libsonare.libraz.net-2563eb)](https://libsonare.libraz.net)

**A dependency-free audio DSP toolkit for C++, Python, and the browser —
librosa-compatible analysis plus broadcast-grade mastering, mixing, and editing.**

Apache-2.0, zero runtime dependencies, one codebase for native and WebAssembly.
The same processors that run in C++ run in the browser via WASM — no Python,
no GPL/AGPL, no model weights.

📖 **[Documentation](https://libsonare.libraz.net)** &nbsp;·&nbsp; 🎧 **[Browser-local Demos](https://libsonare.libraz.net/demos)** &nbsp;·&nbsp; [Getting Started](https://libsonare.libraz.net/docs/getting-started)

- **Analysis (librosa-compatible)** — BPM, key, chord (Viterbi/HMM smoothing,
  inversions, key-context), beat, downbeat, time signature, section, timbre,
  dynamics, pitch (YIN / pYIN), tempogram / PLP, NNLS chroma, EBU R128 loudness
  (LUFS), and room acoustics (blind RT60/EDT, or ISO-style RT60/EDT/C50/C80/D50
  from a measured IR). Defaults match librosa and are validated against
  generated librosa reference values in CI.
- **Mastering (66 named DSP processors, 18 in the default chain)** — EQ,
  dynamics, multiband, stereo, saturation, repair, maximizer, and reference
  matching, implemented against published references: ITU-R BS.1770-4 loudness
  and inter-sample true-peak limiting, Linkwitz-Riley crossovers with all-pass
  phase compensation, Vicanek matched-Z biquads, ADAA-antialiased clippers, a
  Dempwolf 12AX7 triode model for tube saturation, Lemire sliding max, and
  polyphase FIR oversampling. Repair is classical DSP by design (spectral
  subtraction / MMSE-STSA / LogMMSE), not DNN source separation or spectral
  repair.
- **Mixing & routing** — a real-time-safe channel-strip / bus model
  (denormal-guarded, lock-free parameter changes, plugin-delay compensation)
  with pan modes, width, sends, FX buses, goniometer / true-peak metering,
  scene presets, and offline stereo rendering.
- **Editing & creative FX** — time stretch / pitch shift, pitch correction,
  note-region stretch, voice-change pitch + formant controls, five reverb
  engines (convolution, Dattorro plate, FDN, velvet-noise, and a geometric
  room engine), chorus / flanger / phaser, stereo delay, and ducking.
- **Geometric room acoustics** — synthesize a room impulse response from
  shoebox geometry (`synthesizeRir`), blindly estimate an equivalent room from
  a recording (`estimateRoom` → volume / dimensions / per-band absorption /
  DRR + honest confidence), and morph a recording's reverberation toward a
  target room (`roomMorph`). Apache-2.0, dependency-free, deterministic.
- **Everywhere, one license** — Apache-2.0 across the entire stack
  (C++, C, Python, Node, WASM, and CLI).

## Installation

```bash
npm install @libraz/libsonare         # JavaScript / TypeScript (WASM, takes Float32Array)
pip install libsonare                  # Python (WAV/MP3 — see "Supported audio formats" for M4A/AAC)
```

For Node.js with native file decoding, build
[`@libraz/libsonare-native`](bindings/node/) from source:

```bash
cd bindings/node
yarn install
yarn build  # auto-detects FFmpeg via pkg-config (WAV/MP3 if absent, +M4A/AAC/FLAC/OGG if present)
```

To force a specific mode:

```bash
SONARE_FFMPEG=0 yarn build  # explicitly disable FFmpeg
SONARE_FFMPEG=1 yarn build  # require FFmpeg (fails if dev libs missing)
```

## Quick Start

### JavaScript / TypeScript (WASM)

`@libraz/libsonare` accepts decoded `Float32Array` samples — use the Web Audio
API or a JS decoder to obtain them. Mastering DSP is included in the default WASM build.

**Analysis**

```typescript
import { init, detectBpm, detectKey, analyze } from '@libraz/libsonare';

await init();

const bpm = detectBpm(samples, sampleRate);
const key = detectKey(samples, sampleRate);  // { name: "C major", confidence: 0.95 }
const result = analyze(samples, sampleRate);

// Advanced key options are opt-in; defaults preserve existing behavior.
const keyWithOptions = detectKey(samples, sampleRate, {
  useHpss: true,
  loudnessWeighted: true,
  highPassHz: 80,
  nFft: 4096,
  hopLength: 512,
});
```

**Room acoustics**

```typescript
import {
  analyzeImpulseResponse,
  detectAcoustic,
  estimateRoom,
  synthesizeRir,
  roomMorph,
} from '@libraz/libsonare';

// Ordinary audio: blind RT60/EDT estimate. C50/C80/D50 are NaN in blind mode.
const blind = detectAcoustic(samples, sampleRate);

// Measured impulse response: ISO-style RT60/EDT plus clarity metrics.
const room = analyzeImpulseResponse(irSamples, sampleRate);

// Blindly estimate an equivalent room from a recording: volume / dimensions /
// per-band absorption / DRR, with an honest confidence score.
const estimate = estimateRoom(samples, sampleRate);

// Synthesize a room impulse response from shoebox geometry.
const { rir } = synthesizeRir({ lengthM: 7, widthM: 5, heightM: 3, absorption: 0.2 });

// Morph a recording's reverberation toward a target room (creative FX).
const morphed = roomMorph(samples, sampleRate, { lengthM: 12, widthM: 9, wet: 0.6 });
```

**Rhythm & chords**

```typescript
import { analyze, detectDownbeats, detectChords } from '@libraz/libsonare';

const downbeats = detectDownbeats(samples, sampleRate);  // bar-start times (s)
const { timeSignature } = analyze(samples, sampleRate);  // { numerator: 4, denominator: 4 }

// Chord detection extras are all opt-in (defaults preserve existing behavior).
const chords = detectChords(samples, sampleRate, {
  useHmm: true,            // Viterbi/HMM temporal smoothing
  detectInversions: true,  // slash chords via detected bass note
  useKeyContext: true,     // bias toward in-key chords
  chromaMethod: 'nnls',    // NNLS chroma instead of plain STFT chroma
});
```

**Tempogram, NNLS chroma & loudness**

```typescript
import {
  onsetEnvelope, tempogram, fourierTempogram, tempogramRatio, plp,
  nnlsChroma, lufs,
} from '@libraz/libsonare';

// Onset strength envelope feeds the tempo-domain features.
const env = onsetEnvelope(samples, sampleRate);
const tg = tempogram(env, sampleRate);          // { winLength, nFrames, data }
const ft = fourierTempogram(env, sampleRate);   // { nBins, nFrames, data }
const ratios = tempogramRatio(tg.data, tg.winLength, sampleRate);
const pulse = plp(env, sampleRate);             // predominant local pulse

const chroma = nnlsChroma(samples, sampleRate); // { nChroma: 12, nFrames, data }

// EBU R128 loudness metering (separate from the mastering loudness target).
const loud = lufs(samples, sampleRate);
// { integratedLufs, momentaryLufs, shortTermLufs, loudnessRange }
```

**Pitch and timbre results**

`pitchYin` and `pitchPyin` keep unvoiced frames as `NaN` by default, matching librosa-style pitch tracks. Pass `fillNa: true` when downstream code needs a finite series and should treat unvoiced frames as `0`.

`analyzeTimbre` returns both aggregate timbre metrics and `timbreOverTime`, a per-analysis-window series with the same brightness, warmth, density, roughness, and complexity fields.

| Runtime | Pitch option | Time-varying timbre field |
|---------|--------------|---------------------------|
| JavaScript / WASM / Node | `fillNa` | `timbreOverTime` |
| Python | `fill_na` | `timbre_over_time` (`timbreOverTime` alias) |
| C ABI | `fill_na` | `timbre_over_time` + `timbre_over_time_count` |

**Spectral features, decomposition & effects (librosa-compatible)**

```typescript
import {
  spectralContrast, polyFeatures, zeroCrossings, pitchTuning, estimateTuning,
  decompose, nnFilter, remix, phaseVocoder, hpssWithResidual,
  lufsInterleaved, ebur128LoudnessRange,
} from '@libraz/libsonare';

// Spectral features
const contrast = spectralContrast(samples, sampleRate); // Matrix2d (nBands+1) x nFrames
const poly = polyFeatures(samples, sampleRate);         // Matrix2d (order+1) x nFrames
const crossings = zeroCrossings(samples);               // Int32Array of crossing indices

// Tuning estimation (deviation in fractions of a bin)
const tuning = estimateTuning(samples, sampleRate);
const fromF0 = pitchTuning(frequencies);

// Spectrogram decomposition: NMF factors + nearest-neighbour filtering
const { w, h } = decompose(spectrogram, nFeatures, nFrames, 8); // n_components = 8
const filtered = nnFilter(spectrogram, nFeatures, nFrames);

// Effects: interval remix, phase-vocoder time-scaling, HPSS + residual
const remixed = remix(samples, Int32Array.from([0, 22050, 44100, 66150]));
const faster = phaseVocoder(samples, 1.5, sampleRate);  // rate > 1 → faster
const { harmonic, percussive, residual } = hpssWithResidual(samples, sampleRate);

// Multichannel / standards-compliant loudness
const multi = lufsInterleaved(interleaved, 2, sampleRate); // channel-weighted LUFS + LRA
const lra = ebur128LoudnessRange(samples, sampleRate);     // EBU R128 loudness range (LU)
```

**Mastering**

```typescript
import {
  init,
  masteringChain,
  masteringChainStereo,
  masteringPairAnalyze,
  masteringPairProcess,
  masteringPairProcessorNames,
  masteringProcess,
  masteringProcessorNames,
} from '@libraz/libsonare';

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

// Apply a single named processor
const compressed = masteringProcess('dynamics.compressor', samples, sampleRate, {
  thresholdDb: -24,
  ratio: 1.5,
});

// Reference-based mastering
const matched = masteringPairProcess('match.abCrossfade', source, reference, sampleRate, {
  mix: 0.25,
});
const loudnessJson = masteringPairAnalyze(
  'match.referenceLoudness', source, reference, sampleRate,
);

// Discover available processors
masteringProcessorNames();     // ['dynamics.compressor', 'eq.parametric', ...]
masteringPairProcessorNames(); // ['match.abCrossfade', ...]
```

Preset-driven mastering and the block-by-block streaming variant are also
exposed. WASM uses **nested** config for `masteringChain` /
`StreamingMasteringChain`, while `masterAudio` overrides use **flat
dot-notation keys** (mirroring the Node and Python overrides API):

```typescript
// Mastering presets (one-shot) and streaming variant
import { masterAudio, masteringPresetNames, StreamingMasteringChain } from '@libraz/libsonare';
masteringPresetNames(); // ['pop', 'edm', 'acoustic', 'hipHop', 'aiMusic', 'speech', 'streaming', 'youtube', 'broadcast', 'podcast', 'audiobook', 'cinema', 'jpop', 'ambient', 'lofi', 'classical', 'drumAndBass', 'techno', 'metal', 'trap', 'rnb', 'jazz', 'kpop', 'trance', 'gameOst']
const out = masterAudio(samples, sampleRate, 'aiMusic', { 'loudness.targetLufs': -13 });

const chain = new StreamingMasteringChain({ eq: { tiltDb: 0.5 } });
chain.prepare(48000, 512, 1);
const block = chain.processMono(new Float32Array(512));
chain.delete();
```

**Mixing**

```typescript
import { mixStereo, mixingScenePresetJson, mixingScenePresetNames } from '@libraz/libsonare';

mixingScenePresetNames(); // ['vocalReverbSend', ...]
const sceneJson = mixingScenePresetJson('vocalReverbSend');

const mix = mixStereo([vocalL, musicL], [vocalR, musicR], sampleRate, {
  faderDb: [-3, -12],
  pan: [0, -0.2],
  width: [1, 0.9],
});
// { left, right, meters }
```

**DAW editing DSP**

```typescript
import { noteStretch, pitchCorrectToMidi, voiceChange } from '@libraz/libsonare';

const corrected = pitchCorrectToMidi(samples, sampleRate, 69, 70);
const stretchedNote = noteStretch(samples, sampleRate, 12000, 24000, 1.25);
const changed = voiceChange(samples, sampleRate, 5, 1.1);
```

### Python

`pip install libsonare` ships a **WAV/MP3-only wheel** (matching librosa / pydub /
soundfile conventions). For M4A/AAC/FLAC/OGG either pre-convert with external
`ffmpeg`, or rebuild from source with FFmpeg linked:

```bash
SONARE_FFMPEG=1 pip install libsonare --no-binary libsonare
# requires system FFmpeg dev libs: brew install ffmpeg / apt install libavformat-dev libavcodec-dev libavutil-dev libswresample-dev
```

```python
import libsonare

audio = libsonare.Audio.from_file("song.mp3")
print(f"BPM: {audio.detect_bpm()}, Key: {audio.detect_key()}")

# Advanced key options are opt-in; defaults preserve existing behavior.
key_with_options = audio.detect_key(
    use_hpss=True,
    loudness_weighted=True,
    high_pass_hz=80.0,
)

acoustic = audio.detect_acoustic()  # blind RT60/EDT; C50/C80/D50 are NaN
ir_params = libsonare.analyze_impulse_response(ir_samples, sample_rate=sr)

# Downbeats, time signature, and chord extras (all opt-in)
downbeats = audio.detect_downbeats()              # bar-start times (s)
time_signature = audio.analyze().time_signature   # e.g. 4/4
chords = audio.detect_chords(
    use_hmm=True,             # Viterbi/HMM temporal smoothing
    detect_inversions=True,   # slash chords via detected bass note
    use_key_context=True,     # bias toward in-key chords
    chroma_method="nnls",     # NNLS chroma instead of plain STFT chroma
)

# Tempogram / NNLS chroma / EBU R128 loudness
env = audio.onset_envelope()                     # onset strength envelope
n_frames, tg = libsonare.tempogram(env, sample_rate=sr)
n_frames_ft, ft = libsonare.fourier_tempogram(env, sample_rate=sr)
ratios = libsonare.tempogram_ratio(tg)
pulse = libsonare.plp(env, sample_rate=sr)

nf, chroma = audio.nnls_chroma()                 # (n_frames, 12 x n_frames row-major)

loud = audio.lufs()  # integrated_lufs / momentary_lufs / short_term_lufs / loudness_range
mom = audio.momentary_lufs()                     # per-block time series
short = audio.short_term_lufs()

# librosa-compatible spectral features, decomposition & effects
contrast = libsonare.spectral_contrast(samples, sample_rate=sr)  # (n_bands+1) x n_frames
poly = libsonare.poly_features(samples, sample_rate=sr)          # (order+1) x n_frames
crossings = libsonare.zero_crossings(samples)                    # crossing indices
tuning = libsonare.estimate_tuning(samples, sample_rate=sr)      # deviation (bins fraction)
offset = libsonare.pitch_tuning(frequencies)

w, h = libsonare.decompose(spectrogram, n_features, n_frames, 8)  # NMF factors
filtered = libsonare.nn_filter(spectrogram, n_features, n_frames)

remixed = libsonare.remix(samples, [0, sr, 2 * sr, 3 * sr], sample_rate=sr)
faster = libsonare.phase_vocoder(samples, sample_rate=sr, rate=1.5)
hpss = libsonare.hpss_with_residual(samples, sample_rate=sr)      # harmonic/percussive/residual

multi = libsonare.lufs_interleaved(interleaved, channels=2, sample_rate=sr)
lra = libsonare.ebur128_loudness_range(samples, sample_rate=sr)   # EBU R128 LRA (LU)

# Mastering chain — returns MasteringResult(samples, sample_rate,
# input_lufs, output_lufs, applied_gain_db, latency_samples)
result = audio.mastering(target_lufs=-14.0, ceiling_db=-1.0)
print(f"{result.input_lufs:.1f} LUFS → {result.output_lufs:.1f} LUFS "
      f"(gain {result.applied_gain_db:+.2f} dB)")

# Single processor / reference matching
compressed = libsonare.mastering_process(
    "dynamics.compressor", samples, sample_rate=44100,
    params={"thresholdDb": -24, "ratio": 1.5},
)
loudness_json = libsonare.mastering_pair_analyze(
    "match.referenceLoudness", source, reference, sample_rate=44100,
)

# Discover available processors
libsonare.mastering_processor_names()       # ['dynamics.compressor', ...]
libsonare.mastering_pair_processor_names()  # ['match.abCrossfade', ...]

# Preset-based chain (one-shot) + streaming
libsonare.mastering_preset_names()  # ['pop', 'edm', 'acoustic', 'hipHop', 'aiMusic', 'speech', 'streaming', 'youtube', 'broadcast', 'podcast', 'audiobook', 'cinema', 'jpop', 'ambient', 'lofi', 'classical', 'drumAndBass', 'techno', 'metal', 'trap', 'rnb', 'jazz', 'kpop', 'trance', 'gameOst']
result = libsonare.master_audio(samples, sample_rate=sr, preset='aiMusic',
                                 overrides={'loudness.targetLufs': -13})

with libsonare.StreamingMasteringChain({'eq.tilt.tiltDb': 0.5}) as chain:
    chain.prepare(44100, 512, 1)
    out = chain.process_mono([0.0] * 512)

# Mixing presets and offline stereo rendering
libsonare.mixing_scene_preset_names()  # ['vocalReverbSend', ...]
scene_json = libsonare.mixing_scene_preset_json("vocalReverbSend")
mix = libsonare.mix_stereo(
    [(vocal_l, vocal_r), (music_l, music_r)],
    sample_rate=sr,
    fader_db=[-3.0, -12.0],
    pan=[0.0, -0.2],
    width=[1.0, 0.9],
)
```

### Python CLI

```bash
pip install libsonare

# Analysis
sonare analyze song.mp3
# > Estimated BPM : 161.00 BPM  (conf 75.0%)
# > Estimated Key : C major  (conf 100.0%)

sonare bpm song.mp3 --json

# Extended analysis (parity with the C++ CLI)
sonare acoustic room.wav --json          # blind RT60/EDT (add --ir for IR-based clarity metrics)
sonare estimate-room room.wav --json     # blind room: volume/dimensions/absorption/DRR + confidence
sonare synthesize-rir --length 7 --width 5 --height 3 -o rir.wav   # RIR from shoebox geometry
sonare room-morph dry.wav --length 12 --width 9 --wet 0.6 -o morphed.wav  # morph toward a target room
sonare lufs song.wav --series            # EBU R128 integrated/momentary/short-term
sonare rhythm song.wav --json
sonare dynamics song.wav --json
sonare timbre song.wav --json
sonare tempogram song.wav --json
sonare nnls-chroma song.wav --json

# Mastering
sonare mastering song.wav -o mastered.wav --target-lufs -14
sonare mastering-processor song.wav --processor dynamics.compressor \
    --params thresholdDb=-24,ratio=1.5 -o compressed.wav
sonare mastering-pair-analyze source.wav --reference reference.wav \
    --analysis match.referenceLoudness
sonare mastering-processors                 # list available processors

# Mixing
sonare mixing-presets
sonare mixing-preset --preset vocalReverbSend
sonare mix input.wav -o mix.wav --fader-db -3 --pan 0.1 --pan-mode stereo-pan --width 1.1

# DAW editing DSP
sonare pitch-correct vocal.wav --current-midi 69 --target-midi 70 -o corrected.wav
sonare note-stretch vocal.wav --onset 12000 --offset 24000 --ratio 1.25 -o stretched.wav
sonare voice-change vocal.wav --pitch-semitones 5 --formant-factor 1.1 -o changed.wav
```

### C++

```cpp
#include <sonare/sonare.h>           // analysis + features + effects
#include <sonare/mastering/master.h> // mastering chain & processors

auto audio = sonare::Audio::from_file("music.mp3");
auto result = sonare::MusicAnalyzer(audio).analyze();
std::cout << "BPM: " << result.bpm
          << ", Key: " << result.key.to_string() << std::endl;
```

## Features

### Analysis (librosa-compatible)

| Music                     | Spectral / Pitch     | Streaming           |
|---------------------------|----------------------|---------------------|
| BPM / Tempo               | STFT / iSTFT         | Real-time analyzer  |
| Key Detection             | Mel Spectrogram      | Incremental BPM     |
| Beat / Downbeat tracking  | MFCC                 | Incremental key     |
| Time signature / meter    | Chroma / NNLS chroma | Onset events        |
| Chord (HMM / inversions)  | CQT / VQT            |                     |
| Section Detection         | Tempogram / PLP      |                     |
| Timbre / Dynamics         | Spectral Features    |                     |
| Pitch (YIN / pYIN)        | Onset Detection      |                     |
| RT60 / EDT / C50          | Room acoustics       |                     |
| Loudness (EBU R128 LUFS)  | Onset envelope       |                     |

### Mastering (66 DSP processors)

| Dynamics                  | EQ                        | Multiband / Stereo                  |
|---------------------------|---------------------------|-------------------------------------|
| Compressor                | Parametric / Graphic      | Multiband comp / EQ / limiter       |
| Limiter / Brickwall       | Linear / Minimum phase    | Linkwitz-Riley crossover (phase-comp)|
| Expander / Gate           | Dynamic EQ                | Stereo imager / M-S / Haas          |
| De-esser                  | Passive / stepped EQ      | Phase align / mono maker / compat   |
| Transient shaper          | Tilt / shelving           |                                     |

| Saturation / Repair                | Maximizer / Match                       | Building blocks            |
|------------------------------------|-----------------------------------------|----------------------------|
| Tube (Dempwolf 12AX7) / Tape       | True-peak limiter (ITU-R BS.1770-4)     | Polyphase FIR oversampler  |
| Transformer / Exciter / Bitcrusher | Loudness optimizer (LUFS target)        | ADAA-antialiased shaping   |
| Declick / Declip / Decrackle       | Adaptive release                        | Vicanek matched-Z biquads  |
| Denoise / Dereverb / Dehum         | Reference EQ / loudness / spectrum      | Partitioned convolver      |

Repair is classical DSP by design. `denoise_classical` covers spectral
subtraction, MMSE-STSA, and LogMMSE with explicit noise estimation; DNN
restoration, source separation, and interactive spectral repair are out of scope.

EQ phase modes preserve existing coefficient defaults: Zero Latency keeps RBJ
biquads for compatibility, while Natural Phase resolves bands through Vicanek
matched-Z IIR. High-frequency shelf designs fall back to RBJ when the Vicanek
endpoint gain error exceeds the fixed tolerance.

Mastering is built by default (`BUILD_MASTERING=ON`). Disable with
`cmake -DBUILD_MASTERING=OFF` to ship analysis-only builds.

### Mixing / routing

| Channel strips                   | Routing / scene API            | Metering / QA                 |
|----------------------------------|--------------------------------|-------------------------------|
| Input trim / fader / polarity    | Sends and FX buses             | Peak / RMS / true peak        |
| Balance / stereo / dual pan      | Bus inserts and graph PDC      | Correlation / mono width      |
| Width and gain automation        | C / Node / Python / WASM / CLI | Golden hashes and RT tests    |
| Insert hosting and sidechain keys| Persistent scene mixers        | No-allocation process checks  |

Mixing is built by default (`BUILD_MIXING=ON`) and depends on the mastering
processor interfaces for insert hosting. Disable with `cmake -DBUILD_MIXING=OFF`
for analysis/mastering-only builds.

## Performance

Analysis runs natively in C++ and uses multi-threading where it helps (HPSS
median filtering, the full `analyze()` pipeline). On the benchmark fixture,
iterative algorithms such as HPSS and pYIN, and the full pipeline, are
meaningfully faster than the equivalent librosa calls; single FFT-bound
features (STFT, Mel, MFCC) are roughly on par. WebAssembly is single-threaded,
so the multi-threaded gains do not apply there.

Mastering DSP uses ITU-spec polyphase oversampling, antiderivative
anti-aliasing (ADAA), and Eigen for SIMD-friendly linear algebra on hot paths.

See [Benchmarks](https://libsonare.libraz.net/docs/benchmarks) for the
methodology and per-feature numbers.

## librosa Compatibility

Default parameters match librosa:
- Sample rate: 22050 Hz
- n_fft: 2048, hop_length: 512, n_mels: 128
- fmin: 0.0, fmax: sr/2

## Supported audio formats

| Format | Default¹ | With FFmpeg² | WASM (`@libraz/libsonare`) |
|--------|----------|--------------|----------------------------|
| WAV (PCM 16/24/32, float32) | ✅ | ✅ | n/a (samples in) |
| MP3 | ✅ | ✅ | n/a |
| M4A / AAC / FLAC / OGG / Opus / WMA / ... | ❌ (clear error message) | ✅ | n/a (use Web Audio API) |

¹ **Default**: PyPI wheel (`pip install libsonare`) and source builds where FFmpeg
dev libs are not present. PyPI wheels are deterministically pinned to this mode
so installation never depends on the user's `libavformat`.

² **With FFmpeg**: source build with FFmpeg linked. CMake auto-detects via
pkg-config (`-DSONARE_WITH_FFMPEG=AUTO`, the default for `make build`), and you
can force on/off with `-DSONARE_WITH_FFMPEG=ON`/`OFF`. Python equivalent:
`SONARE_FFMPEG=1 pip install libsonare --no-binary libsonare`. Node native:
`SONARE_FFMPEG=1 yarn build`.

WASM does not bundle a file decoder by design; pass `Float32Array` samples obtained from
the Web Audio API or another JS decoder.

## Build from Source

```bash
# Native (auto-detects FFmpeg; mastering and mixing on by default)
make build && make test

# Analysis-only (smaller binary)
cmake -B build -DBUILD_MASTERING=OFF -DBUILD_MIXING=OFF && cmake --build build

# WebAssembly (mastering included)
make wasm

# Release (optimized)
make release
```

## Documentation

Full docs and browser-local demos: **[libsonare.libraz.net](https://libsonare.libraz.net)** ([demos](https://libsonare.libraz.net/demos)).

**Learn first**
- [Introduction](https://libsonare.libraz.net/docs/introduction) · [Getting Started](https://libsonare.libraz.net/docs/getting-started) · [Installation](https://libsonare.libraz.net/docs/installation) · [Examples](https://libsonare.libraz.net/docs/examples)

**API by runtime**
- [Browser / WASM](https://libsonare.libraz.net/docs/wasm) · [JavaScript](https://libsonare.libraz.net/docs/js-api) · [Python](https://libsonare.libraz.net/docs/python-api) · [Node.js Native](https://libsonare.libraz.net/docs/native-bindings) · [C++](https://libsonare.libraz.net/docs/cpp-api) · [CLI](https://libsonare.libraz.net/docs/cli)

**Build by task**
- [Mastering Processors](https://libsonare.libraz.net/docs/mastering-processors) · [Mixing Engine](https://libsonare.libraz.net/docs/mixing) · [Editing DSP](https://libsonare.libraz.net/docs/editing-dsp) · [Realtime & Streaming](https://libsonare.libraz.net/docs/realtime-streaming) · [Room Acoustics](https://libsonare.libraz.net/docs/acoustic-analysis)

**Understand the details**
- [Architecture](https://libsonare.libraz.net/docs/architecture) · [librosa Compatibility](https://libsonare.libraz.net/docs/librosa-compatibility) · [Benchmarks](https://libsonare.libraz.net/docs/benchmarks) · [Glossary](https://libsonare.libraz.net/docs/glossary)

## Non-goals

libsonare intentionally does **not** include:

- **Plugin-grade creative instruments/effects** — use Tone.js, a plugin host, or your DAW
- **Built-in audio synthesis** (oscillators, samplers, instrument DSP) — out of scope
- **Real-time I/O abstraction** (PortAudio/JACK wrappers) — callers handle I/O
- **Full DAW application workflow** (UI, device setup, plugin hosting implementation) —
  callers provide those layers; libsonare focuses on the headless arrangement/runtime core
- **Deep-learning models** (no bundled weights, no inference runtime) — keeps the
  library dependency-free and Apache-2.0 pure

These boundaries keep the library focused on **analysis + mastering + mixer DSP +
headless arrangement runtime** and allow us to maintain the dependency-free property.

## License

[Apache-2.0](LICENSE)
