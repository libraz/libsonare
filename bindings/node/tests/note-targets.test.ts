/**
 * Reference-melody assignment: reading targets out of an SMF, and writing each
 * note's shift from the target it overlaps.
 *
 * Every tuning field is driven as a PAIR of calls differing in that field alone,
 * and the two answers are asserted to differ — a field the addon never read would
 * satisfy an "it does not throw" case exactly as one it honoured. The three
 * policies are compared against each other for the same reason: each has to
 * produce an answer neither of the other two does.
 *
 * The notes are hand-built on half-second spans at 48 kHz and every median is
 * 440 Hz, which is MIDI 69 exactly, so each expected shift is `targetMidi - 69`
 * rather than a tolerance. The reference melodies are real SMF bytes written by
 * the library's own exporter — the fixture, not the thing under test.
 */

import { describe, expect, it } from 'vitest';
import type { NoteObject, NoteTarget, NoteTargetUnmatchedPolicy } from '../src/index.js';
import {
  assignNoteTargets,
  extractNotes,
  noteTargetsFromSmf,
  Project,
  renderNotes,
} from '../src/index.js';

const SR = 48000;
const HALF_SECOND = SR / 2;
/** 440 Hz is MIDI 69 exactly, so every expected shift below is `target - 69`. */
const A4_HZ = 440;

function noteAt(onsetSample: number, offsetSample: number, medianHz: number): NoteObject {
  return {
    onsetSample,
    offsetSample,
    frameStart: 0,
    frameEnd: 0,
    medianHz,
    medianCents: 0,
    f0Stability: 1,
    amplitude: new Float32Array(0),
    edit: {
      timeOffsetSamples: 0,
      amplitudeEnvelope: new Float32Array(0),
      pitchShiftSemitones: 0,
      gainDb: 0,
      timeStretchRatio: 1,
      formantShiftSemitones: 0,
      vibratoDepthChange: 0,
      driftChange: 0,
      muted: false,
    },
  };
}

function target(startSec: number, endSec: number, targetMidi: number): NoteTarget {
  return { startSec, endSec, targetMidi };
}

/** One note over the first target, and one a second later with nothing near it. */
function matchedAndStranded(): NoteObject[] {
  return [noteAt(0, HALF_SECOND, A4_HZ), noteAt(3 * HALF_SECOND, 4 * HALF_SECOND, A4_HZ)];
}

const shift = (note: NoteObject): number => Number(note.edit.pitchShiftSemitones.toFixed(4));

/**
 * Two quarter notes at 120 BPM: C4 over 0–0.5 s and G4 over 0.5–1.0 s.
 *
 * The exporter writes the tempo map as its own track, which carries no MIDI, so
 * the melody is track 0 in the reader's counting of MIDI-carrying tracks.
 *
 * The clip outlasts its own last event: a clip span is half-open, so a note-off
 * written at the clip's end is not exported, and the reader would then — rightly
 * — drop a note-on that never closes.
 */
function twoQuarterNotesSmf(): Buffer {
  const project = Project.create();
  try {
    project.setTempoSegments([{ startPpq: 0, bpm: 120 }]);
    const { clipId } = project.addMidiClip(0, 3);
    project.setMidiEvents(clipId, [
      Project.midiNoteOn(0, 0, 0, 60, 100),
      Project.midiNoteOff(1, 0, 0, 60, 0),
      Project.midiNoteOn(1, 0, 0, 67, 100),
      Project.midiNoteOff(2, 0, 0, 67, 0),
    ]);
    return project.exportSmf();
  } finally {
    project.destroy();
  }
}

describe('noteTargetsFromSmf', () => {
  it("returns one target per closed note, at the file's own tempo", () => {
    const targets = noteTargetsFromSmf({ data: twoQuarterNotesSmf() });
    expect(targets).toHaveLength(2);
    expect(targets[0].startSec).toBeCloseTo(0, 4);
    expect(targets[0].endSec).toBeCloseTo(0.5, 4);
    expect(targets[0].targetMidi).toBe(60);
    expect(targets[1].startSec).toBeCloseTo(0.5, 4);
    expect(targets[1].endSec).toBeCloseTo(1, 4);
    expect(targets[1].targetMidi).toBe(67);
  });

  it('reads track 0 by default, and refuses an index the file does not have', () => {
    const data = twoQuarterNotesSmf();
    expect(noteTargetsFromSmf({ data })).toEqual(noteTargetsFromSmf({ data, trackIndex: 0 }));
    expect(() => noteTargetsFromSmf({ data, trackIndex: 1 })).toThrow();
    expect(() => noteTargetsFromSmf({ data, trackIndex: -1 })).toThrow();
  });

  it('refuses a fractional track index, bytes that are not a file, and a wrong type', () => {
    const data = twoQuarterNotesSmf();
    expect(() => noteTargetsFromSmf({ data, trackIndex: 0.5 })).toThrow(RangeError);
    expect(() => noteTargetsFromSmf({ data: new Uint8Array(64).fill(0x7f) })).toThrow();
    expect(() => noteTargetsFromSmf({ data: [] as unknown as Uint8Array })).toThrow(TypeError);
  });

  it('accepts a Uint8Array as well as the Buffer the exporter returns', () => {
    const data = twoQuarterNotesSmf();
    expect(noteTargetsFromSmf({ data: new Uint8Array(data) })).toEqual(
      noteTargetsFromSmf({ data }),
    );
  });
});

describe('assignNoteTargets writes the shift its target asks for', () => {
  it('assigns every overlapping note and reports the count', () => {
    const notes = [noteAt(0, HALF_SECOND, A4_HZ), noteAt(HALF_SECOND, 2 * HALF_SECOND, A4_HZ)];
    const result = assignNoteTargets({
      notes,
      sampleRate: SR,
      targets: [target(0, 0.5, 60), target(0.5, 1, 72)],
    });
    expect(result.assignedCount).toBe(2);
    expect(shift(result.notes[0])).toBeCloseTo(-9, 4);
    expect(shift(result.notes[1])).toBeCloseTo(3, 4);
    expect(result.notes[0].edit.muted).toBe(false);
  });

  it("returns a new array and leaves the caller's own notes alone", () => {
    const notes = [noteAt(0, HALF_SECOND, A4_HZ)];
    const before = notes[0].edit.pitchShiftSemitones;
    const result = assignNoteTargets({ notes, sampleRate: SR, targets: [target(0, 0.5, 72)] });
    expect(shift(result.notes[0])).toBeCloseTo(3, 4);
    expect(result.notes).not.toBe(notes);
    expect(result.notes[0]).not.toBe(notes[0]);
    expect(notes[0].edit.pitchShiftSemitones).toBe(before);
  });

  it('assigns nothing, and refuses nothing, for an empty take or an empty reference', () => {
    expect(assignNoteTargets({ notes: [], sampleRate: SR, targets: [] })).toEqual({
      notes: [],
      assignedCount: 0,
    });
    const stranded = assignNoteTargets({
      notes: [noteAt(0, HALF_SECOND, A4_HZ)],
      sampleRate: SR,
      targets: [],
    });
    expect(stranded.assignedCount).toBe(0);
    expect(shift(stranded.notes[0])).toBe(0);
  });
});

describe('each tuning field is consumed', () => {
  /** The whole answer for one policy, as one comparable value. */
  function underPolicy(policy: NoteTargetUnmatchedPolicy): string {
    const result = assignNoteTargets({
      notes: matchedAndStranded(),
      sampleRate: SR,
      targets: [target(0, 0.5, 72)],
      unmatchedPolicy: policy,
    });
    return JSON.stringify({
      assignedCount: result.assignedCount,
      edits: result.notes.map((note) => [shift(note), note.edit.muted]),
    });
  }

  it('the three policies each produce an answer the other two do not', () => {
    const answers = (['leave', 'mute', 'nearest'] as const).map(underPolicy);
    expect(new Set(answers).size).toBe(answers.length);
  });

  it('leave keeps the stranded note as recorded, mute silences it, nearest reaches it', () => {
    const leave = assignNoteTargets({
      notes: matchedAndStranded(),
      sampleRate: SR,
      targets: [target(0, 0.5, 72)],
      unmatchedPolicy: 'leave',
    });
    expect(leave.assignedCount).toBe(1);
    expect(shift(leave.notes[1])).toBe(0);
    expect(leave.notes[1].edit.muted).toBe(false);

    const mute = assignNoteTargets({
      notes: matchedAndStranded(),
      sampleRate: SR,
      targets: [target(0, 0.5, 72)],
      unmatchedPolicy: 'mute',
    });
    expect(mute.assignedCount).toBe(1);
    expect(mute.notes[1].edit.muted).toBe(true);
    // The matched note is not muted, so the policy reached only the other one.
    expect(mute.notes[0].edit.muted).toBe(false);

    const nearest = assignNoteTargets({
      notes: matchedAndStranded(),
      sampleRate: SR,
      targets: [target(0, 0.5, 72)],
      unmatchedPolicy: 'nearest',
    });
    expect(nearest.assignedCount).toBe(2);
    expect(shift(nearest.notes[1])).toBeCloseTo(3, 4);
    expect(nearest.notes[1].edit.muted).toBe(false);
  });

  it('an omitted policy is the leave policy', () => {
    const omitted = assignNoteTargets({
      notes: matchedAndStranded(),
      sampleRate: SR,
      targets: [target(0, 0.5, 72)],
    });
    expect(
      JSON.stringify({
        assignedCount: omitted.assignedCount,
        edits: omitted.notes.map((note) => [shift(note), note.edit.muted]),
      }),
    ).toBe(underPolicy('leave'));
  });

  it('minOverlapRatio decides whether a target counts', () => {
    // The target covers 0.2 s of a 0.5 s note, which is 40%.
    const call = (minOverlapRatio?: number) =>
      assignNoteTargets({
        notes: [noteAt(0, HALF_SECOND, A4_HZ)],
        sampleRate: SR,
        targets: [target(0.3, 0.7, 72)],
        minOverlapRatio,
      });

    const strict = call(0.5);
    const loose = call(0.25);
    expect(strict.assignedCount).not.toBe(loose.assignedCount);
    expect(strict.assignedCount).toBe(0);
    expect(shift(strict.notes[0])).toBe(0);
    expect(loose.assignedCount).toBe(1);
    expect(shift(loose.notes[0])).toBeCloseTo(3, 4);

    // 0 is its own meaning -- any overlap at all counts -- and NOT a request for
    // the default, which would have refused this 40% overlap.
    expect(call(0).assignedCount).toBe(1);
    expect(call().assignedCount).toBe(0);
  });

  it('the default minOverlapRatio sits between a 40% and a 60% overlap', () => {
    // Brackets the seeded 0.5 without restating it: the same call with no
    // minOverlapRatio answers differently either side of it.
    const withOverlap = (endSec: number) =>
      assignNoteTargets({
        notes: [noteAt(0, HALF_SECOND, A4_HZ)],
        sampleRate: SR,
        targets: [target(0, endSec, 72)],
      }).assignedCount;
    expect(withOverlap(0.2)).toBe(0);
    expect(withOverlap(0.3)).toBe(1);
  });

  it('maxCorrectionSemitones saturates the shift rather than refusing it', () => {
    // Two octaves up and two down from MIDI 69.
    const call = (maxCorrectionSemitones?: number) =>
      assignNoteTargets({
        notes: [noteAt(0, HALF_SECOND, A4_HZ), noteAt(HALF_SECOND, 2 * HALF_SECOND, A4_HZ)],
        sampleRate: SR,
        targets: [target(0, 0.5, 93), target(0.5, 1, 45)],
        maxCorrectionSemitones,
      });

    const octave = call(12);
    const fifth = call(7);
    expect(shift(octave.notes[0])).not.toBe(shift(fifth.notes[0]));
    expect(shift(octave.notes[0])).toBeCloseTo(12, 4);
    expect(shift(octave.notes[1])).toBeCloseTo(-12, 4);
    expect(shift(fifth.notes[0])).toBeCloseTo(7, 4);
    expect(shift(fifth.notes[1])).toBeCloseTo(-7, 4);
    // Both arms count as assigned: the bound saturates the correction and does
    // not withhold the assignment.
    expect(fifth.assignedCount).toBe(2);

    // 0 is a bound of zero, not a request for the default: the notes are still
    // assigned and neither one moves.
    const pinned = call(0);
    expect(pinned.assignedCount).toBe(2);
    expect(shift(pinned.notes[0])).toBe(0);
    expect(shift(pinned.notes[1])).toBe(0);
    // Which the default would not have done, so the 0 demonstrably arrived.
    expect(shift(call().notes[0])).toBeCloseTo(12, 4);
  });

  it('a note with no measured pitch is never edited, whatever the policy says', () => {
    for (const medianHz of [0, Number.NaN, -1]) {
      for (const policy of ['mute', 'nearest'] as const) {
        const result = assignNoteTargets({
          notes: [noteAt(0, HALF_SECOND, medianHz)],
          sampleRate: SR,
          targets: [target(0, 0.5, 72)],
          unmatchedPolicy: policy,
        });
        expect(result.assignedCount, `${medianHz} ${policy}`).toBe(0);
        expect(shift(result.notes[0])).toBe(0);
        expect(result.notes[0].edit.muted).toBe(false);
      }
    }
  });
});

describe('assignNoteTargets refuses what it cannot act on', () => {
  const notes = () => [noteAt(0, HALF_SECOND, A4_HZ)];
  const targets = () => [target(0, 0.5, 72)];

  it('refuses a non-array notes or targets, and an entry that is not an object', () => {
    expect(() =>
      assignNoteTargets({
        notes: 'no' as unknown as NoteObject[],
        sampleRate: SR,
        targets: targets(),
      }),
    ).toThrow(TypeError);
    expect(() =>
      assignNoteTargets({
        notes: notes(),
        sampleRate: SR,
        targets: 'no' as unknown as NoteTarget[],
      }),
    ).toThrow(TypeError);
    expect(() =>
      assignNoteTargets({
        notes: notes(),
        sampleRate: SR,
        targets: [7 as unknown as NoteTarget],
      }),
    ).toThrow(/each target must be a plain object/);
  });

  it('refuses a note without its span and a target missing a field, naming them', () => {
    expect(() =>
      assignNoteTargets({
        notes: [{ ...noteAt(0, HALF_SECOND, A4_HZ), onsetSample: undefined as unknown as number }],
        sampleRate: SR,
        targets: targets(),
      }),
    ).toThrow(/note.onsetSample is required/);
    expect(() =>
      assignNoteTargets({
        notes: notes(),
        sampleRate: SR,
        targets: [{ startSec: 0, endSec: 0.5 } as unknown as NoteTarget],
      }),
    ).toThrow(/targets\[0\].targetMidi must be a number/);
  });

  it('refuses an unknown policy spelling and a non-string one', () => {
    expect(() =>
      assignNoteTargets({
        notes: notes(),
        sampleRate: SR,
        targets: targets(),
        unmatchedPolicy: 'silence' as NoteTargetUnmatchedPolicy,
      }),
    ).toThrow(RangeError);
    expect(() =>
      assignNoteTargets({
        notes: notes(),
        sampleRate: SR,
        targets: targets(),
        unmatchedPolicy: 2 as unknown as NoteTargetUnmatchedPolicy,
      }),
    ).toThrow(TypeError);
  });

  it('refuses a ratio and a bound outside their domains, and a value no float holds', () => {
    const call = (options: Record<string, unknown>) =>
      assignNoteTargets({ notes: notes(), sampleRate: SR, targets: targets(), ...options });
    expect(() => call({ minOverlapRatio: 1.5 })).toThrow();
    expect(() => call({ minOverlapRatio: -0.1 })).toThrow();
    expect(() => call({ maxCorrectionSemitones: -1 })).toThrow();
    for (const bad of [Number.NaN, Number.POSITIVE_INFINITY, 1e39]) {
      expect(() => call({ minOverlapRatio: bad }), `${bad}`).toThrow(RangeError);
      expect(() => call({ maxCorrectionSemitones: bad }), `${bad}`).toThrow(RangeError);
    }
    expect(() => call({ minOverlapRatio: '0.5' })).toThrow(TypeError);
  });

  it('refuses a sample rate that cannot convert a span to seconds', () => {
    expect(() => assignNoteTargets({ notes: notes(), sampleRate: 0, targets: targets() })).toThrow(
      RangeError,
    );
  });
});

describe('the whole chain, on a segmented take', () => {
  const HOP = 512;
  const FRAMES = 48;
  const LENGTH = FRAMES * HOP;

  function sungA4(): Float32Array {
    const samples = new Float32Array(LENGTH);
    for (let i = 0; i < LENGTH; i += 1) {
      samples[i] = 0.5 * Math.sin((2 * Math.PI * A4_HZ * i) / SR);
    }
    return samples;
  }

  function maxDifference(a: Float32Array, b: Float32Array): number {
    let worst = 0;
    for (let i = 0; i < a.length; i += 1) {
      worst = Math.max(worst, Math.abs((a[i] as number) - (b[i] as number)));
    }
    return worst;
  }

  it('carries the extracted notes measured fields through to a render', () => {
    const samples = sungA4();
    const notes = extractNotes({
      samples,
      sampleRate: SR,
      f0Hz: new Float32Array(FRAMES).fill(A4_HZ),
      voiced: new Int32Array(FRAMES).fill(1),
      frameRate: SR / HOP,
    });
    expect(notes.length).toBeGreaterThan(0);

    // The take was sung at A4 throughout and the file asks for C4 over the span
    // the take covers, so the assignment is nine semitones down.
    const targets = noteTargetsFromSmf({ data: twoQuarterNotesSmf() });
    const tuned = assignNoteTargets({ notes, sampleRate: SR, targets });
    expect(tuned.assignedCount).toBe(1);
    expect(shift(tuned.notes[0])).toBeCloseTo(-9, 2);

    // The measured fields and the amplitude curve never reached the C ABI, so
    // they are the caller's own objects rather than copies of them.
    expect(tuned.notes[0].amplitude).toBe(notes[0].amplitude);
    expect(tuned.notes[0].medianCents).toBe(notes[0].medianCents);
    expect(tuned.notes[0].f0Stability).toBe(notes[0].f0Stability);
    expect(tuned.notes[0].frameEnd).toBe(notes[0].frameEnd);

    const rendered = renderNotes({ samples, sampleRate: SR, notes: tuned.notes });
    expect(rendered).toHaveLength(samples.length);
    expect(maxDifference(samples, rendered)).toBeGreaterThan(0.05);
  });
});
