/**
 * `splitSilenceCommon` reports the UNION of what {@link splitSilence} finds for
 * each signal, merged where intervals touch — what several takes of one part
 * share is their silence, not their sound, so a cut between merged intervals
 * must land in a gap every signal agrees is quiet. This is deliberately the
 * union, never the intersection: a take's own non-silent range survives even
 * where every other signal is silent there.
 */

import { beforeAll, describe, expect, it } from 'vitest';
import { init, splitSilence, splitSilenceCommon } from '../dist/index.js';

describe('splitSilenceCommon', () => {
  const sampleRate = 22050;
  const topDb = 60;
  const frameLength = 2048;
  const hopLength = 512;

  beforeAll(async () => {
    await init();
  });

  /** A one-second buffer with a 440 Hz tone confined to `[startSec, endSec)`, silent elsewhere. */
  function toneWindow(startSec: number, endSec: number): Float32Array {
    const samples = new Float32Array(sampleRate);
    const start = Math.round(startSec * sampleRate);
    const end = Math.round(endSec * sampleRate);
    for (let i = start; i < end; i++) {
      samples[i] = Math.sin((2 * Math.PI * 440 * i) / sampleRate);
    }
    return samples;
  }

  it('matches splitSilence exactly for a single signal', () => {
    const a = toneWindow(0, 0.2);
    const solo = Array.from(splitSilence(a, topDb, frameLength, hopLength));
    const common = Array.from(splitSilenceCommon({ signals: [a], topDb, frameLength, hopLength }));
    expect(common).toEqual(solo);
  });

  it('returns the union of each signal, not their intersection', () => {
    const a = toneWindow(0, 0.2);
    const b = toneWindow(0.5, 0.7);
    const soloA = Array.from(splitSilence(a, topDb, frameLength, hopLength));
    const soloB = Array.from(splitSilence(b, topDb, frameLength, hopLength));
    const combined = Array.from(
      splitSilenceCommon({ signals: [a, b], topDb, frameLength, hopLength }),
    );
    // B's own sounding interval survives even though A is silent throughout it.
    expect(combined).toEqual([...soloA, ...soloB].sort((x, y) => x - y));
  });

  it('is order-independent', () => {
    const a = toneWindow(0, 0.2);
    const b = toneWindow(0.5, 0.7);
    const forward = Array.from(
      splitSilenceCommon({ signals: [a, b], topDb, frameLength, hopLength }),
    );
    const reversed = Array.from(
      splitSilenceCommon({ signals: [b, a], topDb, frameLength, hopLength }),
    );
    expect(reversed).toEqual(forward);
  });

  it('ignores a signal cut before its content starts', () => {
    const a = toneWindow(0, 0.2);
    const b = toneWindow(0.5, 0.7);
    const bCutShort = b.slice(0, Math.round(0.3 * sampleRate));
    const result = Array.from(
      splitSilenceCommon({ signals: [a, bCutShort], topDb, frameLength, hopLength }),
    );
    expect(result).toEqual(Array.from(splitSilence(a, topDb, frameLength, hopLength)));
  });

  it('succeeds with an empty result when every signal is silent', () => {
    const silentA = new Float32Array(sampleRate);
    const silentB = new Float32Array(sampleRate);
    const result = splitSilenceCommon({
      signals: [silentA, silentB],
      topDb,
      frameLength,
      hopLength,
    });
    expect(result).toBeInstanceOf(Int32Array);
    expect(result.length).toBe(0);
  });

  it('rejects an empty signals array', () => {
    expect(() => splitSilenceCommon({ signals: [], topDb, frameLength, hopLength })).toThrow();
  });

  it('rejects an empty signal mixed in with a real one', () => {
    const a = toneWindow(0, 0.2);
    expect(() =>
      splitSilenceCommon({ signals: [a, new Float32Array(0)], topDb, frameLength, hopLength }),
    ).toThrow();
  });

  it('rejects a null/undefined entry by name', () => {
    const a = toneWindow(0, 0.2);
    expect(() =>
      splitSilenceCommon({
        signals: [a, null as unknown as Float32Array],
        topDb,
        frameLength,
        hopLength,
      }),
    ).toThrow(/must be a Float32Array/);
  });

  it('rejects an entry that is not array-like', () => {
    const a = toneWindow(0, 0.2);
    expect(() =>
      splitSilenceCommon({
        signals: [a, {} as unknown as Float32Array],
        topDb,
        frameLength,
        hopLength,
      }),
    ).toThrow();
  });

  it('falls back to the same defaults splitSilence uses', () => {
    const a = toneWindow(0, 0.2);
    const b = toneWindow(0.5, 0.7);
    const omitted = Array.from(splitSilenceCommon({ signals: [a, b] }));
    const spelled = Array.from(
      splitSilenceCommon({ signals: [a, b], topDb: 60.0, frameLength: 2048, hopLength: 512 }),
    );
    expect(omitted).toEqual(spelled);
  });
});
