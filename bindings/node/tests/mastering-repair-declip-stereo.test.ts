import { describe, expect, it } from 'vitest';
import {
  masterAudioStereo,
  masteringRepairDeclip,
  masteringRepairDeclipStereo,
} from '../src/index.js';

const SR = 22050;
const LINKED = { linkChannels: true };

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
  it('repairs each channel on its own by default, leaving every unclipped sample bit-identical', () => {
    const { left, right } = fixture();
    const result = masteringRepairDeclipStereo({ left, right, sampleRate: SR });
    const clipped = (plane: Float32Array) =>
      new Set(Array.from(plane).flatMap((value, i) => (value === 1.0 ? [i] : [])));
    const untouchedOutside = (input: Float32Array, output: Float32Array) => {
      const own = clipped(input);
      return input.every((value, i) => own.has(i) || output[i] === value);
    };
    expect(untouchedOutside(left, result.channels[0])).toBe(true);
    expect(untouchedOutside(right, result.channels[1])).toBe(true);
    expect(result.reports[0].linkedRuns).toBe(0);
    expect(result.reports[1].linkedRuns).toBe(0);
    // Two independent passes: each channel is the mono declip of itself.
    expect(result.channels[0]).toEqual(masteringRepairDeclip(left, SR).samples);
    expect(result.channels[1]).toEqual(masteringRepairDeclip(right, SR).samples);
    expect(
      masteringRepairDeclipStereo({ left, right, sampleRate: SR, linkChannels: false }),
    ).toEqual(result);
  });

  it('refuses a linkChannels that is not a boolean, and on the mono entry point', () => {
    const { left, right } = fixture();
    expect(() =>
      masteringRepairDeclipStereo({ left, right, sampleRate: SR, linkChannels: 1 as never }),
    ).toThrow(TypeError);
    expect(() =>
      masteringRepairDeclip({ samples: left, sampleRate: SR, linkChannels: true } as never),
    ).toThrow(TypeError);
  });

  it('with linkChannels, reconstructs the union of both channels runs and links the narrower side', () => {
    const { left, right } = fixture();
    const result = masteringRepairDeclipStereo({ left, right, sampleRate: SR, ...LINKED });

    expect(result.channels[0]).toHaveLength(left.length);
    expect(result.channels[1]).toHaveLength(right.length);
    expect(result.channels[0].every(Number.isFinite)).toBe(true);
    expect(result.channels[1].every(Number.isFinite)).toBe(true);

    // The fixture only witnesses linking while each channel detects its own
    // clipped samples in its own right. A channel that detected nothing still
    // satisfies loose inequalities and reads exactly like linking working.
    expect(result.reports[0].detected.runCount).toBe(3);
    expect(result.reports[0].detected.clippedSamples).toBe(
      ISOLATED_LEFT_LENGTH + LEFT_NARROW_LENGTH + LEFT_WIDE_LENGTH,
    );
    expect(result.reports[1].detected.runCount).toBe(2);
    expect(result.reports[1].detected.clippedSamples).toBe(RIGHT_WIDE_LENGTH + RIGHT_NARROW_LENGTH);

    // The discriminating property: the narrower side's reconstruction reaches
    // past its own clipped samples because the wider side's run drags the
    // union boundary further out.
    expect(result.reports[0].linkedRuns).toBeGreaterThan(0);
    expect(result.reports[1].linkedRuns).toBeGreaterThan(0);
    // Every union run holding one of a channel's clipped samples is either
    // reconstructed or left clipped: left sits in three (its isolated run and
    // the two shared unions), right in the two shared ones.
    const accounted = (r: (typeof result.reports)[number]) =>
      r.lpcReconstructedRuns + r.unreconstructedRuns;
    expect(accounted(result.reports[0])).toBe(3);
    expect(accounted(result.reports[1])).toBe(2);

    // Not one channel restated.
    expect(Array.from(result.channels[0])).not.toEqual(Array.from(result.channels[1]));
  });

  it('with linkChannels, leaves a channel with no clipped sample in a run untouched there', () => {
    const { left, right } = fixture();
    const original = fixture();
    const result = masteringRepairDeclipStereo({ left, right, sampleRate: SR, ...LINKED });

    // The right channel never clips near the isolated left-only run, so
    // reconstructing unclipped audio to match the left side never happens --
    // the right channel is left exactly as it was there.
    for (let i = 0; i < ISOLATED_LEFT_LENGTH; i += 1) {
      expect(result.channels[1][ISOLATED_LEFT_START + i]).toBe(
        original.right[ISOLATED_LEFT_START + i],
      );
    }
  });

  it('with linkChannels, matches the mono facade on a run only one channel ever clips, diverges where linked', () => {
    const { left, right } = fixture();
    const stereo = masteringRepairDeclipStereo({ left, right, sampleRate: SR, ...LINKED });
    const mono = masteringRepairDeclip(left, SR).samples;

    // The isolated run's union bound equals left's own detected extent
    // exactly (right never clips there), so stereo and mono reconstruct the
    // identical span the identical way.
    for (let i = 0; i < ISOLATED_LEFT_LENGTH; i += 1) {
      expect(stereo.channels[0][ISOLATED_LEFT_START + i]).toBeCloseTo(
        mono[ISOLATED_LEFT_START + i],
        5,
      );
    }

    // At the linked region, the right channel's wider run drags the union
    // bound past what the mono pass -- which never sees the right channel --
    // would ever reconstruct, so the stereo and mono results diverge there.
    const linkedTail = LEFT_NARROW_START + LEFT_NARROW_LENGTH;
    expect(stereo.channels[0][linkedTail]).not.toBeCloseTo(mono[linkedTail], 5);
  });

  it('carries the options bag through to the detector', () => {
    const { left, right } = fixture();

    // Two separate claims, because reaching the validator does not prove the
    // value survives to the detector. clipThresholdDb is range-checked to at most 0,
    // so an out-of-range value proves only that the wrapper passed it along.
    expect(() =>
      masteringRepairDeclipStereo({ left, right, sampleRate: SR, clipThresholdDb: 6 }),
    ).toThrow();

    // A threshold under the tone's own amplitude makes the tone itself read as
    // clipped, so the run count moves. That is the detector consuming it.
    const strict = masteringRepairDeclipStereo({ left, right, sampleRate: SR });
    const loose = masteringRepairDeclipStereo({
      left,
      right,
      sampleRate: SR,
      clipThresholdDb: -8,
    });
    expect(strict.reports[0].detected.runCount).toBeGreaterThan(0);
    expect(loose.reports[0].detected.runCount).toBeGreaterThan(strict.reports[0].detected.runCount);
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

  it('chain repair.declip follows linkChannels the way the stereo entry does', () => {
    const { left, right } = fixture();
    const chain = (declip: Record<string, unknown>) =>
      masterAudioStereo({
        left,
        right,
        sampleRate: SR,
        overrides: { repair: { declip: { enabled: true, ...declip } } },
      });
    const own = masteringRepairDeclipStereo({ left, right, sampleRate: SR });
    const linked = masteringRepairDeclipStereo({ left, right, sampleRate: SR, ...LINKED });
    const ownChain = chain({});
    const linkedChain = chain(LINKED);
    expect(ownChain.left).toEqual(own.channels[0]);
    expect(ownChain.right).toEqual(own.channels[1]);
    expect(linkedChain.left).toEqual(linked.channels[0]);
    expect(linkedChain.right).toEqual(linked.channels[1]);
    expect(linkedChain.left).not.toEqual(ownChain.left);
  });
});
