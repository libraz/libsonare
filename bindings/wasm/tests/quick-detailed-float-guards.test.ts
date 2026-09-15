/**
 * What a POSITIONAL `float` parameter is allowed to become on its way into the
 * detailed per-domain quick-analysis entry points
 * (`analyzeBpm` / `analyzeRhythm` / `analyzeDynamics` / `analyzeTimbre`,
 * `src/wasm/bindings/analysis/quick_detailed.cpp`), covering the 9 float fields
 * converted there. Same defect as `positional-float-guards.test.ts`: embind's
 * float glue SATURATES, so a caller-chosen finite number wider than `FLT_MAX`
 * (~3.4028235e38) used to arrive as an infinity with no exception raised
 * anywhere. `3.5e38` is the case that matters most, because it is a number a
 * caller could plausibly choose rather than a deliberately extreme one.
 *
 * Acceptance is asserted by reading a value back, never by "it did not
 * throw" — a function that ignored its argument entirely would satisfy every
 * refusal case below in exactly the same way. The bpmMin/bpmMax controls lean
 * on a bound read from `BpmAnalyzer::analyze` (src/analysis/bpm_analyzer.cpp):
 * every detection and fallback path filters or bins candidates against
 * `[bpmMin, bpmMax]` before choosing one, so the returned `bpm` stays inside
 * that range regardless of which branch runs — measured against the built
 * module, not just traced. `startBpm` has no such control here: see the
 * comment above its refusal-only coverage below for what was actually
 * measured.
 */

import { beforeAll, describe, expect, it } from 'vitest';
import {
  analyzeBpm,
  analyzeDynamics,
  analyzeRhythm,
  analyzeTimbre,
  ErrorCode,
  init,
  isSonareError,
  type SonareError,
} from '../dist/index.js';

/** The suffix every one of these refusals ends with, whatever the key is. */
const RANGE_MESSAGE = 'must be a finite number within the 32-bit float range';

/**
 * Values that saturate onto a legal float. These are the ones that matter: each
 * is a number the caller chose, survives every JS-side check, and used to
 * arrive as an infinity a config guard written `x > lo` or `x < hi` waves
 * through.
 */
const SATURATES_ONTO_A_FLOAT = [1e40, -1e40, 3.5e38, 1e300, -1e300];

/** Non-finite values written directly, which must reach the same refusal. */
const NON_FINITE = [Number.NaN, Number.POSITIVE_INFINITY, Number.NEGATIVE_INFINITY];

const REFUSED = [...SATURATES_ONTO_A_FLOAT, ...NON_FINITE];

function capture(run: () => unknown): unknown {
  try {
    run();
    return undefined;
  } catch (error) {
    return error;
  }
}

/** Asserts the caught value is the float-range refusal naming `key`. */
function expectRangeRefusal(caught: unknown, key: string, context: string): void {
  expect(isSonareError(caught), `${context}: expected a SonareError`).toBe(true);
  const error = caught as SonareError;
  expect(error.code, context).toBe(ErrorCode.InvalidParameter);
  expect(error.message, context).toBe(`${key} ${RANGE_MESSAGE}`);
}

// A 256-sample silent buffer. checkedFloatFromVal refuses before this file's
// entry points touch the audio at all, so the refusal cases below do not need
// real content -- only a buffer that clears the JS-side non-empty/finite/
// sample-rate checks that run ahead of the WASM call.
const REFUSAL_AUDIO = new Float32Array(256);
const REFUSAL_SR = 8000;

/** A short burst at the start of each period, rest silent. */
function clickTrain(periodSamples: number, periods: number, burstLength = 3): Float32Array {
  const out = new Float32Array(periodSamples * periods);
  for (let p = 0; p < periods; p++) {
    const start = p * periodSamples;
    for (let i = 0; i < burstLength; i++) {
      out[start + i] = 1;
    }
  }
  return out;
}

// 0.4s period at 8000 Hz -> a 150 BPM click train, six periods.
const BPM_SR = 8000;
const BPM_PERIOD_SAMPLES = 3200;
const bpmClickTrain = clickTrain(BPM_PERIOD_SAMPLES, 6);
const BPM_NFFT = 256;
const BPM_HOP = 64;

/** One float field of one entry point, and how to drive it with a value. */
interface FloatField {
  entry: string;
  key: string;
  run: (value: number) => unknown;
}

const FLOAT_FIELDS: FloatField[] = [
  {
    entry: 'analyzeBpm',
    key: 'bpmMin',
    run: (v) => analyzeBpm(REFUSAL_AUDIO, REFUSAL_SR, { bpmMin: v, bpmMax: 300, startBpm: 120 }),
  },
  {
    entry: 'analyzeBpm',
    key: 'bpmMax',
    run: (v) => analyzeBpm(REFUSAL_AUDIO, REFUSAL_SR, { bpmMin: 30, bpmMax: v, startBpm: 120 }),
  },
  {
    entry: 'analyzeBpm',
    key: 'startBpm',
    run: (v) => analyzeBpm(REFUSAL_AUDIO, REFUSAL_SR, { bpmMin: 30, bpmMax: 300, startBpm: v }),
  },
  {
    entry: 'analyzeRhythm',
    key: 'bpmMin',
    run: (v) => analyzeRhythm(REFUSAL_AUDIO, REFUSAL_SR, { bpmMin: v, bpmMax: 200, startBpm: 120 }),
  },
  {
    entry: 'analyzeRhythm',
    key: 'bpmMax',
    run: (v) => analyzeRhythm(REFUSAL_AUDIO, REFUSAL_SR, { bpmMin: 60, bpmMax: v, startBpm: 120 }),
  },
  {
    entry: 'analyzeRhythm',
    key: 'startBpm',
    run: (v) => analyzeRhythm(REFUSAL_AUDIO, REFUSAL_SR, { bpmMin: 60, bpmMax: 200, startBpm: v }),
  },
  {
    entry: 'analyzeDynamics',
    key: 'windowSec',
    run: (v) => analyzeDynamics(REFUSAL_AUDIO, REFUSAL_SR, { windowSec: v }),
  },
  {
    entry: 'analyzeDynamics',
    key: 'compressionThreshold',
    run: (v) => analyzeDynamics(REFUSAL_AUDIO, REFUSAL_SR, { compressionThreshold: v }),
  },
  {
    entry: 'analyzeTimbre',
    key: 'windowSec',
    run: (v) => analyzeTimbre(REFUSAL_AUDIO, REFUSAL_SR, { windowSec: v }),
  },
];

beforeAll(async () => {
  await init();
});

describe('every converted quick-detailed float parameter refuses a value the float type cannot hold', () => {
  it('covers one field per formerly-plain-float quick-detailed parameter', () => {
    // A field dropped from the table stops being covered without anything
    // going red, which is the one way this file could quietly shrink.
    expect(FLOAT_FIELDS).toHaveLength(9);
    const ids = FLOAT_FIELDS.map((field) => `${field.entry}.${field.key}`);
    expect(new Set(ids).size).toBe(ids.length);
  });

  it.each(FLOAT_FIELDS)('$entry names $key rather than accepting an infinity', ({ key, run }) => {
    for (const value of REFUSED) {
      expectRangeRefusal(
        capture(() => run(value)),
        key,
        `${key} = ${value}`,
      );
    }
  });
});

describe('analyzeBpm consumes bpmMin and bpmMax it accepts', () => {
  // startBpm has no read-back control here: measured against the built module
  // (bpmMin/bpmMax forced to 1e6/2e6, sweeping startBpm across 96/120/180 and
  // values outside every range tried, on both a detectable 150 BPM click
  // train and undetectable silence/noise), the returned bpm never tracked
  // startBpm — it either reflects real detection or a fixed fallback that is
  // neither startBpm nor bpmMin. That is a measured absence on the inputs
  // tried, not a proof the field is dropped; the guard coverage in the
  // refusal table above still applies regardless.

  it('reads bpmMin as changing the result between an unreachable range and real detection', () => {
    // bpm_to_lag(1e6, sr, hopLength) rounds to 0 regardless of audio content,
    // so bpmMin: 1e6 puts every lag out of range and forces BpmAnalyzer's
    // earliest return path. bpmMin: 30 instead gives the 150 BPM click
    // train's autocorrelation peak a reachable window to detect within. The
    // two configs differ only in bpmMin, so a differing result demonstrates
    // bpmMin reaches the analyzer rather than being dropped or defaulted.
    const unreachable = analyzeBpm(bpmClickTrain, BPM_SR, {
      bpmMin: 1e6,
      bpmMax: 2e6,
      nFft: BPM_NFFT,
      hopLength: BPM_HOP,
    });
    const detected = analyzeBpm(bpmClickTrain, BPM_SR, {
      bpmMin: 30,
      bpmMax: 300,
      nFft: BPM_NFFT,
      hopLength: BPM_HOP,
    });
    expect(detected.bpm).not.toBe(unreachable.bpm);
  });

  it('reads bpmMax as bounding the result to the configured range', () => {
    const narrow = analyzeBpm(bpmClickTrain, BPM_SR, {
      bpmMin: 30,
      bpmMax: 40,
      nFft: BPM_NFFT,
      hopLength: BPM_HOP,
    });
    const wide = analyzeBpm(bpmClickTrain, BPM_SR, {
      bpmMin: 60,
      bpmMax: 300,
      nFft: BPM_NFFT,
      hopLength: BPM_HOP,
    });
    expect(narrow.bpm).toBeLessThanOrEqual(40.5);
    expect(wide.bpm).toBeGreaterThanOrEqual(60);
    expect(wide.bpm).toBeLessThanOrEqual(300.5);
  });
});

describe('analyzeRhythm consumes bpmMin and bpmMax it accepts', () => {
  // analyzeRhythm's `bpm` is BeatAnalyzer::bpm(), which BeatAnalyzer::track_beats()
  // sets from the very same BpmAnalyzer with the same bpmMin/bpmMax/startBpm, so
  // the same bound holds. No startBpm read-back control here either -- see the
  // comment above the analyzeBpm describe block for what was measured.

  it('reads bpmMin as changing the result between an unreachable range and real detection', () => {
    const unreachable = analyzeRhythm(bpmClickTrain, BPM_SR, {
      bpmMin: 1e6,
      bpmMax: 2e6,
      nFft: BPM_NFFT,
      hopLength: BPM_HOP,
    });
    const detected = analyzeRhythm(bpmClickTrain, BPM_SR, {
      bpmMin: 60,
      bpmMax: 200,
      nFft: BPM_NFFT,
      hopLength: BPM_HOP,
    });
    expect(detected.bpm).not.toBe(unreachable.bpm);
  });

  it('reads bpmMax as bounding the result to the configured range', () => {
    const narrow = analyzeRhythm(bpmClickTrain, BPM_SR, {
      bpmMin: 30,
      bpmMax: 40,
      nFft: BPM_NFFT,
      hopLength: BPM_HOP,
    });
    const wide = analyzeRhythm(bpmClickTrain, BPM_SR, {
      bpmMin: 60,
      bpmMax: 200,
      nFft: BPM_NFFT,
      hopLength: BPM_HOP,
    });
    expect(narrow.bpm).toBeLessThanOrEqual(40.5);
    expect(wide.bpm).toBeGreaterThanOrEqual(60);
    expect(wide.bpm).toBeLessThanOrEqual(200.5);
  });
});

describe('analyzeDynamics consumes windowSec and compressionThreshold it accepts', () => {
  // Eight single-sample bursts of amplitude 1 in an otherwise silent 8000-sample
  // (1s @ 8000 Hz) buffer: sum of squares is exactly 8, so
  // rms = sqrt(8/8000) = sqrt(1e-3) = 10^-1.5, giving rms_db = -30 dB exactly and
  // peak_db = 0 dB, for a crest factor of 30 dB -- comfortably clear of the 8 dB
  // "compressed" floor regardless of compressionThreshold.
  const DYNAMICS_SR = 8000;
  const DYNAMICS_SAMPLES = 8000;
  const SPIKE_SPACING = 1000;
  const spikyBuffer = (() => {
    const out = new Float32Array(DYNAMICS_SAMPLES);
    for (let i = 0; i < DYNAMICS_SAMPLES; i += SPIKE_SPACING) {
      out[i] = 1;
    }
    return out;
  })();

  it('reads compressionThreshold as flipping isCompressed', () => {
    // dynamicRangeDb (p95 - p10 of the loudness curve) is never negative by
    // construction, so a threshold of 0 always keeps the first isCompressed
    // clause false; the 30 dB crest factor above keeps the second clause
    // false too. A threshold of 1000 dB is far past any value a dB spread
    // computed from a finite epsilon floor could reach, so the first clause
    // is always true instead.
    const lenient = analyzeDynamics(spikyBuffer, DYNAMICS_SR, { compressionThreshold: 0 });
    const strict = analyzeDynamics(spikyBuffer, DYNAMICS_SR, { compressionThreshold: 1000 });
    expect(lenient.isCompressed).toBe(false);
    expect(strict.isCompressed).toBe(true);
  });

  it('reads windowSec back as the loudness curve length', () => {
    // window_samples = window_sec * sr, both exact binary fractions here, so the
    // window count `floor((n_samples - window_samples) / hopLength) + 1` from
    // DynamicsAnalyzer::analyze is exact: (8000-1000)/1000+1 = 8 and
    // (8000-4000)/1000+1 = 5.
    const short = analyzeDynamics(spikyBuffer, DYNAMICS_SR, { windowSec: 0.125, hopLength: 1000 });
    const long = analyzeDynamics(spikyBuffer, DYNAMICS_SR, { windowSec: 0.5, hopLength: 1000 });
    expect(short.loudnessTimes.length).toBe(8);
    expect(long.loudnessTimes.length).toBe(5);
  });
});

describe('analyzeTimbre consumes windowSec it accepts', () => {
  const TIMBRE_SR = 8000;
  const TIMBRE_SAMPLES = 16000;
  const timbreTone = (() => {
    const out = new Float32Array(TIMBRE_SAMPLES);
    for (let i = 0; i < TIMBRE_SAMPLES; i++) {
      out[i] = 0.5 * Math.sin((2 * Math.PI * 300 * i) / TIMBRE_SR);
    }
    return out;
  })();

  it('reads two windowSec values as two different timbreOverTime lengths', () => {
    // TimbreAnalyzer::analyze steps every window_frames = max(1, windowSec *
    // sr / hopLength) frames, so a short window keeps far more entries than a
    // long one over the same ~30-frame analysis.
    const short = analyzeTimbre(timbreTone, TIMBRE_SR, { windowSec: 0.1 });
    const long = analyzeTimbre(timbreTone, TIMBRE_SR, { windowSec: 1.0 });
    expect(short.timbreOverTime.length).toBeGreaterThan(long.timbreOverTime.length);
  });
});
