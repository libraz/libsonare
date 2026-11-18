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
 * A row marked `requestOnly` covers a function with no positional form: the same
 * checks run on the request form alone.
 *
 * The inventory is read from the sources, so an overload with no row fails.
 */

import { readdirSync, readFileSync } from 'node:fs';
import { join } from 'node:path';
import { isDeepStrictEqual } from 'node:util';
import { describe, expect, it } from 'vitest';
import type { PolyphonicAnalysis } from '../src/index.js';
import * as sonare from '../src/index.js';

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
/** An exponentially decaying noise burst, for the impulse-response readers. */
const IR = (() => {
  const noise = lcg(3);
  return Float32Array.from({ length: SR }, (_, i) => noise() * Math.exp(-i / (0.08 * SR)));
})();

const ST = sonare.stft(SHORT, SR, { nFft: 512, hopLength: 128 });
const MEL = sonare.melSpectrogram(SHORT, SR, { nFft: 512, hopLength: 128, nMels: 32 });
const MFCC = sonare.mfcc(SHORT, SR, { nFft: 512, hopLength: 128, nMels: 32, nMfcc: 13 });
const CQT = sonare.cqt(SHORT, SR, { hopLength: 128, fminHz: 65.4, nBins: 36, binsPerOctave: 12 });
const VQT = sonare.vqt(SHORT, SR, {
  hopLength: 128,
  fminHz: 65.4,
  nBins: 36,
  binsPerOctave: 12,
  gamma: 2,
});
const ENV = sonare.onsetStrength(SIG, SR, { nFft: 1024, hopLength: 256 });
const TG = sonare.tempogram({ onsetEnvelope: ENV, sampleRate: SR, hopLength: 256, winLength: 64 });
const CHROMA = sonare.chroma(SIG, SR, { nFft: 1024, hopLength: 256 });
const F0 = new Float32Array(Math.ceil(SIG.length / 256) + 1).fill(220);
const SMALL = Float32Array.from({ length: 80 }, (_, i) => 0.1 + ((i * 37) % 11) / 11);

type Args = Record<string, unknown>;

/** A progress callback slot; each call gets a fresh recorder in its place. */
const PROGRESS = Symbol('progress');

interface Row {
  name: string;
  fn: (...args: never[]) => unknown;
  /**
   * The function takes a request object only. `params` then lists the request
   * fields, and the checks run the request form alone.
   */
  requestOnly?: boolean;
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
      form === 'positional' && row.requestOnly !== true
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

/**
 * The row's values with `invalid` applied: a name a positional parameter owns
 * replaces it, any other lands inside the spread options bag.
 */
function withInvalid(row: Row, specs: Spec[]): Args {
  const bad: Args = { ...row.values };
  const spread = specs.find((spec) => spec.spread);
  for (const [key, value] of Object.entries(row.invalid)) {
    if (specs.some((spec) => !spec.spread && spec.name === key) || spread === undefined) {
      bad[key] = value;
    } else {
      bad[spread.name] = { ...(bad[spread.name] as Args), [key]: value };
    }
  }
  return bad;
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

/** An F0 track with a one-semitone step, a short voiced run and a gap, for the note-set rows. */
const NOTE_FRAME_RATE = SR / 512;
const NOTE_TRACK = (() => {
  const frames = 44;
  const f0Hz = new Float32Array(frames).fill(220);
  const voicedProb = new Float32Array(frames).fill(0.9);
  for (let i = 24; i < 34; i++) {
    f0Hz[i] = 233.08;
  }
  for (let i = 4; i < 8; i++) {
    f0Hz[i] = 0;
    voicedProb[i] = 0;
  }
  // Voiced at the default threshold, unvoiced at a raised one.
  voicedProb.fill(0.6, 40);
  return { f0Hz, voicedProb, frameRate: NOTE_FRAME_RATE };
})();
const NOTE_SET = sonare.extractNotes({ samples: TONE, sampleRate: SR, ...NOTE_TRACK });
const PERCUSSIVE_EVENTS = sonare.extractPercussiveEvents({ samples: SIG, sampleRate: SR });

const ROWS: Row[] = [
  // analysis.ts
  {
    name: 'analyze',
    fn: sonare.analyze,
    params: 'samples sampleRate ...options?',
    values: { samples: SIG, sampleRate: ALT_SR, options: { hopLength: 256, bpmMin: 170 } },
    invalid: { sampleRate: 0 },
  },
  {
    name: 'analyzeAsync',
    fn: sonare.analyzeAsync,
    params: 'samples sampleRate ...options?',
    values: { samples: SIG, sampleRate: ALT_SR, options: { hopLength: 256, bpmMin: 170 } },
    invalid: { sampleRate: 0 },
  },
  {
    name: 'analyzeBpm',
    fn: sonare.analyzeBpm,
    params: 'samples sampleRate ...options?',
    values: { samples: SIG, sampleRate: ALT_SR, options: { hopLength: 256, maxCandidates: 1 } },
    invalid: { sampleRate: 0 },
  },
  {
    name: 'analyzeDynamics',
    fn: sonare.analyzeDynamics,
    params: 'samples sampleRate ...options?',
    values: { samples: SIG, sampleRate: ALT_SR, options: { hopLength: 256, windowSec: 0.2 } },
    invalid: { sampleRate: 0 },
  },
  {
    name: 'analyzeImpulseResponse',
    fn: sonare.analyzeImpulseResponse,
    params: 'samples sampleRate ...options?',
    values: { samples: IR, sampleRate: ALT_SR, options: { nOctaveBands: 3, minDecayDb: 15 } },
    invalid: { sampleRate: 0 },
  },
  {
    name: 'analyzeMelody',
    fn: sonare.analyzeMelody,
    params: 'samples sampleRate ...options?',
    values: { samples: SIG, sampleRate: ALT_SR, options: { hopLength: 128, f0MinHz: 150 } },
    invalid: { sampleRate: 0 },
  },
  {
    name: 'analyzeRhythm',
    fn: sonare.analyzeRhythm,
    params: 'samples sampleRate ...options?',
    values: { samples: SIG, sampleRate: ALT_SR, options: { hopLength: 256 } },
    invalid: { sampleRate: 0 },
  },
  {
    name: 'analyzeSections',
    fn: sonare.analyzeSections,
    params: 'samples sampleRate ...options?',
    values: {
      samples: LONG8,
      sampleRate: ALT_SR,
      options: { hopLength: 1024, minSectionSec: 1, threshold: 1 },
    },
    invalid: { sampleRate: 0 },
    // A threshold high enough to move the result leaves one section, which no hop or minimum length moves.
    controlBase: {
      hopLength: { options: { hopLength: 1024, minSectionSec: 1 } },
      minSectionSec: { options: { hopLength: 1024, minSectionSec: 1 } },
    },
  },
  {
    name: 'analyzeTimbre',
    fn: sonare.analyzeTimbre,
    params: 'samples sampleRate ...options?',
    values: { samples: SIG, sampleRate: ALT_SR, options: { hopLength: 256, nMfcc: 20 } },
    invalid: { sampleRate: 0 },
  },
  {
    name: 'transcribe',
    fn: sonare.transcribe,
    params: 'samples sampleRate ...options?',
    values: {
      samples: SIG,
      sampleRate: ALT_SR,
      options: {
        f0MinHz: 100,
        f0MaxHz: 600,
        minNoteMs: 120,
        referenceHz: 435,
      },
    },
    invalid: { sampleRate: 0 },
  },
  {
    name: 'analyzePolyphonic',
    // The analysis owns a native handle, so the rows compare what it reads back.
    fn: ((...args: never[]) => {
      const analysis = (sonare.analyzePolyphonic as (...a: unknown[]) => PolyphonicAnalysis)(
        ...args,
      );
      try {
        return [analysis.noteCount, analysis.frameCount, Array.from(analysis.render())];
      } finally {
        analysis.destroy();
      }
    }) as never,
    params: 'samples sampleRate ...options?',
    values: {
      samples: SIG,
      sampleRate: ALT_SR,
      options: { nFft: 1024, hopLength: 256, f0MinHz: 200, maxPolyphony: 1 },
    },
    invalid: { sampleRate: 0 },
  },
  {
    name: 'detectBoundaries',
    fn: sonare.detectBoundaries,
    params: 'samples sampleRate ...options?',
    values: {
      samples: LONG8,
      sampleRate: ALT_SR,
      options: { hopLength: 1024, kernelSize: 16, nMfcc: 8, threshold: 0.9 },
    },
    invalid: { sampleRate: 0 },
  },
  {
    name: 'chordFunctionalAnalysis',
    fn: sonare.chordFunctionalAnalysis,
    // keyRoot is required, so the options travel whole in the required-only calls.
    params: 'samples sampleRate ...options',
    values: {
      samples: SIG,
      sampleRate: ALT_SR,
      options: {
        // G, where the detected chord's numeral differs between major and minor.
        keyRoot: 7,
        keyMode: 'minor',
        // At the measured tuning the signal reads as one chord, which leaves minDurationSec
        // nothing to merge; at A440 it reads as two.
        minDurationSec: 0.6,
        tuningCents: 0,
      },
    },
    invalid: { sampleRate: 0 },
  },
  {
    name: 'detectAcoustic',
    fn: sonare.detectAcoustic,
    params: 'samples sampleRate ...options?',
    values: { samples: IR, sampleRate: ALT_SR, options: { nOctaveBands: 3 } },
    invalid: { sampleRate: 0 },
  },
  {
    name: 'detectBeats',
    fn: sonare.detectBeats,
    params: 'samples sampleRate',
    values: { samples: SIG, sampleRate: ALT_SR },
    invalid: { sampleRate: 0 },
  },
  {
    name: 'detectBpm',
    fn: sonare.detectBpm,
    params: 'samples sampleRate',
    values: { samples: SIG, sampleRate: ALT_SR },
    invalid: { sampleRate: 0 },
  },
  {
    name: 'detectChords',
    fn: sonare.detectChords,
    params: 'samples sampleRate ...options?',
    values: { samples: SIG, sampleRate: ALT_SR, options: { hopLength: 256 } },
    invalid: { sampleRate: 0 },
  },
  {
    name: 'detectDownbeats',
    fn: sonare.detectDownbeats,
    params: 'samples sampleRate',
    values: { samples: SIG, sampleRate: ALT_SR },
    invalid: { sampleRate: 0 },
  },
  {
    name: 'detectKey',
    fn: sonare.detectKey,
    params: 'samples sampleRate ...options?',
    values: { samples: SIG, sampleRate: ALT_SR, options: { hopLength: 256, useHpss: true } },
    invalid: { sampleRate: 0 },
  },
  {
    name: 'detectKeyCandidates',
    fn: sonare.detectKeyCandidates,
    params: 'samples sampleRate ...options?',
    values: { samples: SIG, sampleRate: ALT_SR, options: { hopLength: 256, useHpss: true } },
    invalid: { sampleRate: 0 },
  },
  {
    name: 'detectOnsets',
    fn: sonare.detectOnsets,
    params: 'samples sampleRate ...options?',
    values: { samples: SIG, sampleRate: ALT_SR, options: { hopLength: 256, backtrack: true } },
    invalid: { sampleRate: 0 },
  },
  {
    name: 'estimateRoom',
    fn: sonare.estimateRoom,
    params: 'samples sampleRate ...options?',
    values: { samples: IR, sampleRate: ALT_SR, options: { referenceAbsorption: 0.5 } },
    invalid: { sampleRate: 0 },
  },
  {
    name: 'roomGeometryFromEstimate',
    fn: sonare.roomGeometryFromEstimate,
    requestOnly: true,
    params: 'estimate source? listener?',
    values: {
      estimate: {
        lengthM: 6,
        widthM: 4.5,
        heightM: 3,
        bandAbsorption: new Float32Array([0.1, 0.12, 0.15, 0.2, 0.25, 0.3]),
      },
      source: { x: 1, y: 1, z: 1.2 },
      listener: { x: 3, y: 2, z: 1.7 },
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
  // effects_note_ops.ts
  {
    name: 'noteMove',
    fn: sonare.noteMove,
    params: 'samples sampleRate ...options?',
    values: {
      samples: SIG,
      sampleRate: ALT_SR,
      options: { onsetSample: 2000, offsetSample: 8000, targetOnsetSample: 4000 },
    },
    invalid: { sampleRate: 0 },
  },
  {
    name: 'noteStretch',
    fn: sonare.noteStretch,
    params: 'samples sampleRate ...options?',
    values: {
      samples: SIG,
      sampleRate: ALT_SR,
      options: { onsetSample: 2000, offsetSample: 8000, stretchRatio: 1.5 },
    },
    invalid: { sampleRate: 0 },
  },
  // effects_separation.ts / feature_decompose.ts
  {
    name: 'harmonic',
    fn: sonare.harmonic,
    params: 'samples sampleRate',
    values: { samples: SIG, sampleRate: ALT_SR },
    invalid: { sampleRate: 0 },
    inert: { sampleRate: 'median filtering in bins and frames reads no rate' },
  },
  {
    name: 'percussive',
    fn: sonare.percussive,
    params: 'samples sampleRate',
    values: { samples: SIG, sampleRate: ALT_SR },
    invalid: { sampleRate: 0 },
    inert: { sampleRate: 'median filtering in bins and frames reads no rate' },
  },
  {
    name: 'hpss',
    fn: sonare.hpss,
    params: 'samples sampleRate ...options?',
    values: {
      samples: SIG,
      sampleRate: ALT_SR,
      options: {
        kernelHarmonic: 17,
        kernelPercussive: 17,
        nFft: 1024,
        hopLength: 256,
        hardMask: true,
      },
    },
    invalid: { nFft: 0 },
  },
  {
    name: 'hpssWithResidual',
    fn: sonare.hpssWithResidual,
    params: 'samples sampleRate ...options?',
    values: {
      samples: SIG,
      sampleRate: ALT_SR,
      options: {
        kernelHarmonic: 17,
        kernelPercussive: 17,
        nFft: 1024,
        hopLength: 256,
        hardMask: true,
      },
    },
    invalid: { nFft: 0 },
  },
  {
    name: 'decompose',
    fn: sonare.decompose,
    requestOnly: true,
    params: 's nComponents nIter? beta? init?',
    values: {
      s: { data: SMALL, rows: 8, cols: 10 },
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
    requestOnly: true,
    params: 's aggregate? k? width?',
    values: { s: { data: SMALL, rows: 8, cols: 10 }, aggregate: 'median', k: 3, width: 2 },
    invalid: { s: { data: SMALL, rows: 1000, cols: 1000 } },
  },
  {
    name: 'remix',
    fn: sonare.remix,
    params: 'samples sampleRate ...options',
    values: {
      samples: SIG,
      sampleRate: ALT_SR,
      options: { intervals: [10000, 15001, 0, 5001], alignZeros: false },
    },
    invalid: { intervals: [0] },
    inert: { sampleRate: 'intervals are in samples' },
  },
  {
    name: 'remixAlignedIntervals',
    fn: sonare.remixAlignedIntervals,
    params: 'samples sampleRate ...options',
    values: {
      samples: SIG,
      sampleRate: ALT_SR,
      options: { intervals: [10000, 15001, 0, 5001], alignZeros: false },
    },
    invalid: { intervals: [0] },
    inert: { sampleRate: 'intervals are in samples' },
  },
  // effects_spectral.ts / effects_timepitch.ts
  {
    name: 'spectralEdit',
    fn: sonare.spectralEdit,
    params: 'samples sampleRate ...options',
    values: {
      samples: SIG,
      sampleRate: SR,
      options: {
        ops: [{ startSample: 0, endSample: 12000, lowHz: 150, highHz: 2000, gainDb: -12 }],
        nFft: 1024,
      },
    },
    invalid: { sampleRate: 0 },
  },
  {
    name: 'timeStretch',
    fn: sonare.timeStretch,
    params: 'samples sampleRate ...options',
    values: {
      samples: SIG,
      sampleRate: SR,
      options: { speedRatio: 1.25, nFft: 1024, hopLength: 256 },
    },
    invalid: { speedRatio: 0 },
  },
  {
    name: 'pitchShift',
    fn: sonare.pitchShift,
    params: 'samples sampleRate ...options',
    values: {
      samples: SIG,
      sampleRate: SR,
      options: { pitchSemitones: 2, nFft: 1024, hopLength: 256 },
    },
    invalid: { sampleRate: 0 },
  },
  {
    name: 'pitchCorrectToMidi',
    fn: sonare.pitchCorrectToMidi,
    params: 'samples sampleRate ...options',
    values: { samples: SIG, sampleRate: ALT_SR, options: { currentMidi: 57, targetMidi: 59 } },
    invalid: { sampleRate: 0 },
  },
  {
    name: 'pitchCorrectTimevarying',
    fn: sonare.pitchCorrectTimevarying,
    params: 'samples sampleRate ...options',
    values: {
      samples: SIG,
      sampleRate: ALT_SR,
      options: { f0Hz: F0, hopLength: 256, targetMidi: 59, retuneAmount: 0.5 },
    },
    invalid: { sampleRate: 0 },
  },
  // feature_framing.ts / feature_units.ts
  {
    name: 'preemphasis',
    fn: sonare.preemphasis,
    params: 'samples sampleRate ...options?',
    values: { samples: SIG, sampleRate: ALT_SR, options: { coef: 0.9, zi: 0.5 } },
    invalid: { samples: [0.5] },
  },
  {
    name: 'deemphasis',
    fn: sonare.deemphasis,
    params: 'samples sampleRate ...options?',
    values: { samples: SIG, sampleRate: ALT_SR, options: { coef: 0.9, zi: 0.5 } },
    invalid: { samples: [0.5] },
  },
  {
    name: 'trimSilence',
    fn: sonare.trimSilence,
    params: 'samples sampleRate ...options?',
    values: {
      samples: SIG,
      sampleRate: SR,
      options: { topDb: 30, frameLength: 1024, hopLength: 256 },
    },
    invalid: { hopLength: 0 },
  },
  {
    name: 'splitSilence',
    fn: sonare.splitSilence,
    params: 'samples sampleRate ...options?',
    values: {
      samples: SIG,
      sampleRate: SR,
      options: { topDb: 30, frameLength: 1024, hopLength: 256 },
    },
    invalid: { hopLength: 0 },
  },
  {
    name: 'trim',
    fn: sonare.trim,
    params: 'samples sampleRate ...options?',
    values: {
      samples: PADDED,
      sampleRate: ALT_SR,
      options: { thresholdDb: -20, frameLength: 512, hopLength: 64 },
    },
    invalid: { hopLength: 0 },
    inert: { sampleRate: 'frames are counted in samples' },
    controlBase: { hopLength: { options: { frameLength: 512, hopLength: 64 } } },
  },
  {
    name: 'frameSignal',
    fn: sonare.frameSignal,
    params: 'samples sampleRate ...options',
    values: { samples: SIG, sampleRate: SR, options: { frameLength: 1024, hopLength: 256 } },
    invalid: { hopLength: 0 },
  },
  {
    name: 'fixLength',
    fn: sonare.fixLength,
    requestOnly: true,
    params: 'values targetSize padValue?',
    values: { values: SMALL, targetSize: 100, padValue: 0.5 },
    invalid: { targetSize: -1 },
  },
  {
    name: 'padCenter',
    fn: sonare.padCenter,
    requestOnly: true,
    params: 'values targetSize padValue?',
    values: { values: SMALL, targetSize: 100, padValue: 0.5 },
    invalid: { targetSize: -1 },
  },
  {
    name: 'vectorNormalize',
    fn: sonare.vectorNormalize,
    requestOnly: true,
    params: 'values norm? threshold?',
    values: { values: SMALL, norm: 'l1', threshold: 50 },
    invalid: { norm: 'l9' },
    controlBase: { norm: { threshold: undefined } },
  },
  {
    name: 'amplitudeToDb',
    fn: sonare.amplitudeToDb,
    requestOnly: true,
    params: 'values ref? amin? topDb?',
    values: { values: SMALL, ref: 0.5, amin: 0.2, topDb: 10 },
    invalid: { amin: -1 },
    controlBase: { amin: { topDb: undefined } },
  },
  {
    name: 'powerToDb',
    fn: sonare.powerToDb,
    requestOnly: true,
    params: 'values ref? amin? topDb?',
    values: { values: SMALL, ref: 0.5, amin: 0.2, topDb: 5 },
    invalid: { amin: -1 },
    controlBase: { amin: { topDb: undefined } },
  },
  // feature_inverse.ts
  {
    name: 'clicks',
    fn: sonare.clicks,
    requestOnly: true,
    params: 'times sampleRate length? frequencyHz? clickDurationSec?',
    values: {
      times: new Float32Array([0.1, 0.5]),
      sampleRate: ALT_SR,
      length: 30000,
      frequencyHz: 2000,
      clickDurationSec: 0.05,
    },
    invalid: { sampleRate: 0 },
  },
  {
    name: 'chirp',
    fn: sonare.chirp,
    requestOnly: true,
    params: 'fminHz? fmaxHz? sampleRate durationSec? linear?',
    values: { fminHz: 200, fmaxHz: 2000, sampleRate: ALT_SR, durationSec: 0.2, linear: true },
    invalid: { sampleRate: 0 },
  },
  {
    name: 'tone',
    fn: sonare.tone,
    requestOnly: true,
    params: 'frequencyHz? sampleRate durationSec? phase? amplitude?',
    values: { frequencyHz: 330, sampleRate: ALT_SR, durationSec: 0.2, phase: 0.5, amplitude: 0.3 },
    invalid: { sampleRate: 0 },
  },
  {
    name: 'cqtToAudio',
    fn: sonare.cqtToAudio,
    requestOnly: true,
    params: 'magnitude sampleRate hopLength? fminHz? binsPerOctave? nIter?',
    values: {
      magnitude: CQT.magnitude,
      sampleRate: ALT_SR,
      hopLength: 128,
      fminHz: 65.4,
      binsPerOctave: 24,
      nIter: 4,
    },
    invalid: { hopLength: 0 },
  },
  {
    name: 'vqtToAudio',
    fn: sonare.vqtToAudio,
    requestOnly: true,
    params: 'magnitude sampleRate hopLength? fminHz? binsPerOctave? gamma? nIter?',
    values: {
      magnitude: VQT.magnitude,
      sampleRate: ALT_SR,
      hopLength: 128,
      fminHz: 65.4,
      binsPerOctave: 24,
      gamma: 2,
      nIter: 4,
    },
    invalid: { hopLength: 0 },
  },
  {
    name: 'griffinLim',
    fn: sonare.griffinLim,
    requestOnly: true,
    // nFft and hopLength are required: they have to agree with the magnitude's row count.
    params: 'magnitude nFft hopLength sampleRate nIter? momentum?',
    values: {
      magnitude: ST.magnitude,
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
    name: 'melToStft',
    fn: sonare.melToStft,
    requestOnly: true,
    params: 'power sampleRate nFft? fminHz? fmaxHz? htk?',
    values: {
      power: MEL.power,
      sampleRate: ALT_SR,
      nFft: 512,
      fminHz: 50,
      fmaxHz: 8000,
      htk: true,
    },
    invalid: { nFft: 0 },
  },
  {
    name: 'melToAudio',
    fn: sonare.melToAudio,
    requestOnly: true,
    params: 'power sampleRate nFft? hopLength? fminHz? fmaxHz? nIter? htk?',
    values: {
      power: MEL.power,
      sampleRate: ALT_SR,
      nFft: 512,
      hopLength: 128,
      fminHz: 50,
      fmaxHz: 8000,
      nIter: 4,
      htk: true,
    },
    invalid: { hopLength: 0 },
  },
  {
    name: 'mfccToMel',
    fn: sonare.mfccToMel,
    requestOnly: true,
    params: 'coefficients nMels? lifter?',
    values: { coefficients: MFCC.coefficients, nMels: 32, lifter: 22 },
    invalid: { lifter: -1 },
  },
  {
    name: 'mfccToAudio',
    fn: sonare.mfccToAudio,
    requestOnly: true,
    params: 'coefficients nMels? sampleRate nFft? hopLength? fminHz? fmaxHz? nIter? htk? lifter?',
    values: {
      coefficients: MFCC.coefficients,
      nMels: 32,
      sampleRate: ALT_SR,
      nFft: 512,
      hopLength: 128,
      fminHz: 50,
      fmaxHz: 8000,
      nIter: 4,
      htk: true,
      lifter: 22,
    },
    invalid: { hopLength: 0 },
  },
  {
    name: 'phaseVocoder',
    fn: sonare.phaseVocoder,
    params: 'samples sampleRate ...options',
    values: {
      samples: SIG,
      sampleRate: SR,
      options: { speedRatio: 1.25, nFft: 1024, hopLength: 256 },
    },
    invalid: { speedRatio: 0 },
  },
  // feature_loudness.ts
  {
    name: 'ebur128LoudnessRangeLu',
    fn: sonare.ebur128LoudnessRangeLu,
    params: 'samples sampleRate ...options?',
    values: { samples: LONG, sampleRate: ALT_SR, options: { validate: false } },
    invalid: { sampleRate: 0 },
  },
  {
    name: 'lufs',
    fn: sonare.lufs,
    params: 'samples sampleRate ...options?',
    values: { samples: SIG, sampleRate: ALT_SR, options: { validate: false } },
    invalid: { sampleRate: 0 },
  },
  {
    name: 'momentaryLufs',
    fn: sonare.momentaryLufs,
    params: 'samples sampleRate ...options?',
    values: { samples: SIG, sampleRate: ALT_SR, options: { validate: false } },
    invalid: { sampleRate: 0 },
  },
  {
    name: 'shortTermLufs',
    fn: sonare.shortTermLufs,
    params: 'samples sampleRate ...options?',
    values: { samples: LONG, sampleRate: ALT_SR, options: { validate: false } },
    invalid: { sampleRate: 0 },
  },
  {
    name: 'lufsMultichannel',
    fn: sonare.lufsMultichannel,
    requestOnly: true,
    params: 'channels sampleRate',
    values: { channels: [SIG, SIG2], sampleRate: ALT_SR },
    invalid: { sampleRate: 0 },
  },
  {
    name: 'lufsSeriesMultichannel',
    fn: sonare.lufsSeriesMultichannel,
    requestOnly: true,
    params: 'channels sampleRate',
    values: { channels: [SIG, SIG2], sampleRate: ALT_SR },
    invalid: { sampleRate: 0 },
  },
  // feature_pitch.ts
  {
    name: 'chroma',
    fn: sonare.chroma,
    params: 'samples sampleRate ...options?',
    values: { samples: SIG, sampleRate: ALT_SR, options: { nFft: 1024, hopLength: 256 } },
    invalid: { hopLength: 0 },
  },
  {
    name: 'chromaCens',
    fn: sonare.chromaCens,
    params: 'samples sampleRate ...options?',
    values: { samples: SHORT, sampleRate: ALT_SR, options: { hopLength: 1024, binsPerOctave: 24 } },
    invalid: { hopLength: 0 },
  },
  {
    name: 'chromaCqt',
    fn: sonare.chromaCqt,
    params: 'samples sampleRate ...options?',
    values: { samples: SHORT, sampleRate: ALT_SR, options: { hopLength: 1024, binsPerOctave: 24 } },
    invalid: { hopLength: 0 },
  },
  {
    name: 'bassChroma',
    fn: sonare.bassChroma,
    params: 'samples sampleRate ...options?',
    values: { samples: SHORT, sampleRate: ALT_SR, options: { hopLength: 1024 } },
    invalid: { hopLength: 0 },
  },
  {
    name: 'cqt',
    fn: sonare.cqt,
    params: 'samples sampleRate ...options?',
    values: {
      samples: SHORT,
      sampleRate: ALT_SR,
      options: { hopLength: 256, fminHz: 65.4, nBins: 48, binsPerOctave: 24 },
    },
    invalid: { hopLength: 0 },
  },
  {
    name: 'pseudoCqt',
    fn: sonare.pseudoCqt,
    params: 'samples sampleRate ...options?',
    values: {
      samples: SHORT,
      sampleRate: ALT_SR,
      options: { hopLength: 256, fminHz: 65.4, nBins: 48, binsPerOctave: 24 },
    },
    invalid: { hopLength: 0 },
  },
  {
    name: 'hybridCqt',
    fn: sonare.hybridCqt,
    params: 'samples sampleRate ...options?',
    values: {
      samples: SHORT,
      sampleRate: ALT_SR,
      options: { hopLength: 256, fminHz: 65.4, nBins: 48, binsPerOctave: 24 },
    },
    invalid: { hopLength: 0 },
  },
  {
    name: 'vqt',
    fn: sonare.vqt,
    params: 'samples sampleRate ...options?',
    values: {
      samples: SHORT,
      sampleRate: ALT_SR,
      options: { hopLength: 256, fminHz: 65.4, nBins: 48, binsPerOctave: 24, gamma: 5 },
    },
    invalid: { hopLength: 0 },
  },
  {
    name: 'estimateTuning',
    fn: sonare.estimateTuning,
    params: 'samples sampleRate ...options?',
    values: {
      samples: SIG,
      sampleRate: ALT_SR,
      options: { nFft: 1024, hopLength: 64, resolution: 0.001, binsPerOctave: 24 },
    },
    invalid: { hopLength: 0 },
  },
  {
    name: 'piptrack',
    fn: sonare.piptrack,
    params: 'samples sampleRate ...options?',
    values: {
      samples: SIG,
      sampleRate: ALT_SR,
      options: { nFft: 1024, hopLength: 256, fminHz: 200, fmaxHz: 1000, threshold: 0.3 },
    },
    invalid: { hopLength: 0 },
  },
  {
    name: 'pitchYin',
    fn: sonare.pitchYin,
    params: 'samples sampleRate ...options?',
    values: {
      samples: TONE_NOISY_TAIL,
      sampleRate: ALT_SR,
      options: {
        frameLength: 1024,
        hopLength: 256,
        f0MinHz: 250,
        f0MaxHz: 500,
        threshold: 0.5,
      },
    },
    invalid: { hopLength: 0 },
  },
  {
    name: 'pitchPyin',
    fn: sonare.pitchPyin,
    params: 'samples sampleRate ...options?',
    values: {
      samples: TONE,
      sampleRate: ALT_SR,
      options: {
        frameLength: 1024,
        hopLength: 256,
        f0MinHz: 150,
        f0MaxHz: 400,
        threshold: 0.2,
        fillNa: true,
      },
    },
    invalid: { hopLength: 0 },
  },
  {
    name: 'pitchTuning',
    fn: sonare.pitchTuning,
    requestOnly: true,
    params: 'frequencies resolution? binsPerOctave?',
    values: { frequencies: new Float32Array([224, 448, 672]), resolution: 0.05, binsPerOctave: 24 },
    invalid: { resolution: 0 },
  },
  {
    name: 'nnlsChroma',
    fn: sonare.nnlsChroma,
    params: 'samples sampleRate ...options?',
    values: {
      samples: SIG,
      sampleRate: ALT_SR,
      options: { hopLength: 1024, enableStftBlend: false },
    },
    invalid: { sampleRate: 0 },
  },
  // feature_rhythm.ts
  {
    name: 'onsetStrength',
    fn: sonare.onsetStrength,
    params: 'samples sampleRate ...options?',
    values: {
      samples: SIG,
      sampleRate: ALT_SR,
      options: { nFft: 1024, hopLength: 256, nMels: 64 },
    },
    invalid: { sampleRate: 0 },
  },
  {
    name: 'onsetStrengthMulti',
    fn: sonare.onsetStrengthMulti,
    params: 'samples sampleRate ...options?',
    values: {
      samples: SIG,
      sampleRate: ALT_SR,
      options: { nFft: 1024, hopLength: 256, nMels: 64, nBands: 6 },
    },
    invalid: { hopLength: 0 },
  },
  {
    name: 'onsetBacktrack',
    fn: sonare.onsetBacktrack,
    requestOnly: true,
    params: 'events energy',
    values: { events: [5, 20, 40], energy: ENV },
    invalid: { events: [1.5] },
  },
  {
    name: 'peakPick',
    fn: sonare.peakPick,
    requestOnly: true,
    params: 'values preMax postMax preAvg postAvg delta wait',
    values: { values: ENV, preMax: 2, postMax: 2, preAvg: 4, postAvg: 4, delta: 0.05, wait: 3 },
    invalid: { wait: -1 },
  },
  {
    name: 'tempogram',
    fn: sonare.tempogram,
    requestOnly: true,
    params: 'onsetEnvelope sampleRate hopLength? winLength? mode? center? norm?',
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
    requestOnly: true,
    params: 'onsetEnvelope sampleRate hopLength? winLength? bpmMin? nBins?',
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
    name: 'fourierTempogram',
    fn: sonare.fourierTempogram,
    requestOnly: true,
    params: 'onsetEnvelope sampleRate hopLength? winLength? center?',
    values: {
      onsetEnvelope: ENV,
      sampleRate: ALT_SR,
      hopLength: 256,
      winLength: 64,
      center: false,
    },
    invalid: { winLength: 0 },
    inert: {
      sampleRate: 'the frequency axis is in frames',
      hopLength: 'the frequency axis is in frames',
    },
  },
  {
    name: 'plp',
    fn: sonare.plp,
    requestOnly: true,
    params: 'onsetEnvelope sampleRate hopLength? bpmMin? bpmMax? winLength?',
    values: {
      onsetEnvelope: ENV,
      sampleRate: ALT_SR,
      hopLength: 256,
      bpmMin: 130,
      bpmMax: 250,
      winLength: 128,
    },
    invalid: { winLength: 0 },
  },
  {
    name: 'tempogramRatio',
    fn: sonare.tempogramRatio,
    requestOnly: true,
    params: 'tempogram sampleRate hopLength? factors?',
    values: {
      tempogram: TG,
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
  // feature_spectral.ts
  {
    name: 'stft',
    fn: sonare.stft,
    params: 'samples sampleRate ...options?',
    values: { samples: SIG, sampleRate: ALT_SR, options: { nFft: 1024, hopLength: 256 } },
    invalid: { hopLength: 0 },
  },
  {
    name: 'stftDb',
    fn: sonare.stftDb,
    params: 'samples sampleRate ...options?',
    values: { samples: SIG, sampleRate: ALT_SR, options: { nFft: 1024, hopLength: 256 } },
    invalid: { hopLength: 0 },
  },
  {
    name: 'reassignedSpectrogram',
    fn: sonare.reassignedSpectrogram,
    params: 'samples sampleRate ...options?',
    values: {
      samples: SIG,
      sampleRate: ALT_SR,
      options: { nFft: 1024, hopLength: 256, refPower: 0.5, fillNan: true },
    },
    invalid: { hopLength: 0 },
  },
  {
    name: 'melSpectrogram',
    fn: sonare.melSpectrogram,
    params: 'samples sampleRate ...options?',
    values: {
      samples: SIG,
      sampleRate: ALT_SR,
      options: { nFft: 1024, hopLength: 256, nMels: 64, fminHz: 50, fmaxHz: 8000, htk: true },
    },
    invalid: { hopLength: 0 },
  },
  {
    name: 'mfcc',
    fn: sonare.mfcc,
    params: 'samples sampleRate ...options?',
    values: {
      samples: SIG,
      sampleRate: ALT_SR,
      options: {
        nFft: 1024,
        hopLength: 256,
        nMels: 64,
        nMfcc: 13,
        fminHz: 50,
        fmaxHz: 8000,
        htk: true,
        lifter: 22,
      },
    },
    invalid: { hopLength: 0 },
  },
  {
    name: 'melDelta',
    fn: sonare.melDelta,
    requestOnly: true,
    params: 'features width?',
    values: { features: MFCC.coefficients, width: 5 },
    invalid: { width: 4 },
  },
  {
    name: 'pcen',
    fn: sonare.pcen,
    requestOnly: true,
    params: 'values gain? timeConstant?',
    values: { values: MEL.power, gain: 0.5, timeConstant: 0.2 },
    invalid: { gain: Number.NaN },
  },
  {
    name: 'polyFeatures',
    fn: sonare.polyFeatures,
    params: 'samples sampleRate ...options?',
    values: { samples: SIG, sampleRate: ALT_SR, options: { nFft: 1024, hopLength: 256, order: 2 } },
    invalid: { hopLength: 0 },
  },
  {
    name: 'spectralBandwidth',
    fn: sonare.spectralBandwidth,
    params: 'samples sampleRate ...options?',
    values: { samples: SIG, sampleRate: ALT_SR, options: { nFft: 1024, hopLength: 256, p: 3 } },
    invalid: { hopLength: 0 },
  },
  {
    name: 'spectralCentroid',
    fn: sonare.spectralCentroid,
    params: 'samples sampleRate ...options?',
    values: { samples: SIG, sampleRate: ALT_SR, options: { nFft: 1024, hopLength: 256 } },
    invalid: { hopLength: 0 },
  },
  {
    name: 'spectralContrast',
    fn: sonare.spectralContrast,
    params: 'samples sampleRate ...options?',
    values: {
      samples: SIG,
      sampleRate: ALT_SR,
      options: { nFft: 1024, hopLength: 256, nBands: 4, fminHz: 100, quantile: 0.1 },
    },
    invalid: { hopLength: 0 },
  },
  {
    name: 'spectralFlatness',
    fn: sonare.spectralFlatness,
    params: 'samples sampleRate ...options?',
    values: { samples: SIG, sampleRate: ALT_SR, options: { nFft: 1024, hopLength: 256 } },
    invalid: { hopLength: 0 },
  },
  {
    name: 'spectralFlux',
    fn: sonare.spectralFlux,
    params: 'samples sampleRate ...options?',
    values: { samples: SIG, sampleRate: ALT_SR, options: { nFft: 1024, hopLength: 256, lag: 2 } },
    invalid: { hopLength: 0 },
  },
  {
    name: 'spectralRolloff',
    fn: sonare.spectralRolloff,
    params: 'samples sampleRate ...options?',
    values: {
      samples: SIG,
      sampleRate: ALT_SR,
      options: { nFft: 1024, hopLength: 256, rollPercent: 0.5 },
    },
    invalid: { hopLength: 0 },
  },
  {
    name: 'rmsEnergy',
    fn: sonare.rmsEnergy,
    params: 'samples sampleRate ...options?',
    values: { samples: SIG, sampleRate: ALT_SR, options: { frameLength: 1024, hopLength: 256 } },
    invalid: { hopLength: 0 },
  },
  {
    name: 'zeroCrossingRate',
    fn: sonare.zeroCrossingRate,
    params: 'samples sampleRate ...options?',
    values: { samples: SIG, sampleRate: ALT_SR, options: { frameLength: 1024, hopLength: 256 } },
    invalid: { hopLength: 0 },
  },
  {
    name: 'zeroCrossings',
    fn: sonare.zeroCrossings,
    params: 'samples sampleRate ...options?',
    values: {
      samples: SIG,
      sampleRate: ALT_SR,
      options: { threshold: 0.01, refMagnitude: true, pad: false, zeroPos: false },
    },
    invalid: { threshold: Number.NaN },
  },
  {
    name: 'fixFrames',
    fn: sonare.fixFrames,
    requestOnly: true,
    params: 'frames xMin? xMax? pad?',
    values: { frames: [3, 9, 40, 70], xMin: 5, xMax: 60, pad: false },
    invalid: { xMin: 1.5 },
  },
  {
    name: 'tonnetz',
    fn: sonare.tonnetz,
    requestOnly: true,
    params: 'chromagram',
    values: { chromagram: CHROMA.features },
    invalid: { chromagram: { data: new Float32Array(0), rows: 12, cols: 0 } },
  },
  // mastering
  {
    name: 'masteringNormalizeLoudness',
    fn: sonare.masteringNormalizeLoudness,
    params: 'samples sampleRate ...options?',
    values: { samples: SIG, sampleRate: ALT_SR, options: { targetLufs: -18, ceilingDb: -3 } },
    invalid: { sampleRate: 0 },
  },
  {
    name: 'masterAudio',
    fn: sonare.masterAudio,
    params: 'samples sampleRate ...options?',
    values: {
      samples: SIG,
      sampleRate: ALT_SR,
      options: {
        preset: 'speech',
        overrides: { loudness: { targetLufs: -20 } },
        onProgress: PROGRESS,
      },
    },
    invalid: { preset: 'no-such-preset' },
  },
  {
    name: 'masterAudioAsync',
    fn: sonare.masterAudioAsync,
    params: 'samples sampleRate ...options?',
    values: {
      samples: SIG,
      sampleRate: ALT_SR,
      options: { preset: 'speech', overrides: { loudness: { targetLufs: -20 } } },
    },
    invalid: { preset: 'no-such-preset' },
  },
  {
    name: 'masterAudioStereo',
    fn: sonare.masterAudioStereo,
    requestOnly: true,
    params: 'left right sampleRate preset? overrides? onProgress?',
    values: {
      left: SIG,
      right: SIG2,
      sampleRate: ALT_SR,
      preset: 'speech',
      overrides: { loudness: { targetLufs: -20 } },
      onProgress: PROGRESS,
    },
    invalid: { preset: 'no-such-preset' },
  },
  {
    name: 'masterAudioStereoAsync',
    fn: sonare.masterAudioStereoAsync,
    requestOnly: true,
    params: 'left right sampleRate preset overrides?',
    values: {
      left: SIG,
      right: SIG2,
      sampleRate: ALT_SR,
      preset: 'speech',
      overrides: { loudness: { targetLufs: -20 } },
    },
    invalid: { preset: 'no-such-preset' },
  },
  {
    name: 'masteringApplyProcessor',
    fn: sonare.masteringApplyProcessor,
    params: 'samples sampleRate ...options',
    values: {
      samples: SIG,
      sampleRate: ALT_SR,
      options: { processor: 'dynamics.compressor', params: { ratio: 4, thresholdDb: -30 } },
    },
    invalid: { processor: 'no.such' },
  },
  {
    name: 'masteringApplyProcessorStereo',
    fn: sonare.masteringApplyProcessorStereo,
    requestOnly: true,
    params: 'processor left right sampleRate params?',
    values: {
      processor: 'stereo.imager',
      left: SIG,
      right: SIG2,
      sampleRate: ALT_SR,
      params: { width: 1.6 },
    },
    invalid: { processor: 'no.such' },
  },
  {
    name: 'masteringApplyPairProcessor',
    fn: sonare.masteringApplyPairProcessor,
    requestOnly: true,
    params: 'processor source reference sampleRate params?',
    values: {
      processor: 'match.abCrossfade',
      source: SIG,
      reference: SIG2,
      sampleRate: ALT_SR,
      params: { mix: 0.3 },
    },
    invalid: { processor: 'no.such' },
  },
  {
    name: 'masteringApplyPairProcessorStereo',
    fn: sonare.masteringApplyPairProcessorStereo,
    requestOnly: true,
    params: 'processor sourceLeft sourceRight referenceLeft referenceRight sampleRate params?',
    values: {
      processor: 'match.abCrossfade',
      sourceLeft: SIG,
      sourceRight: SIG2,
      referenceLeft: SIG2,
      referenceRight: SIG,
      sampleRate: ALT_SR,
      params: { mix: 0.3 },
    },
    invalid: { processor: 'no.such' },
  },
  {
    name: 'masteringAnalyzePair',
    fn: sonare.masteringAnalyzePair,
    requestOnly: true,
    params: 'analysis source reference sampleRate params?',
    values: {
      analysis: 'match.tonalBalanceLogBands',
      source: SIG,
      reference: SIG2,
      sampleRate: ALT_SR,
      params: { bandsPerOctave: 2, highHz: 8000 },
    },
    invalid: { analysis: 'no.such' },
  },
  {
    name: 'masteringAnalyzeStereo',
    fn: sonare.masteringAnalyzeStereo,
    requestOnly: true,
    params: 'analysis left right sampleRate params',
    values: {
      analysis: 'stereo.monoCompatCheckLogBands',
      left: SIG,
      right: SIG2,
      sampleRate: ALT_SR,
      params: { bandsPerOctave: 2, highHz: 8000 },
    },
    invalid: { analysis: 'no.such' },
  },
  {
    name: 'masteringAssistantSuggest',
    fn: sonare.masteringAssistantSuggest,
    params: 'samples sampleRate ...options?',
    values: { samples: SIG, sampleRate: ALT_SR, options: { targetLufs: -13 } },
    invalid: { sampleRate: 0 },
  },
  {
    name: 'masteringAssistantSuggestChain',
    fn: sonare.masteringAssistantSuggestChain,
    params: 'samples sampleRate ...options?',
    values: { samples: SIG, sampleRate: ALT_SR, options: { targetLufs: -13 } },
    invalid: { sampleRate: 0 },
  },
  {
    name: 'masteringAudioProfile',
    fn: sonare.masteringAudioProfile,
    params: 'samples sampleRate ...options?',
    values: { samples: SIG, sampleRate: ALT_SR, options: { nFft: 1024, hopLength: 256 } },
    invalid: { sampleRate: 0 },
  },
  {
    name: 'masteringStreamingPreview',
    fn: sonare.masteringStreamingPreview,
    params: 'samples sampleRate ...options?',
    values: {
      samples: SIG,
      sampleRate: ALT_SR,
      options: { platforms: [{ name: 'quiet', targetLufs: -24, ceilingDb: -3 }] },
    },
    invalid: { sampleRate: 0 },
  },
  {
    name: 'streamingLoudnessGain',
    fn: sonare.streamingLoudnessGain,
    params: 'samples sampleRate ...options?',
    values: {
      samples: SIG,
      sampleRate: ALT_SR,
      options: { config: { loudness: { targetLufs: -18 } } },
    },
    invalid: { sampleRate: 0 },
  },
  {
    name: 'masteringDynamicsCompressor',
    fn: sonare.masteringDynamicsCompressor,
    params: 'samples sampleRate ...options?',
    values: { samples: SIG, sampleRate: ALT_SR, options: { thresholdDb: -30, ratio: 6 } },
    invalid: { sampleRate: 0 },
  },
  {
    name: 'masteringDynamicsGate',
    fn: sonare.masteringDynamicsGate,
    params: 'samples sampleRate ...options?',
    values: { samples: SIG, sampleRate: ALT_SR, options: { thresholdDb: -20, rangeDb: -30 } },
    invalid: { sampleRate: 0 },
  },
  {
    name: 'masteringDynamicsTransientShaper',
    fn: sonare.masteringDynamicsTransientShaper,
    params: 'samples sampleRate ...options?',
    values: { samples: SIG, sampleRate: ALT_SR, options: { attackGainDb: 6, sustainGainDb: -3 } },
    invalid: { sampleRate: 0 },
  },
  {
    name: 'normalize',
    fn: sonare.normalize,
    params: 'samples sampleRate ...options?',
    values: { samples: SIG, sampleRate: ALT_SR, options: { targetDb: -6, mode: 'rms' } },
    invalid: { mode: 'loud' },
    inert: { sampleRate: 'peak and RMS gain read no rate' },
  },
  // effects_note_ops.ts / effects_percussive.ts / effects_timepitch.ts / feature_decompose.ts
  {
    name: 'autoTune',
    fn: sonare.autoTune,
    params: 'samples sampleRate ...options?',
    values: {
      samples: TONE,
      sampleRate: ALT_SR,
      options: { key: { root: 9, mode: 'minor' }, strength: 0.5 },
    },
    invalid: { sampleRate: 0 },
  },
  {
    name: 'decomposeStems',
    fn: sonare.decomposeStems,
    params: 'samples sampleRate ...options?',
    values: {
      samples: SHORT,
      sampleRate: SR,
      options: { nComponents: 2, nFft: 512, hopLength: 128, nIter: 8, beta: 1, maskPower: 2 },
    },
    invalid: { nComponents: -1 },
  },
  {
    name: 'extractNotes',
    fn: sonare.extractNotes,
    params: 'samples sampleRate ...options',
    values: {
      samples: TONE,
      sampleRate: SR,
      options: {
        ...NOTE_TRACK,
        segmentationThresholdCents: 150,
        minNoteMs: 100,
        referenceHz: 432,
        voicedThreshold: 0.7,
      },
    },
    invalid: { frameRate: -1 },
  },
  {
    name: 'splitNote',
    fn: sonare.splitNote,
    params: 'samples sampleRate ...options',
    values: {
      samples: TONE,
      sampleRate: SR,
      options: { ...NOTE_TRACK, notes: NOTE_SET, index: 3, frame: 40, referenceHz: 432 },
    },
    invalid: { index: -1 },
  },
  {
    name: 'mergeNotes',
    fn: sonare.mergeNotes,
    params: 'samples sampleRate ...options',
    values: {
      samples: TONE,
      sampleRate: SR,
      options: { ...NOTE_TRACK, notes: NOTE_SET, first: 0, last: 1, referenceHz: 432 },
    },
    invalid: { first: 1 },
  },
  {
    name: 'renderNotes',
    fn: sonare.renderNotes,
    params: 'samples sampleRate ...options',
    values: {
      samples: TONE,
      sampleRate: SR,
      options: {
        notes: NOTE_SET.map((note) => ({ ...note, edit: { ...note.edit, gainDb: -6 } })),
        fadeMs: 40,
      },
    },
    invalid: { notes: 'none' },
  },
  {
    name: 'extractPercussiveEvents',
    fn: sonare.extractPercussiveEvents,
    params: 'samples sampleRate ...options?',
    values: {
      samples: SIG,
      sampleRate: SR,
      options: { onsetWait: 20, maxEventMs: 120 },
    },
    invalid: { minPercussiveRatio: 2 },
  },
  {
    name: 'renderPercussiveEvents',
    fn: sonare.renderPercussiveEvents,
    params: 'samples sampleRate ...options',
    values: {
      samples: SIG,
      sampleRate: SR,
      options: {
        events: PERCUSSIVE_EVENTS.map((event) => ({
          ...event,
          edit: { ...event.edit, gainDb: -6 },
        })),
        fadeMs: 30,
      },
    },
    invalid: { events: 'none' },
  },
  // repair
  {
    name: 'masteringRepairDetectClicks',
    fn: sonare.masteringRepairDetectClicks,
    params: 'samples sampleRate ...options?',
    values: { samples: CLICKY, sampleRate: ALT_SR, options: { neighborRatio: 8 } },
    invalid: { sampleRate: 0 },
  },
  {
    name: 'masteringRepairDetectClipping',
    fn: sonare.masteringRepairDetectClipping,
    params: 'samples sampleRate ...options?',
    values: { samples: CLIPPED, sampleRate: ALT_SR, options: { clipThresholdDb: -20 } },
    invalid: { sampleRate: 0 },
  },
  {
    name: 'masteringRepairDetectCrackle',
    fn: sonare.masteringRepairDetectCrackle,
    params: 'samples sampleRate ...options?',
    values: { samples: CLICKY, sampleRate: ALT_SR, options: { threshold: 5 } },
    invalid: { sampleRate: 0 },
  },
  {
    name: 'masteringRepairDetectHum',
    fn: sonare.masteringRepairDetectHum,
    params: 'samples sampleRate ...options?',
    values: {
      samples: HUMMED,
      sampleRate: ALT_SR,
      options: { fundamentalHz: 60, searchRangeHz: 5 },
    },
    invalid: { sampleRate: 0 },
  },
  {
    name: 'masteringRepairDetectNoiseFloor',
    fn: sonare.masteringRepairDetectNoiseFloor,
    params: 'samples sampleRate ...options?',
    values: { samples: SIG, sampleRate: ALT_SR, options: { nFft: 512 } },
    invalid: { sampleRate: 0 },
  },
  {
    name: 'masteringRepairDetectReverb',
    fn: sonare.masteringRepairDetectReverb,
    params: 'samples sampleRate ...options?',
    values: { samples: SIG, sampleRate: ALT_SR, options: { nFft: 512 } },
    invalid: { sampleRate: 0 },
  },
  {
    name: 'masteringRepairDetectTrimRange',
    fn: sonare.masteringRepairDetectTrimRange,
    params: 'samples sampleRate ...options?',
    values: { samples: PADDED, sampleRate: ALT_SR, options: { thresholdDb: -20 } },
    invalid: { sampleRate: 0 },
  },
  {
    name: 'masteringRepairDeclick',
    fn: sonare.masteringRepairDeclick,
    params: 'samples sampleRate ...options?',
    values: { samples: CLICKY, sampleRate: ALT_SR, options: { lpcOrder: 4, maxClickSamples: 1 } },
    invalid: { sampleRate: 0 },
    inert: { sampleRate: 'clicks are found and refilled in samples' },
    controlBase: { lpcOrder: { options: { lpcOrder: 4 } } },
  },
  {
    name: 'masteringRepairDeclip',
    fn: sonare.masteringRepairDeclip,
    params: 'samples sampleRate ...options?',
    values: {
      samples: CLIPPED,
      sampleRate: ALT_SR,
      options: { clipThresholdDb: -20, lpcOrder: 8 },
    },
    invalid: { sampleRate: 0 },
    inert: { sampleRate: 'clipped runs are found and refilled in samples' },
  },
  {
    name: 'masteringRepairDecrackle',
    fn: sonare.masteringRepairDecrackle,
    params: 'samples sampleRate ...options?',
    values: { samples: CLICKY, sampleRate: ALT_SR, options: { threshold: 5 } },
    invalid: { sampleRate: 0 },
    inert: { sampleRate: 'crackle is found in samples' },
  },
  {
    name: 'masteringRepairDehum',
    fn: sonare.masteringRepairDehum,
    params: 'samples sampleRate ...options?',
    values: { samples: HUMMED, sampleRate: ALT_SR, options: { fundamentalHz: 60, harmonics: 2 } },
    invalid: { sampleRate: 0 },
  },
  {
    name: 'masteringRepairDenoiseClassical',
    fn: sonare.masteringRepairDenoiseClassical,
    params: 'samples sampleRate ...options?',
    values: { samples: SIG, sampleRate: ALT_SR, options: { reductionDb: 20, nFft: 512 } },
    invalid: { sampleRate: 0 },
    inert: { sampleRate: 'the STFT runs in bins and frames' },
  },
  {
    name: 'masteringRepairDereverbClassical',
    fn: sonare.masteringRepairDereverbClassical,
    params: 'samples sampleRate ...options?',
    values: { samples: SIG, sampleRate: ALT_SR, options: { t60Sec: 1.2, nFft: 512 } },
    invalid: { sampleRate: 0 },
  },
  {
    name: 'masteringRepairDereverbConfigForRoom',
    fn: sonare.masteringRepairDereverbConfigForRoom,
    requestOnly: true,
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
    name: 'masteringRepairTrimSilence',
    fn: sonare.masteringRepairTrimSilence,
    params: 'samples sampleRate ...options?',
    values: { samples: SIG, sampleRate: ALT_SR, options: { paddingSamples: 300 } },
    invalid: { sampleRate: 0 },
    inert: { sampleRate: 'the threshold mode counts samples, not seconds' },
  },
  // metering
  {
    name: 'meteringPeakDb',
    fn: sonare.meteringPeakDb,
    params: 'samples sampleRate ...options?',
    values: { samples: SIG, sampleRate: ALT_SR, options: { validate: false } },
    invalid: { sampleRate: 0 },
    inert: { sampleRate: 'a sample peak reads no rate' },
  },
  {
    name: 'meteringRmsDb',
    fn: sonare.meteringRmsDb,
    params: 'samples sampleRate ...options?',
    values: { samples: SIG, sampleRate: ALT_SR, options: { validate: false } },
    invalid: { sampleRate: 0 },
    inert: { sampleRate: 'a whole-buffer RMS reads no rate' },
  },
  {
    name: 'meteringCrestFactorDb',
    fn: sonare.meteringCrestFactorDb,
    params: 'samples sampleRate ...options?',
    values: { samples: SIG, sampleRate: ALT_SR, options: { validate: false } },
    invalid: { sampleRate: 0 },
    inert: { sampleRate: 'a peak-to-RMS ratio reads no rate' },
  },
  {
    name: 'meteringDcOffset',
    fn: sonare.meteringDcOffset,
    params: 'samples sampleRate ...options?',
    values: { samples: SIG, sampleRate: ALT_SR, options: { validate: false } },
    invalid: { sampleRate: 0 },
    inert: { sampleRate: 'a mean reads no rate' },
  },
  {
    name: 'meteringSilenceRatio',
    fn: sonare.meteringSilenceRatio,
    params: 'samples sampleRate ...options?',
    values: {
      samples: SIG,
      sampleRate: ALT_SR,
      options: { thresholdDb: -20, frameLength: 256, hopLength: 64, validate: false },
    },
    invalid: { hopLength: 0 },
    inert: { sampleRate: 'frames are counted in samples' },
  },
  {
    name: 'meteringTruePeakDbtp',
    fn: sonare.meteringTruePeakDbtp,
    params: 'samples sampleRate ...options?',
    values: {
      samples: SIG,
      sampleRate: ALT_SR,
      options: { oversampleFactor: 2, validate: false },
    },
    invalid: { oversampleFactor: 3 },
    inert: { sampleRate: 'oversampling is rate-relative' },
  },
  {
    name: 'meteringDetectClipping',
    fn: sonare.meteringDetectClipping,
    params: 'samples sampleRate ...options?',
    values: {
      samples: SIG,
      sampleRate: ALT_SR,
      options: { thresholdDb: -20, minRegionSamples: 2 },
    },
    invalid: { sampleRate: 0 },
    inert: { sampleRate: 'clipped regions are counted in samples' },
  },
  {
    name: 'meteringDynamicRange',
    fn: sonare.meteringDynamicRange,
    params: 'samples sampleRate ...options?',
    values: { samples: SIG, sampleRate: ALT_SR, options: { windowSec: 0.1, hopSec: 0.05 } },
    invalid: { sampleRate: 0 },
  },
  {
    name: 'meteringStereoCorrelation',
    fn: sonare.meteringStereoCorrelation,
    requestOnly: true,
    params: 'left right sampleRate ...options?',
    values: { left: SIG, right: SIG2, sampleRate: ALT_SR, options: { validate: false } },
    invalid: { sampleRate: 0 },
  },
  {
    name: 'meteringStereoWidth',
    fn: sonare.meteringStereoWidth,
    requestOnly: true,
    params: 'left right sampleRate ...options?',
    values: { left: SIG, right: SIG2, sampleRate: ALT_SR, options: { validate: false } },
    invalid: { sampleRate: 0 },
  },
  {
    name: 'meteringVectorscope',
    fn: sonare.meteringVectorscope,
    requestOnly: true,
    params: 'left right sampleRate ...options?',
    values: { left: SIG, right: SIG2, sampleRate: ALT_SR, options: { maxPoints: 64 } },
    invalid: { sampleRate: 0 },
  },
  {
    name: 'meteringPhaseScope',
    fn: sonare.meteringPhaseScope,
    requestOnly: true,
    params: 'left right sampleRate ...options?',
    values: { left: SIG, right: SIG2, sampleRate: ALT_SR, options: { maxPoints: 64 } },
    invalid: { sampleRate: 0 },
  },
  {
    name: 'meteringSpectrum',
    fn: sonare.meteringSpectrum,
    params: 'samples sampleRate ...options?',
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
    params: 'samples sampleRate ...options?',
    values: { samples: SIG, sampleRate: ALT_SR, options: { startSample: 4096, nFft: 1024 } },
    invalid: { sampleRate: 0 },
  },
  {
    name: 'waveformPeaks',
    fn: sonare.waveformPeaks,
    requestOnly: true,
    params: 'channels ...options?',
    values: { channels: [SIG, SIG2], options: { samplesPerBucket: 256 } },
    invalid: { channels: [] },
  },
  {
    name: 'waveformPeakPyramid',
    fn: sonare.waveformPeakPyramid,
    requestOnly: true,
    params: 'channels ...options?',
    values: { channels: [SIG, SIG2], options: { samplesPerBucketLevels: [128, 1024] } },
    invalid: { channels: [] },
  },
  // mixing_oneshot.ts
  {
    name: 'mixStereo',
    fn: sonare.mixStereo,
    requestOnly: true,
    params: 'strips sampleRate',
    values: {
      strips: [
        { left: SIG, right: SIG2, faderDb: -6, pan: 0.4 },
        { left: SIG2, right: SIG, faderDb: 0, pan: -0.4 },
      ],
      sampleRate: ALT_SR,
    },
    invalid: { sampleRate: 0 },
  },
  {
    name: 'resample',
    fn: sonare.resample,
    params: 'samples srcSr targetSr',
    rename: { srcSr: 'sampleRate', targetSr: 'targetSampleRate' },
    values: { samples: SIG, srcSr: SR, targetSr: 16000 },
    invalid: { targetSr: 0 },
  },
  // voice_changer.ts
  {
    name: 'voiceChange',
    fn: sonare.voiceChange,
    params: 'samples sampleRate ...options?',
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
    requestOnly: true,
    params: 'channels sampleRate config?',
    values: { channels: [SIG, SIG2], sampleRate: ALT_SR, config: 'choirHarmony' },
    invalid: { sampleRate: 0 },
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
    const heads = [
      ...source.matchAll(/^export (?:async )?function (\w+)(?:<[^(]*>)?\(\s*([\w]+)\??:/gm),
    ];
    const byName = new Map<string, string[]>();
    for (const [, name, first] of heads) {
      byName.set(name, [...(byName.get(name) ?? []), first]);
    }
    for (const [name, firsts] of byName) {
      // A request overload plus at least one positional overload.
      if (firsts[0] === 'request' && firsts.length >= 2) {
        names.add(name);
      }
    }
  }
  return [...names].sort();
}

describe('request-object equivalence', () => {
  it('has one row per exported request-object overload', () => {
    expect(
      ROWS.filter((row) => row.requestOnly !== true)
        .map((row) => row.name)
        .sort(),
    ).toEqual(overloadedFunctions());
  });

  it('names an exported function in every request-only row', () => {
    for (const row of ROWS.filter((r) => r.requestOnly === true)) {
      expect(row.fn, row.name).toBe((sonare as unknown as Record<string, unknown>)[row.name]);
      expect(typeof row.fn, row.name).toBe('function');
    }
  });

  describe.each(ROWS)('$name', (row) => {
    const specs = specsOf(row);
    const requiredOnly = Object.fromEntries(
      specs.filter((spec) => !spec.optional).map((spec) => [spec.name, row.values[spec.name]]),
    );

    it('returns the positional result for non-default values', async () => {
      const positional = await run(row, specs, row.values, 'positional');
      expect(positional).not.toHaveProperty('error');
      if (row.requestOnly !== true) {
        expect(await run(row, specs, row.values, 'request')).toEqual(positional);
      }
    });

    it('shares the positional defaults', async () => {
      const request = await run(row, specs, requiredOnly, 'request');
      if (row.requestOnly === true) {
        expect(request).not.toHaveProperty('error');
        return;
      }
      expect(request).toEqual(await run(row, specs, requiredOnly, 'positional'));
    });

    // One call per field; a row that measures its tuning rebuilds the constant-Q kernels once.
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
    }, 20_000);

    it('refuses an invalid value identically in both forms', async () => {
      const bad = withInvalid(row, specs);
      const positional = await run(row, specs, bad, 'positional');
      expect(positional).toHaveProperty('error');
      expect(await run(row, specs, bad, 'request')).toEqual(positional);
    });
  });
});
