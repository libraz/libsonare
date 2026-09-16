/**
 * The two halves of one contract: which scalar conversions refuse a saturated
 * argument and which are documented to carry it through.
 *
 * `timeToFrames` is the refusing half. Its core conversion saturates on
 * purpose — `time_to_frames` returns `INT_MAX` for a non-finite result rather
 * than throwing, and that is a deliberate, documented property of the core's
 * non-throwing scalar family. What was wrong was letting a *binding* hand that
 * number back: a caller's `1e39` is finite in JS, survives every JS-side check,
 * becomes an infinity in embind's f32 demotion, and returned `2147483647` — a
 * frame index nothing downstream can separate from a real one. So the refusal
 * belongs at the binding boundary, and the assertions below are on the VALUE
 * not coming back, not merely on an exception arriving: a test that only
 * checked "it threw" would pass against a version that threw for some other
 * reason entirely.
 *
 * The five scale conversions are the carrying half, and they are here for the
 * opposite reason. They are documented as total on every surface, and a NaN is
 * how an unvoiced frame of a pitch track is spelled, so mapping a whole
 * `pitchPyin` track through one is the intended use. Routing them through the
 * checked reader would reject the convention's own value. These cases exist so
 * that a later sweep applying "narrow every float through checkedFloatFromVal"
 * uniformly fails here instead of silently removing a contract.
 */

import { beforeAll, describe, expect, it } from 'vitest';
import {
  ErrorCode,
  hzToMel,
  hzToMidi,
  hzToNote,
  init,
  isSonareError,
  melToHz,
  midiToHz,
  type SonareError,
  timeToFrames,
} from '../dist/index.js';

const SR = 22050;
const HOP = 512;

/** What `time_to_frames` hands back for a non-finite argument, by design. */
const SATURATED_FRAME_INDEX = 2147483647;

/** The suffix every float-range refusal ends with, whatever the key is. */
const RANGE_MESSAGE = 'must be a finite number within the 32-bit float range';

/**
 * Finite numbers a caller could choose that embind's f32 demotion turns into an
 * infinity. `3.5e38` is the one that matters most: it is barely past `FLT_MAX`
 * rather than deliberately extreme, so nothing about it looks wrong on the way
 * in. `Number.isFinite` returns true for every entry here, which is why a
 * facade-level finiteness check cannot stand in for the boundary one.
 */
const SATURATES_ONTO_A_FLOAT = [1e39, -1e39, 3.5e38, 1e300];

/** Non-finite values written directly, which must reach the same refusal. */
const NON_FINITE = [Number.NaN, Number.POSITIVE_INFINITY, Number.NEGATIVE_INFINITY];

interface Outcome {
  returned: unknown;
  error: unknown;
}

function run(call: () => unknown): Outcome {
  try {
    return { returned: call(), error: undefined };
  } catch (error) {
    return { returned: undefined, error };
  }
}

/** Asserts the caught value is the float-range refusal naming `key`. */
function expectRangeRefusal(caught: unknown, key: string, context: string): void {
  expect(isSonareError(caught), `${context}: expected a SonareError`).toBe(true);
  const error = caught as SonareError;
  expect(error.code, context).toBe(ErrorCode.InvalidParameter);
  expect(error.message, context).toBe(`${key} ${RANGE_MESSAGE}`);
}

beforeAll(async () => {
  await init();
});

describe('timeToFrames refuses a saturated time', () => {
  it('never returns a frame index for a value too wide for a float', () => {
    for (const value of SATURATES_ONTO_A_FLOAT) {
      const context = `timeToFrames(${value})`;
      const outcome = run(() => timeToFrames(value, SR, HOP));

      // The defect, asserted directly: the saturating frame index must not come
      // back. Checked before the error shape, because this is the observable a
      // caller would have acted on.
      expect(outcome.returned, `${context}: returned a frame index`).toBeUndefined();
      expect(outcome.returned, context).not.toBe(SATURATED_FRAME_INDEX);
      expectRangeRefusal(outcome.error, 'time', context);
    }
  });

  it('refuses a value that was already non-finite', () => {
    for (const value of NON_FINITE) {
      const context = `timeToFrames(${value})`;
      const outcome = run(() => timeToFrames(value, SR, HOP));
      expect(outcome.returned, context).toBeUndefined();
      expectRangeRefusal(outcome.error, 'time', context);
    }
  });

  it('still converts two legitimate times to two different frame indices', () => {
    // The control. Every refusal above is also satisfied by an entry point that
    // ignores its argument, so acceptance is asserted by reading values back
    // and requiring them to differ.
    expect(timeToFrames(1.0, SR, HOP)).toBe(43);
    expect(timeToFrames(2.0, SR, HOP)).toBe(86);
    expect(timeToFrames(0, SR, HOP)).toBe(0);
  });
});

describe('the scale conversions stay total', () => {
  it('carries a saturated argument through rather than refusing it', () => {
    // Documented on every surface as total over the whole real line. A refusal
    // here would be a contract change, not a hardening.
    expect(hzToMel(1e39)).toBe(Number.POSITIVE_INFINITY);
    expect(melToHz(1e39)).toBe(Number.POSITIVE_INFINITY);
    expect(hzToMidi(1e39)).toBe(Number.POSITIVE_INFINITY);
    expect(midiToHz(1e39)).toBe(Number.POSITIVE_INFINITY);
    expect(hzToNote(1e39)).toBe('?');
  });

  it('propagates the NaN a pitch track spells an unvoiced frame with', () => {
    // The reason the passthrough exists. A default pitchPyin track fills
    // unvoiced frames with NaN, and mapping one through these is the intended
    // use, so NaN has to survive rather than throw.
    expect(hzToMidi(Number.NaN)).toBeNaN();
    expect(hzToMel(Number.NaN)).toBeNaN();
    expect(hzToNote(Number.NaN)).toBe('?');
  });

  it('still converts two legitimate frequencies to two different results', () => {
    expect(hzToMidi(440)).toBeCloseTo(69, 5);
    expect(hzToMidi(880)).toBeCloseTo(81, 5);
    expect(hzToNote(440)).toBe('A4');
    expect(midiToHz(69)).toBeCloseTo(440, 3);
    expect(hzToMel(1000)).not.toBe(hzToMel(2000));
  });
});
