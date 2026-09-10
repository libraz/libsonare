import { describe, expect, it } from 'vitest';
import type { NoteObject, NoteObjectInput } from '../src/index.js';
import { extractNotes, renderNotes } from '../src/index.js';

const SR = 22050;
const HOP = 512;
const N_FRAMES = 43;
const LENGTH = N_FRAMES * HOP;
const FRAME_RATE = SR / HOP;
// The pitch steps here, so the segmenter splits into exactly two notes whose
// spans tile the buffer: [0, SPLIT_FRAME * HOP) and [SPLIT_FRAME * HOP, LENGTH).
const SPLIT_FRAME = 22;
const SPLIT_SAMPLE = SPLIT_FRAME * HOP;

/** A phase-continuous 440 Hz -> 880 Hz tone plus the F0 track describing it. */
function twoNoteTone(): { samples: Float32Array; f0Hz: Float32Array; voiced: Int32Array } {
  const samples = new Float32Array(LENGTH);
  let phase = 0;
  for (let i = 0; i < LENGTH; i += 1) {
    samples[i] = 0.5 * Math.sin(phase);
    phase += (2 * Math.PI * (i < SPLIT_SAMPLE ? 440 : 880)) / SR;
  }
  const f0Hz = new Float32Array(N_FRAMES);
  for (let frame = 0; frame < N_FRAMES; frame += 1) {
    f0Hz[frame] = frame < SPLIT_FRAME ? 440 : 880;
  }
  return { samples, f0Hz, voiced: new Int32Array(N_FRAMES).fill(1) };
}

function rms(samples: Float32Array, start: number, end: number): number {
  let sum = 0;
  for (let i = start; i < end; i += 1) {
    sum += samples[i] * samples[i];
  }
  return Math.sqrt(sum / (end - start));
}

const tone = twoNoteTone();

function extractTone(): NoteObject[] {
  return extractNotes({
    samples: tone.samples,
    sampleRate: SR,
    f0Hz: tone.f0Hz,
    voiced: tone.voiced,
    frameRate: FRAME_RATE,
  });
}

describe('extractNotes', () => {
  it('segments a stepped pitch track into notes carrying their own amplitude slice', () => {
    const notes = extractTone();

    expect(notes).toHaveLength(2);
    expect(notes[0].onsetSample).toBe(0);
    expect(notes[0].offsetSample).toBe(SPLIT_SAMPLE);
    expect(notes[1].onsetSample).toBe(SPLIT_SAMPLE);
    expect(notes[1].offsetSample).toBe(LENGTH);
    expect(notes[0].medianHz).toBeCloseTo(440, 1);
    expect(notes[1].medianHz).toBeCloseTo(880, 1);
    expect(notes[1].medianCents).toBeCloseTo(1200, 1);

    for (const note of notes) {
      expect(note.amplitude).toBeInstanceOf(Float32Array);
      expect(note.amplitude).toHaveLength(note.frameEnd - note.frameStart);
      expect(note.f0Stability).toBe(1);
    }
  });

  it('returns the identity edit on every note', () => {
    for (const note of extractTone()) {
      expect(note.edit).toEqual({
        timeOffsetSamples: 0,
        pitchShiftSemitones: 0,
        gainDb: 0,
        timeStretchRatio: 1,
        muted: false,
      });
    }
  });

  it('returns an empty array when nothing segments', () => {
    expect(
      extractNotes({
        samples: tone.samples,
        sampleRate: SR,
        f0Hz: new Float32Array(N_FRAMES),
        voiced: new Int32Array(N_FRAMES),
        frameRate: FRAME_RATE,
      }),
    ).toEqual([]);
  });

  it('rejects a malformed request', () => {
    const request = {
      samples: tone.samples,
      sampleRate: SR,
      f0Hz: tone.f0Hz,
      frameRate: FRAME_RATE,
    };
    expect(() => extractNotes({ ...request, frameRate: Number.NaN })).toThrow(TypeError);
    expect(() => extractNotes({ ...request, sampleRate: 0 })).toThrow(RangeError);
    expect(() => extractNotes({ ...request, voiced: new Int32Array(3) })).toThrow(RangeError);
    expect(() => extractNotes({ ...request, voicedProb: new Float32Array(3) })).toThrow(RangeError);
  });
});

describe('renderNotes', () => {
  it('reproduces the input when every edit is the identity', () => {
    expect(renderNotes({ samples: tone.samples, sampleRate: SR, notes: extractTone() })).toEqual(
      tone.samples,
    );
  });

  it('reproduces the input for an empty note set', () => {
    expect(renderNotes({ samples: tone.samples, sampleRate: SR, notes: [] })).toEqual(tone.samples);
  });

  it('applies a gain edit to that note only', () => {
    const notes = extractTone();
    notes[0].edit.gainDb = -20;
    const rendered = renderNotes({ samples: tone.samples, sampleRate: SR, notes });

    // Sampled past the 5 ms edge cross-fade, where the edit's gain is exact.
    expect(rms(rendered, 2000, 9000)).toBeCloseTo(rms(tone.samples, 2000, 9000) * 0.1, 5);
    expect(rendered.subarray(SPLIT_SAMPLE)).toEqual(tone.samples.subarray(SPLIT_SAMPLE));
  });

  it('silences a muted note and leaves its neighbour untouched', () => {
    const notes = extractTone();
    notes[1].edit.muted = true;
    const rendered = renderNotes({ samples: tone.samples, sampleRate: SR, notes });

    expect(rms(rendered, SPLIT_SAMPLE + 2000, LENGTH - 2000)).toBe(0);
    expect(rendered.subarray(0, SPLIT_SAMPLE)).toEqual(tone.samples.subarray(0, SPLIT_SAMPLE));
  });

  it('rejects a malformed request', () => {
    const notes = extractTone();
    expect(() =>
      renderNotes({
        samples: tone.samples,
        sampleRate: SR,
        notes: 'not an array' as unknown as NoteObjectInput[],
      }),
    ).toThrow(TypeError);
    expect(() => renderNotes({ samples: tone.samples, sampleRate: 0, notes })).toThrow(RangeError);
    expect(() =>
      renderNotes({
        samples: tone.samples,
        sampleRate: SR,
        notes: [{ onsetSample: 0, offsetSample: 5000, edit: { gainDb: -6 } }, ...notes],
      }),
    ).toThrow();
  });
});
