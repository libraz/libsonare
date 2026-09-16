import { describe, expect, it } from 'vitest';
import { masteringRepairDeclip, masteringRepairDeclipStereo } from '../src/index.js';

const SR = 22050;

// An isolated run clipped in the left channel only: the union run here equals
// left's own detected extent exactly (right never clips nearby), so
// declipping it stereo must reconstruct it run-for-run the same as mono --
// the fixture's control against "declip one channel and return it twice".
const ISOLATED_LEFT_START = 3000;
const ISOLATED_LEFT_LENGTH = 10;

// Left clips a NARROW run inside a region the right channel clips WIDER, so
// left's reconstruction is dragged past its own clipped samples into the
// span only right detected -- this is what makes leftReport.linkedRuns
// nonzero. A clipped plateau in only one channel produces no linking at all,
// so the fixture needs both channels clipped in the same region with
// different extents, not a click in only one channel.
const LEFT_NARROW_START = 8000;
const LEFT_NARROW_LENGTH = 10;
const RIGHT_WIDE_START = 8000;
const RIGHT_WIDE_LENGTH = 20;

// The mirror image, so rightReport.linkedRuns is exercised too.
const RIGHT_NARROW_START = 15000;
const RIGHT_NARROW_LENGTH = 10;
const LEFT_WIDE_START = 15000;
const LEFT_WIDE_LENGTH = 20;

function sine(freq: number, seconds: number, amp: number): Float32Array {
  const n = Math.floor(SR * seconds);
  const out = new Float32Array(n);
  for (let i = 0; i < n; i += 1) {
    out[i] = amp * Math.sin((2 * Math.PI * freq * i) / SR);
  }
  return out;
}

function clip(buffer: Float32Array, start: number, length: number): void {
  for (let i = 0; i < length; i += 1) {
    buffer[start + i] = 1.0;
  }
}

// Different tones in the two channels so "declip one channel and return it
// twice" fails on content alone, plus amplitude 0.5 -- well under the default
// 0.98 clip threshold -- so nothing clips by accident.
function fixture(): { left: Float32Array; right: Float32Array } {
  const left = sine(440, 1.0, 0.5);
  const right = sine(880, 1.0, 0.5);
  clip(left, ISOLATED_LEFT_START, ISOLATED_LEFT_LENGTH);
  clip(left, LEFT_NARROW_START, LEFT_NARROW_LENGTH);
  clip(right, RIGHT_WIDE_START, RIGHT_WIDE_LENGTH);
  clip(right, RIGHT_NARROW_START, RIGHT_NARROW_LENGTH);
  clip(left, LEFT_WIDE_START, LEFT_WIDE_LENGTH);
  return { left, right };
}

describe('masteringRepairDeclipStereo', () => {
  it('reconstructs the union of both channels runs and links the narrower side', () => {
    const { left, right } = fixture();
    const result = masteringRepairDeclipStereo({ left, right, sampleRate: SR });

    expect(result.left).toHaveLength(left.length);
    expect(result.right).toHaveLength(right.length);
    expect(result.left.every(Number.isFinite)).toBe(true);
    expect(result.right.every(Number.isFinite)).toBe(true);

    // The fixture only witnesses linking while each channel detects its own
    // clipped samples in its own right. A channel that detected nothing still
    // satisfies loose inequalities and reads exactly like linking working.
    expect(result.leftReport.detected.runCount).toBe(3);
    expect(result.leftReport.detected.sampleCount).toBe(
      ISOLATED_LEFT_LENGTH + LEFT_NARROW_LENGTH + LEFT_WIDE_LENGTH,
    );
    expect(result.rightReport.detected.runCount).toBe(2);
    expect(result.rightReport.detected.sampleCount).toBe(RIGHT_WIDE_LENGTH + RIGHT_NARROW_LENGTH);

    // The discriminating property: the narrower side's reconstruction reaches
    // past its own clipped samples because the wider side's run drags the
    // union boundary further out.
    expect(result.leftReport.linkedRuns).toBeGreaterThan(0);
    expect(result.rightReport.linkedRuns).toBeGreaterThan(0);
    expect(result.leftReport.repairedSamples).toBeGreaterThan(
      result.leftReport.detected.sampleCount,
    );
    expect(result.rightReport.repairedSamples).toBeGreaterThan(
      result.rightReport.detected.sampleCount,
    );

    // Not one channel restated.
    expect(Array.from(result.left)).not.toEqual(Array.from(result.right));
  });

  it('leaves a channel with no clipped sample in a run untouched there', () => {
    const { left, right } = fixture();
    const original = fixture();
    const result = masteringRepairDeclipStereo({ left, right, sampleRate: SR });

    // The right channel never clips near the isolated left-only run, so
    // reconstructing unclipped audio to match the left side never happens --
    // the right channel is left exactly as it was there.
    for (let i = 0; i < ISOLATED_LEFT_LENGTH; i += 1) {
      expect(result.right[ISOLATED_LEFT_START + i]).toBe(original.right[ISOLATED_LEFT_START + i]);
    }
  });

  it('matches the mono facade on a run only one channel ever clips, diverges where linked', () => {
    const { left, right } = fixture();
    const stereo = masteringRepairDeclipStereo({ left, right, sampleRate: SR });
    const mono = masteringRepairDeclip(left, SR);

    // The isolated run's union bound equals left's own detected extent
    // exactly (right never clips there), so stereo and mono reconstruct the
    // identical span the identical way.
    for (let i = 0; i < ISOLATED_LEFT_LENGTH; i += 1) {
      expect(stereo.left[ISOLATED_LEFT_START + i]).toBeCloseTo(mono[ISOLATED_LEFT_START + i], 5);
    }

    // At the linked region, the right channel's wider run drags the union
    // bound past what the mono pass -- which never sees the right channel --
    // would ever reconstruct, so the stereo and mono results diverge there.
    const linkedTail = LEFT_NARROW_START + LEFT_NARROW_LENGTH;
    expect(stereo.left[linkedTail]).not.toBeCloseTo(mono[linkedTail], 5);
  });

  it('carries the options bag through to the detector', () => {
    const { left, right } = fixture();

    // Two separate claims, because reaching the validator does not prove the
    // value survives to the detector. clipThreshold is range-checked in (0, 1],
    // so an out-of-range value proves only that the wrapper passed it along.
    expect(() =>
      masteringRepairDeclipStereo({ left, right, sampleRate: SR, clipThreshold: 1.5 }),
    ).toThrow();

    // A threshold under the tone's own amplitude makes the tone itself read as
    // clipped, so the run count moves. That is the detector consuming it.
    const strict = masteringRepairDeclipStereo({ left, right, sampleRate: SR });
    const loose = masteringRepairDeclipStereo({
      left,
      right,
      sampleRate: SR,
      clipThreshold: 0.4,
    });
    expect(strict.leftReport.detected.runCount).toBeGreaterThan(0);
    expect(loose.leftReport.detected.runCount).toBeGreaterThan(strict.leftReport.detected.runCount);
  });

  it('refuses bad arguments with a catchable error', () => {
    const { left, right } = fixture();
    expect(() =>
      masteringRepairDeclipStereo({ left, right: right.slice(0, 10), sampleRate: SR }),
    ).toThrow();
    expect(() =>
      masteringRepairDeclipStereo({
        left: undefined as unknown as Float32Array,
        right,
        sampleRate: SR,
      }),
    ).toThrow();
    expect(() =>
      masteringRepairDeclipStereo({
        left,
        right: [1, 2, 3] as unknown as Float32Array,
        sampleRate: SR,
      }),
    ).toThrow();
    expect(() => masteringRepairDeclipStereo({ left, right, sampleRate: Number.NaN })).toThrow();
  });
});
