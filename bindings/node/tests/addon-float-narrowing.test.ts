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
/** Too short for the tempo search to hold one lag, so no tempo is measured. */
const SHORT_TONE = Float32Array.from({ length: 256 }, (_, i) => 0.5 * Math.sin(0.3 * i));

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
const CC_BINDING = {
  ccNumber: 74,
  channel: 4,
  kind: 'controlChange7',
  paramId: 88,
  minValue: 0,
  maxValue: 1,
};

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
  return new native.Mixer(addon.mixingScenePresetJson('commentaryDucking'), 48000, 8);
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
  /**
   * True when the argument seeds a search whose seed no reported field reflects, so no two values
   * give different results; the refusals are still driven.
   */
  controlUnobservable?: boolean;
  call: (value: number) => unknown;
}

/** Four chords, eight seconds, so a chord-detection bound has segments to merge or drop. */
const CHORDS = (() => {
  const roots = [
    [261.6, 329.6, 392],
    [220, 261.6, 329.6],
    [174.6, 220, 261.6],
    [196, 246.9, 293.7],
  ];
  const out = new Float32Array(SAMPLE_RATE * 8);
  for (let i = 0; i < out.length; i++) {
    let sample = 0;
    for (const hz of roots[Math.floor(i / (SAMPLE_RATE * 0.5)) % 4]) {
      sample += 0.2 * Math.sin((2 * Math.PI * hz * i) / SAMPLE_RATE);
    }
    out[i] = sample;
  }
  return out;
})();

/**
 * `detectChords` with the options the three float bounds act on: beat sync off,
 * because the beat-synchronous path never reads the smoothing window or the
 * threshold. Everything past the bounds is the entry point's own default.
 */
function detectChordsWith(minDuration: number, smoothing: number, threshold: number): unknown {
  return JSON.stringify(
    native.detectChords(
      CHORDS,
      SAMPLE_RATE,
      minDuration,
      smoothing,
      threshold,
      false,
      2048,
      512,
      false,
      false,
      ...Array(6).fill(undefined),
      0,
      false,
    ),
  );
}

/** `chordFunctionalAnalysis`, positioned like `detectChordsWith`. */
function functionalChordsWith(minDuration: number, smoothing: number, threshold: number): unknown {
  return JSON.stringify(
    native.chordFunctionalAnalysis(
      CHORDS,
      undefined,
      undefined,
      SAMPLE_RATE,
      minDuration,
      smoothing,
      threshold,
      false,
      2048,
      512,
      false,
      false,
      ...Array(4).fill(undefined),
      0,
      false,
    ),
  );
}

/** The pulsed fixture as an Audio handle, for the instance tempo methods. */
const PULSED_AUDIO = native.Audio.fromBuffer(PULSED, SAMPLE_RATE);

/** Two click times, so a click's frequency and duration both reach the rendered samples. */
const CLICK_TIMES = Float32Array.from([0.05, 0.2]);

/** A MIDI 1.0 CC pair on controller 1 and its fine byte, which a learn run turns into a binding. */
const CC_LEARN_EVENTS = [
  { ppq: 0, word0: 0x40b20140 | 0 },
  { ppq: 0.1, word0: 0x40b22100 },
];

const KNOWN_AMPLITUDES = Float32Array.from([1, 0.5, 0.01, 1e-5]);
const KNOWN_DECIBELS = Float32Array.from([0, -10]);

/** A project holding one track and one clip, read back through its serialised form. */
function projectAfter(
  /* biome-ignore lint/suspicious/noExplicitAny: the addon is untyped here on purpose. */
  act: (project: any, trackId: number, clipId: number) => void,
): string {
  /* biome-ignore lint/suspicious/noExplicitAny: the addon is untyped here on purpose. */
  const project = new native.Project() as any;
  try {
    const trackId = project.addTrack({ kind: 0, name: 'lead' });
    const clipId = project.addClip({
      trackId,
      startPpq: 0,
      lengthPpq: 4,
      gain: 1,
      audio: new Float32Array(480),
      audioChannels: 1,
      audioSampleRate: 48000,
    });
    act(project, trackId, clipId);
    return project.toJson();
  } finally {
    project.destroy?.();
  }
}

/** The output of a monitored input block at the given monitor gain. */
function inputMonitorOutput(gain: number): number {
  /* biome-ignore lint/suspicious/noExplicitAny: the addon is untyped here on purpose. */
  const engine = new native.RealtimeEngine(48000, 128) as any;
  try {
    engine.setInputMonitor(true, gain);
    engine.play();
    const block = new Float32Array(128).fill(0.25);
    return magnitudeSum(engine.process([block, new Float32Array(block)])[0]);
  } finally {
    engine.destroy();
  }
}

/**
 * Every argument routed through `OptionalFloatArg`.
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
    call: (v) => native.analyzeSections(BLOCKED, SAMPLE_RATE, 2048, 512, v).sections.length,
  },
  {
    name: 'analyzeSections threshold',
    site: 'analysis/progress.cpp AnalyzeSections threshold',
    control: [0, 5],
    call: (v) => JSON.stringify(native.analyzeSections(BLOCKED, SAMPLE_RATE, 2048, 512, 0.5, v)),
  },
  {
    name: 'analyzeMelody fmax',
    site: 'analysis/progress.cpp fmax',
    control: [2093, 300],
    call: (v) => native.analyzeMelody(BLOCKED, SAMPLE_RATE, 65, v, 2048, 256, 0.1).meanFrequency,
  },
  {
    name: 'vqt fmin',
    site: 'features/advanced.cpp Vqt fmin',
    control: [32.7, 130.81],
    call: (v) => native.vqt(AUDIO, SAMPLE_RATE, 512, v, 24, 12).frequencies[0],
  },
  {
    name: 'vqt filterScale',
    site: 'features/advanced.cpp Vqt filterScale',
    control: [1, 0.5],
    call: (v) => magnitudeSum(native.vqt(AUDIO, SAMPLE_RATE, 512, 32.7, 24, 12, v).magnitude),
  },
  {
    name: 'cqt filterScale',
    site: 'features/advanced.cpp CqtLike filterScale',
    control: [1, 0.5],
    call: (v) => magnitudeSum(native.cqt(AUDIO, SAMPLE_RATE, 512, 32.7, 24, 12, v).magnitude),
  },
  {
    name: 'pseudoCqt filterScale',
    site: 'features/advanced.cpp CqtLike filterScale',
    control: [1, 0.5],
    call: (v) => magnitudeSum(native.pseudoCqt(AUDIO, SAMPLE_RATE, 512, 32.7, 24, 12, v).magnitude),
  },
  {
    name: 'hybridCqt filterScale',
    site: 'features/advanced.cpp CqtLike filterScale',
    control: [1, 0.5],
    call: (v) => magnitudeSum(native.hybridCqt(AUDIO, SAMPLE_RATE, 512, 32.7, 24, 12, v).magnitude),
  },
  {
    name: 'biquadFilter cutoffHz',
    site: 'sonare_wrap_effects.cpp BiquadFilter cutoffHz',
    control: [200, 2000],
    call: (v) => magnitudeSum(native.biquadFilter(AUDIO, SAMPLE_RATE, 0, 2, false, v)),
  },
  {
    name: 'biquadFilter centerHz',
    site: 'sonare_wrap_effects.cpp BiquadFilter centerHz',
    control: [440, 2000],
    call: (v) =>
      magnitudeSum(native.biquadFilter(AUDIO, SAMPLE_RATE, 2, 2, false, undefined, v, 200)),
  },
  {
    name: 'biquadFilter bandwidthHz',
    site: 'sonare_wrap_effects.cpp BiquadFilter bandwidthHz',
    control: [50, 2000],
    call: (v) =>
      magnitudeSum(native.biquadFilter(AUDIO, SAMPLE_RATE, 2, 2, false, undefined, 440, v)),
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
        native.vqtToAudio(
          CQT.magnitude,
          CQT.nBins,
          CQT.nFrames,
          SAMPLE_RATE,
          512,
          v,
          12,
          undefined,
          4,
        ),
      ),
  },
  {
    name: 'melToStft fmin',
    site: 'features/advanced.cpp MelToStft fmin',
    control: [0, 300],
    call: (v) =>
      magnitudeSum(
        native.melToStft(MEL.power, 32, MEL.nFrames, SAMPLE_RATE, 1024, v, 0, false).magnitude,
      ),
  },
  {
    name: 'melToStft fmax',
    site: 'features/advanced.cpp MelToStft fmax',
    control: [0, 4000],
    call: (v) =>
      magnitudeSum(
        native.melToStft(MEL.power, 32, MEL.nFrames, SAMPLE_RATE, 1024, 0, v, false).magnitude,
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
    name: 'tuningToReferenceHz a4',
    site: 'features/advanced.cpp TuningToReferenceHz a4',
    control: [440, 432],
    call: (v) => native.tuningToReferenceHz(0.25, v),
  },
  {
    name: 'referenceHzToTuning a4',
    site: 'features/advanced.cpp ReferenceHzToTuning a4',
    control: [440, 432],
    call: (v) => native.referenceHzToTuning(446, v),
  },
  {
    name: 'splitSilenceCommon topDb',
    site: 'features/signal.cpp SplitSilenceCommon topDb',
    control: [60, 20],
    call: (v) => native.splitSilenceCommon([GATED], v, 2048, 512).length,
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
    name: 'plp bpmMin',
    site: 'features/rhythm.cpp Plp bpmMin',
    control: [30, 120],
    call: (v) => magnitudeSum(native.plp(PULSED, SAMPLE_RATE, 512, v, 300, 384)),
  },
  {
    name: 'plp bpmMax',
    site: 'features/rhythm.cpp Plp bpmMax',
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
    name: 'mastering ceilingDbtp',
    site: 'effects/mastering.cpp Mastering ceilingDbtp',
    control: [-1, -12],
    call: (v) => peak(native.mastering(AUDIO, SAMPLE_RATE, -14, v).samples),
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
    site: 'features/signal.cpp Preemphasis zi',
    control: [0, 1],
    call: (v) => native.preemphasis(Float32Array.from([1, 2, 3, 4]), 0.97, v)[0],
  },
  {
    name: 'deemphasis zi',
    site: 'features/signal.cpp Deemphasis zi',
    control: [0, 1],
    call: (v) => native.deemphasis(Float32Array.from([1, 2, 3, 4]), 0.97, v)[0],
  },
  {
    name: 'Audio.detectBpm bpmMin',
    site: 'sonare_wrap.cpp DetectBpmInstance bpmMin',
    control: [60, 130],
    call: (v) => PULSED_AUDIO.detectBpm(v, 200),
  },
  {
    name: 'Audio.detectBpm bpmMax',
    site: 'sonare_wrap.cpp DetectBpmInstance bpmMax',
    control: [200, 100],
    call: (v) => PULSED_AUDIO.detectBpm(60, v),
  },
  {
    name: 'Audio.detectBeats bpmMin',
    site: 'sonare_wrap.cpp DetectBeatsInstance bpmMin',
    control: [60, 130],
    call: (v) => JSON.stringify(PULSED_AUDIO.detectBeats(v, 200)),
  },
  {
    name: 'Audio.detectBeats bpmMax',
    site: 'sonare_wrap.cpp DetectBeatsInstance bpmMax',
    control: [200, 100],
    call: (v) => JSON.stringify(PULSED_AUDIO.detectBeats(60, v)),
  },
  {
    name: 'detectBpm bpmMin',
    site: 'sonare_wrap_analysis.cpp DetectBpm bpmMin',
    control: [60, 130],
    call: (v) => native.detectBpm(PULSED, SAMPLE_RATE, v, 200),
  },
  {
    name: 'detectBpm bpmMax',
    site: 'sonare_wrap_analysis.cpp DetectBpm bpmMax',
    control: [200, 100],
    call: (v) => native.detectBpm(PULSED, SAMPLE_RATE, 60, v),
  },
  {
    name: 'detectBeats bpmMin',
    site: 'sonare_wrap_analysis.cpp DetectBeats bpmMin',
    control: [60, 130],
    call: (v) => JSON.stringify(native.detectBeats(PULSED, SAMPLE_RATE, v, 200)),
  },
  {
    name: 'detectBeats bpmMax',
    site: 'sonare_wrap_analysis.cpp DetectBeats bpmMax',
    control: [200, 100],
    call: (v) => JSON.stringify(native.detectBeats(PULSED, SAMPLE_RATE, 60, v)),
  },
  {
    name: 'analyzeBpm bpmMin',
    site: 'sonare_wrap_analysis.cpp AnalyzeBpm bpmMin',
    control: [60, 130],
    call: (v) => JSON.stringify(native.analyzeBpm(PULSED, SAMPLE_RATE, v, 200, 120, 2048, 512, 3)),
  },
  {
    name: 'analyzeBpm bpmMax',
    site: 'sonare_wrap_analysis.cpp AnalyzeBpm bpmMax',
    control: [200, 100],
    call: (v) => JSON.stringify(native.analyzeBpm(PULSED, SAMPLE_RATE, 60, v, 120, 2048, 512, 3)),
  },
  {
    name: 'analyzeBpm startBpm',
    site: 'sonare_wrap_analysis.cpp AnalyzeBpm startBpm',
    // The seed only steers the beat tracker; a tempo it cannot back is reported as not measured.
    control: [90, 150],
    controlUnobservable: true,
    call: (v) =>
      JSON.stringify(native.analyzeBpm(SHORT_TONE, SAMPLE_RATE, 60, 200, v, 2048, 512, 3)),
  },
  {
    name: 'analyzeRhythm startBpm',
    site: 'sonare_wrap_analysis.cpp AnalyzeRhythm startBpm',
    control: [90, 150],
    controlUnobservable: true,
    call: (v) =>
      JSON.stringify(native.analyzeRhythm(SHORT_TONE, SAMPLE_RATE, 60, 200, v, 2048, 512)),
  },
  {
    name: 'analyzeRhythm bpmMin',
    site: 'sonare_wrap_analysis.cpp AnalyzeRhythm bpmMin',
    control: [60, 130],
    call: (v) => JSON.stringify(native.analyzeRhythm(PULSED, SAMPLE_RATE, v, 200, 120, 2048, 512)),
  },
  {
    name: 'analyzeRhythm bpmMax',
    site: 'sonare_wrap_analysis.cpp AnalyzeRhythm bpmMax',
    control: [200, 100],
    call: (v) => JSON.stringify(native.analyzeRhythm(PULSED, SAMPLE_RATE, 60, v, 120, 2048, 512)),
  },
  {
    name: 'detectChords minDuration',
    site: 'sonare_wrap_harmony.cpp DetectChords minDuration',
    control: [0, 3],
    call: (v) => detectChordsWith(v, 2, 0.5),
  },
  {
    name: 'detectChords smoothingWindow',
    site: 'sonare_wrap_harmony.cpp DetectChords smoothingWindow',
    control: [0, 8],
    call: (v) => detectChordsWith(0.3, v, 0.5),
  },
  {
    name: 'detectChords threshold',
    site: 'sonare_wrap_harmony.cpp DetectChords threshold',
    control: [0, 0.99],
    call: (v) => detectChordsWith(0.3, 2, v),
  },
  {
    name: 'chordFunctionalAnalysis minDuration',
    site: 'sonare_wrap_harmony.cpp FunctionalAnalysis minDuration',
    control: [0, 3],
    call: (v) => functionalChordsWith(v, 2, 0.5),
  },
  {
    name: 'chordFunctionalAnalysis smoothingWindow',
    site: 'sonare_wrap_harmony.cpp FunctionalAnalysis smoothingWindow',
    control: [0, 8],
    call: (v) => functionalChordsWith(0.3, v, 0.5),
  },
  {
    name: 'chordFunctionalAnalysis threshold',
    site: 'sonare_wrap_harmony.cpp FunctionalAnalysis threshold',
    control: [0, 0.99],
    call: (v) => functionalChordsWith(0.3, 2, v),
  },
  {
    name: 'fade fadeInSec',
    site: 'sonare_wrap_effects.cpp Fade fadeInSec',
    control: [0, 0.5],
    call: (v) => magnitudeSum(native.fade(AUDIO, SAMPLE_RATE, v, 0)),
  },
  {
    name: 'fade fadeOutSec',
    site: 'sonare_wrap_effects.cpp Fade fadeOutSec',
    control: [0, 0.5],
    call: (v) => magnitudeSum(native.fade(AUDIO, SAMPLE_RATE, 0, v)),
  },
  {
    name: 'normalizeStereo targetDb',
    site: 'sonare_wrap_effects.cpp NormalizeStereo targetDb',
    control: [0, -6],
    call: (v) => JSON.stringify(native.normalizeStereo(AUDIO, AUDIO, SAMPLE_RATE, v)),
  },
  {
    name: 'melSpectrogram fmin',
    site: 'sonare_wrap_features.cpp MelSpectrogramFn fmin',
    control: [0, 300],
    call: (v) =>
      JSON.stringify(native.melSpectrogram(AUDIO, SAMPLE_RATE, 1024, 256, 32, v, 0, false)),
  },
  {
    name: 'melSpectrogram fmax',
    site: 'sonare_wrap_features.cpp MelSpectrogramFn fmax',
    control: [0, 4000],
    call: (v) =>
      JSON.stringify(native.melSpectrogram(AUDIO, SAMPLE_RATE, 1024, 256, 32, 0, v, false)),
  },
  {
    name: 'mfcc fmin',
    site: 'sonare_wrap_features.cpp Mfcc fmin',
    control: [0, 300],
    call: (v) => JSON.stringify(native.mfcc(AUDIO, SAMPLE_RATE, 512, 256, 32, 13, v)),
  },
  {
    name: 'mfcc fmax',
    site: 'sonare_wrap_features.cpp Mfcc fmax',
    control: [0, 2000],
    call: (v) => JSON.stringify(native.mfcc(AUDIO, SAMPLE_RATE, 512, 256, 32, 13, 0, v)),
  },
  {
    name: 'mfcc lifter',
    site: 'sonare_wrap_features.cpp Mfcc lifter',
    control: [0, 22],
    call: (v) => JSON.stringify(native.mfcc(AUDIO, SAMPLE_RATE, 512, 256, 32, 13, 0, 0, false, v)),
  },
  {
    name: 'piptrack fminHz',
    site: 'sonare_wrap_features.cpp Piptrack fminHz',
    control: [150, 1000],
    call: (v) => JSON.stringify(native.piptrack(AUDIO, SAMPLE_RATE, 1024, 256, v, 4000, 0.1)),
  },
  {
    name: 'piptrack fmaxHz',
    site: 'sonare_wrap_features.cpp Piptrack fmaxHz',
    control: [4000, 500],
    call: (v) => JSON.stringify(native.piptrack(AUDIO, SAMPLE_RATE, 1024, 256, 150, v, 0.1)),
  },
  {
    name: 'piptrack threshold',
    site: 'sonare_wrap_features.cpp Piptrack threshold',
    control: [0.1, 0.9],
    call: (v) => JSON.stringify(native.piptrack(AUDIO, SAMPLE_RATE, 1024, 256, 150, 4000, v)),
  },
  {
    name: 'reassignedSpectrogram refPower',
    site: 'sonare_wrap_features.cpp ReassignedSpectrogram refPower',
    control: [1e-6, 1],
    call: (v) => JSON.stringify(native.reassignedSpectrogram(AUDIO, SAMPLE_RATE, 1024, 256, v)),
  },
  {
    name: 'pitchYin f0MinHz',
    site: 'sonare_wrap_features.cpp PitchYin f0MinHz',
    control: [65, 500],
    call: (v) => JSON.stringify(native.pitchYin(AUDIO, SAMPLE_RATE, 2048, 512, v, 2093, 0.1)),
  },
  {
    name: 'pitchYin f0MaxHz',
    site: 'sonare_wrap_features.cpp PitchYin f0MaxHz',
    control: [2093, 150],
    call: (v) => JSON.stringify(native.pitchYin(AUDIO, SAMPLE_RATE, 2048, 512, 65, v, 0.1)),
  },
  {
    name: 'pitchYin threshold',
    site: 'sonare_wrap_features.cpp PitchYin threshold',
    control: [0.1, 0.9],
    call: (v) => JSON.stringify(native.pitchYin(AUDIO, SAMPLE_RATE, 2048, 512, 65, 2093, v)),
  },
  {
    name: 'pitchPyin f0MinHz',
    site: 'sonare_wrap_features.cpp PitchPyin f0MinHz',
    control: [65, 500],
    call: (v) =>
      JSON.stringify(native.pitchPyin(AUDIO, SAMPLE_RATE, 2048, 512, v, 2093, 0.1, false)),
  },
  {
    name: 'pitchPyin f0MaxHz',
    site: 'sonare_wrap_features.cpp PitchPyin f0MaxHz',
    control: [2093, 150],
    call: (v) => JSON.stringify(native.pitchPyin(AUDIO, SAMPLE_RATE, 2048, 512, 65, v, 0.1, false)),
  },
  {
    name: 'pitchPyin threshold',
    site: 'sonare_wrap_features.cpp PitchPyin threshold',
    control: [0.1, 0.9],
    call: (v) =>
      JSON.stringify(native.pitchPyin(AUDIO, SAMPLE_RATE, 2048, 512, 65, 2093, v, false)),
  },
  {
    name: 'tone duration',
    site: 'features/signal.cpp Tone duration',
    control: [0.1, 0.5],
    call: (v) => native.tone(440, SAMPLE_RATE, v).length,
  },
  {
    name: 'tone phase',
    site: 'features/signal.cpp Tone phase',
    control: [0, 1.5],
    call: (v) => JSON.stringify(native.tone(440, SAMPLE_RATE, 0.1, v).slice(0, 8)),
  },
  {
    name: 'tone amplitude',
    site: 'features/signal.cpp Tone amplitude',
    control: [1, 0.5],
    call: (v) => magnitudeSum(native.tone(440, SAMPLE_RATE, 0.1, 0, v)),
  },
  {
    name: 'chirp duration',
    site: 'features/signal.cpp Chirp duration',
    control: [0.1, 0.5],
    call: (v) => native.chirp(100, 1000, SAMPLE_RATE, v, true).length,
  },
  {
    name: 'clicks frequency',
    site: 'features/signal.cpp Clicks frequency',
    control: [1000, 3000],
    call: (v) => magnitudeSum(native.clicks(CLICK_TIMES, SAMPLE_RATE, SAMPLE_RATE, v, 0.05)),
  },
  {
    name: 'clicks duration',
    site: 'features/signal.cpp Clicks duration',
    control: [0.01, 0.05],
    call: (v) => magnitudeSum(native.clicks(CLICK_TIMES, SAMPLE_RATE, SAMPLE_RATE, 1000, v)),
  },
  {
    name: 'dbToPower ref',
    site: 'features/conversion.cpp DbToPower ref',
    control: [1, 2],
    call: (v) => JSON.stringify(native.dbToPower(KNOWN_DECIBELS, v)),
  },
  {
    name: 'dbToAmplitude ref',
    site: 'features/conversion.cpp DbToAmplitude ref',
    control: [1, 2],
    call: (v) => JSON.stringify(native.dbToAmplitude(KNOWN_DECIBELS, v)),
  },
  {
    name: 'midiCcLearn minValue',
    site: 'addon.cpp MidiCcLearn minValue',
    control: [0, 0.5],
    call: (v) => JSON.stringify(native.midiCcLearn(CC_LEARN_EVENTS, 77, v, 1)),
  },
  {
    name: 'midiCcLearn maxValue',
    site: 'addon.cpp MidiCcLearn maxValue',
    control: [1, 0.5],
    call: (v) => JSON.stringify(native.midiCcLearn(CC_LEARN_EVENTS, 77, 0, v)),
  },
  {
    name: 'Project.setTrackGain gain',
    site: 'project/edit.cpp SetTrackGain gain',
    control: [1, 0.5],
    call: (v) => projectAfter((project, trackId) => project.setTrackGain(trackId, v)),
  },
  {
    name: 'Project.setTrackPan pan',
    site: 'project/edit.cpp SetTrackPan pan',
    control: [0, 0.5],
    call: (v) => projectAfter((project, trackId) => project.setTrackPan(trackId, v)),
  },
  {
    name: 'Project.setClipGain gain',
    site: 'project/edit.cpp SetClipGain gain',
    control: [1, 0.5],
    call: (v) => projectAfter((project, _trackId, clipId) => project.setClipGain(clipId, v)),
  },
  {
    name: 'RealtimeEngine.setInputMonitor gain',
    site: 'engine/clips_capture.cpp SetInputMonitor gain',
    control: [1, 0.5],
    call: (v) => inputMonitorOutput(v),
  },
];

describe('an optional float argument is refused rather than saturated', () => {
  it.each(FINITE_ARGUMENTS.map((argument) => [argument.name, argument] as const))(
    '%s',
    (_name, argument) => {
      const [low, high] = argument.control;
      // Positive control: the argument is consumed, so an entry point that
      // ignored it could not reach the refusals below looking like this one.
      if (!argument.controlUnobservable) {
        expect(argument.call(low)).not.toBe(argument.call(high));
      }
      for (const value of [...PAST_FLOAT_RANGE, ...NON_FINITE]) {
        expect(() => argument.call(value), `${argument.name} ${value}`).toThrow(RangeError);
      }
    },
  );
});

/**
 * The two gammas, read against the core's gamma domain.
 *
 * Absent is the C ABI's NaN, the librosa-compatible ERB-derived automatic value,
 * so a written NaN is a wrong value rather than a second spelling of absent.
 */
const GAMMA_ARGUMENTS: FloatArgument[] = [
  {
    name: 'vqt gamma',
    site: 'features/advanced.cpp Vqt gamma',
    control: [0, 5],
    call: (v) =>
      magnitudeSum(native.vqt(AUDIO, SAMPLE_RATE, 512, 32.7, 24, 12, undefined, v).magnitude),
  },
  {
    name: 'vqtToAudio gamma',
    site: 'features/advanced.cpp VqtToAudio gamma',
    control: [0, 5],
    call: (v) =>
      magnitudeSum(
        native.vqtToAudio(CQT.magnitude, CQT.nBins, CQT.nFrames, SAMPLE_RATE, 512, 32.7, 12, v, 4),
      ),
  },
];

/** The automatic gamma the transform resolved, as `vqt` reports it. */
const AUTOMATIC_GAMMA: number = native.vqt(AUDIO, SAMPLE_RATE, 512, 32.7, 24, 12).gamma;

describe('a gamma is given or absent, and absent is the automatic ERB value', () => {
  it.each(GAMMA_ARGUMENTS.map((argument) => [argument.name, argument] as const))(
    '%s',
    (_name, argument) => {
      const [low, high] = argument.control;
      expect(argument.call(low)).not.toBe(argument.call(high));
      expect(AUTOMATIC_GAMMA).toBeGreaterThan(0);
      expect(argument.call(undefined as unknown as number)).toBe(argument.call(AUTOMATIC_GAMMA));
      for (const value of [...PAST_FLOAT_RANGE, ...NON_FINITE, -1]) {
        expect(() => argument.call(value), `${argument.name} ${value}`).toThrow(RangeError);
        expect(() => argument.call(value), `${argument.name} ${value}`).toThrow(/gamma must be/);
      }
    },
  );
});

/**
 * The dB conversions' `topDb`, read against the core's [0, +inf] domain.
 *
 * +inf is the written spelling of no clipping, and a finite value past `FLT_MAX` saturates to it,
 * which clips nothing either: `max - 1e39` lies under every level the data can hold.
 */
describe('a dB conversion topDb meets the core domain, where Infinity means no clipping', () => {
  const CONVERSIONS = [
    ['powerToDb', (v: number) => JSON.stringify(native.powerToDb(KNOWN_AMPLITUDES, 1, 1e-12, v))],
    [
      'amplitudeToDb',
      (v: number) => JSON.stringify(native.amplitudeToDb(KNOWN_AMPLITUDES, 1, 1e-6, v)),
    ],
  ] as const;

  it.each(CONVERSIONS)('%s topDb', (name, call) => {
    expect(call(10)).not.toBe(call(80));
    const unclipped = call(Number.POSITIVE_INFINITY);
    expect(call(1e39)).toBe(unclipped);
    expect(call(1000)).toBe(unclipped);
    for (const value of [-1e39, Number.NEGATIVE_INFINITY, Number.NaN, -1]) {
      expect(() => call(value), `${name} topDb ${value}`).toThrow(RangeError);
      expect(() => call(value), `${name} topDb ${value}`).toThrow(/topDb must be at least 0/);
    }
  });
});

describe('the table covers the whole reader population', () => {
  it('drives every OptionalFloatArg call site in the addon', () => {
    const calls = addonSources()
      .filter((source) => source.file !== 'sonare_wrap_options.h')
      .flatMap(
        (source) => source.text.match(/OptionalFloatArg\((?:env|info\.Env\(\)), info,/g) ?? [],
      );
    const sites = new Set(FINITE_ARGUMENTS.map((argument) => argument.site));
    expect(sites.size).toBe(calls.length);
  });

  it('reads both gammas against the gamma domain, outside that population', () => {
    const gammas = addonSources()
      .filter((source) => source.file === 'features/advanced.cpp')
      .flatMap((source) => source.text.match(/OptionalDomainArg\(env, info, \d+, "gamma"/g) ?? []);
    expect(gammas).toHaveLength(GAMMA_ARGUMENTS.length);
  });
});

/**
 * The required arguments and object keys, which read through
 * `node_narrow_float` at the site rather than through an argument helper.
 *
 * These have no fallback: each sits after an explicit arity-and-type gate that
 * has already refused a missing or wrong-typed argument, so a helper carrying a
 * fallback value would write a default into the source that can never be
 * selected.
 *
 * Most of this population is already refused somewhere downstream — the mixer's
 * C ABI guards `finite()` on every setter, the streaming analyzer throws
 * a refusal, `sonare_engine_set_automation_lane` rejects a non-finite
 * breakpoint. For those the repair is the error wording and the name, not the
 * outcome. It is exactly why the discriminating value has to be the finite
 * `1e39`: an `Infinity` would be refused by the core with no reader in front of
 * it, and the assertion would pass against unrepaired code.
 */
const REQUIRED_FLOAT_ARGUMENTS: FloatArgument[] = [
  {
    name: 'timeToFrames time',
    site: 'features/conversion.cpp TimeToFrames time',
    control: [1, 2],
    call: (v) => native.timeToFrames(v, SAMPLE_RATE, 512),
  },
  {
    name: 'synthesizeRir source x',
    site: 'sonare_wrap_options.h XyzProperty component',
    control: [1, 2],
    call: (v) => magnitudeSum(rir({ source: { x: v, y: 1, z: 1.2 } })),
  },
  {
    name: 'noteSegments frameRate',
    site: 'sonare_wrap_features.cpp NoteSegments frameRate',
    control: [100, 200],
    call: (v) => native.noteSegments(NOTE_F0, NOTE_PROB, null, v).at(-1)?.endSec,
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
    site: 'effects/note_editing.cpp ExtractNotes frameRate',
    control: [NOTE_RATE, NOTE_RATE * 2],
    call: (v) =>
      native.extractNotes(NOTE_AUDIO, SAMPLE_RATE, NOTE_F0, v, { voicedProb: NOTE_PROB }).at(-1)
        ?.offsetSample,
  },
  {
    name: 'decomposeNotePitch frameRate',
    site: 'effects/note_editing.cpp DecomposeNotePitch frameRate',
    control: [NOTE_RATE, NOTE_RATE * 2],
    call: (v) => magnitudeSum(native.decomposeNotePitch(SUSTAINED_F0, v, 441, 3).vibratoCents),
  },
  {
    name: 'decomposeNotePitch medianHz',
    site: 'effects/note_editing.cpp DecomposeNotePitch medianHz',
    control: [441, 220],
    call: (v) => native.decomposeNotePitch(SUSTAINED_F0, NOTE_RATE, v, 3).centerHz,
  },
  {
    name: 'decomposeNotePitch vibratoCutoffHz',
    site: 'effects/note_editing.cpp DecomposeNotePitch vibratoCutoffHz',
    control: [3, 0.5],
    call: (v) =>
      magnitudeSum(native.decomposeNotePitch(SUSTAINED_F0, NOTE_RATE, 441, v).vibratoCents),
  },
  {
    name: 'splitNote frameRate',
    site: 'effects/note_editing.cpp SplitNote frameRate',
    control: [NOTE_RATE, NOTE_RATE * 2],
    call: (v) =>
      native
        .splitNote(NOTE_AUDIO, SAMPLE_RATE, NOTE_F0, v, NOTES, 0, 5, { voicedProb: NOTE_PROB })
        .at(-1)?.offsetSample,
  },
  {
    name: 'mergeNotes frameRate',
    site: 'effects/note_editing.cpp MergeNotes frameRate',
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
    call: (v) => magnitudeSum(native.voiceChange(SHORT, SAMPLE_RATE, v, 1, 'relative')),
  },
  {
    name: 'voiceChange formantFactor',
    site: 'sonare_wrap_effects.cpp VoiceChange formantFactor',
    control: [1, 1.5],
    call: (v) => magnitudeSum(native.voiceChange(SHORT, SAMPLE_RATE, 0, v, 'relative')),
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
    call: (v) => native.midiParamToCc([CC_BINDING], 88, v, 0, 0)?.word0,
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
    name: 'preemphasis coef',
    site: 'features/signal.cpp Preemphasis coef',
    // Coefficients outside the usual unit interval remain valid finite input;
    // the output must still prove that the addon consumed the argument.
    control: [-1.5, 1.5],
    call: (v) => magnitudeSum(native.preemphasis(Float32Array.from([1, 2, 3, 4]), v)),
  },
  {
    name: 'deemphasis coef',
    site: 'features/signal.cpp Deemphasis coef',
    control: [-1.5, 1.5],
    call: (v) => magnitudeSum(native.deemphasis(Float32Array.from([1, 2, 3, 4]), v)),
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

describe('the clipping threshold is given or absent', () => {
  // Absent is the C ABI's NaN, the library's clip level; a written value meets the core's domain.
  const clipped = (v?: number) =>
    native.meteringDetectClipping(AUDIO, SAMPLE_RATE, v, 1).clippedSamples;
  it('meteringDetectClipping thresholdDb', () => {
    expect(clipped(-0.01)).not.toBe(clipped(-14));
    for (const value of [...PAST_FLOAT_RANGE, ...NON_FINITE, -200]) {
      expect(() => clipped(value), `thresholdDb ${value}`).toThrow(
        /thresholdDb must be finite and in \[-120, 0\]/,
      );
    }
  });

  it('audio.detectClipping thresholdDb', () => {
    // biome-ignore lint/suspicious/noExplicitAny: the addon is untyped here on purpose.
    const audio = newGatedAudio() as any;
    const instance = (thresholdDb?: number): number =>
      audio.detectClipping({ thresholdDb, minRegionSamples: 1 }).clippedSamples;
    expect(instance(-0.01)).not.toBe(instance(-14));
    for (const value of NON_FINITE) {
      expect(() => instance(value), `thresholdDb ${value}`).toThrow(/thresholdDb must be finite/);
    }
  });
});

describe('a pcen option refuses a written non-finite value by its key', () => {
  const pcen = (options: Record<string, number>) =>
    magnitudeSum(native.pcen(STFT.magnitude, STFT.nBins, STFT.nFrames, options));
  it.each(['timeConstant', 'gain', 'bias', 'power', 'eps'])('%s', (key) => {
    expect(pcen({ gain: 0.98 })).not.toBe(pcen({ gain: 0.5 }));
    for (const value of NON_FINITE) {
      expect(() => pcen({ [key]: value }), `${key} ${value}`).toThrow(RangeError);
      expect(() => pcen({ [key]: value }), `${key} ${value}`).toThrow(
        new RegExp(`${key} must be finite`),
      );
    }
  });
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
  const mixer = new native.Mixer(
    addon.mixingScenePresetJson('commentaryDucking'),
    48000,
    MIX_BLOCK,
  );
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

const PLATFORM = { name: 'test', targetLufs: -14, ceilingDbtp: -1 };

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
    maxSec: 0.2,
    source: { x: 1, y: 1, z: 1.2 },
    listener: { x: 3, y: 2, z: 1.2 },
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
    name: 'StreamAnalyzer setReferenceHz referenceHz',
    site: 'streaming/stream_analyzer.cpp SetReferenceHz referenceHz',
    control: [440, 466.16],
    call: (v) => magnitudeSum(analyzerFrames((a) => a.setReferenceHz(v)).chroma),
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
    name: 'masteringStreamingPreview targetLufs',
    site: 'effects/mastering_pair.cpp MasteringStreamingPreview targetLufs',
    control: [-14, -20],
    call: (v) =>
      native.masteringStreamingPreview(SHORT, SAMPLE_RATE, [{ ...PLATFORM, targetLufs: v }]),
  },
  {
    name: 'masteringStreamingPreview ceilingDbtp',
    site: 'effects/mastering_pair.cpp MasteringStreamingPreview ceilingDbtp',
    control: [-1, -12],
    call: (v) =>
      native.masteringStreamingPreview(SHORT, SAMPLE_RATE, [{ ...PLATFORM, ceilingDbtp: v }]),
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
    name: 'masteringStreamingPreviewStereo ceilingDbtp',
    site: 'effects/mastering_pair.cpp MasteringStreamingPreviewStereo ceilingDbtp',
    control: [-1, -20],
    call: (v) =>
      native.masteringStreamingPreviewStereo(SHORT, SHORT, SAMPLE_RATE, [
        { ...PLATFORM, ceilingDbtp: v },
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
        engine.setAutomationLane(7, [{ ppq: 0, value: v, curveToNext: 0 }]);
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
 * reader's own refusal is then distinguishable from the core's by its wording.
 */
describe('setExpectedDuration reaches the core, and 1e39 does not reach it at all', () => {
  /* biome-ignore lint/suspicious/noExplicitAny: the addon is untyped here on purpose. */
  function analyzer(): any {
    return new native.StreamAnalyzer({ sampleRate: SAMPLE_RATE, nFft: 512, hopLength: 256 });
  }

  it('streaming/stream_analyzer.cpp SetExpectedDuration durationSeconds', () => {
    expect(() => analyzer().setExpectedDuration(30)).not.toThrow();
    // The core owns the non-negative half of the domain, so the argument
    // demonstrably arrives: its refusal lacks the reader's float-range wording.
    expect(() => analyzer().setExpectedDuration(-1)).toThrow(RangeError);
    expect(() => analyzer().setExpectedDuration(-1)).not.toThrow(
      /must be finite and in \[-3\.4028235e38, 3\.4028235e38\]/,
    );
    for (const value of [...PAST_FLOAT_RANGE, ...NON_FINITE]) {
      expect(() => analyzer().setExpectedDuration(value), `${value}`).toThrow(RangeError);
    }
  });
});

/**
 * The object keys and array elements that do not read through
 * `node_narrow_float` at the site.
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
    site: 'sonare_wrap_options.h FloatVectorFromValue element',
    control: [2, 4],
    call: (v) => magnitudeSum(native.tempogramRatio(TEMPOGRAM, 32, SAMPLE_RATE, 512, [v])),
  },
  {
    name: 'synthesizeRir bandAbsorption',
    site: 'sonare_wrap_options.h FloatVectorFromValue element',
    control: [0.1, 0.9],
    call: (v) => magnitudeSum(rir({ bandAbsorption: [v, v, v, v] })),
  },
  {
    name: 'synthesizeRir bandScattering',
    site: 'sonare_wrap_options.h FloatVectorFromValue element',
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
 * The sites whose value has no in-domain control or no caller, driven by their own
 * case above rather than by a table. Counted here so the population stays exact.
 */
const UNCONTROLLED_SITES = [
  'streaming/stream_analyzer.cpp SetExpectedDuration durationSeconds',
  // Reads the key confidence of a result the core has just built, not a caller's argument: no entry point can hand it a value to refuse.
  'sonare_wrap_utils.cpp EnrichFullAnalysisObject confidence',
];

/**
 * Sites in the required-argument and state tables whose reader is the bail-out
 * `RequiredFloatValue`, not `node_narrow_float`: driven here for the
 * refusal, but not part of the `node_narrow_float` call-site population.
 */
const BAILOUT_READER_SITES = new Set([
  'sonare_wrap_engine.cpp SetAutomationLane value',
  'sonare_wrap_features.cpp NoteSegments frameRate',
  'features/conversion.cpp TimeToFrames time',
  'analysis/metering.cpp ParseScaleArgs midi',
  'features/advanced.cpp CqtLike fmin',
  'features/signal.cpp TrimSilence topDb',
  'features/signal.cpp SplitSilence topDb',
  'features/signal.cpp Preemphasis coef',
  'features/signal.cpp Deemphasis coef',
  'analysis/metering.cpp MeteringDetectClipping threshold',
  'analysis/metering.cpp MeteringDynamicRange windowSec',
  'analysis/metering.cpp MeteringDynamicRange hopSec',
  'analysis/metering.cpp MeteringDynamicRange lowPercentile',
  'analysis/metering.cpp MeteringDynamicRange highPercentile',
  'effects/mastering_pair.cpp MasteringStreamingPreview targetLufs',
  'effects/mastering_pair.cpp MasteringStreamingPreview ceilingDbtp',
  'effects/mastering_pair.cpp MasteringStreamingPreviewStereo targetLufs',
  'effects/mastering_pair.cpp MasteringStreamingPreviewStereo ceilingDbtp',
]);

/**
 * Sites in the required-argument table whose narrowing call sits inside a shared
 * reader in `sonare_wrap_options.h`: driven here through an entry point that
 * reads with it, but not part of the per-file call-site population.
 */
const SHARED_READER_SITES = new Set([
  'sonare_wrap_options.h XyzProperty component',
  'sonare_wrap_options.h FloatVectorFromValue element',
]);

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

  it('drives every node_narrow_float call site in the addon', () => {
    const calls = addonSources()
      // The shared header holds the reader itself plus the members built on it,
      // none of which is a site.
      .filter((source) => source.file !== 'sonare_wrap_options.h')
      .flatMap((source) => source.text.match(/node_narrow_float\(/g) ?? []);
    const sites = new Set([
      ...REQUIRED_FLOAT_ARGUMENTS.map((argument) => argument.site).filter(
        (site) => !BAILOUT_READER_SITES.has(site) && !SHARED_READER_SITES.has(site),
      ),
      ...REQUIRED_FLOAT_STATE_ARGUMENTS.map((argument) => argument.site).filter(
        (site) => !BAILOUT_READER_SITES.has(site),
      ),
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

  it('drives every node_narrow_float_element call site in the addon', () => {
    const calls = addonSources()
      .filter((source) => source.file !== 'sonare_wrap_options.h')
      .flatMap((source) => source.text.match(/node_narrow_float_element\(/g) ?? []);
    const sites = new Set(
      KEY_AND_ELEMENT_ARGUMENTS.filter(
        (argument) => argument.site.endsWith(' element') && !SHARED_READER_SITES.has(argument.site),
      ).map((argument) => argument.site),
    );
    expect(sites.size).toBe(calls.length);
  });
});

/**
 * The five unit conversions, which keep the C ABI's own conversion.
 *
 * Four are total functions, and the librosa mirrors propagate a NaN the way the
 * reference does. The fifth, `hzToNote`, receives the value unchanged and the
 * core refuses a frequency with no note by code (`src/core/convert.cpp`). The C
 * ABI (`features_spectral_pitch.cpp`) and the WASM facade both pass the value
 * straight through, so a refusal ahead of the core would put this surface alone
 * out of step with the oracle rather than closing a divergence.
 *
 * `1e39` is asserted alongside the non-finite spellings and must NOT throw in the
 * total four: it saturates to an infinity exactly as the C conversion does.
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
    site: 'features/conversion.cpp HzToMel hz',
    control: [440, 880],
    call: (v) => native.hzToMel(v),
    propagates: SATURATES,
  },
  {
    name: 'melToHz mel',
    site: 'features/conversion.cpp MelToHz mel',
    control: [1000, 2000],
    call: (v) => native.melToHz(v),
    propagates: SATURATES,
  },
  {
    name: 'hzToMidi hz',
    site: 'features/conversion.cpp HzToMidi hz',
    control: [440, 880],
    call: (v) => native.hzToMidi(v),
    propagates: SATURATES,
  },
  {
    name: 'midiToHz midi',
    site: 'features/conversion.cpp MidiToHz midi',
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
    site: 'features/conversion.cpp HzToNote hz',
    control: [440, 880],
    call: (v) => native.hzToNote(v),
    // The core refuses every frequency with no note, so there is no value to
    // propagate; the refusal is asserted in its own case below.
    propagates: [],
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
describe('the default pyin track maps through hzToNote once its unvoiced frames are dropped', () => {
  it('produces NaN frames, refuses them as a RangeError, and names the voiced ones', () => {
    const voicedThenSilent = new Float32Array(Math.round(SAMPLE_RATE * 0.25));
    for (let i = 0; i < voicedThenSilent.length / 2; i++) {
      voicedThenSilent[i] = 0.4 * Math.sin((2 * Math.PI * 220 * i) / SAMPLE_RATE);
    }
    const { f0 } = native.pitchPyin(voicedThenSilent, SAMPLE_RATE, 2048, 512, 65, 2093, 0.1, false);
    const track = Array.from(f0 as Float32Array);
    expect(track.some(Number.isNaN)).toBe(true);
    const voiced = track.filter((hz) => Number.isFinite(hz) && hz > 0);
    expect(voiced.length).toBeGreaterThan(0);

    expect(() => native.hzToNote(Number.NaN)).toThrow(RangeError);
    for (const hz of voiced) {
      expect(native.hzToNote(hz)).not.toBe('');
    }
  });
});

/** The option keys read as a float off an object, each refused past the float range by name. */
const ANALYZER_CONFIG_KEYS = [
  'fmin',
  'fmax',
  'referenceHz',
  'keyUpdateIntervalSec',
  'bpmUpdateIntervalSec',
];
const QUANTIZE_KEYS = ['melDbMin', 'melDbMax', 'onsetMax', 'rmsMax', 'centroidMax'];
const EQ_BAND_KEYS = [
  'frequencyHz',
  'gainDb',
  'q',
  'proportionalQStrength',
  'thresholdDb',
  'ratio',
  'rangeDb',
  'attackMs',
  'releaseMs',
  'detectorDelayMs',
  'sidechainFreqHz',
  'sidechainQ',
];
/** An in-range value for the keys whose domain excludes the 100 the others accept. */
const IN_RANGE: Record<string, number> = { referenceHz: 440, melDbMin: -60 };
const CHAIN_OPTION_KEYS = ['loudnessStaticGainDb', 'loudnessStaticGainPeakDbtp'];

describe('a streaming option key is refused past the float range rather than saturated', () => {
  const analyzerConfig = { sampleRate: SAMPLE_RATE, nFft: 512, hopLength: 256, nMels: 8 };

  function analyzerMel(config: Record<string, unknown>): string {
    const analyzer = new native.StreamAnalyzer({ ...analyzerConfig, ...config });
    try {
      analyzer.process(SHORT);
      return JSON.stringify(Array.from(analyzer.readFramesSoa(8).mel ?? []));
    } finally {
      analyzer.destroy();
    }
  }

  // Coverage, not a regression guard: a saturated infinity on these keys was already refused by name downstream.
  it.each(ANALYZER_CONFIG_KEYS)(
    'StreamAnalyzer config %s (coverage: refused downstream too)',
    (key) => {
      expect(() =>
        new native.StreamAnalyzer({ ...analyzerConfig, [key]: IN_RANGE[key] ?? 100 }).destroy(),
      ).not.toThrow();
      for (const value of PAST_FLOAT_RANGE) {
        expect(
          () => new native.StreamAnalyzer({ ...analyzerConfig, [key]: value }),
          `${value}`,
        ).toThrow(RangeError);
        expect(() => new native.StreamAnalyzer({ ...analyzerConfig, [key]: value })).toThrow(
          new RegExp(key),
        );
      }
    },
  );

  it('StreamAnalyzer config fmin and fmax reach the mel bands', () => {
    expect(analyzerMel({ fmin: 0 })).not.toBe(analyzerMel({ fmin: 2000 }));
    expect(analyzerMel({ fmax: 4000 })).not.toBe(analyzerMel({ fmax: 8000 }));
  });

  it.each(QUANTIZE_KEYS)('StreamAnalyzer readFramesU8 and readFramesI16 %s', (key) => {
    const analyzer = new native.StreamAnalyzer({ ...analyzerConfig });
    try {
      analyzer.process(SHORT);
      expect(() => analyzer.readFramesU8(1, { [key]: IN_RANGE[key] ?? 100 })).not.toThrow();
      for (const value of PAST_FLOAT_RANGE) {
        expect(() => analyzer.readFramesU8(1, { [key]: value }), `${value}`).toThrow(RangeError);
        expect(() => analyzer.readFramesI16(1, { [key]: value }), `${value}`).toThrow(RangeError);
        expect(() => analyzer.readFramesU8(1, { [key]: value })).toThrow(new RegExp(key));
      }
    } finally {
      analyzer.destroy();
    }
  });

  function eqOutput(band: Record<string, unknown>): string {
    const eq = new native.StreamingEqualizer({ sampleRate: SAMPLE_RATE, maxBlockSize: 512 });
    try {
      eq.setBand(0, { type: 'peak', frequencyHz: 1000, gainDb: 6, q: 1, enabled: true, ...band });
      const tone = new Float32Array(512);
      for (let i = 0; i < tone.length; i++) {
        tone[i] = 0.25 * Math.sin((2 * Math.PI * 1000 * i) / SAMPLE_RATE);
      }
      const { left } = eq.processStereo(tone, tone.slice());
      return JSON.stringify(Array.from(left as Float32Array));
    } finally {
      eq.destroy();
    }
  }

  it.each(EQ_BAND_KEYS)('StreamingEqualizer setBand %s', (key) => {
    const eq = new native.StreamingEqualizer({ sampleRate: SAMPLE_RATE, maxBlockSize: 512 });
    try {
      for (const value of PAST_FLOAT_RANGE) {
        expect(() => eq.setBand(0, { type: 'peak', [key]: value }), `${value}`).toThrow(RangeError);
        expect(() => eq.setBand(0, { type: 'peak', [key]: value })).toThrow(new RegExp(key));
      }
    } finally {
      eq.destroy();
    }
  });

  it('StreamingEqualizer setBand frequencyHz, gainDb and q reach the response', () => {
    expect(eqOutput({ frequencyHz: 500 })).not.toBe(eqOutput({ frequencyHz: 4000 }));
    expect(eqOutput({ gainDb: 3 })).not.toBe(eqOutput({ gainDb: 9 }));
    expect(eqOutput({ q: 0.5 })).not.toBe(eqOutput({ q: 4 }));
  });

  // Neither key is observable without a measured program through a configured chain.
  it.each(CHAIN_OPTION_KEYS)('StreamingMasteringChain option %s', (key) => {
    for (const value of PAST_FLOAT_RANGE) {
      expect(() => new native.StreamingMasteringChain(null, { [key]: value }), `${value}`).toThrow(
        RangeError,
      );
      expect(() => new native.StreamingMasteringChain(null, { [key]: value })).toThrow(
        new RegExp(key),
      );
    }
  });
});

describe('the addon never casts a reader result straight to float', () => {
  /** Block and line comments removed, newlines kept so line numbers hold. */
  function stripComments(text: string): string {
    return text
      .replace(/\/\*[\s\S]*?\*\//g, (m) => m.replace(/[^\n]/g, ' '))
      .replace(/\/\/[^\n]*/g, '');
  }

  const DIRECT_CAST =
    /static_cast<float>\(\s*(?:Optional\w*|Required\w*|DoubleProperty|\w+Property|(?:[\w.\->]|\[\w*\]|<[\w:]+>|\(\))*\.DoubleValue\(\))/g;

  it('finds no static_cast<float> over a reader result', () => {
    const sources = addonSources();
    expect(sources.length).toBeGreaterThan(0);
    const hits: string[] = [];
    for (const { file, text } of sources) {
      const stripped = stripComments(text);
      for (const match of stripped.matchAll(DIRECT_CAST)) {
        const line = stripped.slice(0, match.index).split('\n').length;
        hits.push(`${file}:${line}`);
      }
    }
    expect(hits).toEqual([]);
  });

  it('matches the shape it is meant to forbid', () => {
    expect('x = static_cast<float>(DoubleProperty(o, "k", 1));'.match(DIRECT_CAST)).not.toBeNull();
    expect(
      'x = static_cast<float>(v.As<Napi::Number>().DoubleValue());'.match(DIRECT_CAST),
    ).not.toBeNull();
    expect('x = FloatProperty(o, "k", 1);'.match(DIRECT_CAST)).toBeNull();
  });
});
