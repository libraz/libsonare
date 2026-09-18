import { describe, expect, it } from 'vitest';
import { Mixer } from '../src/index.js';

const SAMPLE_RATE = 48000;
// One block is 10.67 ms at 48 kHz, well past the strip's ~5 ms smoothing
// window, so an unsettled ramp lives entirely inside the first block.
const BLOCK_SIZE = 512;

// 6144 samples, comfortably past the 1583 the smoothers take to land.
const CONVERGENCE_BLOCKS = 12;

// An asymmetric input keeps the width smoother observable in its own right,
// rather than only through the pan stage ahead of it.
const INPUT_LEFT = 0.5;
const INPUT_RIGHT = -0.25;

// One strip straight to the master, with a non-default fader, pan and width so
// every smoother settle() touches has somewhere to glide from.
const SETTLE_SCENE = JSON.stringify({
  version: 1,
  buses: [{ id: 'master', role: 'master', inserts: [] }],
  connections: [{ source: 's', destination: 'master' }],
  strips: [
    {
      id: 's',
      inputTrimDb: 0,
      faderDb: -3,
      pan: 0.3,
      panMode: 0,
      panLaw: 0,
      width: 1.2,
      channelDelaySamples: 0,
      dualPanLeft: -1,
      dualPanRight: 1,
      inserts: [],
      muted: false,
      polarityInvertLeft: false,
      polarityInvertRight: false,
      sends: [],
      soloSafe: false,
      soloed: false,
      vcaOffsetDb: 0,
    },
  ],
  vcaGroups: [],
});

function buildMixer(): Mixer {
  const mixer = Mixer.fromSceneJson(SETTLE_SCENE, SAMPLE_RATE, BLOCK_SIZE);
  mixer.compile();
  return mixer;
}

function constantBlock(): { left: Float32Array[]; right: Float32Array[] } {
  return {
    left: [new Float32Array(BLOCK_SIZE).fill(INPUT_LEFT)],
    right: [new Float32Array(BLOCK_SIZE).fill(INPUT_RIGHT)],
  };
}

/**
 * The level the strip holds once its smoothers have run out, measured rather
 * than assumed. The approach is far slower than the ~5 ms time constant
 * suggests: the output first comes within 0.1 % of this value at sample 1583
 * (33 ms), so a single block would land short and read as a mismatch.
 */
function convergedLevel(): { left: number; right: number } {
  const mixer = buildMixer();
  const { left, right } = constantBlock();
  let block = mixer.processStereo(left, right);
  for (let index = 1; index < CONVERGENCE_BLOCKS; index += 1) {
    block = mixer.processStereo(left, right);
  }
  const level = { left: block.left[BLOCK_SIZE - 1], right: block.right[BLOCK_SIZE - 1] };
  mixer.destroy();
  return level;
}

describe('Mixer.settle', () => {
  it('measures a ramp across the head of an unsettled first block', () => {
    const converged = convergedLevel();
    const mixer = buildMixer();
    const { left, right } = constantBlock();
    const block = mixer.processStereo(left, right);

    // Positive control for the settled case below: without settle() the strip
    // opens at unity trim/fader/pan/width and glides to the scene's values, so
    // the head of the block sits far from where the block ends up. Measured
    // 0.4987 against a converged 0.2563 (+5.8 dB) on the left, 0.2500 against
    // 0.2164 on the right.
    const headLeft = Math.abs(block.left[0] - converged.left);
    const headRight = Math.abs(block.right[0] - converged.right);
    expect(headLeft).toBeGreaterThan(0.1);
    expect(headRight).toBeGreaterThan(0.01);

    // The offset is a ramp rather than a different destination: it has largely
    // decayed by the end of the same block (measured 0.0233 from 0.2424).
    const tailLeft = Math.abs(block.left[BLOCK_SIZE - 1] - converged.left);
    expect(tailLeft).toBeLessThan(headLeft / 5);
  });

  it('opens the first block at the converged level once settled', () => {
    const converged = convergedLevel();
    const mixer = buildMixer();
    mixer.settle('s');
    const { left, right } = constantBlock();
    const block = mixer.processStereo(left, right);

    // The very first sample is already the converged value; a settle() that did
    // nothing would land 0.24 away on the left rather than within 1e-4.
    expect(Math.abs(block.left[0] - converged.left)).toBeLessThan(1e-4);
    expect(Math.abs(block.right[0] - converged.right)).toBeLessThan(1e-4);

    // No ramp anywhere in the block, not merely a correct first sample.
    for (let index = 0; index < BLOCK_SIZE; index += 1) {
      expect(Math.abs(block.left[index] - block.left[0])).toBeLessThan(1e-7);
      expect(Math.abs(block.right[index] - block.right[0])).toBeLessThan(1e-7);
    }
  });

  it('accepts a strip index as well as an id and rejects an unknown strip', () => {
    const mixer = buildMixer();
    expect(() => mixer.settle(0)).not.toThrow();
    expect(() => mixer.settle('does-not-exist')).toThrow(/mixer strip not found/);
    expect(() => mixer.settle(7)).toThrow(/out of range/);
  });
});
