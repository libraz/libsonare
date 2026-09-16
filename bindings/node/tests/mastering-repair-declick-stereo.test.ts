import { describe, expect, it } from 'vitest';
import { masteringRepairDeclick, masteringRepairDeclickStereo } from '../src/index.js';

const SR = 22050;
const LEFT_CLICKS = [1000, 3000, 5000];
const RIGHT_ONLY_CLICK = 2000;

function sine(freq: number, seconds: number, amp: number): Float32Array {
  const n = Math.floor(SR * seconds);
  const out = new Float32Array(n);
  for (let i = 0; i < n; i += 1) {
    out[i] = amp * Math.sin((2 * Math.PI * freq * i) / SR);
  }
  return out;
}

// The two channels carry different tones and their clicks sit at disjoint
// positions, so an implementation that declicks one channel and returns it
// twice fails on content alone.
//
// amp 0.2, not 0.3: a run counts as a click only while it stands
// neighborRatio (4.0) above its neighbours, so a 1.0 spike clears the ratio at
// 0.2 everywhere and misses it near a 0.3 tone's own peak -- which would make
// detection depend on the phase the click happened to land on.
function fixture(): { left: Float32Array; right: Float32Array } {
  const left = sine(440, 0.3, 0.2);
  const right = sine(880, 0.3, 0.2);
  for (const pos of LEFT_CLICKS) {
    left[pos] = 1.0;
  }
  right[RIGHT_ONLY_CLICK] = 1.0;
  return { left, right };
}

describe('masteringRepairDeclickStereo', () => {
  it('repairs the union of both channels runs', () => {
    const { left, right } = fixture();
    const result = masteringRepairDeclickStereo({ left, right, sampleRate: SR });

    expect(result.left).toHaveLength(left.length);
    expect(result.right).toHaveLength(right.length);
    expect(result.left.every(Number.isFinite)).toBe(true);
    expect(result.right.every(Number.isFinite)).toBe(true);

    // The fixture only witnesses linking while each channel detects its own
    // clicks. A channel that detected nothing still satisfies "repaired more
    // than it detected" and reads exactly like linking working.
    expect(result.leftReport.detected.count).toBe(LEFT_CLICKS.length);
    expect(result.rightReport.detected.count).toBe(1);

    // The discriminating property: a run only one channel selected is repaired
    // in both, so each side borrows what the other detected.
    expect(result.leftReport.linkedRuns).toBeGreaterThan(0);
    expect(result.rightReport.linkedRuns).toBeGreaterThan(0);
    expect(result.leftReport.repairedRuns).toBeGreaterThan(result.leftReport.detected.count);
    expect(result.rightReport.repairedRuns).toBeGreaterThan(result.rightReport.detected.count);

    // Not one channel restated.
    expect(Array.from(result.left)).not.toEqual(Array.from(result.right));
  });

  it('differs from the mono entry point at the other channels click', () => {
    const { left, right } = fixture();
    const stereo = masteringRepairDeclickStereo({ left, right, sampleRate: SR });
    const mono = masteringRepairDeclick(left, SR);

    // The mono pass never sees the right channel's click, so it leaves the left
    // channel alone there. Without this the stereo entry could be the mono one
    // called twice.
    expect(stereo.left[RIGHT_ONLY_CLICK]).not.toBe(mono[RIGHT_ONLY_CLICK]);
    expect(stereo.left[LEFT_CLICKS[0]]).toBeCloseTo(mono[LEFT_CLICKS[0]], 5);
  });

  it('carries the options bag through to the detector', () => {
    const { left, right } = fixture();
    const loose = masteringRepairDeclickStereo({ left, right, sampleRate: SR, neighborRatio: 2.0 });
    const tight = masteringRepairDeclickStereo({
      left,
      right,
      sampleRate: SR,
      neighborRatio: 50.0,
    });

    // A ratio no run can clear leaves nothing to repair, so the option is
    // reaching the detector rather than being dropped on the way.
    expect(tight.leftReport.detected.count).toBe(0);
    expect(tight.leftReport.repairedRuns).toBe(0);
    expect(loose.leftReport.detected.count).toBeGreaterThan(0);
  });

  it('refuses bad arguments with a catchable error', () => {
    const { left, right } = fixture();
    expect(() =>
      masteringRepairDeclickStereo({ left, right: right.slice(0, 10), sampleRate: SR }),
    ).toThrow();
    expect(() =>
      masteringRepairDeclickStereo({
        left: undefined as unknown as Float32Array,
        right,
        sampleRate: SR,
      }),
    ).toThrow();
    expect(() => masteringRepairDeclickStereo({ left, right, sampleRate: Number.NaN })).toThrow();
    expect(() =>
      masteringRepairDeclickStereo({ left, right, sampleRate: SR, maxClickSamples: 0 }),
    ).toThrow();
  });
});
