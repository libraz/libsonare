import { describe, expect, it } from 'vitest';
import {
  decomposeNotePitch,
  extractNotes,
  mergeNotes,
  phaseVocoder,
  pitchShift,
  renderNotes,
  splitNote,
  timeStretch,
} from '../src/index.js';

/**
 * The facade-level finite check on a required scalar, and the addon narrowing
 * behind it.
 *
 * Each entry point is first shown to consume its scalar: two legitimate values
 * select results that differ, so an entry point that ignored the argument could
 * not reach the refusals below looking like one that honours it. Then:
 *
 * - a non-finite value is refused by the facade as a `RangeError` naming the
 *   argument, which is what the shared `assertFiniteScalar` throws;
 * - `1e39` -- finite, but past `FLT_MAX` -- is refused by the addon's narrowing
 *   reader rather than saturating to an infinity. An infinity is refused a step
 *   earlier, so it cannot stand in for this value.
 */

const SR = 22050;
const HOP = 512;
const N_FRAMES = 43;
const LENGTH = N_FRAMES * HOP;
const FRAME_RATE = SR / HOP;
const SPLIT_FRAME = 22;

/** Finite, and larger than any 32-bit float. */
const PAST_FLT_MAX = 1e39;
const NON_FINITE = [Number.NaN, Number.POSITIVE_INFINITY, Number.NEGATIVE_INFINITY];
const NARROWING_REFUSAL = /must be a finite number within the 32-bit float range/;

function toneFromTrack(f0Hz: Float32Array): Float32Array {
  const samples = new Float32Array(LENGTH);
  let phase = 0;
  for (let i = 0; i < LENGTH; i += 1) {
    const hz = f0Hz[Math.min(Math.floor(i / HOP), f0Hz.length - 1)] as number;
    phase += (2 * Math.PI * hz) / SR;
    samples[i] = 0.5 * Math.sin(phase);
  }
  return samples;
}

/** A pitch that steps once, so the segmenter returns exactly two notes. */
function twoNoteTrack(): { samples: Float32Array; f0Hz: Float32Array; voiced: Int32Array } {
  const f0Hz = new Float32Array(N_FRAMES);
  const voiced = new Int32Array(N_FRAMES);
  for (let f = 0; f < N_FRAMES; f += 1) {
    f0Hz[f] = f < SPLIT_FRAME ? 220 : 330;
    voiced[f] = 1;
  }
  return { samples: toneFromTrack(f0Hz), f0Hz, voiced };
}

/** A 5 Hz vibrato over a 0.4 Hz drift: a curve edit is a no-op without one. */
function vibratoTrack(): { samples: Float32Array; f0Hz: Float32Array; voiced: Int32Array } {
  const f0Hz = new Float32Array(N_FRAMES);
  const voiced = new Int32Array(N_FRAMES);
  for (let f = 0; f < N_FRAMES; f += 1) {
    const cents =
      60 * Math.sin((2 * Math.PI * 5 * f) / FRAME_RATE) +
      40 * Math.sin((2 * Math.PI * 0.4 * f) / FRAME_RATE);
    f0Hz[f] = 220 * 2 ** (cents / 1200);
    voiced[f] = 1;
  }
  return { samples: toneFromTrack(f0Hz), f0Hz, voiced };
}

const tone = twoNoteTrack();
const vibrato = vibratoTrack();
const track = { samples: tone.samples, sampleRate: SR, f0Hz: tone.f0Hz, voiced: tone.voiced };

function zeroCrossings(x: Float32Array): number {
  let count = 0;
  for (let i = 1; i < x.length; i += 1) {
    if ((x[i - 1] as number) < 0 !== (x[i] as number) < 0) {
      count += 1;
    }
  }
  return count;
}

function absSum(x: Float32Array): number {
  let total = 0;
  for (let i = 0; i < x.length; i += 1) {
    total += Math.abs(x[i] as number);
  }
  return total;
}

/** The sample a note ends at is its frame bound converted through `frameRate`. */
function expectedOffsetSample(frameEnd: number, frameRate: number): number {
  return Math.min(Math.round((frameEnd / frameRate) * SR), LENGTH);
}

describe('a required facade scalar is consumed, then refused by name', () => {
  it('timeStretch consumes rate', () => {
    // Half speed doubles the output, double speed halves it.
    expect(timeStretch(tone.samples, SR, 0.5).length).toBe(LENGTH * 2);
    expect(timeStretch(tone.samples, SR, 2).length).toBe(LENGTH / 2);

    for (const bad of NON_FINITE) {
      expect(() => timeStretch(tone.samples, SR, bad)).toThrow(RangeError);
      expect(() => timeStretch(tone.samples, SR, bad)).toThrow(
        /timeStretch: rate must be a finite number/,
      );
    }
    expect(() => timeStretch(tone.samples, SR, PAST_FLT_MAX)).toThrow(NARROWING_REFUSAL);
  });

  it('pitchShift consumes semitones', () => {
    // A fifth up raises the fundamental, so the shifted tone crosses zero more
    // often; the length is unchanged, so only the content can tell them apart.
    const unshifted = pitchShift(tone.samples, SR, 0);
    const up = pitchShift(tone.samples, SR, 7);
    expect(up.length).toBe(unshifted.length);
    expect(zeroCrossings(up)).toBeGreaterThan(zeroCrossings(unshifted) * 1.2);

    for (const bad of NON_FINITE) {
      expect(() => pitchShift(tone.samples, SR, bad)).toThrow(RangeError);
      expect(() => pitchShift(tone.samples, SR, bad)).toThrow(
        /pitchShift: semitones must be a finite number/,
      );
    }
    expect(() => pitchShift(tone.samples, SR, PAST_FLT_MAX)).toThrow(NARROWING_REFUSAL);
  });

  it('phaseVocoder consumes rate', () => {
    expect(phaseVocoder(tone.samples, SR, 0.5).length).toBe(LENGTH * 2);
    expect(phaseVocoder(tone.samples, SR, 2).length).toBe(LENGTH / 2);

    for (const bad of NON_FINITE) {
      expect(() => phaseVocoder(tone.samples, SR, bad)).toThrow(RangeError);
      expect(() => phaseVocoder(tone.samples, SR, bad)).toThrow(
        /phaseVocoder: rate must be a finite number/,
      );
    }
    expect(() => phaseVocoder(tone.samples, SR, PAST_FLT_MAX)).toThrow(NARROWING_REFUSAL);
  });

  it('extractNotes consumes frameRate', () => {
    // The frame bounds are the segmenter's own and do not move; the sample span
    // each note reports is the bound converted through the rate, so that is
    // where a rate the call ignored would show.
    const atTrackRate = extractNotes({ ...track, frameRate: FRAME_RATE });
    const atDouble = extractNotes({ ...track, frameRate: 100 });
    expect(atTrackRate[0]?.offsetSample).toBe(expectedOffsetSample(SPLIT_FRAME, FRAME_RATE));
    expect(atDouble[0]?.offsetSample).toBe(expectedOffsetSample(SPLIT_FRAME, 100));
    expect(atDouble[0]?.offsetSample).not.toBe(atTrackRate[0]?.offsetSample);

    for (const bad of NON_FINITE) {
      expect(() => extractNotes({ ...track, frameRate: bad })).toThrow(RangeError);
      expect(() => extractNotes({ ...track, frameRate: bad })).toThrow(
        /extractNotes: frameRate must be a finite number/,
      );
    }
    expect(() => extractNotes({ ...track, frameRate: PAST_FLT_MAX })).toThrow(NARROWING_REFUSAL);
  });

  it('splitNote consumes frameRate', () => {
    const notes = extractNotes({ ...track, frameRate: FRAME_RATE });
    const atTrackRate = splitNote({ ...track, frameRate: FRAME_RATE, notes, index: 0, frame: 10 });
    const atSlowRate = splitNote({ ...track, frameRate: 5, notes, index: 0, frame: 10 });
    expect(atTrackRate[0]?.offsetSample).toBe(expectedOffsetSample(10, FRAME_RATE));
    expect(atSlowRate[0]?.offsetSample).toBe(expectedOffsetSample(10, 5));
    expect(atSlowRate[0]?.offsetSample).not.toBe(atTrackRate[0]?.offsetSample);

    for (const bad of NON_FINITE) {
      expect(() => splitNote({ ...track, frameRate: bad, notes, index: 0, frame: 10 })).toThrow(
        RangeError,
      );
      expect(() => splitNote({ ...track, frameRate: bad, notes, index: 0, frame: 10 })).toThrow(
        /splitNote: frameRate must be a finite number/,
      );
    }
    expect(() =>
      splitNote({ ...track, frameRate: PAST_FLT_MAX, notes, index: 0, frame: 10 }),
    ).toThrow(NARROWING_REFUSAL);
  });

  it('mergeNotes consumes frameRate', () => {
    const notes = extractNotes({ ...track, frameRate: FRAME_RATE });
    const merge = (frameRate: number) =>
      mergeNotes({ ...track, frameRate, notes, first: 0, last: notes.length - 1 });
    const atTrackRate = merge(FRAME_RATE);
    const atFastRate = merge(500);
    expect(atTrackRate).toHaveLength(1);
    expect(atTrackRate[0]?.offsetSample).toBe(expectedOffsetSample(N_FRAMES, FRAME_RATE));
    expect(atFastRate[0]?.offsetSample).toBe(expectedOffsetSample(N_FRAMES, 500));
    expect(atFastRate[0]?.offsetSample).not.toBe(atTrackRate[0]?.offsetSample);

    for (const bad of NON_FINITE) {
      expect(() => merge(bad)).toThrow(RangeError);
      expect(() => merge(bad)).toThrow(/mergeNotes: frameRate must be a finite number/);
    }
    expect(() => merge(PAST_FLT_MAX)).toThrow(NARROWING_REFUSAL);
  });

  it('renderNotes consumes frameRate when a curve edit reads the track', () => {
    const vibratoTrackArgs = {
      samples: vibrato.samples,
      sampleRate: SR,
      f0Hz: vibrato.f0Hz,
      voiced: vibrato.voiced,
    };
    const notes = extractNotes({ ...vibratoTrackArgs, frameRate: FRAME_RATE });
    const render = (frameRate?: number, flatten = true) =>
      renderNotes({
        samples: vibrato.samples,
        sampleRate: SR,
        notes: notes.map((note) => ({
          ...note,
          edit: { ...note.edit, vibratoDepthChange: flatten ? -1 : 0 },
        })),
        f0Hz: vibrato.f0Hz,
        frameRate,
        vibratoCutoffHz: 3,
      });

    // The rate decides which modulation counts as vibrato, so flattening at two
    // rates removes two different components. Both must also differ from the
    // unedited render, or the pair would agree by doing nothing.
    const untouched = absSum(render(FRAME_RATE, false));
    const atTrackRate = absSum(render(FRAME_RATE));
    const atFastRate = absSum(render(200));
    expect(atTrackRate).not.toBeCloseTo(untouched, 3);
    expect(atFastRate).not.toBeCloseTo(untouched, 3);
    expect(atFastRate).not.toBeCloseTo(atTrackRate, 3);

    for (const bad of [...NON_FINITE, undefined]) {
      expect(() => render(bad)).toThrow(RangeError);
      expect(() => render(bad)).toThrow(/renderNotes: frameRate must be a finite number/);
    }
    expect(() => render(PAST_FLT_MAX)).toThrow(NARROWING_REFUSAL);
  });

  it('decomposeNotePitch consumes frameRate', () => {
    const frames = 400;
    const centreHz = 196;
    const curve = new Float32Array(frames);
    for (let i = 0; i < frames; i += 1) {
      const cents =
        50 * Math.sin((2 * Math.PI * 5 * i) / 100) + 30 * Math.sin((2 * Math.PI * 0.5 * i) / 100);
      curve[i] = centreHz * 2 ** (cents / 1200);
    }
    const decompose = (frameRate: number) =>
      decomposeNotePitch({ f0Hz: curve, frameRate, medianHz: centreHz });

    // The rate moves the boundary between the two curves. Their sum is the
    // note's own pitch either way, so the pair cannot differ by decomposing
    // different inputs -- only by splitting the same one elsewhere.
    const fast = decompose(100);
    const slow = decompose(25);
    for (const frame of [0, 37, 200]) {
      expect((fast.driftCents[frame] as number) + (fast.vibratoCents[frame] as number)).toBeCloseTo(
        (slow.driftCents[frame] as number) + (slow.vibratoCents[frame] as number),
        3,
      );
    }
    expect(fast.driftCents[0]).not.toBeCloseTo(slow.driftCents[0] as number, 3);

    for (const bad of NON_FINITE) {
      expect(() => decompose(bad)).toThrow(RangeError);
      expect(() => decompose(bad)).toThrow(/decomposeNotePitch: frameRate must be a finite number/);
    }
    expect(() => decompose(PAST_FLT_MAX)).toThrow(NARROWING_REFUSAL);
  });
});
