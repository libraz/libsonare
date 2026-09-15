/**
 * An optional float argument is refused, not saturated into a legal quantity.
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
  for (let i = 0; i < out.length; i++) out[i] = Math.abs(Math.sin(i * 0.7)) + 0.01;
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

function magnitudeSum(values: Iterable<number>): number {
  let total = 0;
  for (const value of values) total += Math.abs(value);
  return total;
}

function peak(values: Iterable<number>): number {
  let highest = 0;
  for (const value of values) highest = Math.max(highest, Math.abs(value));
  return highest;
}

function newMixer(): unknown {
  return new native.Mixer(mixingScenePresetJson('commentaryDucking'), 48000, 8);
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
    call: (v) => magnitudeSum(native.spectralContrast(AUDIO, SAMPLE_RATE, 512, 256, 4, v, 0.02).data),
  },
  {
    name: 'spectralContrast quantile',
    site: 'features/advanced.cpp SpectralContrast quantile',
    control: [0.02, 0.4],
    call: (v) => magnitudeSum(native.spectralContrast(AUDIO, SAMPLE_RATE, 512, 256, 4, 200, v).data),
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
    call: (v) => magnitudeSum(native.vectorNormalize(Float32Array.from([0.001, 0.002, 3, 4]), 2, v)),
  },
  {
    name: 'meteringSilenceRatio thresholdDb',
    site: 'analysis/metering.cpp MeteringSilenceRatio thresholdDb',
    control: [-45, -5],
    call: (v) => native.meteringSilenceRatio(GATED, SAMPLE_RATE, v, 1024, 256),
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
    site: 'effects/dynamics_repair.cpp Trim thresholdDb',
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
