/**
 * A float argument is refused, not saturated into a legal quantity.
 *
 * `FloatValue()` is a double-to-float conversion that SATURATES: `1e39` arrives
 * as `Infinity`, which several of these fields read as a sentinel and the rest
 * carry into the DSP as a legal-looking magnitude. The value that matters is
 * therefore the FINITE one past `FLT_MAX`, not `Infinity`: three arguments in
 * this population — both VQT `gamma`s, `normalize`'s `targetDb` and
 * `scaleQuantizeMidi`'s `referenceMidi` — are refused downstream when handed an
 * `Infinity` even with no reader in front of them, so a suite built on
 * `Infinity` would go green against the unrepaired code and prove nothing.
 *
 * Each argument opens with a CONSUMED positive control: two legitimate values
 * whose results differ, because without one an entry point that never reads the
 * argument passes exactly as an entry point that accepted it.
 *
 * Driven against the addon rather than the TypeScript facade: the facade's own
 * guards would pre-empt the native check, and the addon is the boundary a
 * generated binding or a direct consumer reaches.
 */

import { describe, expect, it } from 'vitest';
import { mixingScenePresetJson } from '../src/index.js';
import { addon } from '../src/native.js';
import { addonSources } from './_addon_sources.js';

/* biome-ignore lint/suspicious/noExplicitAny: the addon is untyped here on purpose. */
const native = addon as any;

const SAMPLE_RATE = 22050;

/** A finite value past `FLT_MAX`, in both signs — what saturation turns into an infinity. */
const PAST_FLOAT_RANGE = [1e39, -1e39];

/** The spellings a field with no "unspecified" value has no reading for. */
const NON_FINITE = [Number.POSITIVE_INFINITY, Number.NEGATIVE_INFINITY, Number.NaN];

function tone(seconds: number, partials: Array<[number, number]>): Float32Array {
  const out = new Float32Array(Math.round(SAMPLE_RATE * seconds));
  for (let i = 0; i < out.length; i++) {
    let sample = 0;
    for (const [hz, amp] of partials) {
      sample += amp * Math.sin((2 * Math.PI * hz * i) / SAMPLE_RATE);
    }
    out[i] = sample;
  }
  return out;
}

/** Two partials an octave-and-a-half apart, so a frequency bound has something to cut. */
const AUDIO = tone(2, [
  [220, 0.3],
  [1300, 0.15],
]);

/** Four timbre/level blocks, so section and melody bounds land on real boundaries. */
const BLOCKED = (() => {
  const out = new Float32Array(SAMPLE_RATE * 6);
  const hz = [220, 660, 220, 880];
  const amp = [0.35, 0.12, 0.4, 0.15];
  for (let i = 0; i < out.length; i++) {
    const block = Math.floor(i / (SAMPLE_RATE * 1.5)) % 4;
    out[i] = amp[block] * Math.sin((2 * Math.PI * hz[block] * i) / SAMPLE_RATE);
  }
  return out;
})();

/** Loud region, silence, quiet region — a trim / split threshold has to choose. */
const GATED = (() => {
  const out = new Float32Array(SAMPLE_RATE * 3);
  for (let i = Math.round(SAMPLE_RATE * 0.5); i < SAMPLE_RATE; i++) {
    out[i] = 0.5 * Math.sin((2 * Math.PI * 220 * i) / SAMPLE_RATE);
  }
  for (let i = Math.round(SAMPLE_RATE * 2); i < SAMPLE_RATE * 2.4; i++) {
    out[i] = 0.02 * Math.sin((2 * Math.PI * 220 * i) / SAMPLE_RATE);
  }
  return out;
})();

/** A slow amplitude envelope, so the loudness percentiles are not all one value. */
const SWELLING = (() => {
  const out = new Float32Array(SAMPLE_RATE * 6);
  for (let i = 0; i < out.length; i++) {
    const envelope = 0.02 + 0.9 * Math.abs(Math.sin((Math.PI * i) / (SAMPLE_RATE * 1.5)));
    out[i] = envelope * Math.sin((2 * Math.PI * 220 * i) / SAMPLE_RATE);
  }
  return out;
})();

/** Sixteen decaying transients at 120 BPM, so a tempo bound has a peak to include or exclude. */
const PULSED = (() => {
  const out = new Float32Array(SAMPLE_RATE * 8);
  for (let beat = 0; beat < 16; beat++) {
    const start = Math.round(beat * SAMPLE_RATE * 0.5);
    for (let i = 0; i < 800; i++) {
      out[start + i] = Math.exp(-i / 200) * Math.sin((2 * Math.PI * 900 * i) / SAMPLE_RATE);
    }
  }
  return out;
})();

/** A strictly positive matrix, which NMF requires. */
const MATRIX = (() => {
  const out = new Float32Array(8 * 12);
  for (let i = 0; i < out.length; i++) {
    out[i] = Math.abs(Math.sin(i * 0.7)) + 0.01;
  }
  return out;
})();

// Analysed once: every row below reads one of these rather than re-running the
// transform per case.
const STFT = native.stft(AUDIO, SAMPLE_RATE, 512, 256);
const CQT = native.cqt(AUDIO, SAMPLE_RATE, 512, 32.7, 24, 12);
// A shorter slice than its siblings: the only rows reading MEL are NNLS
// inversions whose cost scales with the frame count, and the argument under test
// is read before the solve starts, so extra frames buy the assertion nothing.
const MEL_SECONDS = 0.4;
const MEL = native.melSpectrogram(
  AUDIO.subarray(0, Math.round(SAMPLE_RATE * MEL_SECONDS)),
  SAMPLE_RATE,
  1024,
  256,
  32,
  0,
  0,
  false,
);
const MFCC = native.mfcc(AUDIO, SAMPLE_RATE, 512, 256, 32, 13);

/**
 * A short tone for the required-argument rows.
 *
 * Every one of those runs its whole transform seven times — two control arms
 * and five refusals — so they take the shortest signal the transform accepts
 * rather than a shared multi-second one.
 */
const SHORT = tone(0.15, [[220, 0.4]]);

// A note track: audio plus the pitch/voicing arrays whose frame rate several of
// the rows below vary. Two unvoiced frames split it into the two notes
// `mergeNotes` needs, and `splitNote` cuts the first of them.
const NOTE_FRAMES = 24;
const NOTE_HOP = 256;
const NOTE_RATE = SAMPLE_RATE / NOTE_HOP;
const NOTE_AUDIO = tone((NOTE_FRAMES * NOTE_HOP) / SAMPLE_RATE, [[441, 0.4]]);
const NOTE_F0 = new Float32Array(NOTE_FRAMES).fill(441);
const NOTE_PROB = new Float32Array(NOTE_FRAMES).fill(0.9);
for (const frame of [10, 11]) {
  NOTE_F0[frame] = 0;
  NOTE_PROB[frame] = 0;
}

/**
 * The same track without the gap, and with a 5 Hz vibrato: the pitch
 * decomposition splits drift from vibrato, so a perfectly steady track leaves
 * both halves at zero and no cutoff or frame rate can move them.
 */
const SUSTAINED_F0 = Float32Array.from({ length: NOTE_FRAMES }, (_, i) =>
  Math.fround(441 * (1 + 0.02 * Math.sin((2 * Math.PI * 5 * i) / NOTE_RATE))),
);

const NOTES = native.extractNotes(NOTE_AUDIO, SAMPLE_RATE, NOTE_F0, NOTE_RATE, {
  voicedProb: NOTE_PROB,
});

/** One parameter binding, so `midiParamToCc` has a parameter to map. */
const CC_BINDING = { ccNumber: 74, channel: 4, kind: 0, paramId: 88, minValue: 0, maxValue: 1 };

function magnitudeSum(values: Iterable<number>): number {
  let total = 0;
  for (const value of values) {
    total += Math.abs(value);
  }
  return total;
}

function peak(values: Iterable<number>): number {
  let highest = 0;
  for (const value of values) {
    highest = Math.max(highest, Math.abs(value));
  }
  return highest;
}

function newMixer(): unknown {
  return new native.Mixer(mixingScenePresetJson('commentaryDucking'), 48000, 8);
}

/** The gated fixture as a handle, for the metering methods that take one. */
function newGatedAudio(): unknown {
  return native.Audio.fromBuffer(GATED, SAMPLE_RATE);
}

interface FloatArgument {
  /** The addon entry point and argument, as the failure should name them. */
  name: string;
  /** The reader call site, so the table's coverage of the population is countable. */
  site: string;
  /** Two legitimate values whose results must differ. */
  control: [number, number];
  call: (value: number) => unknown;
}

/**
 * Every argument routed through `node_arg_finite_float`.
 *
 * `site` is the reader call, not the entry point: `cqt`, `pseudoCqt` and
 * `hybridCqt` share one `fmin` read through `CqtLike`, and `referenceMidi` is
 * read once in `ParseScaleArgs` for two entry points. Both are driven per entry
 * point and counted once.
 */
const FINITE_ARGUMENTS: FloatArgument[] = [
  {
    name: 'analyzeSections minSectionSec',
    site: 'analysis/progress.cpp minSectionSec',
    control: [0.5, 4],
    call: (v) => native.analyzeSections(BLOCKED, SAMPLE_RATE, 2048, 512, v).length,
  },
  {
    name: 'analyzeMelody fmin',
    site: 'analysis/progress.cpp fmin',
    control: [65, 500],
    call: (v) => native.analyzeMelody(BLOCKED, SAMPLE_RATE, v, 2093, 2048, 256, 0.1).meanFrequency,
  },
  {
    name: 'analyzeMelody fmax',
    site: 'analysis/progress.cpp fmax',
    control: [2093, 300],
    call: (v) => native.analyzeMelody(BLOCKED, SAMPLE_RATE, 65, v, 2048, 256, 0.1).meanFrequency,
  },
  {
    name: 'analyzeMelody threshold',
    site: 'analysis/progress.cpp threshold',
    control: [0.1, 0.95],
    call: (v) => native.analyzeMelody(BLOCKED, SAMPLE_RATE, 65, 2093, 2048, 256, v).meanFrequency,
  },
  {
    name: 'cqt fmin',
    site: 'features/advanced.cpp CqtLike fmin',
    control: [32.7, 130.81],
    call: (v) => native.cqt(AUDIO, SAMPLE_RATE, 512, v, 24, 12).frequencies[0],
  },
  {
    name: 'pseudoCqt fmin',
    site: 'features/advanced.cpp CqtLike fmin',
    control: [32.7, 130.81],
    call: (v) => native.pseudoCqt(AUDIO, SAMPLE_RATE, 512, v, 24, 12).frequencies[0],
  },
  {
    name: 'hybridCqt fmin',
    site: 'features/advanced.cpp CqtLike fmin',
    control: [32.7, 130.81],
    call: (v) => native.hybridCqt(AUDIO, SAMPLE_RATE, 512, v, 24, 12).frequencies[0],
  },
  {
    name: 'vqt fmin',
    site: 'features/advanced.cpp Vqt fmin',
    control: [32.7, 130.81],
    call: (v) => native.vqt(AUDIO, SAMPLE_RATE, 512, v, 24, 12, -1).frequencies[0],
  },
  {
    name: 'cqtToAudio fmin',
    site: 'features/advanced.cpp CqtToAudio fmin',
    control: [32.7, 130.81],
    call: (v) =>
      magnitudeSum(
        native.cqtToAudio(CQT.magnitude, CQT.nBins, CQT.nFrames, SAMPLE_RATE, 512, v, 12, 4),
      ),
  },
  {
    name: 'vqtToAudio fmin',
    site: 'features/advanced.cpp VqtToAudio fmin',
    control: [32.7, 130.81],
    call: (v) =>
      magnitudeSum(
        native.vqtToAudio(CQT.magnitude, CQT.nBins, CQT.nFrames, SAMPLE_RATE, 512, v, 12, -1, 4),
      ),
  },
  {
    name: 'melToStft fmin',
    site: 'features/advanced.cpp MelToStft fmin',
    control: [0, 300],
    call: (v) =>
      magnitudeSum(
        native.melToStft(MEL.power, 32, MEL.nFrames, SAMPLE_RATE, 1024, v, 0, false).power,
      ),
  },
  {
    name: 'melToStft fmax',
    site: 'features/advanced.cpp MelToStft fmax',
    control: [0, 4000],
    call: (v) =>
      magnitudeSum(
        native.melToStft(MEL.power, 32, MEL.nFrames, SAMPLE_RATE, 1024, 0, v, false).power,
      ),
  },
  {
    name: 'melToAudio fmin',
    site: 'features/advanced.cpp MelToAudio fmin',
    control: [0, 300],
    call: (v) =>
      magnitudeSum(
        native.melToAudio(MEL.power, 32, MEL.nFrames, SAMPLE_RATE, 1024, 256, v, 0, 4, false),
      ),
  },
  {
    name: 'melToAudio fmax',
    site: 'features/advanced.cpp MelToAudio fmax',
    control: [0, 4000],
    call: (v) =>
      magnitudeSum(
        native.melToAudio(MEL.power, 32, MEL.nFrames, SAMPLE_RATE, 1024, 256, 0, v, 4, false),
      ),
  },
  {
    name: 'griffinLim momentum',
    site: 'features/advanced.cpp GriffinLim momentum',
    control: [0, 0.99],
    call: (v) =>
      magnitudeSum(
        native.griffinLim(STFT.magnitude, STFT.nBins, STFT.nFrames, SAMPLE_RATE, 512, 256, 8, v),
      ),
  },
  {
    name: 'mfccToMel lifter',
    site: 'features/advanced.cpp MfccToMel lifter',
    control: [0, 20],
    call: (v) =>
      magnitudeSum(native.mfccToMel(MFCC.coefficients, MFCC.nMfcc, MFCC.nFrames, 32, v).power),
  },
  {
    name: 'mfccToAudio fmin',
    site: 'features/advanced.cpp MfccToAudio fmin',
    control: [0, 300],
    call: (v) =>
      magnitudeSum(
        native.mfccToAudio(
          MFCC.coefficients,
          MFCC.nMfcc,
          MFCC.nFrames,
          32,
          SAMPLE_RATE,
          512,
          256,
          v,
          0,
          4,
          false,
          0,
        ),
      ),
  },
  {
    name: 'mfccToAudio fmax',
    site: 'features/advanced.cpp MfccToAudio fmax',
    control: [0, 4000],
    call: (v) =>
      magnitudeSum(
        native.mfccToAudio(
          MFCC.coefficients,
          MFCC.nMfcc,
          MFCC.nFrames,
          32,
          SAMPLE_RATE,
          512,
          256,
          0,
          v,
          4,
          false,
          0,
        ),
      ),
  },
  {
    name: 'mfccToAudio lifter',
    site: 'features/advanced.cpp MfccToAudio lifter',
    control: [0, 20],
    call: (v) =>
      magnitudeSum(
        native.mfccToAudio(
          MFCC.coefficients,
          MFCC.nMfcc,
          MFCC.nFrames,
          32,
          SAMPLE_RATE,
          512,
          256,
          0,
          0,
          4,
          false,
          v,
        ),
      ),
  },
  {
    name: 'spectralContrast fmin',
    site: 'features/advanced.cpp SpectralContrast fmin',
    control: [200, 1000],
    call: (v) =>
      magnitudeSum(native.spectralContrast(AUDIO, SAMPLE_RATE, 512, 256, 4, v, 0.02).data),
  },
  {
    name: 'spectralContrast quantile',
    site: 'features/advanced.cpp SpectralContrast quantile',
    control: [0.02, 0.4],
    call: (v) =>
      magnitudeSum(native.spectralContrast(AUDIO, SAMPLE_RATE, 512, 256, 4, 200, v).data),
  },
  {
    name: 'zeroCrossings threshold',
    site: 'features/advanced.cpp ZeroCrossings threshold',
    control: [1e-10, 0.25],
    call: (v) => native.zeroCrossings(AUDIO, v).length,
  },
  {
    name: 'pitchTuning resolution',
    site: 'features/advanced.cpp PitchTuning resolution',
    control: [0.01, 0.2],
    call: (v) => native.pitchTuning(Float32Array.from([440, 441, 450, 460]), v),
  },
  {
    name: 'estimateTuning resolution',
    site: 'features/advanced.cpp EstimateTuning resolution',
    control: [0.01, 0.2],
    call: (v) => native.estimateTuning(AUDIO, SAMPLE_RATE, 512, 256, v, 12),
  },
  {
    name: 'trimSilence topDb',
    site: 'features/signal.cpp TrimSilence topDb',
    control: [60, 20],
    call: (v) => native.trimSilence(GATED, v, 2048, 512).endSample,
  },
  {
    name: 'splitSilence topDb',
    site: 'features/signal.cpp SplitSilence topDb',
    control: [60, 20],
    call: (v) => native.splitSilence(GATED, v, 2048, 512).length,
  },
  {
    name: 'padCenter padValue',
    site: 'features/signal.cpp PadCenter padValue',
    control: [0, 7],
    call: (v) => Array.from(native.padCenter(Float32Array.from([1, 2]), 6, v)).join(','),
  },
  {
    name: 'fixLength padValue',
    site: 'features/signal.cpp FixLength padValue',
    control: [0, 7],
    call: (v) => Array.from(native.fixLength(Float32Array.from([1, 2]), 5, v)).join(','),
  },
  {
    name: 'vectorNormalize threshold',
    site: 'features/signal.cpp VectorNormalize threshold',
    control: [0, 10],
    call: (v) =>
      magnitudeSum(native.vectorNormalize(Float32Array.from([0.001, 0.002, 3, 4]), 2, v)),
  },
  {
    name: 'meteringSilenceRatio thresholdDb',
    site: 'analysis/metering.cpp MeteringSilenceRatio thresholdDb',
    control: [-45, -5],
    call: (v) => native.meteringSilenceRatio(GATED, SAMPLE_RATE, v, 1024, 256),
  },
  {
    // `audio.silenceRatio` reaches its own reader, not the free function's.
    name: 'audio.silenceRatio thresholdDb',
    site: 'analysis/metering.cpp SilenceRatioInstance thresholdDb',
    control: [-45, -5],
    call: (v) => {
      /* biome-ignore lint/suspicious/noExplicitAny: the addon is untyped here on purpose. */
      const audio = newGatedAudio() as any;
      return audio.silenceRatio(v, 1024, 256);
    },
  },
  {
    name: 'meteringDetectClipping threshold',
    site: 'analysis/metering.cpp MeteringDetectClipping threshold',
    control: [0.999, 0.2],
    call: (v) => native.meteringDetectClipping(AUDIO, SAMPLE_RATE, v, 1).clippedSamples,
  },
  {
    name: 'meteringDynamicRange windowSec',
    site: 'analysis/metering.cpp MeteringDynamicRange windowSec',
    control: [0.4, 0.05],
    call: (v) => native.meteringDynamicRange(SWELLING, SAMPLE_RATE, v, 0.1, -1, -1).dynamicRangeDb,
  },
  {
    name: 'meteringDynamicRange hopSec',
    site: 'analysis/metering.cpp MeteringDynamicRange hopSec',
    control: [0.2, 0.02],
    call: (v) =>
      Object.keys(native.meteringDynamicRange(SWELLING, SAMPLE_RATE, 0.4, v, -1, -1).windowRmsDb)
        .length,
  },
  {
    name: 'meteringDynamicRange lowPercentile',
    site: 'analysis/metering.cpp MeteringDynamicRange lowPercentile',
    control: [0.1, 0.4],
    call: (v) =>
      native.meteringDynamicRange(SWELLING, SAMPLE_RATE, 0.4, 0.1, v, 0.95).lowPercentileDb,
  },
  {
    name: 'meteringDynamicRange highPercentile',
    site: 'analysis/metering.cpp MeteringDynamicRange highPercentile',
    control: [0.95, 0.6],
    call: (v) =>
      native.meteringDynamicRange(SWELLING, SAMPLE_RATE, 0.4, 0.1, 0.1, v).highPercentileDb,
  },
  {
    name: 'scaleQuantizeMidi referenceMidi',
    site: 'analysis/metering.cpp ParseScaleArgs referenceMidi',
    control: [0, 69.5],
    call: (v) => native.scaleQuantizeMidi(0, 0b101010110101, 60.4, v),
  },
  {
    name: 'scaleCorrectionSemitones referenceMidi',
    site: 'analysis/metering.cpp ParseScaleArgs referenceMidi',
    control: [0, 69.5],
    call: (v) => native.scaleCorrectionSemitones(0, 0b101010110101, 60.4, v),
  },
  {
    name: 'cyclicTempogram bpmMin',
    site: 'features/rhythm.cpp CyclicTempogram bpmMin',
    control: [60, 150],
    call: (v) => native.cyclicTempogram(PULSED, SAMPLE_RATE, 512, 384, v, 20).data[5],
  },
  {
    name: 'plp tempoMin',
    site: 'features/rhythm.cpp Plp tempoMin',
    control: [30, 120],
    call: (v) => magnitudeSum(native.plp(PULSED, SAMPLE_RATE, 512, v, 300, 384)),
  },
  {
    name: 'plp tempoMax',
    site: 'features/rhythm.cpp Plp tempoMax',
    control: [300, 150],
    call: (v) => magnitudeSum(native.plp(PULSED, SAMPLE_RATE, 512, 30, v, 384)),
  },
  {
    name: 'nnlsChroma blendWeight',
    site: 'features/rhythm.cpp NnlsChroma blendWeight',
    control: [0.55, 0.05],
    call: (v) => magnitudeSum(native.nnlsChroma(AUDIO, SAMPLE_RATE, true, v, 4096, 512).data),
  },
  {
    name: 'normalize targetDb',
    site: 'sonare_wrap_effects.cpp Normalize targetDb',
    control: [0, -6],
    call: (v) => peak(native.normalize(AUDIO, SAMPLE_RATE, v, 'peak')),
  },
  {
    name: 'trim thresholdDb',
    site: 'sonare_wrap_effects.cpp Trim thresholdDb',
    control: [-60, -20],
    call: (v) => native.trim(GATED, SAMPLE_RATE, v).length,
  },
  {
    name: 'mastering targetLufs',
    site: 'effects/mastering.cpp Mastering targetLufs',
    control: [-14, -20],
    call: (v) => native.mastering(AUDIO, SAMPLE_RATE, v, -1).outputLufs,
  },
  {
    name: 'mastering ceilingDb',
    site: 'effects/mastering.cpp Mastering ceilingDb',
    control: [-1, -12],
    call: (v) => peak(native.mastering(AUDIO, SAMPLE_RATE, -14, v).samples),
  },
  {
    name: 'mixer addSend sendDb',
    site: 'mixer/sends_metering.cpp AddSend sendDb',
    control: [-6, 0],
    call: (v) => {
      /* biome-ignore lint/suspicious/noExplicitAny: the addon is untyped here on purpose. */
      const mixer = newMixer() as any;
      mixer.addSend('host', 'host-extra', 'master', v, 0);
      const scene = JSON.parse(mixer.toSceneJson());
      return scene.strips.find((strip: { id: string }) => strip.id === 'host').sends[0].sendDb;
    },
  },
  {
    // `init` is pinned to nndsvd: sonare_decompose's default initialiser is
    // "random", which would make the control itself non-deterministic.
    name: 'decompose beta',
    site: 'effects/extra.cpp Decompose beta',
    control: [2, 1],
    call: (v) => magnitudeSum(native.decompose(MATRIX, 8, 12, 2, 20, v, 'nndsvd').w.data),
  },
  {
    // Optional in substance: `useZi` is `info[2].IsNumber()`, so the fallback is
    // the same one the reader writes. 0 vs 1 shifts out[0] by exactly `coef`.
    name: 'preemphasis zi',
    site: 'sonare_wrap_features.cpp Preemphasis zi',
    control: [0, 1],
    call: (v) => native.preemphasis(Float32Array.from([1, 2, 3, 4]), 0.97, v)[0],
  },
  {
    name: 'deemphasis zi',
    site: 'sonare_wrap_features.cpp Deemphasis zi',
    control: [0, 1],
    call: (v) => native.deemphasis(Float32Array.from([1, 2, 3, 4]), 0.97, v)[0],
  },
];

describe('an optional float argument is refused rather than saturated', () => {
  it.each(FINITE_ARGUMENTS.map((argument) => [argument.name, argument] as const))(
    '%s',
    (_name, argument) => {
      const [low, high] = argument.control;
      // Positive control: the argument is consumed, so an entry point that
      // ignored it could not reach the refusals below looking like this one.
      expect(argument.call(low)).not.toBe(argument.call(high));
      for (const value of [...PAST_FLOAT_RANGE, ...NON_FINITE]) {
        expect(() => argument.call(value), `${argument.name} ${value}`).toThrow(RangeError);
      }
    },
  );
});

/**
 * The two arguments that stay on `node_arg_float`.
 *
 * A NaN gamma SELECTS the librosa-compatible ERB-derived automatic value at
 * `src/feature/vqt.cpp`, so refusing it would be a regression of a working
 * feature rather than the repair of a doc mismatch. The saturation defect still
 * closes: the reader refuses `1e39` under either verdict.
 */
const PASSTHRU_ARGUMENTS: FloatArgument[] = [
  {
    name: 'vqt gamma',
    site: 'features/advanced.cpp Vqt gamma',
    control: [-1, 0],
    call: (v) => magnitudeSum(native.vqt(AUDIO, SAMPLE_RATE, 512, 32.7, 24, 12, v).magnitude),
  },
  {
    name: 'vqtToAudio gamma',
    site: 'features/advanced.cpp VqtToAudio gamma',
    control: [-1, 0],
    call: (v) =>
      magnitudeSum(
        native.vqtToAudio(CQT.magnitude, CQT.nBins, CQT.nFrames, SAMPLE_RATE, 512, 32.7, 12, v, 4),
      ),
  },
];

describe('a gamma keeps its NaN, which selects the automatic ERB value', () => {
  it.each(PASSTHRU_ARGUMENTS.map((argument) => [argument.name, argument] as const))(
    '%s',
    (_name, argument) => {
      const [automatic, standardCqt] = argument.control;
      expect(argument.call(automatic)).not.toBe(argument.call(standardCqt));
      // NaN resolves to the same automatic value a negative gamma selects, which
      // is what a finite refusal here would have taken away.
      expect(argument.call(Number.NaN)).toBe(argument.call(automatic));
      for (const value of PAST_FLOAT_RANGE) {
        expect(() => argument.call(value), `${argument.name} ${value}`).toThrow(RangeError);
      }
      // An infinity is refused by the core's own finiteness check rather than by
      // the reader, so it is NOT a RangeError. This is why the discriminating
      // value above has to be a finite 1e39.
      expect(() => argument.call(Number.POSITIVE_INFINITY)).toThrow();
      expect(() => argument.call(Number.POSITIVE_INFINITY)).not.toThrow(RangeError);
    },
  );
});

describe('the table covers the whole reader population', () => {
  it('drives every node_arg_finite_float call site in the addon', () => {
    const calls = addonSources()
      .filter((source) => source.file !== 'sonare_wrap_options.h')
      .flatMap((source) => source.text.match(/node_arg_finite_float\(info,/g) ?? []);
    const sites = new Set(FINITE_ARGUMENTS.map((argument) => argument.site));
    expect(sites.size).toBe(calls.length);
  });

  it('keeps the two deliberately passed-through gammas out of that population', () => {
    const gammas = addonSources()
      .filter((source) => source.file === 'features/advanced.cpp')
      .flatMap((source) => source.text.match(/const float gamma = node_arg_float\(info,/g) ?? []);
    expect(gammas).toHaveLength(PASSTHRU_ARGUMENTS.length);
  });
});

/**
 * The required arguments and object keys, which read through
 * `node_narrow_finite_float` at the site rather than through an argument helper.
 *
 * These have no fallback: each sits after an explicit arity-and-type gate that
 * has already refused a missing or wrong-typed argument, so a helper carrying a
 * fallback value would write a default into the source that can never be
 * selected.
 *
 * Most of this population is already refused somewhere downstream — the mixer's
 * C ABI guards `finite()` on every setter, the streaming analyzer throws
 * `InvalidParameter`, `sonare_engine_set_automation_lane` rejects a non-finite
 * breakpoint. For those the repair is the error class and the name, not the
 * outcome. It is exactly why the discriminating value has to be the finite
 * `1e39`: an `Infinity` would be refused by the core with no reader in front of
 * it, and the assertion would pass against unrepaired code.
 */
const REQUIRED_FLOAT_ARGUMENTS: FloatArgument[] = [
  {
    name: 'timeToFrames time',
    site: 'sonare_wrap_features.cpp TimeToFrames time',
    control: [1, 2],
    call: (v) => native.timeToFrames(v, SAMPLE_RATE, 512),
  },
  {
    name: 'noteSegments frameRate',
    site: 'sonare_wrap_features.cpp NoteSegments frameRate',
    control: [100, 200],
    call: (v) =>
      native.noteSegments({ f0Hz: NOTE_F0, voicedProb: NOTE_PROB, frameRate: v }).at(-1)
        ?.endSeconds,
  },
  {
    name: 'timeStretch rate',
    site: 'sonare_wrap_effects.cpp TimeStretch rate',
    control: [0.5, 2],
    call: (v) => native.timeStretch(SHORT, SAMPLE_RATE, v).length,
  },
  {
    name: 'pitchShift semitones',
    site: 'sonare_wrap_effects.cpp PitchShift semitones',
    control: [0, 12],
    call: (v) => magnitudeSum(native.pitchShift(SHORT, SAMPLE_RATE, v)),
  },
  {
    name: 'pitchCorrectToMidi currentMidi',
    site: 'sonare_wrap_effects.cpp PitchCorrectToMidi currentMidi',
    control: [60, 72],
    call: (v) => magnitudeSum(native.pitchCorrectToMidi(SHORT, SAMPLE_RATE, v, 69)),
  },
  {
    name: 'pitchCorrectToMidi targetMidi',
    site: 'sonare_wrap_effects.cpp PitchCorrectToMidi targetMidi',
    control: [60, 72],
    call: (v) => magnitudeSum(native.pitchCorrectToMidi(SHORT, SAMPLE_RATE, 69, v)),
  },
  {
    name: 'pitchCorrectToMidiTimevarying targetMidi',
    site: 'sonare_wrap_effects.cpp PitchCorrectToMidiTimevarying targetMidi',
    control: [60, 72],
    call: (v) =>
      magnitudeSum(
        native.pitchCorrectToMidiTimevarying(NOTE_AUDIO, SAMPLE_RATE, SUSTAINED_F0, v, NOTE_HOP),
      ),
  },
  {
    name: 'noteStretch stretchRatio',
    site: 'sonare_wrap_effects.cpp NoteStretch stretchRatio',
    control: [1, 2],
    call: (v) => magnitudeSum(native.noteStretch(SHORT, SAMPLE_RATE, 0, SHORT.length, v)),
  },
  {
    name: 'extractNotes frameRate',
    site: 'sonare_wrap_effects.cpp ExtractNotes frameRate',
    control: [NOTE_RATE, NOTE_RATE * 2],
    call: (v) =>
      native.extractNotes(NOTE_AUDIO, SAMPLE_RATE, NOTE_F0, v, { voicedProb: NOTE_PROB }).at(-1)
        ?.offsetSample,
  },
  {
    name: 'decomposeNotePitch frameRate',
    site: 'sonare_wrap_effects.cpp DecomposeNotePitch frameRate',
    control: [NOTE_RATE, NOTE_RATE * 2],
    call: (v) => magnitudeSum(native.decomposeNotePitch(SUSTAINED_F0, v, 441, 3).vibratoCents),
  },
  {
    name: 'decomposeNotePitch medianHz',
    site: 'sonare_wrap_effects.cpp DecomposeNotePitch medianHz',
    control: [441, 220],
    call: (v) => native.decomposeNotePitch(SUSTAINED_F0, NOTE_RATE, v, 3).centreHz,
  },
  {
    name: 'decomposeNotePitch vibratoCutoffHz',
    site: 'sonare_wrap_effects.cpp DecomposeNotePitch vibratoCutoffHz',
    control: [3, 0.5],
    call: (v) =>
      magnitudeSum(native.decomposeNotePitch(SUSTAINED_F0, NOTE_RATE, 441, v).vibratoCents),
  },
  {
    name: 'splitNote frameRate',
    site: 'sonare_wrap_effects.cpp SplitNote frameRate',
    control: [NOTE_RATE, NOTE_RATE * 2],
    call: (v) =>
      native
        .splitNote(NOTE_AUDIO, SAMPLE_RATE, NOTE_F0, v, NOTES, 0, 5, { voicedProb: NOTE_PROB })
        .at(-1)?.offsetSample,
  },
  {
    name: 'mergeNotes frameRate',
    site: 'sonare_wrap_effects.cpp MergeNotes frameRate',
    control: [NOTE_RATE, NOTE_RATE * 2],
    call: (v) =>
      native
        .mergeNotes(NOTE_AUDIO, SAMPLE_RATE, NOTE_F0, v, NOTES, 0, 1, { voicedProb: NOTE_PROB })
        .at(-1)?.offsetSample,
  },
  {
    name: 'voiceChange pitchSemitones',
    site: 'sonare_wrap_effects.cpp VoiceChange pitchSemitones',
    control: [0, 7],
    call: (v) => magnitudeSum(native.voiceChange(SHORT, SAMPLE_RATE, v, 1)),
  },
  {
    name: 'voiceChange formantFactor',
    site: 'sonare_wrap_effects.cpp VoiceChange formantFactor',
    control: [1, 1.5],
    call: (v) => magnitudeSum(native.voiceChange(SHORT, SAMPLE_RATE, 0, v)),
  },
  {
    name: 'phaseVocoder rate',
    site: 'effects/extra.cpp PhaseVocoder rate',
    control: [0.5, 2],
    call: (v) => native.phaseVocoder(SHORT, SAMPLE_RATE, v).length,
  },
  {
    name: 'scaleQuantizeMidi midi',
    site: 'analysis/metering.cpp ParseScaleArgs midi',
    control: [60.4, 61.6],
    call: (v) => native.scaleQuantizeMidi(0, 0b101010110101, v, 0),
  },
  {
    name: 'scaleCorrectionSemitones midi',
    site: 'analysis/metering.cpp ParseScaleArgs midi',
    control: [60.4, 61.6],
    call: (v) => native.scaleCorrectionSemitones(0, 0b101010110101, v, 0),
  },
  {
    name: 'midiParamToCc unitValue',
    site: 'addon.cpp MidiParamToCc unitValue',
    control: [0, 1],
    call: (v) => native.midiParamToCc([CC_BINDING], 88, v, 0, 0)?.data0,
  },
];

describe('a required float argument is refused rather than saturated', () => {
  it.each(REQUIRED_FLOAT_ARGUMENTS.map((argument) => [argument.name, argument] as const))(
    '%s',
    (_name, argument) => {
      const [low, high] = argument.control;
      expect(argument.call(low)).not.toBe(argument.call(high));
      for (const value of [...PAST_FLOAT_RANGE, ...NON_FINITE]) {
        expect(() => argument.call(value), `${argument.name} ${value}`).toThrow(RangeError);
      }
    },
  );
});

/** The block the automation rows mix. */
const MIX_BLOCK = 512;

/* biome-ignore lint/suspicious/noExplicitAny: the addon is untyped here on purpose. */
type Mixer = any;

/** Apply one setter to a fresh mixer and return the scene it serializes to. */
function mixerScene(apply: (mixer: Mixer) => void): string {
  const mixer = newMixer() as Mixer;
  apply(mixer);
  return mixer.toSceneJson();
}

/**
 * Apply one scheduler to a fresh mixer, mix one block, and return both channel
 * levels.
 *
 * Antiphase, and the channels are reported separately: a pan or a width moves
 * energy between them without changing their sum, so a mono-summed observable
 * would leave those two rows with no control at all.
 */
function mixerLevel(apply: (mixer: Mixer) => void): string {
  const mixer = new native.Mixer(mixingScenePresetJson('commentaryDucking'), 48000, MIX_BLOCK);
  apply(mixer);
  mixer.compile();
  const left: Float32Array[] = [];
  const right: Float32Array[] = [];
  for (let strip = 0; strip < mixer.stripCount(); strip++) {
    const channel = new Float32Array(MIX_BLOCK);
    for (let i = 0; i < MIX_BLOCK; i++) {
      channel[i] = 0.5 * Math.sin((2 * Math.PI * 1000 * i) / 48000);
    }
    left.push(channel);
    right.push(channel.map((sample) => -sample));
  }
  const out = mixer.processStereo(left, right);
  return `${magnitudeSum(out.left)}/${magnitudeSum(out.right)}`;
}

/** Analyse SHORT through a fresh analyzer the caller has configured. */
/* biome-ignore lint/suspicious/noExplicitAny: the addon is untyped here on purpose. */
function analyzerFrames(apply: (analyzer: any) => void): Record<string, Float32Array> {
  const analyzer = new native.StreamAnalyzer({
    sampleRate: SAMPLE_RATE,
    nFft: 512,
    hopLength: 256,
    nMels: 8,
    computeChroma: true,
  });
  apply(analyzer);
  analyzer.process(SHORT);
  const frames = analyzer.readFramesSoa(8);
  analyzer.destroy();
  return frames;
}

const PLATFORM = { name: 'test', targetLufs: -14, ceilingDb: -1 };

/** A tempogram for the `factors` rows, computed once. */
const TEMPOGRAM = native.fourierTempogram(
  native.onsetEnvelope(PULSED, SAMPLE_RATE, 512, 128),
  SAMPLE_RATE,
  512,
  32,
  true,
  true,
).data;

/**
 * A small, short room impulse: the `band*` rows synthesize one per arm.
 *
 * Source and listener are placed explicitly, because the defaults sit outside a
 * room this small and the synthesis then reports an error and an empty impulse.
 */
function rir(options: Record<string, unknown>): Float32Array {
  return native.synthesizeRir({
    sampleRate: SAMPLE_RATE,
    lengthM: 4,
    widthM: 3,
    heightM: 2.5,
    ismOrder: 1,
    maxSeconds: 0.2,
    sourceX: 1,
    sourceY: 1,
    sourceZ: 1.2,
    listenerX: 3,
    listenerY: 2,
    listenerZ: 1.2,
    ...options,
  }).rir;
}

const REQUIRED_FLOAT_STATE_ARGUMENTS: FloatArgument[] = [
  {
    name: 'StreamAnalyzer setNormalizationGain gain',
    site: 'streaming/stream_analyzer.cpp SetNormalizationGain gain',
    control: [1, 4],
    call: (v) => magnitudeSum(analyzerFrames((a) => a.setNormalizationGain(v)).rmsEnergy),
  },
  {
    name: 'StreamAnalyzer setTuningRefHz refHz',
    site: 'streaming/stream_analyzer.cpp SetTuningRefHz refHz',
    control: [440, 466.16],
    call: (v) => magnitudeSum(analyzerFrames((a) => a.setTuningRefHz(v)).chroma),
  },
  {
    name: 'Mixer setInputTrimDb db',
    site: 'mixer/strip_state.cpp SetInputTrimDb db',
    control: [0, -6],
    call: (v) => mixerScene((m) => m.setInputTrimDb('host', v)),
  },
  {
    name: 'Mixer setFaderDb db',
    site: 'mixer/strip_state.cpp SetFaderDb db',
    control: [0, -6],
    call: (v) => mixerScene((m) => m.setFaderDb('host', v)),
  },
  {
    name: 'Mixer setPan pan',
    site: 'mixer/strip_state.cpp SetPan pan',
    control: [-1, 1],
    call: (v) => mixerScene((m) => m.setPan('host', v)),
  },
  {
    name: 'Mixer setWidth width',
    site: 'mixer/strip_state.cpp SetWidth width',
    control: [1, 0],
    call: (v) => mixerScene((m) => m.setWidth('host', v)),
  },
  {
    name: 'Mixer setVcaOffsetDb offsetDb',
    site: 'mixer/strip_state.cpp SetVcaOffsetDb offsetDb',
    control: [0, -6],
    call: (v) => mixerScene((m) => m.setVcaOffsetDb('host', v)),
  },
  {
    name: 'Mixer setDualPan leftPan',
    site: 'mixer/strip_state.cpp SetDualPan leftPan',
    control: [-1, 1],
    call: (v) => mixerScene((m) => m.setDualPan('host', v, 1)),
  },
  {
    name: 'Mixer setDualPan rightPan',
    site: 'mixer/strip_state.cpp SetDualPan rightPan',
    control: [-1, 1],
    call: (v) => mixerScene((m) => m.setDualPan('host', -1, v)),
  },
  {
    name: 'Mixer setSendDb sendDb',
    site: 'mixer/sends_metering.cpp SetSendDb sendDb',
    control: [0, -6],
    call: (v) =>
      mixerScene((m) => {
        const index = m.addSend('host', 'host-extra', 'master', 0, 0);
        m.setSendDb('host', index, v);
      }),
  },
  {
    name: 'Mixer addVcaGroup gainDb',
    site: 'mixer/topology_automation.cpp AddVcaGroup gainDb',
    control: [0, -6],
    call: (v) => mixerScene((m) => m.addVcaGroup('vca', v, ['host'])),
  },
  {
    name: 'Mixer setVcaGroupGainDb gainDb',
    site: 'mixer/topology_automation.cpp SetVcaGroupGainDb gainDb',
    control: [0, -6],
    call: (v) =>
      mixerScene((m) => {
        m.addVcaGroup('vca', 0, ['host']);
        m.setVcaGroupGainDb('vca', v);
      }),
  },
  {
    name: 'Mixer scheduleFaderAutomation faderDb',
    site: 'mixer/topology_automation.cpp ScheduleFaderAutomation faderDb',
    control: [0, -24],
    call: (v) => mixerLevel((m) => m.scheduleFaderAutomation('host', 0, v, 0)),
  },
  {
    name: 'Mixer schedulePanAutomation pan',
    site: 'mixer/topology_automation.cpp SchedulePanAutomation pan',
    control: [-1, 1],
    call: (v) => mixerLevel((m) => m.schedulePanAutomation('host', 0, v, 0)),
  },
  {
    name: 'Mixer scheduleWidthAutomation width',
    site: 'mixer/topology_automation.cpp ScheduleWidthAutomation width',
    control: [1, 0],
    call: (v) => mixerLevel((m) => m.scheduleWidthAutomation('host', 0, v, 0)),
  },
  {
    name: 'Mixer scheduleSendAutomation db',
    site: 'mixer/topology_automation.cpp ScheduleSendAutomation db',
    control: [0, -24],
    call: (v) =>
      mixerLevel((m) => {
        const index = m.addSend('host', 'host-extra', 'master', 0, 0);
        m.scheduleSendAutomation('host', index, 0, v, 0);
      }),
  },
  {
    name: 'Mixer scheduleInsertAutomation value',
    site: 'sonare_wrap_mixer.cpp ScheduleInsertAutomation value',
    // Insert 1 of the host strip is the compressor; its first parameter is the
    // threshold, so 0 dB leaves the block alone and -60 dB squeezes it.
    control: [0, -60],
    call: (v) => mixerLevel((m) => m.scheduleInsertAutomation(0, 1, 0, 0, v, 0)),
  },
  {
    name: 'mixStereo inputTrimDb',
    site: 'effects/mixing.cpp MixStereo inputTrimDb',
    control: [0, -6],
    call: (v) => peak(native.mixStereo([SHORT], [SHORT], SAMPLE_RATE, { inputTrimDb: v }).left),
  },
  {
    name: 'mixStereo faderDb',
    site: 'effects/mixing.cpp MixStereo faderDb',
    control: [0, -6],
    call: (v) => peak(native.mixStereo([SHORT], [SHORT], SAMPLE_RATE, { faderDb: v }).left),
  },
  {
    name: 'mixStereo pan',
    site: 'effects/mixing.cpp MixStereo pan',
    control: [-1, 1],
    call: (v) => peak(native.mixStereo([SHORT], [SHORT], SAMPLE_RATE, { pan: v }).left),
  },
  {
    name: 'mixStereo width',
    site: 'effects/mixing.cpp MixStereo width',
    control: [1, 0],
    call: (v) =>
      peak(
        native.mixStereo([SHORT], [SHORT.map((s: number) => -s)], SAMPLE_RATE, { width: v }).left,
      ),
  },
  {
    name: 'mastering releaseMs',
    site: 'effects/mastering.cpp Mastering releaseMs',
    // Neither arm may be 0: that is the ZeroIsDefault sentinel, and a control
    // built on it would measure the sentinel rather than the value.
    control: [20, 200],
    // A target and ceiling the limiter has to work for: at -14 / -1 this input
    // never reaches the ceiling, and a release nothing recovers from is not a
    // control.
    call: (v) => magnitudeSum(native.mastering(AUDIO, SAMPLE_RATE, -6, -12, 4, v).samples),
  },
  {
    name: 'masteringStreamingPreview targetLufs',
    site: 'effects/mastering_pair.cpp MasteringStreamingPreview targetLufs',
    control: [-14, -20],
    call: (v) =>
      native.masteringStreamingPreview(SHORT, SAMPLE_RATE, [{ ...PLATFORM, targetLufs: v }]),
  },
  {
    name: 'masteringStreamingPreview ceilingDb',
    site: 'effects/mastering_pair.cpp MasteringStreamingPreview ceilingDb',
    control: [-1, -12],
    call: (v) =>
      native.masteringStreamingPreview(SHORT, SAMPLE_RATE, [{ ...PLATFORM, ceilingDb: v }]),
  },
  {
    name: 'masteringStreamingPreviewStereo targetLufs',
    site: 'effects/mastering_pair.cpp MasteringStreamingPreviewStereo targetLufs',
    control: [-14, -20],
    call: (v) =>
      native.masteringStreamingPreviewStereo(SHORT, SHORT, SAMPLE_RATE, [
        { ...PLATFORM, targetLufs: v },
      ]),
  },
  {
    name: 'masteringStreamingPreviewStereo ceilingDb',
    site: 'effects/mastering_pair.cpp MasteringStreamingPreviewStereo ceilingDb',
    control: [-1, -20],
    call: (v) =>
      native.masteringStreamingPreviewStereo(SHORT, SHORT, SAMPLE_RATE, [
        { ...PLATFORM, ceilingDb: v },
      ]),
  },
  {
    name: 'RealtimeEngine setAutomationLane value',
    site: 'sonare_wrap_engine.cpp SetAutomationLane value',
    control: [0, -24],
    call: (v) => {
      /* biome-ignore lint/suspicious/noExplicitAny: the addon is untyped here on purpose. */
      const engine = new native.RealtimeEngine(48000, 128) as any;
      try {
        engine.addParameter({ id: 7, name: 'gain', unit: 'dB', minValue: -60, maxValue: 12 });
        engine.setGraph({
          nodes: [
            { id: 'in', numPorts: 2 },
            { id: 'gain', type: 1, gainDb: 0, numPorts: 2 },
            { id: 'out', numPorts: 2 },
          ],
          connections: [
            { sourceNode: 'in', sourcePort: 0, destNode: 'gain', destPort: 0 },
            { sourceNode: 'in', sourcePort: 1, destNode: 'gain', destPort: 1 },
            { sourceNode: 'gain', sourcePort: 0, destNode: 'out', destPort: 0 },
            { sourceNode: 'gain', sourcePort: 1, destNode: 'out', destPort: 1 },
          ],
          inputNode: 'in',
          outputNode: 'out',
          numChannels: 2,
          parameterBindings: [{ paramId: 7, nodeId: 'gain' }],
        });
        engine.setAutomationLane(7, [{ ppq: 0, value: v }]);
        engine.play();
        const block = new Float32Array(128).fill(0.25);
        return magnitudeSum(engine.process([block, new Float32Array(block)])[0]);
      } finally {
        engine.destroy();
      }
    },
  },
];

describe('a required float state setter is refused rather than saturated', () => {
  it.each(REQUIRED_FLOAT_STATE_ARGUMENTS.map((argument) => [argument.name, argument] as const))(
    '%s',
    (_name, argument) => {
      const [low, high] = argument.control;
      expect(argument.call(low)).not.toBe(argument.call(high));
      for (const value of [...PAST_FLOAT_RANGE, ...NON_FINITE]) {
        expect(() => argument.call(value), `${argument.name} ${value}`).toThrow(RangeError);
      }
    },
  );
});

/**
 * `setExpectedDuration` has no in-domain control: the value reaches one
 * comparison in the chord-progression lock and no getter reports it back.
 *
 * What stands in for one is a differential rejection against the core's own
 * domain — a non-negative duration is accepted and a negative one is refused —
 * which is what an entry point that never read its argument could not do. The
 * reader's own refusal is then distinguishable from the core's by class.
 */
describe('setExpectedDuration reaches the core, and 1e39 does not reach it at all', () => {
  /* biome-ignore lint/suspicious/noExplicitAny: the addon is untyped here on purpose. */
  function analyzer(): any {
    return new native.StreamAnalyzer({ sampleRate: SAMPLE_RATE, nFft: 512, hopLength: 256 });
  }

  it('streaming/stream_analyzer.cpp SetExpectedDuration durationSeconds', () => {
    expect(() => analyzer().setExpectedDuration(30)).not.toThrow();
    // The core owns the non-negative half of the domain and reports it as a
    // SonareError, so the argument demonstrably arrives.
    expect(() => analyzer().setExpectedDuration(-1)).toThrow();
    expect(() => analyzer().setExpectedDuration(-1)).not.toThrow(RangeError);
    for (const value of [...PAST_FLOAT_RANGE, ...NON_FINITE]) {
      expect(() => analyzer().setExpectedDuration(value), `${value}`).toThrow(RangeError);
    }
  });
});

/**
 * The object keys and array elements that do not read through
 * `node_narrow_finite_float` at the site.
 *
 * `gain` is a real optional key, so it takes the object-key reader and keeps
 * its 1.0 default. The two array readers take the element reader, which also
 * closes what they used to do with a NON-numeric entry: one produced a dummy
 * float beside a pending JS exception, the other substituted 0.0f — an in-domain
 * absorption coefficient the caller never asked for.
 */
const KEY_AND_ELEMENT_ARGUMENTS: FloatArgument[] = [
  {
    name: 'Project addClip gain',
    site: 'project/edit.cpp AddClip gain',
    control: [1, 0.5],
    call: (v) => {
      /* biome-ignore lint/suspicious/noExplicitAny: the addon is untyped here on purpose. */
      const project = new native.Project() as any;
      try {
        const track = project.addTrack({ kind: 0 });
        project.addClip({ trackId: track, startPpq: 0, lengthPpq: 4, gain: v, audioChannels: 0 });
        return project.toJson();
      } finally {
        project.destroy?.();
      }
    },
  },
  {
    name: 'tempogramRatio factors',
    site: 'sonare_wrap_utils.h FloatVectorFromValue element',
    control: [2, 4],
    call: (v) => magnitudeSum(native.tempogramRatio(TEMPOGRAM, 32, SAMPLE_RATE, 512, [v])),
  },
  {
    name: 'synthesizeRir bandAbsorption',
    site: 'sonare_wrap_acoustic.cpp NodeFloatArrayOption element',
    control: [0.1, 0.9],
    call: (v) => magnitudeSum(rir({ bandAbsorption: [v, v, v, v] })),
  },
  {
    name: 'synthesizeRir bandScattering',
    site: 'sonare_wrap_acoustic.cpp NodeFloatArrayOption element',
    control: [0.1, 0.9],
    call: (v) => magnitudeSum(rir({ bandScattering: [v, v, v, v] })),
  },
];

describe('an object key and an array element are refused rather than saturated', () => {
  it.each(KEY_AND_ELEMENT_ARGUMENTS.map((argument) => [argument.name, argument] as const))(
    '%s',
    (_name, argument) => {
      const [low, high] = argument.control;
      expect(argument.call(low)).not.toBe(argument.call(high));
      for (const value of [...PAST_FLOAT_RANGE, ...NON_FINITE]) {
        expect(() => argument.call(value), `${argument.name} ${value}`).toThrow(RangeError);
      }
    },
  );

  it('refuses a non-numeric array entry by index instead of substituting a legal one', () => {
    expect(() => native.tempogramRatio(TEMPOGRAM, 32, SAMPLE_RATE, 512, ['2'])).toThrow(
      /factors\[0\]/,
    );
    expect(() => rir({ bandAbsorption: [0.1, null] })).toThrow(/bandAbsorption\[1\]/);
  });
});

/**
 * The one site whose argument has no in-domain control, driven by its own case
 * above rather than by a table. Counted here so the population stays exact.
 */
const UNCONTROLLED_SITES = ['streaming/stream_analyzer.cpp SetExpectedDuration durationSeconds'];

describe('the tables cover the whole required-read population', () => {
  it('keeps 1e39 the discriminating value it is documented to be', () => {
    // Finite as a caller wrote it, an infinity at float width: exactly what
    // FloatValue() does to it. An assertion written on Infinity instead would
    // be answered by the core at the two thirds of these sites that guard
    // finiteness themselves, and would prove nothing about the reader.
    for (const value of PAST_FLOAT_RANGE) {
      expect(Number.isFinite(value)).toBe(true);
      expect(Number.isFinite(Math.fround(value))).toBe(false);
    }
  });

  it('drives every node_narrow_finite_float call site in the addon', () => {
    const calls = addonSources()
      // The shared header holds the reader itself plus the three members built
      // on it, none of which is a site.
      .filter((source) => source.file !== 'sonare_wrap_options.h')
      .flatMap((source) => source.text.match(/node_narrow_finite_float\(/g) ?? []);
    const sites = new Set([
      ...REQUIRED_FLOAT_ARGUMENTS.map((argument) => argument.site),
      ...REQUIRED_FLOAT_STATE_ARGUMENTS.map((argument) => argument.site),
      ...UNCONTROLLED_SITES,
    ]);
    expect(sites.size).toBe(calls.length);
  });

  it('drives every node_float_as_c_abi call site in the addon', () => {
    const calls = addonSources()
      .filter((source) => source.file !== 'sonare_wrap_options.h')
      .flatMap((source) => source.text.match(/node_float_as_c_abi\(/g) ?? []);
    const sites = new Set(C_ABI_FLOAT_ARGUMENTS.map((argument) => argument.site));
    expect(sites.size).toBe(calls.length);
  });

  it('drives every node_narrow_finite_float_element call site in the addon', () => {
    const calls = addonSources()
      .filter((source) => source.file !== 'sonare_wrap_options.h')
      .flatMap((source) => source.text.match(/node_narrow_finite_float_element\(/g) ?? []);
    const sites = new Set(
      KEY_AND_ELEMENT_ARGUMENTS.filter((argument) => argument.site.endsWith(' element')).map(
        (argument) => argument.site,
      ),
    );
    expect(sites.size).toBe(calls.length);
  });
});

/**
 * The five unit conversions, which keep the C ABI's own conversion.
 *
 * These are total functions: `src/core/convert.cpp:67` answers a non-finite `hz`
 * with `"?"` — the same answer it gives a non-positive frequency or one past the
 * representable MIDI range — and the librosa mirrors propagate a NaN the way the
 * reference does. The C ABI (`features_spectral_pitch.cpp`) and the WASM facade
 * both pass the value straight through, so a refusal here would put this surface
 * alone out of step with the oracle rather than closing a divergence.
 *
 * `1e39` is asserted alongside the non-finite spellings and must NOT throw: it
 * saturates to an infinity exactly as the C conversion does.
 */
interface CAbiFloatArgument {
  name: string;
  /** The reader call site, so the table's coverage of the population is countable. */
  site: string;
  /** Two legitimate values whose results must differ. */
  control: [number, number];
  call: (value: number) => unknown;
  /** What each spelling propagates to, measured against the C conversion. */
  propagates: Array<[number, unknown]>;
}

const SATURATES = [
  [1e39, Number.POSITIVE_INFINITY],
  [-1e39, Number.NEGATIVE_INFINITY],
  [Number.POSITIVE_INFINITY, Number.POSITIVE_INFINITY],
  [Number.NEGATIVE_INFINITY, Number.NEGATIVE_INFINITY],
  [Number.NaN, Number.NaN],
] as Array<[number, unknown]>;

const C_ABI_FLOAT_ARGUMENTS: CAbiFloatArgument[] = [
  {
    name: 'hzToMel hz',
    site: 'sonare_wrap_features.cpp HzToMel hz',
    control: [440, 880],
    call: (v) => native.hzToMel(v),
    propagates: SATURATES,
  },
  {
    name: 'melToHz mel',
    site: 'sonare_wrap_features.cpp MelToHz mel',
    control: [1000, 2000],
    call: (v) => native.melToHz(v),
    propagates: SATURATES,
  },
  {
    name: 'hzToMidi hz',
    site: 'sonare_wrap_features.cpp HzToMidi hz',
    control: [440, 880],
    call: (v) => native.hzToMidi(v),
    propagates: SATURATES,
  },
  {
    name: 'midiToHz midi',
    site: 'sonare_wrap_features.cpp MidiToHz midi',
    control: [69, 81],
    call: (v) => native.midiToHz(v),
    // Not SATURATES: 2**-Infinity is 0, so this one bottoms out rather than
    // carrying the sign through.
    propagates: [
      [1e39, Number.POSITIVE_INFINITY],
      [-1e39, 0],
      [Number.POSITIVE_INFINITY, Number.POSITIVE_INFINITY],
      [Number.NEGATIVE_INFINITY, 0],
      [Number.NaN, Number.NaN],
    ],
  },
  {
    name: 'hzToNote hz',
    site: 'sonare_wrap_features.cpp HzToNote hz',
    control: [440, 880],
    call: (v) => native.hzToNote(v),
    // The core's documented answer for every frequency with no note, non-finite
    // among them; `tests/core/convert_test.cpp` pins it on the C++ side.
    propagates: SATURATES.map(([input]) => [input, '?']),
  },
];

describe('a unit conversion keeps the C ABI conversion rather than refusing', () => {
  it.each(C_ABI_FLOAT_ARGUMENTS.map((argument) => [argument.name, argument] as const))(
    '%s',
    (_name, argument) => {
      const [low, high] = argument.control;
      expect(argument.call(low)).not.toBe(argument.call(high));
      for (const [input, expected] of argument.propagates) {
        expect(argument.call(input), `${argument.name} ${input}`).toBe(expected);
      }
      // Permissive about the VALUE, not about the type: the entry point's own
      // gate still refuses a non-number by name.
      expect(() => argument.call('440' as unknown as number)).toThrow(TypeError);
    },
  );
});

/**
 * The library's own default pipeline feeds a NaN into one of those conversions.
 *
 * `pyin`'s `fillNa` defaults to false, so an unvoiced frame comes back as NaN
 * (`src/feature/pitch.cpp:661-667`). Mapping that track through `hzToNote` is
 * the canonical librosa-shaped use, and a finite-only reader on the conversion
 * would break it. The NaN count is asserted first: without one present the rest
 * of this case would pass against a track that never exercised it.
 */
describe('the default pyin track still maps through hzToNote', () => {
  it('produces NaN frames and converts them without throwing', () => {
    const voicedThenSilent = new Float32Array(Math.round(SAMPLE_RATE * 0.25));
    for (let i = 0; i < voicedThenSilent.length / 2; i++) {
      voicedThenSilent[i] = 0.4 * Math.sin((2 * Math.PI * 220 * i) / SAMPLE_RATE);
    }
    const { f0 } = native.pitchPyin(voicedThenSilent, SAMPLE_RATE, 2048, 512, 65, 2093, 0.1, false);
    const track = Array.from(f0 as Float32Array);
    expect(track.some(Number.isNaN)).toBe(true);
    expect(track.some((hz) => Number.isFinite(hz) && hz > 0)).toBe(true);

    const notes = track.map((hz) => native.hzToNote(hz));
    expect(notes).toHaveLength(track.length);
    expect(notes.filter((note) => note === '?').length).toBe(track.filter(Number.isNaN).length);
  });
});
