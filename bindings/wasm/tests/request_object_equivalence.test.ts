/**
 * Request-object versus positional equivalence for every exported function
 * that has both forms. Each row names the positional parameter order once and
 * the harness derives the request from it, then asserts on a non-silent input:
 *
 * - both forms return the same result for non-default values, and for the
 *   required arguments alone (so the two forms share their defaults);
 * - every optional field the row passes moves the positional result when it is
 *   omitted (the control that makes a dropped field visible in the equality);
 * - an invalid value is refused with the same error name and message by both.
 *
 * The inventory is read from the sources, so an overload with no row fails.
 */

import { readdirSync, readFileSync } from 'node:fs';
import { join } from 'node:path';
import { isDeepStrictEqual } from 'node:util';
import { describe, expect, it } from 'vitest';
import * as sonare from '../src/index';

await sonare.init();

const SR = 22050;
/** A sample rate no function defaults to, so passing it is observable. */
const ALT_SR = 32000;

function lcg(seed: number): () => number {
  let state = seed >>> 0;
  return () => {
    state = (Math.imul(state, 1664525) + 1013904223) >>> 0;
    return state / 2 ** 32 - 0.5;
  };
}

/**
 * Beat-gated harmonic tones with a click per beat and a pitch change halfway,
 * ending in a near-silent tail, so onset, pitch, chroma, silence and loudness
 * readers all have something to measure.
 */
function signal(seconds: number, root = 220, seed = 1): Float32Array {
  const n = Math.round(SR * seconds);
  const out = new Float32Array(n);
  const noise = lcg(seed);
  const beat = Math.round(SR * 0.25);
  const voiced = Math.round(n * 0.8);
  for (let i = 0; i < n; i++) {
    const t = i / SR;
    const f = i < n / 2 ? root : root * 1.5;
    const env = Math.exp(-((i % beat) / SR) * 6);
    let v = 0;
    if (i < voiced) {
      v =
        0.18 *
        env *
        (Math.sin(2 * Math.PI * f * t) +
          0.5 * Math.sin(2 * Math.PI * f * 1.26 * t) +
          0.4 * Math.sin(2 * Math.PI * f * 1.5 * t) +
          0.2 * Math.sin(2 * Math.PI * f * 2 * t));
      if (i % beat < 40) {
        v += 0.3 * (1 - (i % beat) / 40) * noise();
      }
    }
    out[i] = v + 0.002 * noise();
  }
  return out;
}

const SIG = signal(1);
const SIG2 = signal(1, 330, 7);
/** A short excerpt for the constant-Q and inverse rows, whose cost grows fastest with length. */
const SHORT = SIG.subarray(0, Math.round(SR * 0.3));
const LONG = signal(3.5);
/** A pure tone with an exactly silent tail, for the pitch trackers' voicing. */
const TONE = Float32Array.from({ length: SR }, (_, i) =>
  i < SR * 0.8 ? 0.3 * Math.sin((2 * Math.PI * 220 * i) / SR) : 0,
);
/** The same tone over a noise tail, which YIN reads as unvoiced rather than as a period. */
const TONE_NOISY_TAIL = (() => {
  const noise = lcg(11);
  return TONE.map((v, i) => (i < SR * 0.8 ? v : 0.05 * noise()));
})();
/** Two contrasting four-second halves, for section segmentation. */
const LONG8 = (() => {
  const a = signal(4, 220, 1);
  const out = new Float32Array(a.length * 2);
  out.set(a);
  out.set(signal(4, 330, 5), a.length);
  return out;
})();
/** The voiced part of SIG between silent margins, for the trimmers. */
const PADDED = (() => {
  const out = new Float32Array(SR * 2);
  out.set(SIG.subarray(0, Math.round(SR * 0.8)), SR / 2);
  return out;
})();
const CLIPPED = SIG.map((v) => Math.max(-0.12, Math.min(0.12, v * 2)));
/** Sparse full-scale impulses over the tones, for the click and crackle repairs. */
const CLICKY = SIG.map((v, i) => (i % 1500 === 700 ? 0.9 : i % 1500 === 701 ? -0.8 : v));
const HUMMED = SIG.map((v, i) => v + 0.05 * Math.sin((2 * Math.PI * 60 * i) / SR));
const STEREO = (() => {
  const out = new Float32Array(SIG.length * 2);
  for (let i = 0; i < SIG.length; i++) {
    out[2 * i] = SIG[i];
    out[2 * i + 1] = SIG2[i];
  }
  return out;
})();
/** An exponentially decaying noise burst, for the impulse-response readers. */
const IR = (() => {
  const noise = lcg(3);
  return Float32Array.from({ length: SR }, (_, i) => noise() * Math.exp(-i / (0.08 * SR)));
})();

const ST = sonare.stft(SHORT, SR, 512, 128);
const MEL = sonare.melSpectrogram(SHORT, SR, 512, 128, 32);
const MFCC = sonare.mfcc(SHORT, SR, 512, 128, 32, 13);
const CQT = sonare.cqt(SHORT, SR, 128, 65.4, 36, 12);
const VQT = sonare.vqt(SHORT, SR, 128, 65.4, 36, 12, 2);
const ENV = sonare.onsetEnvelope(SIG, SR, 1024, 256);
const TG = sonare.tempogram(ENV, SR, 256, 64);
const CHROMA = sonare.chroma(SIG, SR, 1024, 256);
const F0 = new Float32Array(Math.ceil(SIG.length / 256) + 1).fill(220);
const SMALL = Float32Array.from({ length: 80 }, (_, i) => 0.1 + ((i * 37) % 11) / 11);

type Args = Record<string, unknown>;

/** A progress callback slot; each call gets a fresh recorder in its place. */
const PROGRESS = Symbol('progress');

interface Row {
  name: string;
  fn: (...args: never[]) => unknown;
  /** Positional order; `?` marks an optional parameter, `...` one spread into the request. */
  params: string;
  values: Args;
  /** Overrides both forms must refuse with the same error. */
  invalid: Args;
  /** Request field name for a positional parameter spelled differently. */
  rename?: Record<string, string>;
  /** Fields with no per-field control, and why. */
  inert?: Record<string, string>;
  /**
   * Per-field overrides of the values the control drops the field from, for a
   * field another passed field can mask (a floor that hides a clamp).
   */
  controlBase?: Record<string, Args>;
}

interface Spec {
  name: string;
  optional: boolean;
  spread: boolean;
}

function specsOf(row: Row): Spec[] {
  return row.params.split(/\s+/).map((token) => ({
    name: token.replace(/^\.\.\./, '').replace(/\?$/, ''),
    optional: token.endsWith('?'),
    spread: token.startsWith('...'),
  }));
}

function positionalArgs(specs: Spec[], values: Args): unknown[] {
  const args = specs.map((spec) => values[spec.name]);
  while (args.length > 0 && args[args.length - 1] === undefined) {
    args.pop();
  }
  return args;
}

function requestOf(row: Row, specs: Spec[], values: Args): Args {
  const request: Args = {};
  for (const spec of specs) {
    const value = values[spec.name];
    if (value === undefined) {
      continue;
    }
    if (spec.spread) {
      Object.assign(request, value);
    } else {
      request[row.rename?.[spec.name] ?? spec.name] = value;
    }
  }
  return request;
}

type Outcome = { value: unknown; progressed: boolean } | { error: string };

/** Replace each PROGRESS slot (top level or one level into an object) with a recorder. */
function materialize(values: Args): { values: Args; progressed: () => boolean } {
  let calls = 0;
  const recorder = () => {
    calls += 1;
  };
  const swap = (value: unknown): unknown => {
    if (value === PROGRESS) {
      return recorder;
    }
    if (
      value !== null &&
      typeof value === 'object' &&
      Object.getPrototypeOf(value) === Object.prototype
    ) {
      return Object.fromEntries(Object.entries(value).map(([k, v]) => [k, swap(v)]));
    }
    return value;
  };
  const swapped = Object.fromEntries(Object.entries(values).map(([k, v]) => [k, swap(v)]));
  return { values: swapped, progressed: () => calls > 0 };
}

async function run(
  row: Row,
  specs: Spec[],
  values: Args,
  form: 'positional' | 'request',
): Promise<Outcome> {
  const { values: live, progressed } = materialize(values);
  const call = row.fn as (...args: unknown[]) => unknown;
  try {
    const value =
      form === 'positional'
        ? await call(...positionalArgs(specs, live))
        : await call(requestOf(row, specs, live));
    return { value, progressed: progressed() };
  } catch (error) {
    return { error: `${(error as Error).name}: ${(error as Error).message}` };
  }
}

interface Field {
  param: string;
  key?: string;
}

function fieldsOf(row: Row, specs: Spec[]): Field[] {
  const fields: Field[] = [];
  for (const spec of specs) {
    const value = row.values[spec.name];
    if (value === undefined) {
      continue;
    }
    if (spec.spread) {
      for (const key of Object.keys(value as Args)) {
        fields.push({ param: spec.name, key });
      }
    } else if (spec.optional) {
      fields.push({ param: spec.name });
    }
  }
  return fields.filter((field) => !(row.inert && (field.key ?? field.param) in row.inert));
}

function without(values: Args, field: Field): Args {
  const next = { ...values };
  if (field.key === undefined) {
    next[field.param] = undefined;
  } else {
    const { [field.key]: _, ...rest } = values[field.param] as Args;
    next[field.param] = rest;
  }
  return next;
}

/** The row's values with a NaN written into the first audio buffer. */
function poisoned(specs: Spec[], values: Args): Args {
  for (const spec of specs) {
    const value = values[spec.name];
    if (value instanceof Float32Array) {
      const copy = value.slice();
      copy[0] = Number.NaN;
      return { ...values, [spec.name]: copy };
    }
  }
  throw new Error('row has no audio buffer to poison');
}

const ROWS: Row[] = [
  {
    name: 'noteStretch',
    fn: sonare.noteStretch,
    params: 'samples sampleRate? ...options?',
    values: {
      samples: SIG,
      sampleRate: ALT_SR,
      options: { onsetSample: 2000, offsetSample: 8000, stretchRatio: 1.5 },
    },
    invalid: { sampleRate: 0 },
  },

  {
    name: 'noteMove',
    fn: sonare.noteMove,
    params: 'samples sampleRate? ...options?',
    values: {
      samples: SIG,
      sampleRate: ALT_SR,
      options: { onsetSample: 2000, offsetSample: 8000, targetOnsetSample: 4000 },
    },
    invalid: { sampleRate: 0 },
  },

  {
    name: 'hpss',
    fn: sonare.hpss,
    params: 'samples sampleRate? kernelHarmonic? kernelPercussive? nFft? hopLength? hardMask?',
    values: {
      samples: SIG,
      sampleRate: ALT_SR,
      kernelHarmonic: 17,
      kernelPercussive: 17,
      nFft: 1024,
      hopLength: 256,
      hardMask: true,
    },
    invalid: { nFft: 0 },
  },

  {
    name: 'harmonic',
    fn: sonare.harmonic,
    params: 'samples sampleRate? ...options?',
    values: {
      options: { validate: false },
      samples: SIG,
      sampleRate: ALT_SR,
    },
    invalid: { sampleRate: 0 },
    inert: { sampleRate: 'median filtering in bins and frames reads no rate' },
  },

  {
    name: 'percussive',
    fn: sonare.percussive,
    params: 'samples sampleRate? ...options?',
    values: {
      options: { validate: false },
      samples: SIG,
      sampleRate: ALT_SR,
    },
    invalid: { sampleRate: 0 },
    inert: { sampleRate: 'median filtering in bins and frames reads no rate' },
  },

  {
    name: 'spectralEdit',
    fn: sonare.spectralEdit,
    params: 'samples sampleRate ops? ...options?',
    values: {
      samples: SIG,
      sampleRate: SR,
      ops: [{ startSample: 0, endSample: 12000, lowHz: 150, highHz: 2000, gainDb: -12 }],
      options: { nFft: 1024 },
    },
    invalid: { sampleRate: 0 },
  },

  {
    name: 'timeStretch',
    fn: sonare.timeStretch,
    params: 'samples sampleRate rate nFft? hopLength? ...options?',
    values: {
      options: { validate: false },
      samples: SIG,
      sampleRate: SR,
      rate: 1.25,
      nFft: 1024,
      hopLength: 256,
    },
    invalid: { rate: 0 },
  },

  {
    name: 'pitchShift',
    fn: sonare.pitchShift,
    params: 'samples sampleRate semitones nFft? hopLength? ...options?',
    values: {
      options: { validate: false },
      samples: SIG,
      sampleRate: SR,
      semitones: 2,
      nFft: 1024,
      hopLength: 256,
    },
    invalid: { sampleRate: 0 },
  },

  {
    name: 'pitchCorrectToMidi',
    fn: sonare.pitchCorrectToMidi,
    params: 'samples sampleRate? currentMidi? targetMidi? ...options?',
    values: {
      options: { validate: false },
      samples: SIG,
      sampleRate: ALT_SR,
      currentMidi: 57,
      targetMidi: 59,
    },
    invalid: { sampleRate: 0 },
  },

  {
    name: 'pitchCorrectToMidiTimevarying',
    fn: sonare.pitchCorrectToMidiTimevarying,
    params: 'samples f0Hz targetMidi sampleRate? hopLength? voiced? voicedProb? ...options?',
    values: {
      options: { validate: false },
      samples: SIG,
      f0Hz: F0,
      targetMidi: 59,
      sampleRate: ALT_SR,
      hopLength: 256,
      voiced: Array.from(F0, (_, i) => i % 7 !== 0),
    },
    invalid: { sampleRate: 0 },
  },

  {
    name: 'pitchCorrectTimevarying',
    fn: sonare.pitchCorrectTimevarying,
    params: 'samples f0Hz sampleRate? hopLength? ...options?',
    values: {
      samples: SIG,
      f0Hz: F0,
      sampleRate: ALT_SR,
      hopLength: 256,
      options: { targetMidi: 59, retuneAmount: 0.5 },
    },
    invalid: { sampleRate: 0 },
  },

  {
    name: 'voiceChange',
    fn: sonare.voiceChange,
    params: 'samples sampleRate? ...options?',
    values: {
      samples: SIG,
      sampleRate: ALT_SR,
      options: { pitchSemitones: 3, formantFactor: 1.2 },
    },
    invalid: { sampleRate: 0 },
  },

  {
    name: 'voiceChangeRealtime',
    fn: sonare.voiceChangeRealtime,
    params: 'samples sampleRate? preset? ...options?',
    values: {
      samples: STEREO,
      sampleRate: ALT_SR,
      preset: 'bright-idol',
      options: { channels: 2 },
    },
    invalid: { sampleRate: 0 },
  },

  {
    name: 'tone',
    fn: sonare.tone,
    params: 'frequency? sampleRate? duration? phase? amplitude?',
    values: { frequency: 330, sampleRate: ALT_SR, duration: 0.2, phase: 0.5, amplitude: 0.3 },
    invalid: { sampleRate: 0 },
  },

  {
    name: 'chirp',
    fn: sonare.chirp,
    params: 'fmin? fmax? sampleRate? duration? linear?',
    values: { fmin: 200, fmax: 2000, sampleRate: ALT_SR, duration: 0.2, linear: false },
    invalid: { sampleRate: 0 },
  },

  {
    name: 'clicks',
    fn: sonare.clicks,
    params: 'times sampleRate? length? frequency? clickDuration?',
    values: {
      times: new Float32Array([0.1, 0.5]),
      sampleRate: ALT_SR,
      length: 30000,
      frequency: 2000,
      clickDuration: 0.05,
    },
    invalid: { sampleRate: 0 },
  },

  {
    name: 'powerToDb',
    fn: sonare.powerToDb,
    params: 'values ref? amin? topDb?',
    values: { values: SMALL, ref: 0.5, amin: 0.2, topDb: 5 },
    invalid: { amin: -1 },
    controlBase: { amin: { topDb: undefined } },
  },

  {
    name: 'amplitudeToDb',
    fn: sonare.amplitudeToDb,
    params: 'values ref? amin? topDb?',
    values: { values: SMALL, ref: 0.5, amin: 0.2, topDb: 10 },
    invalid: { amin: -1 },
    controlBase: { amin: { topDb: undefined } },
  },

  {
    name: 'preemphasis',
    fn: sonare.preemphasis,
    params: 'samples coef? zi?',
    values: { samples: SIG, coef: 0.9, zi: 0.5 },
    invalid: { samples: [0.5] },
  },

  {
    name: 'deemphasis',
    fn: sonare.deemphasis,
    params: 'samples coef? zi?',
    values: { samples: SIG, coef: 0.9, zi: 0.5 },
    invalid: { samples: [0.5] },
  },

  {
    name: 'trimSilence',
    fn: sonare.trimSilence,
    params: 'samples topDb? frameLength? hopLength?',
    values: { samples: SIG, topDb: 30, frameLength: 1024, hopLength: 256 },
    invalid: { hopLength: 0 },
  },

  {
    name: 'splitSilence',
    fn: sonare.splitSilence,
    params: 'samples topDb? frameLength? hopLength?',
    values: { samples: SIG, topDb: 30, frameLength: 1024, hopLength: 256 },
    invalid: { hopLength: 0 },
  },

  {
    name: 'frameSignal',
    fn: sonare.frameSignal,
    params: 'samples frameLength hopLength',
    values: { samples: SIG, frameLength: 1024, hopLength: 256 },
    invalid: { hopLength: 0 },
  },

  {
    name: 'padCenter',
    fn: sonare.padCenter,
    params: 'values targetSize padValue?',
    values: { values: SMALL, targetSize: 100, padValue: 0.5 },
    invalid: { targetSize: -1 },
  },

  {
    name: 'fixLength',
    fn: sonare.fixLength,
    params: 'values targetSize padValue?',
    values: { values: SMALL, targetSize: 100, padValue: 0.5 },
    invalid: { targetSize: -1 },
  },

  {
    name: 'fixFrames',
    fn: sonare.fixFrames,
    params: 'frames xMin? xMax? pad?',
    values: { frames: new Int32Array([3, 9, 40, 70]), xMin: 5, xMax: 60, pad: false },
    invalid: { xMin: 1.5 },
  },

  {
    name: 'onsetBacktrack',
    fn: sonare.onsetBacktrack,
    params: 'events energy',
    values: { events: new Int32Array([5, 20, 40]), energy: ENV },
    // An event past the curve is clamped to its last frame, so the refusal is a non-finite curve.
    invalid: { energy: ENV.map((v, i) => (i === 3 ? Number.NaN : v)) },
  },

  {
    name: 'peakPick',
    fn: sonare.peakPick,
    params: 'values preMax postMax preAvg postAvg delta wait',
    values: { values: ENV, preMax: 2, postMax: 2, preAvg: 4, postAvg: 4, delta: 0.05, wait: 3 },
    invalid: { wait: -1 },
  },

  {
    name: 'vectorNormalize',
    fn: sonare.vectorNormalize,
    params: 'values normType? threshold?',
    values: { values: SMALL, normType: 1, threshold: 50 },
    invalid: { normType: 1.5 },
    controlBase: { normType: { threshold: undefined } },
  },

  {
    name: 'pcen',
    fn: sonare.pcen,
    params: 'values nBins nFrames options?',
    values: {
      values: MEL.power,
      nBins: MEL.nMels,
      nFrames: MEL.nFrames,
      options: { gain: 0.5, timeConstant: 0.2 },
    },
    invalid: { nBins: -1 },
  },

  {
    name: 'tonnetz',
    fn: sonare.tonnetz,
    params: 'chromagram nChroma nFrames',
    values: { chromagram: CHROMA.features, nChroma: CHROMA.nChroma, nFrames: CHROMA.nFrames },
    invalid: { nFrames: 0 },
  },

  {
    name: 'tempogram',
    fn: sonare.tempogram,
    params: 'onsetEnvelope sampleRate? hopLength? winLength? mode? center? norm?',
    values: {
      onsetEnvelope: ENV,
      sampleRate: ALT_SR,
      hopLength: 256,
      winLength: 64,
      mode: 'cosine',
      center: false,
      norm: false,
    },
    invalid: { winLength: 0 },
    controlBase: { norm: { mode: undefined } },
    inert: {
      sampleRate: 'the lag axis is in frames',
      hopLength: 'the lag axis is in frames',
    },
  },

  {
    name: 'cyclicTempogram',
    fn: sonare.cyclicTempogram,
    params: 'onsetEnvelope sampleRate? hopLength? winLength? bpmMin? nBins?',
    values: {
      onsetEnvelope: ENV,
      sampleRate: ALT_SR,
      hopLength: 384,
      winLength: 128,

      bpmMin: 40,
      nBins: 24,
    },
    invalid: { winLength: 0 },
  },

  {
    name: 'plp',
    fn: sonare.plp,
    params: 'onsetEnvelope sampleRate? hopLength? tempoMin? tempoMax? winLength?',
    values: {
      onsetEnvelope: ENV,
      sampleRate: ALT_SR,
      hopLength: 256,
      tempoMin: 100,
      tempoMax: 200,
      winLength: 128,
    },
    invalid: { winLength: 0 },
  },

  {
    name: 'decompose',
    fn: sonare.decompose,
    params: 's nFeatures nFrames nComponents nIter? beta?',
    values: {
      s: SMALL,
      nFeatures: 8,
      nFrames: 10,
      nComponents: 2,
      nIter: 20,
      beta: 1,
      init: 'nndsvd',
    },
    invalid: { nComponents: -1 },
  },

  {
    name: 'nnFilter',
    fn: sonare.nnFilter,
    params: 's nFeatures nFrames aggregate? k? width?',
    values: { s: SMALL, nFeatures: 8, nFrames: 10, aggregate: 'median', k: 3, width: 2 },
    invalid: { nFeatures: -1 },
  },

  {
    name: 'remix',
    fn: sonare.remix,
    params: 'samples intervals sampleRate? alignZeros?',
    values: {
      samples: SIG,
      intervals: [10000, 15001, 0, 5001],
      sampleRate: ALT_SR,
      alignZeros: true,
    },
    invalid: { intervals: [0] },
    inert: { sampleRate: 'intervals are in samples' },
  },

  {
    name: 'remixAlignedIntervals',
    fn: sonare.remixAlignedIntervals,
    params: 'samples intervals sampleRate? alignZeros?',
    values: {
      samples: SIG,
      intervals: [10000, 15001, 0, 5001],
      sampleRate: ALT_SR,
      alignZeros: false,
    },
    invalid: { intervals: [0] },
    inert: { sampleRate: 'intervals are in samples' },
  },

  {
    name: 'hpssWithResidual',
    fn: sonare.hpssWithResidual,
    params: 'samples sampleRate? kernelHarmonic? kernelPercussive? nFft? hopLength? hardMask?',
    values: {
      samples: SIG,
      sampleRate: ALT_SR,
      kernelHarmonic: 17,
      kernelPercussive: 17,
      nFft: 1024,
      hopLength: 256,
      hardMask: true,
    },
    invalid: { nFft: 0 },
  },

  {
    name: 'melToStft',
    fn: sonare.melToStft,
    params: 'power nMels nFrames sampleRate? nFft? fmin? fmax? htk? ...options?',
    values: {
      options: { validate: false },
      power: MEL.power,
      nMels: MEL.nMels,
      nFrames: MEL.nFrames,
      sampleRate: ALT_SR,
      nFft: 512,
      fmin: 50,
      fmax: 8000,
      htk: true,
    },
    invalid: { nFft: 0 },
  },

  {
    name: 'melToAudio',
    fn: sonare.melToAudio,
    params: 'power nMels nFrames sampleRate? nFft? hopLength? fmin? fmax? nIter? htk? ...options?',
    values: {
      options: { validate: false },
      power: MEL.power,
      nMels: MEL.nMels,
      nFrames: MEL.nFrames,
      sampleRate: ALT_SR,
      nFft: 512,
      hopLength: 128,
      fmin: 50,
      fmax: 8000,
      nIter: 4,
      htk: true,
    },
    invalid: { hopLength: 0 },
  },

  {
    name: 'griffinLim',
    fn: sonare.griffinLim,
    params: 'magnitude nBins nFrames sampleRate? nFft? hopLength? nIter? momentum? ...options?',
    values: {
      options: { validate: false },
      magnitude: ST.magnitude,
      nBins: ST.nBins,
      nFrames: ST.nFrames,
      sampleRate: ALT_SR,
      nFft: 512,
      hopLength: 128,
      nIter: 4,
      momentum: 0.5,
    },
    invalid: { hopLength: 0 },
    inert: { sampleRate: 'reconstruction runs in bins and frames' },
  },

  {
    name: 'mfccToMel',
    fn: sonare.mfccToMel,
    params: 'coefficients nMfcc nFrames nMels? lifter? ...options?',
    values: {
      options: { validate: false },
      coefficients: MFCC.coefficients,
      nMfcc: MFCC.nMfcc,
      nFrames: MFCC.nFrames,
      nMels: 32,
      lifter: 22,
    },
    invalid: { nMfcc: 7 },
  },

  {
    name: 'mfccToAudio',
    fn: sonare.mfccToAudio,
    params:
      'coefficients nMfcc nFrames nMels? sampleRate? nFft? hopLength? fmin? fmax? nIter? htk? lifter? ...options?',
    values: {
      options: { validate: false },
      coefficients: MFCC.coefficients,
      nMfcc: MFCC.nMfcc,
      nFrames: MFCC.nFrames,
      nMels: 32,
      sampleRate: ALT_SR,
      nFft: 512,
      hopLength: 128,
      fmin: 50,
      fmax: 8000,
      nIter: 4,
      htk: true,
      lifter: 22,
    },
    invalid: { hopLength: 0 },
  },

  {
    name: 'phaseVocoder',
    fn: sonare.phaseVocoder,
    params: 'samples sampleRate rate nFft? hopLength?',
    values: { samples: SIG, sampleRate: SR, rate: 1.25, nFft: 1024, hopLength: 256 },
    invalid: { rate: 0 },
  },

  {
    name: 'lufsInterleaved',
    fn: sonare.lufsInterleaved,
    params: 'samples channels sampleRate? ...options?',
    values: {
      options: { validate: false },
      samples: STEREO,
      channels: 2,
      sampleRate: ALT_SR,
    },
    invalid: { channels: 0 },
  },

  {
    name: 'lufsSeriesInterleaved',
    fn: sonare.lufsSeriesInterleaved,
    params: 'samples channels sampleRate? ...options?',
    values: {
      options: { validate: false },
      samples: STEREO,
      channels: 2,
      sampleRate: ALT_SR,
    },
    invalid: { channels: 0 },
  },

  {
    name: 'ebur128LoudnessRange',
    fn: sonare.ebur128LoudnessRange,
    params: 'samples sampleRate?',
    values: { samples: LONG, sampleRate: ALT_SR },
    invalid: { sampleRate: 0 },
  },

  {
    name: 'nnlsChroma',
    fn: sonare.nnlsChroma,
    params: 'samples sampleRate? ...options?',
    values: {
      samples: SIG,
      sampleRate: ALT_SR,
      options: { hopLength: 1024, enableStftBlend: false },
    },
    invalid: { sampleRate: 0 },
  },

  {
    name: 'cqt',
    fn: sonare.cqt,
    params: 'samples sampleRate? hopLength? fmin? nBins? binsPerOctave? ...options?',
    values: {
      options: { validate: false },
      samples: SHORT,
      sampleRate: ALT_SR,
      hopLength: 256,
      fmin: 65.4,
      nBins: 48,
      binsPerOctave: 24,
    },
    invalid: { hopLength: 0 },
  },

  {
    name: 'pseudoCqt',
    fn: sonare.pseudoCqt,
    params: 'samples sampleRate? hopLength? fmin? nBins? binsPerOctave? ...options?',
    values: {
      options: { validate: false },
      samples: SHORT,
      sampleRate: ALT_SR,
      hopLength: 256,
      fmin: 65.4,
      nBins: 48,
      binsPerOctave: 24,
    },
    invalid: { hopLength: 0 },
  },

  {
    name: 'hybridCqt',
    fn: sonare.hybridCqt,
    params: 'samples sampleRate? hopLength? fmin? nBins? binsPerOctave? ...options?',
    values: {
      options: { validate: false },
      samples: SHORT,
      sampleRate: ALT_SR,
      hopLength: 256,
      fmin: 65.4,
      nBins: 48,
      binsPerOctave: 24,
    },
    invalid: { hopLength: 0 },
  },

  {
    name: 'vqt',
    fn: sonare.vqt,
    params: 'samples sampleRate? hopLength? fmin? nBins? binsPerOctave? gamma? ...options?',
    values: {
      options: { validate: false },
      samples: SHORT,
      sampleRate: ALT_SR,
      hopLength: 256,
      fmin: 65.4,
      nBins: 48,
      binsPerOctave: 24,
      gamma: 5,
    },
    invalid: { hopLength: 0 },
  },

  {
    name: 'cqtToAudio',
    fn: sonare.cqtToAudio,
    params:
      'magnitude nBins nFrames sampleRate? hopLength? fmin? binsPerOctave? nIter? ...options?',
    values: {
      options: { validate: false },
      magnitude: CQT.magnitude,
      nBins: CQT.nBins,
      nFrames: CQT.nFrames,
      sampleRate: ALT_SR,
      hopLength: 128,
      fmin: 65.4,
      binsPerOctave: 24,
      nIter: 4,
    },
    invalid: { hopLength: 0 },
  },

  {
    name: 'vqtToAudio',
    fn: sonare.vqtToAudio,
    params:
      'magnitude nBins nFrames sampleRate? hopLength? fmin? binsPerOctave? gamma? nIter? ...options?',
    values: {
      options: { validate: false },
      magnitude: VQT.magnitude,
      nBins: VQT.nBins,
      nFrames: VQT.nFrames,
      sampleRate: ALT_SR,
      hopLength: 128,
      fmin: 65.4,
      binsPerOctave: 24,
      gamma: 2,
      nIter: 4,
    },
    invalid: { hopLength: 0 },
  },

  {
    name: 'analyzeSections',
    fn: sonare.analyzeSections,
    params: 'samples sampleRate? ...options?',
    values: { samples: LONG8, sampleRate: ALT_SR, options: { hopLength: 1024, minSectionSec: 1 } },
    invalid: { sampleRate: 0 },
  },

  {
    name: 'analyzeMelody',
    fn: sonare.analyzeMelody,
    params: 'samples sampleRate? ...options?',
    values: { samples: SIG, sampleRate: ALT_SR, options: { hopLength: 128, fmin: 150 } },
    invalid: { sampleRate: 0 },
  },

  {
    name: 'onsetEnvelope',
    fn: sonare.onsetEnvelope,
    params: 'samples sampleRate? nFft? hopLength? nMels? ...options?',
    values: {
      options: { validate: false },
      samples: SIG,
      sampleRate: ALT_SR,
      nFft: 1024,
      hopLength: 256,
      nMels: 64,
    },
    invalid: { hopLength: 0 },
  },

  {
    name: 'onsetStrengthMulti',
    fn: sonare.onsetStrengthMulti,
    params: 'samples sampleRate? nFft? hopLength? nMels? nBands? ...options?',
    values: {
      options: { validate: false },
      samples: SIG,
      sampleRate: ALT_SR,
      nFft: 1024,
      hopLength: 256,
      nMels: 64,
      nBands: 6,
    },
    invalid: { hopLength: 0 },
  },

  {
    name: 'fourierTempogram',
    fn: sonare.fourierTempogram,
    params: 'onsetEnvelope sampleRate? hopLength? winLength? center? norm? ...options?',
    values: {
      options: { validate: false },
      onsetEnvelope: ENV,
      sampleRate: ALT_SR,
      hopLength: 256,
      winLength: 64,
      center: false,
      norm: false,
    },
    invalid: { winLength: 0 },
    inert: {
      norm: 'the core Fourier tempogram reads no norm',
      sampleRate: 'the frequency axis is in frames',
      hopLength: 'the frequency axis is in frames',
    },
  },

  {
    name: 'tempogramRatio',
    fn: sonare.tempogramRatio,
    params: 'tempogramData winLength? sampleRate? hopLength? factors? ...options?',
    values: {
      options: { validate: false },
      tempogramData: TG.data,
      winLength: TG.winLength,
      sampleRate: ALT_SR,
      hopLength: 256,
      factors: new Float32Array([0.5, 1, 2]),
    },
    invalid: { hopLength: 0 },
    inert: {
      sampleRate: 'the ratio is taken between lag bins',
      hopLength: 'the ratio is taken between lag bins',
    },
  },

  {
    name: 'lufs',
    fn: sonare.lufs,
    params: 'samples sampleRate? ...options?',
    values: {
      samples: SIG,
      sampleRate: ALT_SR,
      options: { validate: false },
    },
    invalid: { sampleRate: 0 },
  },

  {
    name: 'momentaryLufs',
    fn: sonare.momentaryLufs,
    params: 'samples sampleRate? ...options?',
    values: {
      samples: SIG,
      sampleRate: ALT_SR,
      options: { validate: false },
    },
    invalid: { sampleRate: 0 },
  },

  {
    name: 'shortTermLufs',
    fn: sonare.shortTermLufs,
    params: 'samples sampleRate? ...options?',
    values: {
      samples: LONG,
      sampleRate: ALT_SR,
      options: { validate: false },
    },
    invalid: { sampleRate: 0 },
  },

  {
    name: 'piptrack',
    fn: sonare.piptrack,
    params: 'samples sampleRate? nFft? hopLength? fmin? fmax? threshold?',
    values: {
      samples: SIG,
      sampleRate: ALT_SR,
      nFft: 1024,
      hopLength: 256,
      fmin: 200,
      fmax: 1000,
      threshold: 0.3,
    },
    invalid: { hopLength: 0 },
  },

  {
    name: 'pitchYin',
    fn: sonare.pitchYin,
    params: 'samples sampleRate? frameLength? hopLength? fmin? fmax? threshold? fillNa?',
    values: {
      samples: TONE_NOISY_TAIL,
      sampleRate: ALT_SR,
      frameLength: 1024,
      hopLength: 256,
      fmin: 250,
      fmax: 500,
      threshold: 0.5,
      fillNa: true,
    },
    invalid: { hopLength: 0 },
    inert: { fillNa: 'the core YIN reads no fill_na; only pYIN does' },
  },

  {
    name: 'pitchPyin',
    fn: sonare.pitchPyin,
    params: 'samples sampleRate? frameLength? hopLength? fmin? fmax? threshold? fillNa?',
    values: {
      samples: TONE,
      sampleRate: ALT_SR,
      frameLength: 1024,
      hopLength: 256,
      fmin: 150,
      fmax: 400,
      threshold: 0.2,
      fillNa: true,
    },
    invalid: { hopLength: 0 },
  },

  {
    name: 'pitchTuning',
    fn: sonare.pitchTuning,
    params: 'frequencies resolution? binsPerOctave?',
    values: { frequencies: new Float32Array([224, 448, 672]), resolution: 0.05, binsPerOctave: 24 },
    invalid: { resolution: 0 },
  },

  {
    name: 'estimateTuning',
    fn: sonare.estimateTuning,
    params: 'samples sampleRate? nFft? hopLength? resolution? binsPerOctave?',
    values: {
      samples: SIG,
      sampleRate: ALT_SR,
      nFft: 1024,
      hopLength: 64,
      resolution: 0.001,
      binsPerOctave: 24,
    },
    invalid: { hopLength: 0 },
  },

  {
    name: 'resample',
    fn: sonare.resample,
    params: 'samples srcSr targetSr',
    values: { samples: SIG, srcSr: SR, targetSr: 16000 },
    invalid: { targetSr: 0 },
  },

  {
    name: 'spectralCentroid',
    fn: sonare.spectralCentroid,
    params: 'samples sampleRate? nFft? hopLength?',
    values: { samples: SIG, sampleRate: ALT_SR, nFft: 1024, hopLength: 256 },
    invalid: { hopLength: 0 },
  },

  {
    name: 'spectralContrast',
    fn: sonare.spectralContrast,
    params: 'samples sampleRate? nFft? hopLength? nBands? fmin? quantile?',
    values: {
      samples: SIG,
      sampleRate: ALT_SR,
      nFft: 1024,
      hopLength: 256,
      nBands: 4,
      fmin: 100,
      quantile: 0.1,
    },
    invalid: { hopLength: 0 },
  },

  {
    name: 'polyFeatures',
    fn: sonare.polyFeatures,
    params: 'samples sampleRate? nFft? hopLength? order?',
    values: { samples: SIG, sampleRate: ALT_SR, nFft: 1024, hopLength: 256, order: 2 },
    invalid: { hopLength: 0 },
  },

  {
    name: 'zeroCrossings',
    fn: sonare.zeroCrossings,
    params: 'samples threshold? refMagnitude? pad? zeroPos?',
    values: { samples: SIG, threshold: 0.01, refMagnitude: true, pad: false, zeroPos: false },
    invalid: { threshold: Number.NaN },
  },

  {
    name: 'spectralBandwidth',
    fn: sonare.spectralBandwidth,
    params: 'samples sampleRate? nFft? hopLength? p?',
    values: { samples: SIG, sampleRate: ALT_SR, nFft: 1024, hopLength: 256, p: 3 },
    invalid: { hopLength: 0 },
  },

  {
    name: 'spectralRolloff',
    fn: sonare.spectralRolloff,
    params: 'samples sampleRate? nFft? hopLength? rollPercent?',
    values: { samples: SIG, sampleRate: ALT_SR, nFft: 1024, hopLength: 256, rollPercent: 0.5 },
    invalid: { hopLength: 0 },
  },

  {
    name: 'spectralFlatness',
    fn: sonare.spectralFlatness,
    params: 'samples sampleRate? nFft? hopLength?',
    values: { samples: SIG, sampleRate: ALT_SR, nFft: 1024, hopLength: 256 },
    invalid: { hopLength: 0 },
    inert: { sampleRate: 'a ratio of means over bins reads no rate' },
  },

  {
    name: 'spectralFlux',
    fn: sonare.spectralFlux,
    params: 'samples sampleRate? nFft? hopLength? lag?',
    values: { samples: SIG, sampleRate: ALT_SR, nFft: 1024, hopLength: 256, lag: 2 },
    invalid: { hopLength: 0 },
    inert: { sampleRate: 'a frame-to-frame difference reads no rate' },
  },

  {
    name: 'zeroCrossingRate',
    fn: sonare.zeroCrossingRate,
    params: 'samples sampleRate? frameLength? hopLength?',
    values: { samples: SIG, sampleRate: ALT_SR, frameLength: 1024, hopLength: 256 },
    invalid: { hopLength: 0 },
    inert: { sampleRate: 'frames are counted in samples' },
  },

  {
    name: 'rmsEnergy',
    fn: sonare.rmsEnergy,
    params: 'samples sampleRate? frameLength? hopLength?',
    values: { samples: SIG, sampleRate: ALT_SR, frameLength: 1024, hopLength: 256 },
    invalid: { hopLength: 0 },
    inert: { sampleRate: 'frames are counted in samples' },
  },

  {
    name: 'trim',
    fn: sonare.trim,
    params: 'samples sampleRate thresholdDb? frameLength? hopLength? ...options?',
    values: {
      options: { validate: false },
      samples: PADDED,
      sampleRate: ALT_SR,
      thresholdDb: -20,
      frameLength: 512,
      hopLength: 64,
    },
    invalid: { hopLength: 0 },
    inert: { sampleRate: 'frames are counted in samples' },
    controlBase: { hopLength: { thresholdDb: undefined } },
  },

  {
    name: 'stft',
    fn: sonare.stft,
    params: 'samples sampleRate? nFft? hopLength? ...options?',
    values: {
      options: { validate: false },
      samples: SIG,
      sampleRate: ALT_SR,
      nFft: 1024,
      hopLength: 256,
    },
    invalid: { hopLength: 0 },
  },

  {
    name: 'stftDb',
    fn: sonare.stftDb,
    params: 'samples sampleRate? nFft? hopLength? ...options?',
    values: {
      options: { validate: false },
      samples: SIG,
      sampleRate: ALT_SR,
      nFft: 1024,
      hopLength: 256,
    },
    invalid: { hopLength: 0 },
    inert: { sampleRate: 'bins and frames are counted, not labelled in Hz' },
  },

  {
    name: 'chromaCens',
    fn: sonare.chromaCens,
    params: 'samples sampleRate? hopLength? nChroma? binsPerOctave? ...options?',
    values: {
      options: { validate: false },
      samples: SHORT,
      sampleRate: ALT_SR,
      hopLength: 1024,
      binsPerOctave: 24,
    },
    invalid: { hopLength: 0 },
  },

  {
    name: 'chromaCqt',
    fn: sonare.chromaCqt,
    params: 'samples sampleRate? hopLength? nChroma? binsPerOctave? ...options?',
    values: {
      options: { validate: false },
      samples: SHORT,
      sampleRate: ALT_SR,
      hopLength: 1024,
      binsPerOctave: 24,
    },
    invalid: { hopLength: 0 },
  },

  {
    name: 'bassChroma',
    fn: sonare.bassChroma,
    params: 'samples sampleRate? hopLength? nChroma? ...options?',
    values: {
      options: { validate: false },
      samples: SHORT,
      sampleRate: ALT_SR,
      hopLength: 1024,
    },
    invalid: { hopLength: 0 },
  },

  {
    name: 'melSpectrogram',
    fn: sonare.melSpectrogram,
    params: 'samples sampleRate? nFft? hopLength? nMels? fmin? fmax? htk? ...options?',
    values: {
      options: { validate: false },
      samples: SIG,
      sampleRate: ALT_SR,
      nFft: 1024,
      hopLength: 256,
      nMels: 64,
      fmin: 50,
      fmax: 8000,
      htk: true,
    },
    invalid: { hopLength: 0 },
  },

  {
    name: 'mfcc',
    fn: sonare.mfcc,
    params:
      'samples sampleRate? nFft? hopLength? nMels? nMfcc? fmin? fmax? htk? lifter? ...options?',
    values: {
      options: { validate: false },
      samples: SIG,
      sampleRate: ALT_SR,
      nFft: 1024,
      hopLength: 256,
      nMels: 64,
      nMfcc: 13,
      fmin: 50,
      fmax: 8000,
      htk: true,
      lifter: 22,
    },
    invalid: { hopLength: 0 },
  },

  {
    name: 'melDelta',
    fn: sonare.melDelta,
    params: 'features nFeatures nFrames width?',
    values: { features: MFCC.coefficients, nFeatures: MFCC.nMfcc, nFrames: MFCC.nFrames, width: 5 },
    invalid: { width: 4 },
  },

  {
    name: 'reassignedSpectrogram',
    fn: sonare.reassignedSpectrogram,
    params: 'samples sampleRate? nFft? hopLength? refPower? fillNan?',
    values: {
      samples: SIG,
      sampleRate: ALT_SR,
      nFft: 1024,
      hopLength: 256,
      refPower: 0.5,
      fillNan: true,
    },
    invalid: { hopLength: 0 },
  },

  {
    name: 'chroma',
    fn: sonare.chroma,
    params: 'samples sampleRate? nFft? hopLength? ...options?',
    values: {
      options: { validate: false },
      samples: SIG,
      sampleRate: ALT_SR,
      nFft: 1024,
      hopLength: 256,
    },
    invalid: { hopLength: 0 },
  },

  {
    name: 'normalize',
    fn: sonare.normalize,
    params: 'samples sampleRate targetDb? mode? ...options?',
    values: {
      options: { validate: false },
      samples: SIG,
      sampleRate: ALT_SR,
      targetDb: -6,
      mode: 'rms',
    },
    invalid: { mode: 'loud' },
    inert: { sampleRate: 'peak and RMS gain read no rate' },
  },

  {
    name: 'masteringChain',
    fn: sonare.masteringChain,
    params: 'samples sampleRate? config? onProgress?',
    values: {
      samples: SIG,
      sampleRate: ALT_SR,
      config: { loudness: { targetLufs: -18 } },
      onProgress: PROGRESS,
    },
    invalid: { sampleRate: 0 },
  },

  {
    name: 'masteringChainStereo',
    fn: sonare.masteringChainStereo,
    params: 'left right sampleRate? config? onProgress?',
    values: {
      left: SIG,
      right: SIG2,
      sampleRate: ALT_SR,
      config: { loudness: { targetLufs: -18 } },
      onProgress: PROGRESS,
    },
    invalid: { sampleRate: 0 },
  },

  {
    name: 'masterAudio',
    fn: sonare.masterAudio,
    params: 'samples sampleRate? presetName? overrides? onProgress?',
    rename: { presetName: 'preset' },
    values: {
      samples: SIG,
      sampleRate: ALT_SR,
      presetName: 'speech',
      overrides: { loudness: { targetLufs: -20 } },
      onProgress: PROGRESS,
    },
    invalid: { presetName: 'no-such-preset' },
  },

  {
    name: 'masterAudioStereo',
    fn: sonare.masterAudioStereo,
    params: 'left right sampleRate? presetName? overrides? onProgress?',
    rename: { presetName: 'preset' },
    values: {
      left: SIG,
      right: SIG2,
      sampleRate: ALT_SR,
      presetName: 'speech',
      overrides: { loudness: { targetLufs: -20 } },
      onProgress: PROGRESS,
    },
    invalid: { presetName: 'no-such-preset' },
  },

  {
    name: 'mastering',
    fn: sonare.mastering,
    params: 'samples sampleRate? ...options?',
    values: { samples: SIG, sampleRate: ALT_SR, options: { targetLufs: -18, ceilingDb: -3 } },
    invalid: { sampleRate: 0 },
  },

  {
    name: 'masteringProcess',
    fn: sonare.masteringProcess,
    params: 'processorName samples sampleRate? params?',
    values: {
      processorName: 'dynamics.compressor',
      samples: SIG,
      sampleRate: ALT_SR,
      params: { ratio: 4, thresholdDb: -30 },
    },
    invalid: { processorName: 'no.such' },
  },

  {
    name: 'masteringProcessStereo',
    fn: sonare.masteringProcessStereo,
    params: 'processorName left right sampleRate? params?',
    values: {
      processorName: 'stereo.imager',
      left: SIG,
      right: SIG2,
      sampleRate: ALT_SR,
      params: { width: 1.6 },
    },
    invalid: { processorName: 'no.such' },
  },

  {
    name: 'masteringPairProcess',
    fn: sonare.masteringPairProcess,
    params: 'processorName source reference sampleRate? params?',
    values: {
      processorName: 'match.abCrossfade',
      source: SIG,
      reference: SIG2,
      sampleRate: ALT_SR,
      params: { mix: 0.3 },
    },
    invalid: { processorName: 'no.such' },
  },

  {
    name: 'masteringPairProcessStereo',
    fn: sonare.masteringPairProcessStereo,
    params: 'processorName sourceLeft sourceRight referenceLeft referenceRight sampleRate? params?',
    values: {
      processorName: 'match.abCrossfade',
      sourceLeft: SIG,
      sourceRight: SIG2,
      referenceLeft: SIG2,
      referenceRight: SIG,
      sampleRate: ALT_SR,
      params: { mix: 0.3 },
    },
    invalid: { processorName: 'no.such' },
  },

  {
    name: 'masteringPairAnalyze',
    fn: sonare.masteringPairAnalyze,
    params: 'analysisName source reference sampleRate? params?',
    values: {
      analysisName: 'match.tonalBalanceLogBands',
      source: SIG,
      reference: SIG2,
      sampleRate: ALT_SR,
      params: { bandsPerOctave: 2, highHz: 8000 },
    },
    invalid: { analysisName: 'no.such' },
  },

  {
    name: 'masteringStereoAnalyze',
    fn: sonare.masteringStereoAnalyze,
    params: 'analysisName left right sampleRate? params?',
    values: {
      analysisName: 'stereo.monoCompatCheckLogBands',
      left: SIG,
      right: SIG2,
      sampleRate: ALT_SR,
      params: { bandsPerOctave: 2, highHz: 8000 },
    },
    invalid: { analysisName: 'no.such' },
  },

  {
    name: 'masteringAssistantSuggest',
    fn: sonare.masteringAssistantSuggest,
    params: 'samples sampleRate? params?',
    values: { samples: SIG, sampleRate: ALT_SR, params: { targetLufs: -13 } },
    invalid: { sampleRate: 0 },
  },

  {
    name: 'masteringAudioProfile',
    fn: sonare.masteringAudioProfile,
    params: 'samples sampleRate? params?',
    values: { samples: SIG, sampleRate: ALT_SR, params: { nFft: 1024, hopLength: 256 } },
    invalid: { sampleRate: 0 },
  },

  {
    name: 'masteringStreamingPreview',
    fn: sonare.masteringStreamingPreview,
    params: 'samples sampleRate? platforms?',
    values: {
      samples: SIG,
      sampleRate: ALT_SR,
      platforms: [{ name: 'quiet', targetLufs: -24, ceilingDb: -3 }],
    },
    invalid: { sampleRate: 0 },
  },

  {
    name: 'masteringDynamicsCompressor',
    fn: sonare.masteringDynamicsCompressor,
    params: 'samples sampleRate? ...options?',
    values: { samples: SIG, sampleRate: ALT_SR, options: { thresholdDb: -30, ratio: 6 } },
    invalid: { sampleRate: 0 },
  },

  {
    name: 'masteringDynamicsGate',
    fn: sonare.masteringDynamicsGate,
    params: 'samples sampleRate? ...options?',
    values: { samples: SIG, sampleRate: ALT_SR, options: { thresholdDb: -20, rangeDb: -30 } },
    invalid: { sampleRate: 0 },
  },

  {
    name: 'masteringDynamicsTransientShaper',
    fn: sonare.masteringDynamicsTransientShaper,
    params: 'samples sampleRate? ...options?',
    values: { samples: SIG, sampleRate: ALT_SR, options: { attackGainDb: 6, sustainGainDb: -3 } },
    invalid: { sampleRate: 0 },
  },

  {
    name: 'meteringPeakDb',
    fn: sonare.meteringPeakDb,
    params: 'samples sampleRate? ...options?',
    values: {
      samples: SIG,
      sampleRate: ALT_SR,
      options: { validate: false },
    },
    invalid: { sampleRate: 0 },
    inert: { sampleRate: 'a sample peak reads no rate' },
  },

  {
    name: 'meteringRmsDb',
    fn: sonare.meteringRmsDb,
    params: 'samples sampleRate? ...options?',
    values: {
      samples: SIG,
      sampleRate: ALT_SR,
      options: { validate: false },
    },
    invalid: { sampleRate: 0 },
    inert: { sampleRate: 'a whole-buffer RMS reads no rate' },
  },

  {
    name: 'meteringSilenceRatio',
    fn: sonare.meteringSilenceRatio,
    params: 'samples sampleRate? thresholdDb? frameLength? hopLength? ...options?',
    values: {
      samples: SIG,
      sampleRate: ALT_SR,
      thresholdDb: -20,
      frameLength: 256,
      hopLength: 64,
      options: { validate: false },
    },
    invalid: { hopLength: 0 },
    inert: { sampleRate: 'frames are counted in samples' },
  },

  {
    name: 'meteringCrestFactorDb',
    fn: sonare.meteringCrestFactorDb,
    params: 'samples sampleRate? ...options?',
    values: {
      samples: SIG,
      sampleRate: ALT_SR,
      options: { validate: false },
    },
    invalid: { sampleRate: 0 },
    inert: { sampleRate: 'a peak-to-RMS ratio reads no rate' },
  },

  {
    name: 'meteringDcOffset',
    fn: sonare.meteringDcOffset,
    params: 'samples sampleRate? ...options?',
    values: {
      samples: SIG,
      sampleRate: ALT_SR,
      options: { validate: false },
    },
    invalid: { sampleRate: 0 },
    inert: { sampleRate: 'a mean reads no rate' },
  },

  {
    name: 'meteringTruePeakDb',
    fn: sonare.meteringTruePeakDb,
    params: 'samples sampleRate? oversampleFactor? ...options?',
    values: {
      samples: SIG,
      sampleRate: ALT_SR,
      oversampleFactor: 2,
      options: { validate: false },
    },
    invalid: { oversampleFactor: 3 },
    inert: { sampleRate: 'oversampling is rate-relative' },
  },

  {
    name: 'meteringDetectClipping',
    fn: sonare.meteringDetectClipping,
    params: 'samples sampleRate? ...options?',
    values: { samples: SIG, sampleRate: ALT_SR, options: { threshold: 0.1, minRegionSamples: 2 } },
    invalid: { sampleRate: 0 },
    inert: { sampleRate: 'clipped regions are counted in samples' },
  },

  {
    name: 'meteringDynamicRange',
    fn: sonare.meteringDynamicRange,
    params: 'samples sampleRate? ...options?',
    values: { samples: SIG, sampleRate: ALT_SR, options: { windowSec: 0.1, hopSec: 0.05 } },
    invalid: { sampleRate: 0 },
  },

  {
    name: 'meteringStereoCorrelation',
    fn: sonare.meteringStereoCorrelation,
    params: 'left right sampleRate? ...options?',
    values: {
      left: SIG,
      right: SIG2,
      sampleRate: ALT_SR,
      options: { validate: false },
    },
    invalid: { sampleRate: 0 },
    inert: { sampleRate: 'a correlation reads no rate' },
  },

  {
    name: 'meteringStereoWidth',
    fn: sonare.meteringStereoWidth,
    params: 'left right sampleRate? ...options?',
    values: {
      left: SIG,
      right: SIG2,
      sampleRate: ALT_SR,
      options: { validate: false },
    },
    invalid: { sampleRate: 0 },
    inert: { sampleRate: 'a mid/side ratio reads no rate' },
  },

  {
    name: 'meteringVectorscope',
    fn: sonare.meteringVectorscope,
    params: 'left right sampleRate? ...options?',
    values: { left: SIG, right: SIG2, sampleRate: ALT_SR, options: { maxPoints: 64 } },
    invalid: { sampleRate: 0 },
    inert: { sampleRate: 'scope points are sample pairs' },
  },

  {
    name: 'meteringPhaseScope',
    fn: sonare.meteringPhaseScope,
    params: 'left right sampleRate? ...options?',
    values: { left: SIG, right: SIG2, sampleRate: ALT_SR, options: { maxPoints: 64 } },
    invalid: { sampleRate: 0 },
    inert: { sampleRate: 'scope points are sample pairs' },
  },

  {
    name: 'meteringSpectrum',
    fn: sonare.meteringSpectrum,
    params: 'samples sampleRate? ...options?',
    values: {
      samples: SIG,
      sampleRate: ALT_SR,
      options: { nFft: 1024, applyOctaveSmoothing: true },
    },
    invalid: { sampleRate: 0 },
  },

  {
    name: 'meteringSpectrumFrame',
    fn: sonare.meteringSpectrumFrame,
    params: 'samples sampleRate? frameOffset? ...options?',
    values: { samples: SIG, sampleRate: ALT_SR, frameOffset: 4096, options: { nFft: 1024 } },
    invalid: { sampleRate: 0 },
  },

  {
    name: 'waveformPeaks',
    fn: sonare.waveformPeaks,
    params: 'samples channels ...options?',
    values: { samples: STEREO, channels: 2, options: { samplesPerBucket: 256 } },
    invalid: { channels: 0 },
  },

  {
    name: 'waveformPeakPyramid',
    fn: sonare.waveformPeakPyramid,
    params: 'samples channels ...options?',
    values: { samples: STEREO, channels: 2, options: { samplesPerBucketLevels: [128, 1024] } },
    invalid: { channels: 0 },
  },

  {
    name: 'mixStereo',
    fn: sonare.mixStereo,
    params: 'leftChannels rightChannels sampleRate? ...options?',
    values: {
      leftChannels: [SIG, SIG2],
      rightChannels: [SIG2, SIG],
      sampleRate: ALT_SR,
      options: { faderDb: [-6, 0], pan: [0.4, -0.4] },
    },
    invalid: { sampleRate: 0 },
  },

  {
    name: 'detectBpm',
    fn: sonare.detectBpm,
    params: 'samples sampleRate? ...options?',
    values: {
      options: { validate: false },
      samples: SIG,
      sampleRate: ALT_SR,
    },
    invalid: { sampleRate: 0 },
  },

  {
    name: 'detectKey',
    fn: sonare.detectKey,
    params: 'samples sampleRate? ...options?',
    values: { samples: SIG, sampleRate: ALT_SR, options: { hopLength: 256, useHpss: true } },
    invalid: { sampleRate: 0 },
  },

  {
    name: 'detectKeyCandidates',
    fn: sonare.detectKeyCandidates,
    params: 'samples sampleRate? ...options?',
    values: { samples: SIG, sampleRate: ALT_SR, options: { hopLength: 256, useHpss: true } },
    invalid: { sampleRate: 0 },
  },

  {
    name: 'detectOnsets',
    fn: sonare.detectOnsets,
    params: 'samples sampleRate? ...options?',
    values: { samples: SIG, sampleRate: ALT_SR, options: { hopLength: 256, backtrack: true } },
    invalid: { sampleRate: 0 },
  },

  {
    name: 'detectBeats',
    fn: sonare.detectBeats,
    params: 'samples sampleRate? ...options?',
    values: {
      options: { validate: false },
      samples: SIG,
      sampleRate: ALT_SR,
    },
    invalid: { sampleRate: 0 },
  },

  {
    name: 'detectDownbeats',
    fn: sonare.detectDownbeats,
    params: 'samples sampleRate? ...options?',
    values: {
      options: { validate: false },
      samples: SIG,
      sampleRate: ALT_SR,
    },
    invalid: { sampleRate: 0 },
  },

  {
    name: 'detectChords',
    fn: sonare.detectChords,
    params: 'samples sampleRate? ...options?',
    values: { samples: SIG, sampleRate: ALT_SR, options: { hopLength: 256 } },
    invalid: { sampleRate: 0 },
  },

  {
    name: 'chordFunctionalAnalysis',
    fn: sonare.chordFunctionalAnalysis,
    params: 'samples keyRoot keyMode? sampleRate? ...options?',
    values: {
      samples: SIG,
      keyRoot: 8,
      keyMode: 1,
      sampleRate: ALT_SR,
      options: { minDuration: 0.6 },
    },
    invalid: { sampleRate: 0 },
  },

  {
    name: 'analyze',
    fn: sonare.analyze,
    params: 'samples sampleRate? ...options?',
    values: { samples: SIG, sampleRate: ALT_SR, options: { hopLength: 256, bpmMin: 170 } },
    invalid: { sampleRate: 0 },
  },

  {
    name: 'analyzeImpulseResponse',
    fn: sonare.analyzeImpulseResponse,
    params: 'samples sampleRate? nOctaveBands? minDecayDb?',
    values: { samples: IR, sampleRate: ALT_SR, nOctaveBands: 3, minDecayDb: 15 },
    invalid: { sampleRate: 0 },
  },

  {
    name: 'detectAcoustic',
    fn: sonare.detectAcoustic,
    params: 'samples sampleRate? ...options?',
    values: { samples: IR, sampleRate: ALT_SR, options: { nOctaveBands: 3 } },
    invalid: { sampleRate: 0 },
  },

  {
    name: 'estimateRoom',
    fn: sonare.estimateRoom,
    params: 'samples sampleRate? ...options?',
    values: { samples: IR, sampleRate: ALT_SR, options: { referenceAbsorption: 0.5 } },
    invalid: { sampleRate: 0 },
  },

  {
    name: 'roomGeometryFromEstimate',
    fn: sonare.roomGeometryFromEstimate,
    params: 'estimate ...options?',
    values: {
      estimate: {
        lengthM: 6,
        widthM: 4.5,
        heightM: 3,
        bandAbsorption: new Float32Array([0.1, 0.12, 0.15, 0.2, 0.25, 0.3]),
      },
      options: { source: { x: 1, y: 1, z: 1.2 }, listener: { x: 3, y: 2, z: 1.7 } },
    },
    invalid: { estimate: 'room' },
  },
  {
    name: 'roomMorph',
    fn: sonare.roomMorph,
    params: 'samples sampleRate ...options?',
    values: {
      samples: SIG,
      sampleRate: SR,
      options: { wet: 0.3 },
    },
    invalid: { sampleRate: 0 },
  },

  {
    name: 'analyzeWithProgress',
    fn: sonare.analyzeWithProgress,
    params: 'samples sampleRate onProgress options?',
    values: { samples: SIG, sampleRate: SR, onProgress: PROGRESS, options: { hopLength: 256 } },
    invalid: { sampleRate: 0 },
  },

  {
    name: 'analyzeBpm',
    fn: sonare.analyzeBpm,
    params: 'samples sampleRate? ...options?',
    values: { samples: SIG, sampleRate: ALT_SR, options: { hopLength: 256, maxCandidates: 2 } },
    invalid: { sampleRate: 0 },
  },

  {
    name: 'analyzeRhythm',
    fn: sonare.analyzeRhythm,
    params: 'samples sampleRate? ...options?',
    values: { samples: SIG, sampleRate: ALT_SR, options: { hopLength: 256 } },
    invalid: { sampleRate: 0 },
  },

  {
    name: 'analyzeDynamics',
    fn: sonare.analyzeDynamics,
    params: 'samples sampleRate? ...options?',
    values: { samples: SIG, sampleRate: ALT_SR, options: { hopLength: 256, windowSec: 0.2 } },
    invalid: { sampleRate: 0 },
  },

  {
    name: 'analyzeTimbre',
    fn: sonare.analyzeTimbre,
    params: 'samples sampleRate? ...options?',
    values: { samples: SIG, sampleRate: ALT_SR, options: { hopLength: 256, nMfcc: 20 } },
    invalid: { sampleRate: 0 },
  },

  {
    name: 'masteringRepairDereverbClassical',
    fn: sonare.masteringRepairDereverbClassical,
    params: 'samples sampleRate? ...options?',
    values: { samples: SIG, sampleRate: ALT_SR, options: { t60Sec: 1.2, nFft: 512 } },
    invalid: { sampleRate: 0 },
  },

  {
    name: 'masteringRepairDereverbConfigForRoom',
    fn: sonare.masteringRepairDereverbConfigForRoom,
    params: 'estimate ...config?',
    values: {
      estimate: {
        volume: 2500,
        lengthM: 0,
        widthM: 0,
        heightM: 0,
        drrDb: 0,
        confidence: 0,
        bandAbsorption: new Float32Array(0),
        rt60Bands: new Float32Array([0.9, 0.9, 1, 1.2, 0.9, 0.9]),
      },
      config: { nFft: 512, attenuation: 0.3 },
    },
    invalid: { estimate: 'room' },
  },

  {
    name: 'masteringRepairDeclick',
    fn: sonare.masteringRepairDeclick,
    params: 'samples sampleRate? ...options?',
    values: { samples: CLICKY, sampleRate: ALT_SR, options: { lpcOrder: 4, maxClickSamples: 1 } },
    invalid: { sampleRate: 0 },
    inert: { sampleRate: 'clicks are found and refilled in samples' },
    controlBase: { lpcOrder: { options: { lpcOrder: 4 } } },
  },

  {
    name: 'masteringRepairDeclip',
    fn: sonare.masteringRepairDeclip,
    params: 'samples sampleRate? ...options?',
    values: { samples: CLIPPED, sampleRate: ALT_SR, options: { clipThreshold: 0.1, lpcOrder: 8 } },
    invalid: { sampleRate: 0 },
    inert: { sampleRate: 'clipped runs are found and refilled in samples' },
  },

  {
    name: 'masteringRepairDecrackle',
    fn: sonare.masteringRepairDecrackle,
    params: 'samples sampleRate? ...options?',
    values: { samples: CLICKY, sampleRate: ALT_SR, options: { threshold: 5 } },
    invalid: { sampleRate: 0 },
    inert: { sampleRate: 'crackle is found in samples' },
  },

  {
    name: 'masteringRepairDenoiseClassical',
    fn: sonare.masteringRepairDenoiseClassical,
    params: 'samples sampleRate? ...options?',
    values: { samples: SIG, sampleRate: ALT_SR, options: { reductionDb: 20, nFft: 512 } },
    invalid: { sampleRate: 0 },
    inert: { sampleRate: 'the STFT runs in bins and frames' },
  },

  {
    name: 'masteringRepairDehum',
    fn: sonare.masteringRepairDehum,
    params: 'samples sampleRate? ...options?',
    values: { samples: HUMMED, sampleRate: ALT_SR, options: { fundamentalHz: 60, harmonics: 2 } },
    invalid: { sampleRate: 0 },
  },

  {
    name: 'masteringRepairTrimSilence',
    fn: sonare.masteringRepairTrimSilence,
    params: 'samples sampleRate? ...options?',
    values: { samples: SIG, sampleRate: ALT_SR, options: { paddingSamples: 300 } },
    invalid: { sampleRate: 0 },
    inert: { sampleRate: 'the threshold mode counts samples, not seconds' },
  },

  // WASM-only positional overloads
  {
    name: 'decomposeWithInit',
    fn: sonare.decomposeWithInit,
    params: 's nFeatures nFrames nComponents nIter? beta? init?',
    values: {
      s: SMALL,
      nFeatures: 8,
      nFrames: 10,
      nComponents: 2,
      nIter: 20,
      beta: 1,
      init: 'nndsvd',
    },
    invalid: { nComponents: -1 },
  },
  {
    name: 'masteringChainWithProgress',
    fn: sonare.masteringChainWithProgress,
    params: 'samples sampleRate? config? onProgress?',
    values: {
      samples: SIG,
      sampleRate: ALT_SR,
      config: { loudness: { targetLufs: -18 } },
      onProgress: PROGRESS,
    },
    invalid: { sampleRate: 0 },
  },
  {
    name: 'masteringChainStereoWithProgress',
    fn: sonare.masteringChainStereoWithProgress,
    params: 'left right sampleRate? config? onProgress?',
    values: {
      left: SIG,
      right: SIG2,
      sampleRate: ALT_SR,
      config: { loudness: { targetLufs: -18 } },
      onProgress: PROGRESS,
    },
    invalid: { sampleRate: 0 },
  },
  {
    name: 'masterAudioWithProgress',
    fn: sonare.masterAudioWithProgress,
    params: 'samples sampleRate? presetName? overrides? onProgress?',
    rename: { presetName: 'preset' },
    values: {
      samples: SIG,
      sampleRate: ALT_SR,
      presetName: 'speech',
      overrides: { loudness: { targetLufs: -20 } },
      onProgress: PROGRESS,
    },
    invalid: { presetName: 'no-such-preset' },
  },
  {
    name: 'masterAudioStereoWithProgress',
    fn: sonare.masterAudioStereoWithProgress,
    params: 'left right sampleRate? presetName? overrides? onProgress?',
    rename: { presetName: 'preset' },
    values: {
      left: SIG,
      right: SIG2,
      sampleRate: ALT_SR,
      presetName: 'speech',
      overrides: { loudness: { targetLufs: -20 } },
      onProgress: PROGRESS,
    },
    invalid: { presetName: 'no-such-preset' },
  },
  {
    name: 'meteringVectorscopeDecimated',
    fn: sonare.meteringVectorscopeDecimated,
    params: 'left right sampleRate? maxPoints? ...options?',
    values: {
      left: SIG,
      right: SIG2,
      sampleRate: ALT_SR,
      maxPoints: 64,
      options: { validate: false },
    },
    invalid: { sampleRate: 0 },
    inert: { sampleRate: 'scope points are sample pairs' },
  },
  {
    name: 'meteringPhaseScopeDecimated',
    fn: sonare.meteringPhaseScopeDecimated,
    params: 'left right sampleRate? maxPoints? ...options?',
    values: {
      left: SIG,
      right: SIG2,
      sampleRate: ALT_SR,
      maxPoints: 64,
      options: { validate: false },
    },
    invalid: { sampleRate: 0 },
    inert: { sampleRate: 'scope points are sample pairs' },
  },
  {
    name: 'masteringRepairDeclickStereo',
    fn: sonare.masteringRepairDeclickStereo,
    params: 'left right sampleRate? ...config?',
    values: {
      left: CLICKY,
      right: SIG2,
      sampleRate: ALT_SR,
      config: { lpcOrder: 4, maxClickSamples: 1 },
    },
    invalid: { sampleRate: 0 },
    inert: { sampleRate: 'clicks are found and refilled in samples' },
    controlBase: { lpcOrder: { config: { lpcOrder: 4 } } },
  },
  {
    name: 'masteringRepairDeclipStereo',
    fn: sonare.masteringRepairDeclipStereo,
    params: 'left right sampleRate? ...config?',
    values: {
      left: CLIPPED,
      right: CLIPPED,
      sampleRate: ALT_SR,
      config: { clipThreshold: 0.1, lpcOrder: 8 },
    },
    invalid: { sampleRate: 0 },
    inert: { sampleRate: 'clipped runs are found and refilled in samples' },
  },
  {
    name: 'masteringRepairDecrackleStereo',
    fn: sonare.masteringRepairDecrackleStereo,
    params: 'left right sampleRate? ...config?',
    values: { left: CLICKY, right: CLICKY, sampleRate: ALT_SR, config: { threshold: 5 } },
    invalid: { sampleRate: 0 },
    inert: { sampleRate: 'crackle is found in samples' },
  },
  {
    name: 'masteringRepairDehumStereo',
    fn: sonare.masteringRepairDehumStereo,
    params: 'left right sampleRate? ...config?',
    values: {
      left: HUMMED,
      right: HUMMED,
      sampleRate: ALT_SR,
      config: { fundamentalHz: 60, harmonics: 2 },
    },
    invalid: { sampleRate: 0 },
  },
  {
    name: 'masteringRepairDenoiseClassicalStereo',
    fn: sonare.masteringRepairDenoiseClassicalStereo,
    params: 'left right sampleRate? ...config?',
    values: { left: SIG, right: SIG2, sampleRate: ALT_SR, config: { reductionDb: 20, nFft: 512 } },
    invalid: { sampleRate: 0 },
    inert: { sampleRate: 'the STFT runs in bins and frames' },
  },
  {
    name: 'masteringRepairDenoiseClassicalLinked',
    fn: sonare.masteringRepairDenoiseClassicalLinked,
    params: 'channels sampleRate? ...config?',
    values: { channels: [SIG, SIG2], sampleRate: ALT_SR, config: { reductionDb: 20, nFft: 512 } },
    invalid: { sampleRate: 0 },
    inert: { sampleRate: 'the STFT runs in bins and frames' },
  },
  {
    name: 'masteringRepairDereverbClassicalStereo',
    fn: sonare.masteringRepairDereverbClassicalStereo,
    params: 'left right sampleRate? ...config?',
    values: { left: SIG, right: SIG2, sampleRate: ALT_SR, config: { t60Sec: 1.2, nFft: 512 } },
    invalid: { sampleRate: 0 },
  },
  {
    name: 'masteringRepairDereverbClassicalLinked',
    fn: sonare.masteringRepairDereverbClassicalLinked,
    params: 'channels sampleRate? ...config?',
    values: { channels: [SIG, SIG2], sampleRate: ALT_SR, config: { t60Sec: 1.2, nFft: 512 } },
    invalid: { sampleRate: 0 },
  },
  {
    name: 'masteringRepairDetectReverb',
    fn: sonare.masteringRepairDetectReverb,
    params: 'samples sampleRate? ...options?',
    values: { samples: SIG, sampleRate: ALT_SR, options: { nFft: 512, lateDelayMs: 20 } },
    invalid: { sampleRate: 0 },
  },
  {
    name: 'masteringRepairDetectClicks',
    fn: sonare.masteringRepairDetectClicks,
    params: 'samples sampleRate? ...options?',
    values: { samples: CLICKY, sampleRate: ALT_SR, options: { maxClickSamples: 1 } },
    invalid: { sampleRate: 0 },
    inert: { sampleRate: 'clicks are found in samples' },
  },
  {
    name: 'masteringRepairDetectClipping',
    fn: sonare.masteringRepairDetectClipping,
    params: 'samples sampleRate? ...options?',
    values: { samples: CLIPPED, sampleRate: ALT_SR, options: { clipThreshold: 0.1 } },
    invalid: { sampleRate: 0 },
    inert: { sampleRate: 'clipped runs are found in samples' },
  },
  {
    name: 'masteringRepairDetectCrackle',
    fn: sonare.masteringRepairDetectCrackle,
    params: 'samples sampleRate? ...options?',
    values: { samples: CLICKY, sampleRate: ALT_SR, options: { threshold: 5 } },
    invalid: { sampleRate: 0 },
    inert: { sampleRate: 'crackle is found in samples' },
  },
  {
    name: 'masteringRepairDetectNoiseFloor',
    fn: sonare.masteringRepairDetectNoiseFloor,
    params: 'samples sampleRate? ...options?',
    values: { samples: SIG, sampleRate: ALT_SR, options: { nFft: 512 } },
    invalid: { sampleRate: 0 },
  },
  {
    name: 'masteringRepairDetectHum',
    fn: sonare.masteringRepairDetectHum,
    params: 'samples sampleRate? ...options?',
    values: {
      samples: HUMMED,
      sampleRate: ALT_SR,
      options: { fundamentalHz: 120, searchRangeHz: 10 },
    },
    invalid: { sampleRate: 0 },
  },
  {
    name: 'masteringRepairNoiseBandBins',
    fn: sonare.masteringRepairNoiseBandBins,
    params: 'nFft? sampleRate?',
    values: { nFft: 512, sampleRate: ALT_SR },
    invalid: { nFft: 0 },
  },
  {
    name: 'masteringRepairTrimSilenceStereo',
    fn: sonare.masteringRepairTrimSilenceStereo,
    params: 'left right sampleRate? ...config?',
    values: { left: PADDED, right: PADDED, sampleRate: ALT_SR, config: { paddingSamples: 300 } },
    invalid: { sampleRate: 0 },
    inert: { sampleRate: 'the threshold mode counts samples, not seconds' },
  },
  {
    name: 'masteringRepairDetectTrimRange',
    fn: sonare.masteringRepairDetectTrimRange,
    params: 'samples sampleRate? ...options?',
    values: { samples: PADDED, sampleRate: ALT_SR, options: { paddingSamples: 300 } },
    invalid: { sampleRate: 0 },
    inert: { sampleRate: 'the threshold mode counts samples, not seconds' },
  },
  {
    name: 'masteringRepairDetectTrimRangeStereo',
    fn: sonare.masteringRepairDetectTrimRangeStereo,
    params: 'left right sampleRate? ...config?',
    values: { left: PADDED, right: PADDED, sampleRate: ALT_SR, config: { paddingSamples: 300 } },
    invalid: { sampleRate: 0 },
    inert: { sampleRate: 'the threshold mode counts samples, not seconds' },
  },
];

/**
 * Every exported function that declares a request overload next to a
 * positional one, read from the sources the way the parity tool reads them.
 */
function overloadedFunctions(): string[] {
  const dir = join(import.meta.dirname, '../src');
  const names = new Set<string>();
  for (const file of readdirSync(dir).filter((f) => f.endsWith('.ts'))) {
    const source = readFileSync(join(dir, file), 'utf8');
    const heads = [...source.matchAll(/^export (?:async )?function (\w+)\(\s*([\w]+)\??:/gm)];
    const byName = new Map<string, string[]>();
    for (const [, name, first] of heads) {
      byName.set(name, [...(byName.get(name) ?? []), first]);
    }
    for (const [name, firsts] of byName) {
      // Request overload + positional overload + implementation.
      if (firsts[0] === 'request' && firsts.length > 2) {
        names.add(name);
      }
    }
  }
  return [...names].sort();
}

describe('request-object equivalence', () => {
  it('has one row per exported request-object overload', () => {
    expect(ROWS.map((row) => row.name).sort()).toEqual(overloadedFunctions());
  });

  describe.each(ROWS)('$name', (row) => {
    const specs = specsOf(row);
    const requiredOnly = Object.fromEntries(
      specs.filter((spec) => !spec.optional).map((spec) => [spec.name, row.values[spec.name]]),
    );

    it('returns the positional result for non-default values', async () => {
      const positional = await run(row, specs, row.values, 'positional');
      expect(positional).not.toHaveProperty('error');
      expect(await run(row, specs, row.values, 'request')).toEqual(positional);
    });

    it('shares the positional defaults', async () => {
      expect(await run(row, specs, requiredOnly, 'request')).toEqual(
        await run(row, specs, requiredOnly, 'positional'),
      );
    });

    it('moves the result for every field it passes', async () => {
      const full = await run(row, specs, row.values, 'positional');
      const unmoved: string[] = [];
      for (const field of fieldsOf(row, specs)) {
        const label = field.key ?? field.param;
        const masked = { ...row.values, ...row.controlBase?.[label] };
        // `validate` only decides which layer refuses a non-finite sample.
        const base = label === 'validate' ? poisoned(specs, masked) : masked;
        const reference =
          label === 'validate' || row.controlBase?.[label]
            ? await run(row, specs, base, 'positional')
            : full;
        if (
          isDeepStrictEqual(await run(row, specs, without(base, field), 'positional'), reference)
        ) {
          unmoved.push(label);
        }
      }
      expect(unmoved.join(', ')).toBe('');
    });

    it('refuses an invalid value identically in both forms', async () => {
      const bad = { ...row.values, ...row.invalid };
      const positional = await run(row, specs, bad, 'positional');
      expect(positional).toHaveProperty('error');
      expect(await run(row, specs, bad, 'request')).toEqual(positional);
    });
  });
});
