/**
 * Kernel-size boundary tests for the HPSS entry points of the Node addon.
 *
 * The core refuses both kernels once, at the separation's entry, as an odd
 * integer inside a ceiling, and the refusal names the kernel as the caller
 * spelled it. An oversized kernel used to reach the
 * per-worker allocations, where the refusal depended on whether the host
 * overcommitted, so `OutOfMemory` / `Unknown` are exactly what must never come
 * back and a bare "it throws" would accept them.
 *
 * The addon reads every kernel through N-API's `Int32Value`, which is
 * ECMAScript ToInt32 and therefore wraps rather than saturates. `INT_MAX` is
 * exactly representable as a JS number and arrives intact, so a test that drove
 * only `INT_MAX` would report the guard working while the values one step above
 * it were the live defect: `2 ** 32` wrapped to 0 and `2 ** 32 + 1` to 1, both
 * of which every downstream guard accepts, so the call SUCCEEDED on a kernel the
 * caller never asked for. A refusal-shaped assertion cannot see that, so every
 * wrapping case below also compares its outcome against two results built
 * explicitly here: the default-kernel run, and the run with the wrapped value as
 * a legal kernel.
 */

import { describe, expect, it } from 'vitest';
import { extractPercussiveEvents, hpss, isSonareError } from '../src/index.js';

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

/**
 * Values past the signed 32-bit range, each with what ToInt32 turns it into.
 *
 * The wrapped value is what makes these the interesting inputs rather than
 * `INT_MAX`: two of them land on an ordinary legal kernel and two on 0, which
 * the percussive-event config reads as "use the default". Every one of them
 * therefore has a plausible successful outcome waiting for it if the range check
 * is ever dropped.
 */
const WRAPPING: ReadonlyArray<{ passed: number; wrapsTo: number }> = [
  { passed: 2 ** 32, wrapsTo: 0 },
  { passed: 2 ** 32 + 1, wrapsTo: 1 },
  { passed: 2 ** 32 + 3, wrapsTo: 3 },
  { passed: 3 * 2 ** 32, wrapsTo: 0 },
  { passed: 2 ** 31, wrapsTo: -2147483648 },
];

/** A refusal reduced to the same domain as a result, so the two can be compared. */
const REFUSED = 'refused';

/**
 * One comparable string per outcome. A thrown call collapses to {@link REFUSED};
 * anything else is reduced over its float fields, so two runs that separated on
 * different kernels cannot compare equal.
 */
function outcomeSignature(run: () => unknown): string {
  let value: unknown;
  try {
    value = run();
  } catch {
    return REFUSED;
  }
  const parts: string[] = [];
  for (const [key, field] of Object.entries(value as Record<string, unknown>)) {
    if (field instanceof Float32Array) {
      let digest = 0;
      for (let i = 0; i < field.length; i++) {
        digest = (digest * 31 + field[i]) % 1e9;
      }
      parts.push(`${key}:${field.length}:${digest.toFixed(6)}`);
    }
  }
  return parts.join('|');
}

type Direction = 'harmonic' | 'percussive';

const directions: Direction[] = ['harmonic', 'percussive'];

const hpssKey = (direction: Direction): string =>
  direction === 'harmonic' ? 'kernelHarmonic' : 'kernelPercussive';

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
function expectParameterRefusal(caught: unknown): Error {
  expect(caught).toBeInstanceOf(RangeError);
  // These two are the outcomes the ceiling guard exists to make unreachable, and
  // a regression that reintroduced either would be a different defect from a
  // wrong class.
  expect(isSonareError(caught)).toBe(false);
  return caught as Error;
}

/** The refusal names the entry, the kernel as the caller spelled it, and the ceiling. */
function expectKernelRefusal(caught: unknown, entry: string, key: string): void {
  const error = expectParameterRefusal(caught);
  expect(error.message).toBe(`${entry}: ${key} must be an odd integer in [1, ${CEILING}]`);
}

const entries = [
  {
    name: 'hpss',
    label: 'hpss',
    run: (options: object) => hpss({ samples: tone, sampleRate, ...options }),
  },
  {
    name: 'hpss',
    label: 'hpss with residual',
    run: (options: object) => hpss({ samples: tone, sampleRate, withResidual: true, ...options }),
  },
];

describe('HPSS kernel ceiling', () => {
  for (const entry of entries) {
    for (const direction of directions) {
      for (const kernel of [INT_MAX, FIRST_ABOVE_CEILING]) {
        // Each filter carries its own copy of the guard, so one direction says
        // nothing about the other, and each entry point reaches both.
        it(`${entry.label} refuses ${direction} kernel ${kernel} by naming the ceiling`, () => {
          expectKernelRefusal(
            capture(() => entry.run(kernels(direction, kernel))),
            'hpss',
            hpssKey(direction),
          );
        });
      }

      for (const kernel of [INT_MAX_MINUS_ONE, CEILING]) {
        // INT_MAX - 1 and the ceiling's own value are both even, and one guard
        // refuses parity and magnitude together, so they read as the ceiling does.
        it(`${entry.label} refuses even ${direction} kernel ${kernel} for parity`, () => {
          expectKernelRefusal(
            capture(() => entry.run(kernels(direction, kernel))),
            'hpss',
            hpssKey(direction),
          );
        });
      }

      for (const { passed, wrapsTo } of WRAPPING) {
        it(`${entry.label} refuses a ${direction} kernel of ${passed} by naming the argument`, () => {
          const error = expectParameterRefusal(
            capture(() => entry.run(kernels(direction, passed))),
          );
          // The binding's own refusal, ahead of the narrowing, so it names the
          // argument rather than the median filter the value never reached.
          expect(error.message).toContain(
            `${entry.name}: kernel${direction === 'harmonic' ? 'Harmonic' : 'Percussive'} must be an integer in [-2147483648, 2147483647]`,
          );
          expect(error.message).not.toContain('must be an odd integer');
        });

        it(`${entry.label} never separates a ${direction} kernel of ${passed} as ${wrapsTo}`, () => {
          // The assertion the refusal above cannot make: a dropped range check
          // does not throw, it returns one of these two results.
          const outcome = outcomeSignature(() => entry.run(kernels(direction, passed)));
          expect(outcome).toBe(REFUSED);
          expect(outcome).not.toBe(outcomeSignature(() => entry.run({})));
          // 0 and the most negative int are not legal kernels, so there is no
          // wrapped run to build for them; the refusal is the whole statement.
          if (wrapsTo > 0 && wrapsTo % 2 === 1) {
            expect(outcome).not.toBe(
              outcomeSignature(() => entry.run(kernels(direction, wrapsTo))),
            );
          }
        });
      }
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

    const residual = hpss({ samples: tone, sampleRate, withResidual: true });
    expect(residual.harmonic.length).toBe(tone.length);
    expect(residual.residual?.length).toBe(tone.length);
  });
});

/**
 * The percussive-event facade reads its separation fields from a versioned
 * options object. Every field of that object defaults at 0, so a kernel that
 * wrapped to 0 would select the default and report success — the facade
 * range-checks the four fields by name before the call, and everything that
 * survives that reaches the core's own guards.
 */
describe('percussive-event separation kernel', () => {
  const spans = (events: ReturnType<typeof extractPercussiveEvents>): string =>
    JSON.stringify(events.map((event) => [event.onsetSample, event.offsetSample]));

  /** The event-list counterpart of {@link outcomeSignature}. */
  const eventOutcome = (kernel: Record<string, number>): string => {
    try {
      return spans(extractPercussiveEvents({ samples: hits, sampleRate, ...kernel }));
    } catch {
      return REFUSED;
    }
  };

  for (const direction of directions) {
    const key = direction === 'harmonic' ? 'hpssKernelHarmonic' : 'hpssKernelPercussive';

    for (const { passed, wrapsTo } of WRAPPING) {
      it(`refuses a ${direction} kernel of ${passed} by naming the option`, () => {
        // An options field past the 32-bit range is an argument refusal, as the
        // WASM options reader raises it.
        const caught = capture(() =>
          extractPercussiveEvents({ samples: hits, sampleRate, [key]: passed }),
        );
        expect(caught).toBeInstanceOf(RangeError);
        const error = caught as RangeError;
        expect(error.message).toContain(
          `extractPercussiveEvents: ${key} must be an integer in [-2147483648, 2147483647]`,
        );
        expect(error.message).not.toContain('must be an odd integer');
      });

      it(`never separates a ${direction} kernel of ${passed} as ${wrapsTo}`, () => {
        // The worst outcome this guards is not a wrong refusal but a silent
        // success: 0 is this config's spelling of "use the default", so a kernel
        // that wrapped to it returned the default run's events unchanged.
        const outcome = eventOutcome({ [key]: passed });
        expect(outcome).toBe(REFUSED);
        expect(outcome).not.toBe(eventOutcome({}));
        if (wrapsTo > 0 && wrapsTo % 2 === 1) {
          expect(outcome).not.toBe(eventOutcome({ [key]: wrapsTo }));
        }
      });
    }

    for (const kernel of [INT_MAX, FIRST_ABOVE_CEILING]) {
      it(`refuses ${direction} kernel ${kernel} by naming the ceiling`, () => {
        expectKernelRefusal(
          capture(() => extractPercussiveEvents({ samples: hits, sampleRate, [key]: kernel })),
          'extractPercussiveEvents',
          key,
        );
      });
    }

    it(`refuses an even ${direction} kernel for parity`, () => {
      expectKernelRefusal(
        capture(() =>
          extractPercussiveEvents({ samples: hits, sampleRate, [key]: INT_MAX_MINUS_ONE }),
        ),
        'extractPercussiveEvents',
        key,
      );
    });

    it(`refuses a negative ${direction} kernel`, () => {
      expectParameterRefusal(
        capture(() => extractPercussiveEvents({ samples: hits, sampleRate, [key]: -1 })),
      );
    });

    // The native config reads 0 as "use the default", so an explicit 0 would
    // separate on the default while reporting success; it is refused by name.
    it(`refuses an explicit zero ${direction} kernel by name`, () => {
      const caught = capture(() =>
        extractPercussiveEvents({ samples: hits, sampleRate, [key]: 0 }),
      );
      expect(caught).toBeInstanceOf(RangeError);
      expect((caught as RangeError).message).toBe(
        `extractPercussiveEvents: ${key} must not be 0; omit it for the library value`,
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
