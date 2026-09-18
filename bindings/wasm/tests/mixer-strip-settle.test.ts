/**
 * Behavioural coverage for Mixer.settle in WASM.
 *
 * A strip's input trim, fader, pan and width are smoothed for a live fader, so
 * a render that starts from a freshly built strip opens on a ramp rather than
 * on the level and image the scene asked for. These cases measure both halves:
 * the ramp is there without settle, and settle removes it.
 */

import { beforeAll, describe, expect, it } from 'vitest';
import { init, Mixer } from '../dist/index.js';

const SR = 48000;
const BLOCK = 512;

// Constant per-channel input, asymmetric so the width and pan smoothers move
// the stereo image as well as the level. With a DC input and no inserts the
// strip's output is constant once its smoothers have converged, which is what
// makes "the value the render settles on" directly readable from the samples.
const IN_LEFT = 0.5;
const IN_RIGHT = -0.25;

// Blocks rendered before reading the converged reference. The smoothers reach
// their float32 fixed point within five blocks at this block size; eight is
// that with headroom.
const WARMUP_BLOCKS = 8;

function oneStripScene(): string {
  return JSON.stringify({
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
}

function dcBlock(value: number): Float32Array {
  return new Float32Array(BLOCK).fill(value);
}

/** Renders one block of the steady input through `mixer`. */
function renderBlock(mixer: Mixer): { left: Float32Array; right: Float32Array } {
  return mixer.processStereo([dcBlock(IN_LEFT)], [dcBlock(IN_RIGHT)]);
}

/**
 * Steady-state output of the scene, measured independently of settle: a fresh
 * mixer is left to ride its own ramp out over several blocks and the last
 * sample of the last block is read. Using this rather than the settled render
 * itself is what keeps the settled assertions from comparing settle to itself.
 */
function convergedOutput(): { left: number; right: number } {
  const mixer = Mixer.fromSceneJson(oneStripScene(), SR, BLOCK);
  try {
    mixer.compile();
    let last = renderBlock(mixer);
    for (let block = 1; block < WARMUP_BLOCKS; block++) {
      last = renderBlock(mixer);
    }
    return { left: last.left[BLOCK - 1], right: last.right[BLOCK - 1] };
  } finally {
    mixer.delete();
  }
}

function spread(channel: Float32Array): number {
  let min = channel[0];
  let max = channel[0];
  for (let i = 1; i < channel.length; i++) {
    min = Math.min(min, channel[i]);
    max = Math.max(max, channel[i]);
  }
  return max - min;
}

describe('Mixer.settle (WASM)', () => {
  beforeAll(async () => {
    await init();
  });

  it('leaves a head ramp on the first block when the strip is not settled', () => {
    const converged = convergedOutput();
    const mixer = Mixer.fromSceneJson(oneStripScene(), SR, BLOCK);
    try {
      mixer.compile();
      const out = renderBlock(mixer);

      // The level opens near the raw input (the fader smoother starts at unity)
      // and glides down to the scene's -3 dB: measured 0.4987 against a
      // converged 0.2563, a head 5.8 dB hot.
      expect(out.left[0] - converged.left).toBeGreaterThan(0.2);
      expect(out.left[0] / converged.left).toBeGreaterThan(1.9);

      // The image sweeps with it: the right channel opens 0.0336 off its
      // converged value while the pan and width smoothers travel.
      expect(Math.abs(out.right[0] - converged.right)).toBeGreaterThan(0.03);

      // The ramp is not confined to the first sample; it is still moving at the
      // end of the block, so the whole 512-sample head carries it.
      expect(spread(out.left)).toBeGreaterThan(0.2);
      expect(out.left[BLOCK - 1]).toBeGreaterThan(converged.left);
    } finally {
      mixer.delete();
    }
  });

  it('opens the first sample at the converged level and image once settled', () => {
    const converged = convergedOutput();
    const mixer = Mixer.fromSceneJson(oneStripScene(), SR, BLOCK);
    try {
      mixer.compile();
      mixer.settle(0);
      const out = renderBlock(mixer);

      // Measured residual against the independently converged reference:
      // 5.0e-6 on the left, 1.8e-7 on the right -- four orders of magnitude
      // under the 0.24 ramp the unsettled case shows, so a settle that did
      // nothing fails these by the full ramp.
      expect(Math.abs(out.left[0] - converged.left)).toBeLessThan(1e-4);
      expect(Math.abs(out.right[0] - converged.right)).toBeLessThan(1e-4);

      // Nothing glides afterwards either: with a DC input the settled block is
      // flat end to end.
      expect(spread(out.left)).toBeLessThan(1e-6);
      expect(spread(out.right)).toBeLessThan(1e-6);
    } finally {
      mixer.delete();
    }
  });

  it('changes no scene state', () => {
    const mixer = Mixer.fromSceneJson(oneStripScene(), SR, BLOCK);
    try {
      mixer.compile();
      const before = mixer.toSceneJson();
      mixer.settle(0);
      expect(mixer.toSceneJson()).toBe(before);
    } finally {
      mixer.delete();
    }
  });

  it('rejects an out-of-range strip index', () => {
    const mixer = Mixer.fromSceneJson(oneStripScene(), SR, BLOCK);
    try {
      expect(() => mixer.settle(mixer.stripCount())).toThrow();
    } finally {
      mixer.delete();
    }
  });
});
