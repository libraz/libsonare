/**
 * An enum ordinal names a member, so the readers that take one check its domain.
 *
 * This is a different check from the narrowing ones next door. Narrowing asks
 * whether the value survived the conversion — `31.5` arriving as `31` does not
 * change the quantity that was asked for, so it may pass. An ordinal is not a
 * quantity: `1.5` selecting the second member picks something the caller never
 * spelled, and the call SUCCEEDS, which is why the defect signature here is a
 * plausible result rather than an exception.
 *
 * Each site therefore asserts three things, and the first two are what keep the
 * third from being vacuous:
 *
 *   1. Two or more legal ordinals produce DIFFERENT output. A test built only
 *      on refusals passes against a reader that refuses everything, and a test
 *      built on "it did not throw" passes against one that ignores the field.
 *   2. Each ordinal is bit-identical to the NAME for the same member. Asserting
 *      only that two ordinals differ would also pass on a wrong mapping, where
 *      0 selects the second member and 2 the first; pinning each ordinal to its
 *      name fixes which member was chosen, not merely that a choice was made.
 *   3. A value outside the domain, and a fractional or non-finite one inside it,
 *      are refused.
 *
 * Two fields are deliberately absent. `preAvg` and `postAvg` are read by the
 * same integer path as their siblings, but three probes — onset count, onset
 * positions, and onset positions over a loudness-ramp signal at three `delta`
 * settings — produced identical output for values spanning 1 to 200, so no
 * positive control for them separates and nothing here can claim they are read
 * at all. They are left out rather than asserted on a flat control.
 */

import { beforeAll, describe, expect, it } from 'vitest';
import {
  detectOnsets,
  ErrorCode,
  extractPercussiveEvents,
  init,
  isSonareError,
  masteringDynamicsCompressor,
  RealtimeEngine,
  type SonareError,
  type SynthModRouting,
  tempogram,
} from '../dist/index.js';

beforeAll(async () => {
  await init();
});

/** The spellings `val::as<int>()` saturates: every one of them used to arrive as INT_MAX. */
const SATURATING = [2 ** 31, 2 ** 40, 3e9, 4294967295];

function expectRefusalNaming(field: string, run: () => unknown): void {
  let caught: unknown;
  try {
    run();
  } catch (error) {
    caught = error;
  }
  expect(caught, `expected ${field} to be refused, got no throw`).toBeDefined();
  expect(isSonareError(caught)).toBe(true);
  const error = caught as SonareError;
  expect(error.code).toBe(ErrorCode.InvalidParameter);
  // The field has to be named, or the message cannot tell the caller which of
  // the options they passed was the bad one.
  expect(error.message).toContain(field);
}

// ---------------------------------------------------------------------------
// The shared options-bag integer reader
// ---------------------------------------------------------------------------

const sampleRate = 22050;

/** Two seconds of decaying plucks: enough onsets that the framing options move them. */
const plucks = new Float32Array(sampleRate * 2).map((_, i) => {
  const sinceHit = i % 4410;
  return 0.8 * Math.exp(-(sinceHit / 400)) * Math.sin((2 * Math.PI * 220 * i) / sampleRate);
});

/** The onset POSITIONS, not their count — the count is flat across most of these fields. */
const onsetDigest = (options: Record<string, number | boolean>): string =>
  Array.from(detectOnsets(plucks, sampleRate, options))
    .map((t) => t.toFixed(6))
    .join(',');

/** One integer options-bag field and two legal values measured to separate. */
interface IntFieldCase {
  readonly field: string;
  /** Options the field needs before it reaches a live path. */
  readonly context?: Record<string, number | boolean>;
  /** Two legal values whose outputs differ. */
  readonly separating: readonly [number, number];
}

const INT_FIELDS: readonly IntFieldCase[] = [
  { field: 'nFft', separating: [1024, 2048] },
  { field: 'hopLength', separating: [256, 512] },
  { field: 'preMax', separating: [1, 30] },
  { field: 'postMax', separating: [1, 30] },
  { field: 'wait', separating: [0, 60] },
  // Backtracking has to be on before the search range can move an onset.
  { field: 'backtrackRange', context: { backtrack: true }, separating: [1, 10] },
];

describe('an options-bag integer field refuses a value the conversion would saturate', () => {
  for (const { field, context, separating } of INT_FIELDS) {
    const run = (value: number): string => onsetDigest({ ...context, [field]: value });

    it(`${field}: two legal values reach different onsets`, () => {
      const [low, high] = separating;
      // Without this the refusals below would also pass against a reader that
      // never looked at the field.
      expect(run(low)).not.toBe(run(high));
    });

    it(`${field}: refuses every value that used to arrive as INT_MAX`, () => {
      for (const value of SATURATING) {
        expectRefusalNaming(field, () => run(value));
      }
    });
  }
});

describe('the saturating values are refused rather than collapsed onto one result', () => {
  it('detectOnsets nFft: all four spellings are refused, not merged into INT_MAX', () => {
    // The claim that matters is not "the result differed from the default" -- a
    // saturated read differs from the default too. It is that these four
    // distinct caller mistakes can no longer become one indistinguishable
    // number, which after the fix means each is refused on its own.
    const outcomes = SATURATING.map((value) => {
      try {
        return `ok:${onsetDigest({ nFft: value })}`;
      } catch (error) {
        return `refused:${(error as SonareError).code}`;
      }
    });
    expect(outcomes).toEqual(SATURATING.map(() => `refused:${ErrorCode.InvalidParameter}`));
  });
});

describe('extractPercussiveEvents reads onsetWait through the same guard', () => {
  const hits = new Float32Array(sampleRate * 2).map((_, i) => {
    const sinceHit = i % 4410;
    const transient =
      sinceHit < 200 ? Math.exp(-sinceHit / 40) * (((i * 2654435761) % 2000) / 1000 - 1) : 0;
    return transient + 0.15 * Math.sin((2 * Math.PI * 220 * i) / sampleRate);
  });
  const count = (onsetWait: number): number =>
    extractPercussiveEvents({ samples: hits, sampleRate, onsetWait }).length;

  it('two legal waits keep a different number of events', () => {
    expect(count(0)).toBeGreaterThan(count(20));
  });

  it('refuses every value that used to arrive as INT_MAX', () => {
    for (const value of SATURATING) {
      expectRefusalNaming('onsetWait', () => count(value));
    }
  });
});

// ---------------------------------------------------------------------------
// Ordinal domain: synth patch mod routings
// ---------------------------------------------------------------------------

describe('a synth patch mod routing refuses an ordinal outside its enum', () => {
  const base = { engineMode: 1, waveform: 2, cutoffHz: 600, resonanceQ: 6 };

  const render = (routing: Record<string, unknown>): string => {
    const engine = new RealtimeEngine(48000, 4096);
    try {
      // The out-of-domain ordinals under test are what the declared type exists
      // to forbid; the C++ reader, not the type, is the subject here.
      engine.setSynthInstrument({
        ...base,
        modRoutings: [routing as unknown as SynthModRouting],
      });
      engine.pushMidiNoteOn(0, 0, 0, 60, 100);
      engine.play();
      const out = engine.process([new Float32Array(4096), new Float32Array(4096)]);
      let sum = 0;
      for (const value of out[0]) {
        sum += value * value;
      }
      return Math.sqrt(sum / out[0].length).toFixed(9);
    } finally {
      engine.destroy();
    }
  };

  const toDestination = (destination: number | string): string =>
    render({ source: 1, destination, depth: 4800 });
  const fromSource = (source: number | string): string =>
    render({ source, destination: 2, depth: 4800 });

  it('separates the destinations it is given', () => {
    // pitch-cents, cutoff-cents, amp-gain, resonance-q and vibrato-depth-cents
    // all move the rendered note by a different amount.
    const rendered = [1, 2, 3, 5, 6].map(toDestination);
    expect(new Set(rendered).size).toBe(rendered.length);
  });

  it('separates the sources it is given', () => {
    // amp-env and filter-env are excluded: the two envelopes are identical under
    // this patch's defaults, so they render the same and would read as a failure
    // to distinguish rather than as the degeneracy they are.
    const rendered = [3, 4, 5, 8].map(fromSource);
    expect(new Set(rendered).size).toBe(rendered.length);
  });

  it('refuses none on either end, by ordinal and by name', () => {
    for (const none of [0, 'none']) {
      expect(() => fromSource(none)).toThrow(/none/);
      expect(() => toDestination(none)).toThrow(/none/);
    }
  });

  it('renders each ordinal bit-identically to the name for the same member', () => {
    // Distinctness alone would also hold under a mapping shifted by one; this
    // pins which member each ordinal selected.
    const sources = [
      'none',
      'amp-env',
      'filter-env',
      'lfo1',
      'lfo2',
      'velocity',
      'key-track',
      'mod-wheel',
      'random',
      'breath',
      'aftertouch',
      'expression-cc',
      'pitch-bend',
    ];
    // Ordinal 0 is none, which is refused rather than rendered.
    for (let ordinal = 1; ordinal < sources.length; ordinal++) {
      expect(fromSource(ordinal), `source ordinal ${ordinal}`).toBe(fromSource(sources[ordinal]));
    }
    const destinations = [
      'none',
      'pitch-cents',
      'cutoff-cents',
      'amp-gain',
      'pan-units',
      'resonance-q',
      'vibrato-depth-cents',
      'filter-env-depth',
      'lfo1-rate-scale',
      'excitation-force',
      'excitation-position',
      'excitation-brightness',
      'spectrum-morph',
    ];
    for (let ordinal = 1; ordinal < destinations.length; ordinal++) {
      expect(toDestination(ordinal), `destination ordinal ${ordinal}`).toBe(
        toDestination(destinations[ordinal]),
      );
    }
  });

  it('refuses an ordinal past the end of the table instead of rendering silence', () => {
    // 13, 99 and -1 all used to be accepted and render exactly what 'none'
    // renders, so the caller got a working patch with their routing dropped.
    // Both tables hold 13 values, so 13 is the first ordinal past either end.
    for (const value of [13, 99, -1]) {
      expectRefusalNaming('mod source', () => fromSource(value));
      expectRefusalNaming('mod destination', () => toDestination(value));
    }
  });

  it('refuses a fractional ordinal, which selects a member that was not named', () => {
    for (const value of [1.5, 0.5, Number.NaN, Number.POSITIVE_INFINITY]) {
      expectRefusalNaming('mod source', () => fromSource(value));
      expectRefusalNaming('mod destination', () => toDestination(value));
    }
  });
});

// ---------------------------------------------------------------------------
// Ordinal domain: compressor detector
// ---------------------------------------------------------------------------

describe('the compressor detector refuses an ordinal that is not whole', () => {
  const burst = new Float32Array(4800).map(
    (_, i) => 0.6 * Math.sin((2 * Math.PI * 440 * i) / 48000) * (i < 2400 ? 1 : 0.2),
  );
  const compress = (detector: number | string): string => {
    const { samples } = masteringDynamicsCompressor(burst, 48000, {
      thresholdDb: -20,
      ratio: 8,
      attackMs: 1,
      releaseMs: 50,
      detector: detector as never,
    });
    let sum = 0;
    for (const value of samples) {
      sum += value * value;
    }
    return Math.sqrt(sum / samples.length).toFixed(9);
  };

  it('separates its three detectors', () => {
    const rendered = [0, 1, 2].map(compress);
    expect(new Set(rendered).size).toBe(3);
  });

  it('renders each ordinal bit-identically to its name', () => {
    expect(compress(0)).toBe(compress('peak'));
    expect(compress(1)).toBe(compress('rms'));
    expect(compress(2)).toBe(compress('log_rms'));
  });

  it('refuses a fractional or non-finite detector instead of truncating onto a member', () => {
    // 1.5 selected Rms, 0.5 and NaN selected Peak, 2.5 selected LogRms -- four
    // successful calls, each running a detector the caller did not ask for.
    for (const value of [0.5, 1.5, 2.5, Number.NaN, Number.POSITIVE_INFINITY]) {
      expectRefusalNaming('detector', () => compress(value));
    }
  });

  it('still refuses an ordinal outside the set', () => {
    for (const value of [99, -1, ...SATURATING]) {
      expectRefusalNaming('detector', () => compress(value));
    }
  });
});

// ---------------------------------------------------------------------------
// Ordinal domain: automation curve, reached through the embind class directly
// ---------------------------------------------------------------------------

/**
 * This one has to drive the raw embind class, and that is the whole point.
 *
 * `Project.addAutomationLane` normalizes `curve` in TS before it reaches
 * embind, so a test written against the facade exercises the TS check and never
 * reaches the C++ reader — it would pass whether or not the reader validates
 * anything. The reader's own doc comment says it runs "for a caller that drives
 * the embind class directly", which is the path taken here.
 *
 * Two limits, stated rather than worked around. The raw module does not export
 * `getExceptionMessage`, so a C++ throw arrives as an exception pointer and this
 * file can assert that a value is refused but not which message named it. And
 * the project JSON does not serialize the curve field, so which curve a
 * fractional ordinal used to select is read from the truncation in the source,
 * not measured. What IS measured is acceptance versus refusal, and the legal
 * values below are what keep that from being vacuous.
 */
describe('the automation curve refuses an ordinal the embind path used to take', () => {
  interface RawProject {
    addTrack(desc: { name: string }): number;
    addAutomationLane(trackId: number, desc: unknown): number;
    delete(): void;
  }

  // `SonareModule` does not declare the embind classes, though the module
  // exposes them at runtime — part of why this path had no coverage. Reaching
  // past the declaration is the point of the test, so the cast is deliberate.
  const rawModule = async (): Promise<{ Project: new () => RawProject }> =>
    (await (await import('../dist/sonare.js')).default()) as unknown as {
      Project: new () => RawProject;
    };

  let Project_: new () => RawProject;

  beforeAll(async () => {
    Project_ = (await rawModule()).Project;
  });

  /** Adds one lane carrying `curve` on both breakpoints; throws if the reader refuses. */
  const withCurve = (curve: unknown): void => {
    const project = new Project_();
    try {
      const track = project.addTrack({ name: 't' });
      project.addAutomationLane(track, {
        targetParamId: 7,
        points: [
          { ppq: 0, value: 0, curve },
          { ppq: 480, value: 1, curve },
        ],
      });
    } finally {
      project.delete();
    }
  };

  const refused = (curve: unknown): boolean => {
    try {
      withCurve(curve);
      return false;
    } catch {
      return true;
    }
  };

  it('accepts all four legal ordinals and all five documented spellings', () => {
    // The control. Without it every assertion below would also hold for a
    // reader that refused every curve it was given.
    for (const ordinal of [0, 1, 2, 3]) {
      expect(refused(ordinal), `ordinal ${ordinal} is a legal curve`).toBe(false);
    }
    for (const name of ['linear', 'exponential', 'hold', 's-curve', 'scurve']) {
      expect(refused(name), `'${name}' is a documented spelling`).toBe(false);
    }
  });

  it('refuses a fractional or non-finite ordinal instead of truncating onto a curve', () => {
    // These are the values the guard newly refuses: every one of them was
    // accepted before it, so this is the assertion that would go green again if
    // the check were removed.
    for (const curve of [1.5, 2.9, -0.5, Number.NaN, Number.POSITIVE_INFINITY]) {
      expect(refused(curve), `curve ${String(curve)} is not a whole ordinal`).toBe(true);
    }
  });

  it('refuses an ordinal past the end of the enum and an unknown spelling', () => {
    for (const curve of [4, 99, -1, 2 ** 31, 'bezier', '']) {
      expect(refused(curve), `curve ${String(curve)} is outside the enum`).toBe(true);
    }
  });
});

// ---------------------------------------------------------------------------
// Ordinal domain: tempogram mode
// ---------------------------------------------------------------------------

describe('the tempogram mode refuses an ordinal that is not whole', () => {
  const envelope = new Float32Array(2000).map((_, i) => (i % 20 === 0 ? 1 : 0.05));
  const run = (mode: number | string): string => {
    const { data } = tempogram(envelope, 22050, 512, 128, mode as never);
    let sum = 0;
    for (const value of data) {
      sum += value * value;
    }
    return Math.sqrt(sum / data.length).toFixed(9);
  };

  it('separates its two modes', () => {
    expect(run(0)).not.toBe(run(1));
  });

  it('renders each ordinal bit-identically to its name', () => {
    expect(run(0)).toBe(run('autocorrelation'));
    expect(run(1)).toBe(run('cosine'));
  });

  it('refuses a fractional or non-finite mode instead of truncating onto a member', () => {
    // 0.5 ran autocorrelation and 1.5 ran cosine, both reporting success.
    for (const value of [0.5, 1.5, Number.NaN, Number.POSITIVE_INFINITY]) {
      expectRefusalNaming('tempogram mode', () => run(value));
    }
  });

  it('still refuses an ordinal outside the set', () => {
    for (const value of [99, -1]) {
      expectRefusalNaming('tempogram mode', () => run(value));
    }
  });
});
