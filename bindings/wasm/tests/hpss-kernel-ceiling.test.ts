/**
 * Kernel-size boundary tests for the HPSS entry points of the WASM binding.
 *
 * The two median filters each guard their kernel twice with the same error
 * code: once for odd-and-positive, which carries that code's generic message,
 * and once for the ceiling, which names the filter and the ceiling's value.
 * Separating the two matters most here: WASM's heap is bounded, so before the
 * ceiling guard an oversized kernel surfaced as an allocation failure rather
 * than as a refusal, and a bare "it throws" would accept that.
 *
 * The three entry points do not narrow alike. `hpss` and `hpssWithResidual`
 * hand the kernel to embind, which truncates a number past the signed range
 * into one the parity guard rejects; `extractPercussiveEvents` range-checks its
 * options object first and names the option. Each is asserted for what it
 * actually produces rather than flattened into a single expectation.
 */

import { beforeAll, describe, expect, it } from 'vitest';
import {
  ErrorCode,
  extractPercussiveEvents,
  hpss,
  hpssWithResidual,
  init,
  isSonareError,
  type SonareError,
} from '../dist/index.js';

const sampleRate = 22050;

/** A plain sine, enough frames for the transform to have something to do. */
const tone = new Float32Array(4096).map((_, i) => Math.sin((2 * Math.PI * 440 * i) / sampleRate));

/** A click train over a quiet tone bed, so the detector returns events. */
const hits = (() => {
  const n = sampleRate;
  const out = new Float32Array(n).map((_, i) => 0.02 * Math.sin((2 * Math.PI * 220 * i) / n));
  // A fixed linear congruential sequence rather than Math.random, so a failure
  // reproduces from the file alone.
  let state = 7;
  const next = (): number => {
    state = (state * 1103515245 + 12345) % 2147483648;
    return state / 1073741824 - 1;
  };
  for (let start = 1000; start < n - 1000; start += 2205) {
    for (let j = 0; j < 200; j++) {
      out[start + j] += 0.9 * Math.exp(-j / 25) * next();
    }
  }
  return out;
})();

// The ceiling the refusal message names. Written out rather than derived from a
// shared constant, so a ceiling quietly lowered moves the message and fails here
// instead of following the change. It is a power of two and therefore even, so
// it is not itself a legal kernel: the largest a caller can pass is the odd
// value one below it, and the first value the ceiling guard sees is the odd
// value one above.
const CEILING = 524288;
const LARGEST_LEGAL = 524287;
const FIRST_ABOVE_CEILING = 524289;

const INT_MAX = 2147483647;
/** Even, so it is refused for parity rather than for the ceiling. */
const INT_MAX_MINUS_ONE = 2147483646;

type Direction = 'harmonic' | 'percussive';

const directions: Direction[] = ['harmonic', 'percussive'];

const filterFor = (direction: Direction): string =>
  direction === 'harmonic' ? 'median_filter_horizontal' : 'median_filter_vertical';

/** Kernel arguments with `direction` set to `value` and the other left at 31. */
const kernels = (direction: Direction, value: number) => ({
  kernelHarmonic: direction === 'harmonic' ? value : 31,
  kernelPercussive: direction === 'percussive' ? value : 31,
});

const capture = (run: () => unknown): unknown => {
  try {
    run();
    return undefined;
  } catch (error) {
    return error;
  }
};

/** Every refusal below is a parameter refusal, never an allocation failure. */
function expectParameterRefusal(caught: unknown): SonareError {
  expect(isSonareError(caught)).toBe(true);
  const error = caught as SonareError;
  expect(error.name).toBe('SonareError');
  expect(error.code).toBe(ErrorCode.InvalidParameter);
  expect(error.codeName).toBe('InvalidParameter');
  // Stated separately from the equality above because these two are the
  // outcomes the ceiling guard exists to make unreachable on a bounded heap,
  // and a regression that reintroduced either would be a different defect from
  // a wrong code.
  expect(error.code).not.toBe(ErrorCode.OutOfMemory);
  expect(error.code).not.toBe(ErrorCode.Unknown);
  return error;
}

/** The refusal names the filter, the value passed, and the ceiling. */
function expectCeilingRefusal(caught: unknown, direction: Direction, kernel: number): void {
  const error = expectParameterRefusal(caught);
  expect(error.message).toContain(
    `${filterFor(direction)}: kernel_size ${kernel} exceeds the maximum ${CEILING}`,
  );
}

const entries = [
  { name: 'hpss', run: (options: object) => hpss({ samples: tone, sampleRate, ...options }) },
  {
    name: 'hpssWithResidual',
    run: (options: object) => hpssWithResidual({ samples: tone, sampleRate, ...options }),
  },
];

beforeAll(async () => {
  await init();
});

describe('HPSS kernel ceiling', () => {
  for (const entry of entries) {
    for (const direction of directions) {
      for (const kernel of [INT_MAX, FIRST_ABOVE_CEILING]) {
        // Each filter carries its own copy of the guard, so one direction says
        // nothing about the other, and each entry point reaches both.
        it(`${entry.name} refuses ${direction} kernel ${kernel} by naming the ceiling`, () => {
          expectCeilingRefusal(
            capture(() => entry.run(kernels(direction, kernel))),
            direction,
            kernel,
          );
        });
      }

      for (const kernel of [INT_MAX_MINUS_ONE, CEILING]) {
        // INT_MAX - 1 and the ceiling's own value are both even, so both die on
        // odd-and-positive before magnitude is ever considered. The message must
        // not carry the ceiling, or a test could pass by tripping the wrong guard.
        it(`${entry.name} refuses even ${direction} kernel ${kernel} for parity`, () => {
          const error = expectParameterRefusal(
            capture(() => entry.run(kernels(direction, kernel))),
          );
          expect(error.message).not.toContain('exceeds the maximum');
        });
      }

      it(`${entry.name} truncates a ${direction} kernel past the signed range into a parity refusal`, () => {
        // Neither entry point range-checks its kernel arguments, so a value past
        // the signed range is narrowed on the way in and reaches the parity guard
        // rather than the ceiling guard. Asserted rather than avoided, because
        // the narrowing is what a caller actually gets.
        for (const kernel of [INT_MAX + 1, 2 ** 32]) {
          const error = expectParameterRefusal(
            capture(() => entry.run(kernels(direction, kernel))),
          );
          expect(error.message).not.toContain('exceeds the maximum');
        }
      });
    }
  }

  for (const direction of directions) {
    // Without this, a guard that refused every kernel would satisfy the cases above.
    it(`accepts the largest legal ${direction} kernel`, () => {
      const result = hpss({ samples: tone, sampleRate, ...kernels(direction, LARGEST_LEGAL) });
      expect(result.harmonic.length).toBe(tone.length);
      expect(result.percussive.length).toBe(tone.length);
      expect(result.harmonic.every(Number.isFinite)).toBe(true);
      expect(result.percussive.every(Number.isFinite)).toBe(true);
    });
  }

  it('separates on an ordinary kernel', () => {
    const result = hpss({ samples: tone, sampleRate });
    expect(result.harmonic.length).toBe(tone.length);
    expect(result.harmonic.some((value) => value !== 0)).toBe(true);

    const residual = hpssWithResidual({ samples: tone, sampleRate });
    expect(residual.harmonic.length).toBe(tone.length);
    expect(residual.residual.length).toBe(tone.length);
  });
});

/**
 * The percussive-event facade reads its separation fields from an options
 * object. Unlike the two entry points above it range-checks and sign-checks
 * them by name before the call, so only the values that survive that reach the
 * core's own guards.
 */
describe('percussive-event separation kernel', () => {
  for (const direction of directions) {
    const key = direction === 'harmonic' ? 'hpssKernelHarmonic' : 'hpssKernelPercussive';

    for (const kernel of [INT_MAX, FIRST_ABOVE_CEILING]) {
      it(`refuses ${direction} kernel ${kernel} by naming the ceiling`, () => {
        expectCeilingRefusal(
          capture(() => extractPercussiveEvents({ samples: hits, sampleRate, [key]: kernel })),
          direction,
          kernel,
        );
      });
    }

    it(`refuses an even ${direction} kernel for parity`, () => {
      const error = expectParameterRefusal(
        capture(() =>
          extractPercussiveEvents({ samples: hits, sampleRate, [key]: INT_MAX_MINUS_ONE }),
        ),
      );
      expect(error.message).not.toContain('exceeds the maximum');
    });

    it(`refuses a ${direction} kernel past the signed range by name`, () => {
      const error = expectParameterRefusal(
        capture(() => extractPercussiveEvents({ samples: hits, sampleRate, [key]: INT_MAX + 1 })),
      );
      expect(error.message).toContain(`${key} must be a finite number within the 32-bit integer`);
    });

    it(`refuses a negative ${direction} kernel by name`, () => {
      const error = expectParameterRefusal(
        capture(() => extractPercussiveEvents({ samples: hits, sampleRate, [key]: -1 })),
      );
      expect(error.message).toContain('must not be negative');
    });

    // Every field of the versioned separation config takes its default at 0, so
    // an explicit 0 is indistinguishable from an omitted option — it is not the
    // odd-and-positive refusal the same value draws from hpss. Compared against
    // the default run rather than just "it did not throw", so a 0 that quietly
    // selected something else would still fail.
    it(`treats a zero ${direction} kernel as the default rather than refusing it`, () => {
      const fallback = extractPercussiveEvents({ samples: hits, sampleRate });
      const zeroed = extractPercussiveEvents({ samples: hits, sampleRate, [key]: 0 });
      expect(fallback.length).toBeGreaterThan(0);
      expect(zeroed.map((event) => [event.onsetSample, event.offsetSample])).toEqual(
        fallback.map((event) => [event.onsetSample, event.offsetSample]),
      );
    });

    it(`accepts the largest legal ${direction} kernel`, () => {
      const events = extractPercussiveEvents({
        samples: hits,
        sampleRate,
        [key]: LARGEST_LEGAL,
      });
      expect(events.length).toBeGreaterThan(0);
    });
  }
});
