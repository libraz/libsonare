/**
 * `nonFiniteDiscardCount` on `StreamingMasteringChain`: the companion to
 * `nonFiniteSubstitutionCount` (covered separately in
 * mastering-substitution-count.test.ts, which stays deliberately clean-run
 * only). This file exercises the positive case that one needs a poisoned
 * input to reach.
 */

import { beforeAll, describe, expect, it } from 'vitest';
import { init, StreamingMasteringChain } from '../src/index';

const sampleRate = 48000;
const blockSize = 128;

describe('mastering nonFiniteDiscardCount (WASM)', () => {
  beforeAll(async () => {
    await init();
  });

  it('reports 0 for a clean block and exactly 1 for the block that overflowed the tilt stage', () => {
    // eq.tilt.tiltDb enables the tilt stage (setting any field under a
    // module enables it) and gives it recursive filter cells; the default
    // chain has no stage that keeps state, so its count would sit at zero
    // for a reason unrelated to the method under test.
    const chain = new StreamingMasteringChain({ 'eq.tilt.tiltDb': 24 });
    try {
      chain.prepare(sampleRate, blockSize, 1);

      const clean = new Float32Array(blockSize).fill(0.25);
      chain.processMono(clean);
      expect(chain.nonFiniteDiscardCount()).toBe(0);

      // The chain rejects non-finite input outright, so the poison has to be
      // a finite value a stage overflows internally: 3.0e38 through a 24 dB
      // tilt shelf overflows float32 range inside the filter's own recursion.
      const poisoned = new Float32Array(blockSize).fill(3.0e38);
      chain.processMono(poisoned);
      // Exactly 1: the unit is one processing call, however many cells or
      // stages moved, never a sum across them.
      expect(chain.nonFiniteDiscardCount()).toBe(1);

      // A further clean block adds nothing more: the stage has recovered.
      chain.processMono(clean);
      expect(chain.nonFiniteDiscardCount()).toBe(1);
    } finally {
      chain.delete();
    }
  });
});
