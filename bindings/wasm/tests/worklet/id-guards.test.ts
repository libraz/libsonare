/**
 * Ids the engine accepts are the ids the caller named.
 *
 * Every id here reaches a native setter that refuses an out-of-domain value on
 * its own. That refusal only fires if the value survives the JS layer intact,
 * so a helper that folds one onto a neighbouring legal id defeats the check
 * rather than duplicating it -- and zero is a legal destination, which is what
 * made the substitution invisible.
 *
 * Each case opens with a control: two legitimate ids that resolve differently,
 * so a refusal cannot pass by rejecting everything.
 */

import { describe, expect, it } from 'vitest';
import { normalizeTrackLanes, resolveMarkerSet } from '../../src/worklet/engine-offline';
import { resolveTargetId } from '../../src/worklet/engine-sync';

describe('resolveTargetId reads both spellings through one rule', () => {
  it('resolves a number and the string naming it to the same id', () => {
    expect(resolveTargetId(5)).toBe(5);
    expect(resolveTargetId('5')).toBe(5);
    expect(resolveTargetId('7')).toBe(7);
    expect(resolveTargetId(0)).toBe(0);
  });

  it('refuses a string that names no whole id, rather than reading a prefix', () => {
    // parseInt stops at the first non-digit, so these resolved to 3 and 5.
    expect(() => resolveTargetId('3.5')).toThrow(RangeError);
    expect(() => resolveTargetId('5abc')).toThrow(/must be an integer/);
  });

  it('refuses an unparseable name instead of resolving it to destination zero', () => {
    // The substitution this guard exists for: zero is a real destination, so a
    // typo'd track name routed to it and nothing downstream could object.
    for (const name of ['garbage', '', '   ']) {
      expect(() => resolveTargetId(name)).toThrow(/must be an integer/);
    }
  });

  it('refuses a fractional number by the same rule as a fractional string', () => {
    expect(() => resolveTargetId(2.5)).toThrow(RangeError);
    expect(() => resolveTargetId('2.5')).toThrow(RangeError);
  });
});

describe('mixer lane ids are whole and positive', () => {
  it('keeps two distinct lanes distinct', () => {
    expect(normalizeTrackLanes([], [1, 2]).ids).toEqual([1, 2]);
  });

  it('refuses a fractional or non-positive lane id', () => {
    expect(() => normalizeTrackLanes([], [1, 2.5])).toThrow(/Invalid track id for mixer lane/);
    expect(() => normalizeTrackLanes([], [1, 0])).toThrow(RangeError);
    expect(() => normalizeTrackLanes([], [1, -3])).toThrow(RangeError);
  });
});

describe('marker ppq and id carry the domain the native setter enforces', () => {
  it('resolves two markers at distinct positions', () => {
    expect(resolveMarkerSet([{ ppq: 0 }, { ppq: 480 }], 1).resolved).toHaveLength(2);
  });

  it('refuses a negative ppq here rather than one layer down', () => {
    // Finiteness alone let this through to be refused natively, under the
    // native layer's own name for the argument.
    expect(() => resolveMarkerSet([{ ppq: -1 }], 1)).toThrow(/Invalid marker ppq/);
    expect(() => resolveMarkerSet([{ ppq: Number.NaN }], 1)).toThrow(/Invalid marker ppq/);
  });

  it('refuses a fractional or non-positive explicit id', () => {
    expect(() => resolveMarkerSet([{ ppq: 480, id: 2.5 }], 1)).toThrow(/Invalid marker id/);
    expect(() => resolveMarkerSet([{ ppq: 480, id: 0 }], 1)).toThrow(RangeError);
  });
});
