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
 * The three entry points do not refuse an out-of-range kernel in the same
 * place. `extractPercussiveEvents` reads its options object through the
 * module's own range-checked reader, which names the option; `hpss` and
 * `hpssWithResidual` hand the kernel to a positional embind parameter declared
 * `int`, which WRAPS, so the facade range-checks those before the call. Each is
 * asserted for what it actually produces rather than flattened.
 *
 * `INT_MAX` is exactly representable as a JS number and arrives intact, so a
 * test that drove only `INT_MAX` would report the guard working while the values
 * one step above it were the live defect: `2 ** 32` wrapped to 0 and
 * `2 ** 32 + 1` to 1, both of which every downstream guard accepts, so the call
 * SUCCEEDED on a kernel the caller never asked for. A refusal-shaped assertion
 * cannot see that, so every wrapping case below also compares its outcome
 * against two results built explicitly here: the default-kernel run, and the run
 * with the wrapped value as a legal kernel.
 */

import { beforeAll, describe, expect, it } from 'vitest';
import {
  ErrorCode,
  extractPercussiveEvents,
  hpss,
  hpssWithResidual,
  init,
  isSonareError,
  renderPercussiveEvents,
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

/**
 * Values past the signed 32-bit range, each with what the narrowing turns it
 * into.
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

/**
 * Values the module's reader would accept and TRUNCATE, each with the kernel it
 * would have separated on.
 *
 * These are the inputs the range check cannot see: every one of them is finite
 * and inside the 32-bit range, so the reader passes it and the cast rounds it
 * toward zero. Two of the three land on a kernel that runs -- 31 explicitly,
 * and 0 through the config's zero-is-default rule -- so before the integrality
 * check the call SUCCEEDED on a kernel the caller never asked for.
 */
const FRACTIONAL: ReadonlyArray<{ passed: number; truncatesTo: number }> = [
  { passed: 31.5, truncatesTo: 31 },
  { passed: 32.5, truncatesTo: 32 },
  { passed: -0.5, truncatesTo: 0 },
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

      for (const { passed, wrapsTo } of WRAPPING) {
        it(`${entry.name} refuses a ${direction} kernel of ${passed} by naming the argument`, () => {
          const error = expectParameterRefusal(
            capture(() => entry.run(kernels(direction, passed))),
          );
          // The facade's own refusal, ahead of the narrowing, so it names the
          // argument rather than the median filter the value never reached.
          expect(error.message).toContain(
            `${entry.name}: kernel${direction === 'harmonic' ? 'Harmonic' : 'Percussive'} must be an integer within the signed 32-bit range`,
          );
          expect(error.message).not.toContain('exceeds the maximum');
        });

        it(`${entry.name} never separates a ${direction} kernel of ${passed} as ${wrapsTo}`, () => {
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
 *
 * Range and sign belong to the module's reader; integrality does not, because
 * that reader narrows with a cast. The facade checks it, so the two refusals
 * come from different layers and carry different messages — asserted apart
 * below rather than flattened into "it throws".
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
        const error = expectParameterRefusal(
          capture(() => extractPercussiveEvents({ samples: hits, sampleRate, [key]: passed })),
        );
        expect(error.message).toContain(`${key} must be a finite number within the 32-bit integer`);
        expect(error.message).not.toContain('exceeds the maximum');
      });

      it(`never separates a ${direction} kernel of ${passed} as ${wrapsTo}`, () => {
        // The worst outcome this guards is not a wrong refusal but a silent
        // success: 0 is this config's spelling of "use the default", so a kernel
        // that wrapped to it returned the default run's events unchanged.
        const outcome = eventOutcome({ [key]: passed });
        expect(outcome).toBe(REFUSED);
        expect(outcome).not.toBe(eventOutcome({}));
        if (wrapsTo >= 0 && (wrapsTo === 0 || wrapsTo % 2 === 1)) {
          expect(outcome).not.toBe(eventOutcome({ [key]: wrapsTo }));
        }
      });
    }

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

    for (const { passed, truncatesTo } of FRACTIONAL) {
      it(`refuses a fractional ${direction} kernel of ${passed} for not being an integer`, () => {
        const error = expectParameterRefusal(
          capture(() => extractPercussiveEvents({ samples: hits, sampleRate, [key]: passed })),
        );
        expect(error.message).toContain(`${key} must be an integer`);
        // Refused above the narrowing, so it never reaches the reader's range
        // check nor the core's parity and ceiling guards. Without this, 32.5
        // would pass by tripping the parity guard on the 32 it truncated to,
        // which says nothing about the integrality check.
        expect(error.message).not.toContain('within the 32-bit integer range');
        expect(error.message).not.toContain(filterFor(direction));
      });

      it(`never separates a fractional ${direction} kernel of ${passed} as ${truncatesTo}`, () => {
        // The assertion the refusal cannot make. 31.5 and -0.5 both truncate
        // onto a kernel that SUCCEEDS -- 31 explicitly, and 0 through the
        // zero-is-default rule -- so "it threw" and "it quietly ran the default"
        // are the same shape from outside. Compare the outcomes instead.
        const fallback = eventOutcome({});
        expect(fallback).not.toBe(REFUSED);
        const outcome = eventOutcome({ [key]: passed });
        expect(outcome).toBe(REFUSED);
        expect(outcome).not.toBe(fallback);
        const truncated = eventOutcome({ [key]: truncatesTo });
        // 32 is even, so the core refuses the truncation of 32.5 for parity and
        // there is no successful run to compare against; the message assertion
        // above carries that case on its own.
        if (truncated !== REFUSED) {
          expect(outcome).not.toBe(truncated);
        }
      });
    }

    it(`refuses a NaN ${direction} kernel for not being an integer`, () => {
      const error = expectParameterRefusal(
        capture(() => extractPercussiveEvents({ samples: hits, sampleRate, [key]: Number.NaN })),
      );
      expect(error.message).toContain(`${key} must be an integer`);
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

/**
 * The render half of the percussive-event pair takes the same separation
 * options, so it carries the same integrality gap. Its own kernel sensitivity
 * needs a non-identity edit: a set whose edits are all identity reproduces the
 * input bit for bit and runs no separation, which would make a default-run
 * comparison agree for any kernel at all.
 */
describe('percussive-event render separation kernel', () => {
  for (const direction of directions) {
    const key = direction === 'harmonic' ? 'hpssKernelHarmonic' : 'hpssKernelPercussive';

    const rendered = (kernel: Record<string, number>): string => {
      const events = extractPercussiveEvents({ samples: hits, sampleRate });
      const edited = [
        { ...events[0], edit: { ...events[0].edit, muted: true } },
        ...events.slice(1),
      ];
      const out = renderPercussiveEvents({ samples: hits, sampleRate, events: edited, ...kernel });
      let digest = 0;
      for (let i = 0; i < out.length; i++) {
        digest = (digest * 31 + out[i]) % 1e9;
      }
      return `${out.length}:${digest.toFixed(6)}`;
    };

    for (const { passed, truncatesTo } of FRACTIONAL) {
      it(`refuses a fractional ${direction} kernel of ${passed}`, () => {
        const error = expectParameterRefusal(capture(() => rendered({ [key]: passed })));
        expect(error.message).toContain(`renderPercussiveEvents: ${key} must be an integer`);
      });

      it(`never renders a fractional ${direction} kernel of ${passed} as ${truncatesTo}`, () => {
        const fallback = rendered({});
        let outcome: string;
        try {
          outcome = rendered({ [key]: passed });
        } catch {
          outcome = REFUSED;
        }
        expect(outcome).toBe(REFUSED);
        expect(outcome).not.toBe(fallback);
      });
    }

    // Without this the refusals above are satisfied by a guard that rejects
    // every kernel, and the muted edit is what makes the kernel reach the
    // output at all.
    it(`renders an integral ${direction} kernel and the default differently`, () => {
      expect(rendered({ [key]: 3 })).not.toBe(rendered({}));
    });
  }
});

/**
 * The framing fields sit in the same options bag as the kernels, are read by
 * the same narrowing reader, and share the "0 keeps the default" sentinel. A
 * guard covering only the kernels leaves the two of them with the defect the
 * kernels were fixed out of, on the same call.
 */
describe('percussive-event separation framing', () => {
  const spans = (events: ReturnType<typeof extractPercussiveEvents>): string =>
    JSON.stringify(events.map((event) => [event.onsetSample, event.offsetSample]));

  const outcome = (framing: Record<string, number>): string => {
    try {
      return spans(extractPercussiveEvents({ samples: hits, sampleRate, ...framing }));
    } catch {
      return REFUSED;
    }
  };

  for (const key of ['nFft', 'hopLength'] as const) {
    // The sentinel this whole guard exists for. Asserted first because every
    // claim below is about a value truncating onto it.
    it(`reads a ${key} of 0 as the default rather than refusing it`, () => {
      expect(outcome({ [key]: 0 })).toBe(outcome({}));
      expect(outcome({})).not.toBe(REFUSED);
    });

    for (const passed of [0.5, -0.5, 1024.5]) {
      it(`refuses a fractional ${key} of ${passed} for not being an integer`, () => {
        const error = expectParameterRefusal(
          capture(() => extractPercussiveEvents({ samples: hits, sampleRate, [key]: passed })),
        );
        expect(error.message).toContain(`${key} must be an integer`);
        // -0.5 truncates to 0, so the module's own negativity check cannot see
        // it; a refusal quoting that check would mean the integrality guard
        // never ran.
        expect(error.message).not.toContain('must not be negative');
      });
    }

    it(`refuses a fractional ${key} on the render half too`, () => {
      const events = extractPercussiveEvents({ samples: hits, sampleRate });
      const error = expectParameterRefusal(
        capture(() => renderPercussiveEvents({ samples: hits, sampleRate, events, [key]: 0.5 })),
      );
      expect(error.message).toContain(`${key} must be an integer`);
    });
  }

  // Without this the refusals above are satisfied by a guard that rejects every
  // framing, and nothing shows the field reaches the separation at all.
  it('separates on a framing the caller chose', () => {
    const chosen = outcome({ nFft: 1024, hopLength: 256 });
    expect(chosen).not.toBe(REFUSED);
    expect(chosen).not.toBe(outcome({}));
  });
});
