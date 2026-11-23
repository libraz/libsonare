/**
 * The documented "0 selects the library default" scalars on the WASM surface.
 *
 * These parameters document 0 as the sentinel requesting the library default.
 * The convention only holds when a value that is not the sentinel is either
 * applied or refused -- a value quietly swapped for the default produces a
 * plausible result for a call the caller never made, which is the one outcome
 * that cannot be told apart from working.
 *
 * Each case asserts the outcome: the refusal, or the rendered content. The zero
 * cases are the positive control -- 0 has to match omitting the parameter *and*
 * a non-sentinel value has to differ, or "0 selects the default" is
 * indistinguishable from "0 is applied as 0".
 */

import { beforeAll, describe, expect, it } from 'vitest';
import { init, masteringNormalizeLoudness, meteringSpectrum, RealtimeEngine } from '../src/index';

const SR = 44100;

/** Loud bursts over a quiet bed, so the true-peak limiter engages and the
 *  release time is observable in the output. */
function bursts(n: number): Float32Array {
  const out = new Float32Array(n);
  for (let i = 0; i < n; i++) {
    const envelope = i % 4096 < 256 ? 1.0 : 0.02;
    out[i] = envelope * 0.95 * Math.sin((2 * Math.PI * 110 * i) / SR);
  }
  return out;
}

function maxAbsDiff(a: Float32Array, b: Float32Array): number {
  expect(a.length).toBe(b.length);
  let worst = 0;
  for (let i = 0; i < a.length; i++) {
    worst = Math.max(worst, Math.abs(a[i] - b[i]));
  }
  return worst;
}

// The facade refuses every value that is not a positive finite float, an explicit
// 0 included: the default is reached by omitting the field.
const NON_SENTINEL_REFUSED: [string, number, RegExp][] = [
  ['zero', 0, /releaseMs must be finite and greater than 0$/],
  ['a negative value', -5, /releaseMs must be finite and greater than 0$/],
  ['NaN', Number.NaN, /releaseMs must be finite and greater than 0$/],
  ['positive infinity', Number.POSITIVE_INFINITY, /releaseMs must be finite and greater than 0$/],
  ['negative infinity', Number.NEGATIVE_INFINITY, /releaseMs must be finite and greater than 0$/],
];

describe('masteringNormalizeLoudness releaseMs', () => {
  beforeAll(async () => {
    await init();
  });

  const samples = bursts(SR);
  const master = (releaseMs?: number) =>
    masteringNormalizeLoudness(
      releaseMs === undefined
        ? { samples, sampleRate: SR, targetLufs: -6 }
        : { samples, sampleRate: SR, targetLufs: -6, releaseMs },
    ).samples;

  it.each(NON_SENTINEL_REFUSED)(
    'refuses %s instead of using the default',
    (_label, value, refusal) => {
      expect(() => master(value)).toThrow(refusal);
    },
  );

  it('takes the library default when omitted', () => {
    const omitted = master();
    // 50 ms is the documented default an omitted field resolves to.
    expect(maxAbsDiff(master(50), omitted)).toBe(0);
  });

  it('applies a non-sentinel release time', () => {
    // Without this the equality above would also hold for a release time the
    // limiter ignored.
    const omitted = master();
    expect(maxAbsDiff(master(5), omitted)).toBeGreaterThan(0.1);
    expect(maxAbsDiff(master(200), omitted)).toBeGreaterThan(0.1);
  });
});

describe('meteringSpectrum optional scalars', () => {
  beforeAll(async () => {
    await init();
  });

  const samples = bursts(8192);
  const spectrum = (options: Record<string, number> = {}) =>
    meteringSpectrum({ samples, sampleRate: SR, ...options } as never).db;

  it.each([
    ['nFft', 'nFft must be a power of two in [1, 1073741824]'],
    ['octaveFraction', 'octaveFraction must be an integer in [1, 2147483647]'],
    ['dbRef', 'dbRef must be finite and at least 0'],
    ['dbAmin', 'dbAmin must be finite and at least 0'],
  ])('refuses a negative %s instead of using the default', (field, text) => {
    expect(() => spectrum({ [field]: -5 })).toThrow(new RangeError(`meteringSpectrum: ${text}`));
  });

  it.each(['nFft', 'octaveFraction'])('refuses a non-finite %s', (field) => {
    const text = `meteringSpectrum: ${field} must be an integer in [-2147483648, 2147483647]`;
    expect(() => spectrum({ [field]: Number.NaN })).toThrow(new RangeError(text));
    expect(() => spectrum({ [field]: Number.POSITIVE_INFINITY })).toThrow(new RangeError(text));
  });

  it.each(['dbRef', 'dbAmin'])('refuses a non-finite %s', (field) => {
    // The discriminating pair for the float half: NaN fails both a `< 0` refusal
    // and a `> 0` substitution, so it used to resolve to the library default,
    // and an infinity passed the substitution and was applied as a real level.
    const text = `meteringSpectrum: ${field} must be finite and at least 0`;
    expect(() => spectrum({ [field]: Number.NaN })).toThrow(new RangeError(text));
    expect(() => spectrum({ [field]: Number.POSITIVE_INFINITY })).toThrow(new RangeError(text));
  });

  it.each(['nFft', 'octaveFraction', 'dbRef', 'dbAmin'])(
    'refuses an explicit 0 for %s and takes the default when it is omitted',
    (field) => {
      expect(() => spectrum({ [field]: 0 })).toThrow(RangeError);
      expect(Array.from(spectrum({ [field]: undefined as never }))).toEqual(Array.from(spectrum()));
    },
  );

  it('applies a non-sentinel dbRef and nFft', () => {
    // The controls for the equalities above: both fields really reach the core.
    expect(Array.from(spectrum({ dbRef: 2 }))).not.toEqual(Array.from(spectrum()));
    expect(spectrum({ nFft: 1024 }).length).not.toBe(spectrum().length);
  });
});

describe('setSf2Instrument gain', () => {
  beforeAll(async () => {
    await init();
  });

  const DESTINATION = 7;
  const BLOCK = 128;

  /** One held note through the GM fallback bank (no SoundFont needed). */
  function render(config: Record<string, number> | undefined): Float32Array {
    const engine = new RealtimeEngine({ sampleRate: 48000, maxBlockSize: BLOCK });
    try {
      engine.setSf2Instrument(DESTINATION, config ?? {});
      engine.pushMidiNoteOn(DESTINATION, 0, 0, 60, 100);
      const left = new Float32Array(BLOCK * 8);
      for (let block = 0; block < 8; block++) {
        left.set(
          engine.process([new Float32Array(BLOCK), new Float32Array(BLOCK)])[0],
          block * BLOCK,
        );
      }
      return left;
    } finally {
      engine.destroy();
    }
  }

  it.each([
    ['NaN', Number.NaN],
    ['positive infinity', Number.POSITIVE_INFINITY],
    ['negative infinity', Number.NEGATIVE_INFINITY],
  ])('refuses %s instead of using the player default', (_label, value) => {
    expect(() => render({ gain: value })).toThrow(
      /gain must be finite and in \[-3\.4028235e38, 3\.4028235e38\]$/,
    );
  });

  it('reads 0 as a gain of zero, which renders silence', () => {
    const omitted = render(undefined);
    expect(Math.max(...Array.from(omitted, Math.abs))).toBeGreaterThan(0);
    expect(Math.max(...Array.from(render({ gain: 0 }), Math.abs))).toBe(0);
    // 0.5 is the documented default an omitted gain takes.
    expect(maxAbsDiff(render({ gain: 0.5 }), omitted)).toBe(0);
  });

  it('applies a gain other than the default', () => {
    // The control for the equalities above: the field really reaches the player.
    const omitted = render(undefined);
    expect(maxAbsDiff(render({ gain: 1 }), omitted)).toBeGreaterThan(0.1);
  });

  it.each([-1, -0.0001])('refuses a negative gain of %p', (value) => {
    // The player's constructor replaces a negative gain with its own 0.5, so a
    // value passed through would have been a successful call at a level nobody chose.
    expect(() => render({ gain: value })).toThrow(/gain must be finite and in \[0, 4\]$/);
  });

  it('refuses a gain above the player ceiling', () => {
    expect(() => render({ gain: 4.5 })).toThrow(/gain must be finite and in \[0, 4\]$/);
  });

  it.each([-1, -48])('refuses a negative polyphony of %p', (value) => {
    // polyphony sits one field from gain and its constructor replaces a negative
    // count with the same silence, so it carries the same contract.
    expect(() => render({ polyphony: value })).toThrow(/polyphony must be /);
  });

  it('refuses a polyphony of 0 and takes a positive one', () => {
    // The control: without it the refusals above hold for an entry point that
    // rejects every polyphony.
    expect(() => render({ polyphony: 0 })).toThrow(/polyphony must be an integer in \[1, 64\]$/);
    expect(() => render({ polyphony: 16 })).not.toThrow();
  });
});

describe('setBuiltinInstrument optional scalars', () => {
  beforeAll(async () => {
    await init();
  });

  const DESTINATION = 7;
  const BLOCK = 128;

  /** Four notes at once, so the summed peak reads the voice count too. */
  function chordPeak(config: Record<string, number> | undefined): number {
    const engine = new RealtimeEngine({ sampleRate: 48000, maxBlockSize: BLOCK });
    try {
      engine.setBuiltinInstrument(DESTINATION, config ?? {});
      for (const note of [60, 64, 67, 72]) {
        engine.pushMidiNoteOn(DESTINATION, 0, 0, note, 100);
      }
      let peak = 0;
      for (let block = 0; block < 12; block++) {
        const out = engine.process([new Float32Array(BLOCK), new Float32Array(BLOCK)])[0];
        for (const sample of out) {
          peak = Math.max(peak, Math.abs(sample));
        }
      }
      return peak;
    } finally {
      engine.destroy();
    }
  }

  const FIELDS = ['gain', 'attackMs', 'decayMs', 'sustain', 'releaseMs'] as const;

  it.each(FIELDS)(
    'refuses a zero or negative %s instead of resolving it to the default',
    (field) => {
      // The core reads a non-positive or non-finite field as "use the built-in
      // default" and reports nothing, so a value handed through came back as a
      // successful call at a setting nobody chose.
      expect(() => chordPeak({ [field]: -1 })).toThrow(/must be finite and in \(0, /);
      expect(() => chordPeak({ [field]: 0 })).toThrow(/must be finite and in \(0, /);
    },
  );

  it.each(FIELDS)('refuses a non-finite %s', (field) => {
    expect(() => chordPeak({ [field]: Number.NaN })).toThrow();
    expect(() => chordPeak({ [field]: Number.POSITIVE_INFINITY })).toThrow();
    expect(() => chordPeak({ [field]: Number.NEGATIVE_INFINITY })).toThrow();
  });

  it('refuses a polyphony outside [1, 64], which the core would have replaced', () => {
    for (const bad of [-1, -48, 0, 65]) {
      expect(() => chordPeak({ polyphony: bad })).toThrow(
        /polyphony must be an integer in \[1, 64\]$/,
      );
    }
  });

  it('takes the defaults and a value that changes the render', () => {
    // The controls. Without them every refusal above is satisfied by an entry
    // point that rejects everything, and the equality by one that ignores it.
    const byDefault = chordPeak(undefined);
    expect(byDefault).toBeGreaterThan(0);
    expect(chordPeak({ gain: 0.2 })).toBe(byDefault);
    expect(chordPeak({ gain: 0.8 })).toBeGreaterThan(byDefault);
    // One voice cannot sound four notes. Two against the default's sixteen is
    // not asserted: past the point where every note sounds, more voices move
    // phase rather than energy and the peak stops tracking the count.
    expect(chordPeak({ polyphony: 1 })).toBeLessThan(byDefault);
  });
});
