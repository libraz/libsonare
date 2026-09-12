/**
 * The shared options-bag reader refuses a fractional value instead of truncating it.
 *
 * `checkedIntFromVal` is the one path every options-bag integer field on this
 * surface takes, so integrality belongs there rather than per facade: a facade
 * check covers the fields someone noticed, and it is invisible to a caller
 * driving the embind classes directly. What the reader cannot do is name the
 * function — its message is `<field> must be an integer` with no prefix — which
 * is why the facade checks that exist stay as the better diagnostic rather than
 * being deleted as duplicates.
 *
 * The fields below were chosen for having NO facade integrality check, so a
 * refusal here can only have come from the reader. Each case carries the
 * positive control the refusal cannot supply on its own: the truncation of the
 * same value must still succeed. Without it a reader that refused every numeric
 * option would pass every refusal assertion in this file.
 *
 * `-0.5` is the case worth reading twice. It truncates to 0, and 0 is what every
 * one of these fields reads as "keep the default", so before the check it did
 * not fail — it silently ran the default framing while the caller believed it
 * had set one. A refusal-shaped assertion cannot tell that apart from a value
 * that was simply rejected, so the negative cases also assert the message is not
 * the non-negativity guard's, which is what a truncated -0.5 would have reached.
 */

import { beforeAll, describe, expect, it } from 'vitest';
import {
  detectOnsets,
  ErrorCode,
  extractPercussiveEvents,
  init,
  isSonareError,
  meteringSpectrum,
  meteringSpectrumFrame,
  pcen,
  type SonareError,
} from '../dist/index.js';

const sampleRate = 22050;

/** One second of a fixed tone; long enough for a 2048-point transform to run. */
const tone = new Float32Array(sampleRate).map((_, i) =>
  Math.sin((2 * Math.PI * 440 * i) / sampleRate),
);

/** A small non-negative matrix for pcen, which reads its framing from options. */
const PCEN_BINS = 8;
const PCEN_FRAMES = 16;
const matrix = new Float32Array(PCEN_BINS * PCEN_FRAMES).map((_, i) => 0.1 + (i % 5) * 0.05);

const capture = (run: () => unknown): unknown => {
  try {
    run();
    return undefined;
  } catch (error) {
    return error;
  }
};

function expectParameterRefusal(caught: unknown): SonareError {
  expect(isSonareError(caught)).toBe(true);
  const error = caught as SonareError;
  expect(error.name).toBe('SonareError');
  expect(error.code).toBe(ErrorCode.InvalidParameter);
  expect(error.codeName).toBe('InvalidParameter');
  return error;
}

/** One options-bag field, with the call that reaches it. */
interface FieldCase {
  /** The entry point's exported name, for the test title only. */
  readonly entry: string;
  /** The option key the reader names in its message. */
  readonly field: string;
  /** Extra options the field needs to be reached on a live path. */
  readonly context?: Record<string, number | boolean>;
  /** Runs the entry point with `options` merged over `context`. */
  readonly run: (options: Record<string, number | boolean>) => unknown;
}

const CASES: readonly FieldCase[] = [
  {
    entry: 'meteringSpectrum',
    field: 'nFft',
    run: (options) => meteringSpectrum(tone, sampleRate, options),
  },
  {
    entry: 'meteringSpectrum',
    field: 'octaveFraction',
    // The smoothing flag is what makes the fraction reach the computation; the
    // reader runs on presence either way, so this only keeps the control honest.
    context: { applyOctaveSmoothing: true },
    run: (options) => meteringSpectrum(tone, sampleRate, options),
  },
  {
    entry: 'meteringSpectrumFrame',
    field: 'nFft',
    run: (options) => meteringSpectrumFrame(tone, sampleRate, 0, options),
  },
  {
    entry: 'detectOnsets',
    field: 'nFft',
    run: (options) => detectOnsets(tone, sampleRate, options),
  },
  {
    entry: 'detectOnsets',
    field: 'hopLength',
    run: (options) => detectOnsets(tone, sampleRate, options),
  },
  {
    entry: 'detectOnsets',
    field: 'preMax',
    run: (options) => detectOnsets(tone, sampleRate, options),
  },
  {
    entry: 'detectOnsets',
    field: 'postMax',
    run: (options) => detectOnsets(tone, sampleRate, options),
  },
  {
    entry: 'detectOnsets',
    field: 'preAvg',
    run: (options) => detectOnsets(tone, sampleRate, options),
  },
  {
    entry: 'detectOnsets',
    field: 'postAvg',
    run: (options) => detectOnsets(tone, sampleRate, options),
  },
  {
    entry: 'detectOnsets',
    field: 'wait',
    run: (options) => detectOnsets(tone, sampleRate, options),
  },
  {
    entry: 'pcen',
    field: 'sampleRate',
    run: (options) => pcen(matrix, PCEN_BINS, PCEN_FRAMES, options as Record<string, number>),
  },
  {
    entry: 'pcen',
    field: 'hopLength',
    run: (options) => pcen(matrix, PCEN_BINS, PCEN_FRAMES, options as Record<string, number>),
  },
];

/** The legal value each field's fractional probe truncates onto. */
const FLOOR: Readonly<Record<string, number>> = {
  nFft: 1024,
  octaveFraction: 3,
  hopLength: 512,
  preMax: 30,
  postMax: 30,
  preAvg: 100,
  postAvg: 100,
  wait: 30,
  sampleRate: 22050,
};

beforeAll(async () => {
  await init();
});

describe('the shared options-bag reader refuses a fractional value', () => {
  for (const testCase of CASES) {
    const { entry, field, context = {}, run } = testCase;
    const floor = FLOOR[field] as number;
    const fractional = floor + 0.5;

    it(`${entry}: refuses ${field} = ${fractional} for not being an integer`, () => {
      const error = expectParameterRefusal(capture(() => run({ ...context, [field]: fractional })));
      expect(error.message).toContain(`${field} must be an integer`);
      // The range check is the reader's OTHER rejection and it fires on a
      // different input class. Without this, a range check that had started
      // refusing everything would satisfy the assertion above.
      expect(error.message).not.toContain('within the 32-bit integer range');
      // No facade prefixes this message, which is what says the refusal came
      // from the reader rather than from a JS-side assertion above it. If a
      // facade check is added for this field later, this line is the one that
      // reports the test has stopped measuring what it claims to.
      expect(error.message).not.toContain(`${entry}: ${field} must be an integer`);
    });

    it(`${entry}: still accepts ${field} = ${floor}, the value ${fractional} truncated onto`, () => {
      // The control. A reader that refused every numeric option would pass every
      // refusal assertion in this file and fail only here.
      expect(capture(() => run({ ...context, [field]: floor }))).toBeUndefined();
    });
  }
});

describe('the facade and the reader refuse the same inputs', () => {
  // The claim the docblocks make for keeping both checks is that they differ
  // only in how the message reads, never in WHICH inputs they refuse. That was
  // asserted in prose and not measured, which is the weaker half: a check that
  // converts a silent wrong value into a refusal reads as safe, so nobody
  // re-drives the axis afterwards. `nFft` is the field to drive it on — the
  // same option name reaches a facade-checked entry point and a reader-only one.
  //
  // The two layers reject for different REASONS at the ends of the range: the
  // facade's integrality test passes 2**53 (it really is an integer) and the
  // reader then refuses it for range. That is agreement about the input and
  // disagreement about the responsible layer, which is what this asserts —
  // equality of the refusal SETS, not of the messages.
  const PROBES: ReadonlyArray<{ value: number; refused: boolean; why: string }> = [
    { value: 1024, refused: false, why: 'an ordinary legal size' },
    { value: 0, refused: false, why: 'the documented keep-the-default sentinel' },
    { value: 1024.5, refused: true, why: 'fractional, in range' },
    { value: -0.5, refused: true, why: 'fractional, truncates onto the default' },
    { value: Number.NaN, refused: true, why: 'not finite' },
    { value: Number.POSITIVE_INFINITY, refused: true, why: 'not finite' },
    { value: 2 ** 32, refused: true, why: 'integral but past the 32-bit range' },
    { value: 2 ** 53, refused: true, why: 'integral, and the largest exact JS integer step' },
  ];

  const hits = new Float32Array(sampleRate).map((_, i) =>
    i % 2205 < 200 ? Math.sin((2 * Math.PI * 900 * i) / sampleRate) : 0,
  );

  for (const { value, refused, why } of PROBES) {
    it(`${refused ? 'refuses' : 'accepts'} nFft = ${value} on both layers (${why})`, () => {
      const facadeChecked = capture(() =>
        extractPercussiveEvents({ samples: hits, sampleRate, nFft: value }),
      );
      const readerOnly = capture(() => meteringSpectrum(tone, sampleRate, { nFft: value }));
      expect(facadeChecked === undefined).toBe(!refused);
      expect(readerOnly === undefined).toBe(!refused);
      if (refused) {
        expectParameterRefusal(facadeChecked);
        expectParameterRefusal(readerOnly);
      }
    });
  }
});

describe('a negative fraction is refused before it becomes the default', () => {
  // Restricted to the fields whose zero means "keep the default", which is what
  // made -0.5 succeed silently rather than fail visibly.
  const ZERO_IS_DEFAULT: readonly FieldCase[] = CASES.filter(
    (c) => c.field === 'nFft' || c.field === 'octaveFraction' || c.field === 'hopLength',
  );

  for (const { entry, field, context = {}, run } of ZERO_IS_DEFAULT) {
    it(`${entry}: refuses ${field} = -0.5 rather than reading it as 0`, () => {
      const error = expectParameterRefusal(capture(() => run({ ...context, [field]: -0.5 })));
      expect(error.message).toContain(`${field} must be an integer`);
      // A truncated -0.5 is 0, which is non-negative, so the non-negativity
      // guard never fires on it. Seeing its message here would mean the
      // truncation happened and something further down caught the result.
      expect(error.message).not.toContain('must be non-negative');
    });
  }
});
