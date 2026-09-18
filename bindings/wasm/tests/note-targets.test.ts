/**
 * Reference-melody assignment on the WASM surface.
 *
 * Every knob is checked by a pair of calls differing in that one argument, so a
 * green case cannot come from the knob being ignored. The fixtures are hand-built
 * at one note a second: a note's overlap with a target is then the target's own
 * length in seconds, and every expected shift is a MIDI subtraction.
 *
 * This surface does not go through the C ABI — `sonare_c_daw.cpp` is not linked
 * into the module — so the refusals the C entry point performs are reproduced in
 * the wrapper and are exercised here as its own contract, not as a mirror.
 */

import { beforeAll, describe, expect, it } from 'vitest';
import type { AssignNoteTargetsRequest } from '../src/effects_note_ops';
import {
  assignNoteTargets,
  ErrorCode,
  extractNotes,
  init,
  isSonareError,
  type NoteObject,
  noteTargetsFromSmf,
  Project,
  type SonareError,
} from '../src/index';

const sampleRate = 48000;
/** A4: the fixture notes' measured pitch, so a target's MIDI number is the shift plus 69. */
const a4Hz = 440;

/** One note per second, all on A4 unless a test says otherwise. */
function note(startSec: number, endSec: number, medianHz = a4Hz): NoteObject {
  return {
    onsetSample: Math.round(startSec * sampleRate),
    offsetSample: Math.round(endSec * sampleRate),
    frameStart: Math.round(startSec * 100),
    frameEnd: Math.round(endSec * 100),
    medianHz,
    medianCents: 0,
    f0Stability: 1,
    amplitude: new Float32Array([0.25, 0.5, 0.25]),
    edit: {
      timeOffsetSamples: 0,
      pitchShiftSemitones: 0,
      gainDb: 0,
      timeStretchRatio: 1,
      formantShiftSemitones: 0,
      vibratoDepthChange: 0,
      driftChange: 0,
      muted: false,
      amplitudeEnvelope: new Float32Array(),
    },
  };
}

/** Three consecutive one-second notes; only the first overlaps a `[0, 1)` target. */
function threeNotes(): NoteObject[] {
  return [note(0, 1), note(1, 2), note(2, 3)];
}

function expectSonareError(run: () => unknown, code: ErrorCode): SonareError {
  let thrown: unknown;
  try {
    run();
  } catch (error) {
    thrown = error;
  }
  if (!isSonareError(thrown)) {
    throw new Error(`expected a SonareError, got ${String(thrown)}`);
  }
  expect(thrown.code).toBe(code);
  return thrown;
}

describe('WASM note targets', () => {
  beforeAll(async () => {
    await init();
  });

  describe('assignNoteTargets', () => {
    it('writes the shift from the overlapped target and reports the count', () => {
      const notes = threeNotes();
      const result = assignNoteTargets({
        notes,
        sampleRate,
        targets: [{ startSec: 0, endSec: 1, targetMidi: 72 }],
      });
      expect(result.assignedCount).toBe(1);
      // 72 - 69 = +3 semitones; the two notes the target does not reach are left
      // alone by the default policy.
      expect(result.notes[0].edit.pitchShiftSemitones).toBeCloseTo(3, 4);
      expect(result.notes[1].edit.pitchShiftSemitones).toBe(0);
      expect(result.notes[2].edit.pitchShiftSemitones).toBe(0);
    });

    it('gives the three unmatched policies three different answers', () => {
      const targets = [{ startSec: 0, endSec: 1, targetMidi: 72 }];
      const run = (unmatchedPolicy: 'leave' | 'mute' | 'nearest') =>
        assignNoteTargets({ notes: threeNotes(), sampleRate, targets, unmatchedPolicy });

      const leave = run('leave');
      const mute = run('mute');
      const nearest = run('nearest');

      // The unmatched note at [1, 2): left as recorded, muted, or pulled onto the
      // one target there is. No two of the three agree on it.
      expect([leave.notes[1].edit.pitchShiftSemitones, leave.notes[1].edit.muted]).toEqual([
        0,
        false,
      ]);
      expect([mute.notes[1].edit.pitchShiftSemitones, mute.notes[1].edit.muted]).toEqual([0, true]);
      expect(nearest.notes[1].edit.pitchShiftSemitones).toBeCloseTo(3, 4);
      expect(nearest.notes[1].edit.muted).toBe(false);

      expect(leave.assignedCount).toBe(1);
      expect(mute.assignedCount).toBe(1);
      expect(nearest.assignedCount).toBe(3);
    });

    it('rejects an unknown policy rather than falling back to the default', () => {
      // A name no surface spells, and the ordinal the C enumeration uses: neither
      // reaches the rule, so a caller cannot pass 1 and get 'mute'.
      for (const unmatchedPolicy of ['silence', 'Mute', 1]) {
        const request = {
          notes: threeNotes(),
          sampleRate,
          targets: [{ startSec: 0, endSec: 1, targetMidi: 72 }],
          unmatchedPolicy,
        } as unknown as AssignNoteTargetsRequest;
        expectSonareError(() => assignNoteTargets(request), ErrorCode.InvalidParameter);
      }
    });

    it('saturates the shift at maxCorrectionSemitones, and an explicit 0 survives', () => {
      // 84 - 69 = +15, past the default bound of 12.
      const targets = [{ startSec: 0, endSec: 1, targetMidi: 84 }];
      const run = (maxCorrectionSemitones?: number) =>
        assignNoteTargets({ notes: [note(0, 1)], sampleRate, targets, maxCorrectionSemitones });

      const bounded = run();
      const widened = run(15);
      const zeroed = run(0);
      expect(bounded.notes[0].edit.pitchShiftSemitones).toBeCloseTo(12, 4);
      expect(widened.notes[0].edit.pitchShiftSemitones).toBeCloseTo(15, 4);
      expect(bounded.notes[0].edit.pitchShiftSemitones).not.toBeCloseTo(
        widened.notes[0].edit.pitchShiftSemitones,
        4,
      );
      // 0 is its own meaning: every correction saturates to nothing. It must not
      // read as "unset" and pick the default 12 back up. The note is still
      // assigned, which is what separates this from a note no target reached.
      expect(zeroed.notes[0].edit.pitchShiftSemitones).toBe(0);
      expect(zeroed.assignedCount).toBe(1);
      expect(zeroed.notes[0].edit.pitchShiftSemitones).not.toBeCloseTo(
        bounded.notes[0].edit.pitchShiftSemitones,
        4,
      );
    });

    it('refuses a negative maxCorrectionSemitones', () => {
      expectSonareError(
        () =>
          assignNoteTargets({
            notes: [note(0, 1)],
            sampleRate,
            targets: [{ startSec: 0, endSec: 1, targetMidi: 72 }],
            maxCorrectionSemitones: -1,
          }),
        ErrorCode.InvalidParameter,
      );
    });

    it('gates the match on minOverlapRatio, and an explicit 0 survives', () => {
      // 10 ms of a one-second note: a ratio of 0.01.
      const targets = [{ startSec: 0, endSec: 0.01, targetMidi: 72 }];
      const run = (minOverlapRatio?: number) =>
        assignNoteTargets({ notes: [note(0, 1)], sampleRate, targets, minOverlapRatio })
          .assignedCount;

      const defaulted = run();
      const strict = run(0.3);
      const any = run(0);
      expect(defaulted).toBe(0);
      expect(strict).toBe(0);
      // 0 means "any overlap at all counts", not "use the default 0.5".
      expect(any).toBe(1);
      expect(any).not.toBe(defaulted);
      expect(any).not.toBe(strict);
    });

    it('refuses a minOverlapRatio outside [0, 1]', () => {
      const targets = [{ startSec: 0, endSec: 1, targetMidi: 72 }];
      for (const minOverlapRatio of [-0.1, 1.1]) {
        expectSonareError(
          () => assignNoteTargets({ notes: [note(0, 1)], sampleRate, targets, minOverlapRatio }),
          ErrorCode.InvalidParameter,
        );
      }
    });

    it('takes the longest overlap, and breaks an exact tie on the earlier target', () => {
      const longer = assignNoteTargets({
        notes: [note(0, 1)],
        sampleRate,
        // 0.6 s against 0.5 s.
        targets: [
          { startSec: 0, endSec: 0.6, targetMidi: 72 },
          { startSec: 0.5, endSec: 1, targetMidi: 60 },
        ],
      }).notes[0].edit.pitchShiftSemitones;
      const shorter = assignNoteTargets({
        notes: [note(0, 1)],
        sampleRate,
        // The same pair with the lengths swapped: 0.5 s against 0.6 s.
        targets: [
          { startSec: 0, endSec: 0.5, targetMidi: 72 },
          { startSec: 0.4, endSec: 1, targetMidi: 60 },
        ],
      }).notes[0].edit.pitchShiftSemitones;
      expect(longer).toBeCloseTo(3, 4);
      expect(shorter).toBeCloseTo(-9, 4);

      // Two 0.75 s overlaps: the answer is the target that starts first, in either
      // argument order, so it does not depend on how the targets arrived.
      const early = { startSec: 0, endSec: 0.75, targetMidi: 72 };
      const late = { startSec: 0.25, endSec: 1, targetMidi: 60 };
      for (const targets of [
        [early, late],
        [late, early],
      ]) {
        expect(
          assignNoteTargets({ notes: [note(0, 1)], sampleRate, targets }).notes[0].edit
            .pitchShiftSemitones,
        ).toBeCloseTo(3, 4);
      }
      // The mirror pair, so the tie rule is shown deciding rather than the first
      // array slot winning by accident.
      const earlyLow = { startSec: 0, endSec: 0.75, targetMidi: 60 };
      const lateHigh = { startSec: 0.25, endSec: 1, targetMidi: 72 };
      expect(
        assignNoteTargets({ notes: [note(0, 1)], sampleRate, targets: [lateHigh, earlyLow] })
          .notes[0].edit.pitchShiftSemitones,
      ).toBeCloseTo(-9, 4);
    });

    it('never assigns or edits a note with no measured pitch, under any policy', () => {
      // Each of these is a spelling of "this span carries no pitch": the two
      // non-finite ones, the zero the extractor writes, and a negative value.
      const pitchless = [Number.NaN, Number.POSITIVE_INFINITY, 0, -100];
      const targets = [{ startSec: 0, endSec: 1, targetMidi: 72 }];
      for (const unmatchedPolicy of ['leave', 'mute', 'nearest'] as const) {
        const notes = pitchless.map((medianHz) => {
          const row = note(0, 1, medianHz);
          // A pending edit, so "never edited" is visible as it surviving rather
          // than as a zero that could have been written.
          row.edit.pitchShiftSemitones = 5;
          return row;
        });
        const result = assignNoteTargets({ notes, sampleRate, targets, unmatchedPolicy });
        expect(result.assignedCount).toBe(0);
        for (const row of result.notes) {
          expect(row.edit.pitchShiftSemitones).toBe(5);
          expect(row.edit.muted).toBe(false);
        }
      }
      // A note that does have a pitch is assigned in the same call, so the zero
      // above is the pitch rule and not an inert fixture.
      const mixed = assignNoteTargets({
        notes: [note(0, 1, Number.NaN), note(0, 1)],
        sampleRate,
        targets,
      });
      expect(mixed.assignedCount).toBe(1);
      expect(mixed.notes[1].edit.pitchShiftSemitones).toBeCloseTo(3, 4);
    });

    it('refuses a non-finite target instead of letting it reach the edit', () => {
      const notes = [note(0, 1)];
      const bad = [
        { startSec: Number.NaN, endSec: 1, targetMidi: 72 },
        { startSec: 0, endSec: Number.POSITIVE_INFINITY, targetMidi: 72 },
        { startSec: 0, endSec: 1, targetMidi: Number.NaN },
        // Finite, but past what a 32-bit float holds: reading it raw would make it
        // an infinity the correction clamp reports as the bound.
        { startSec: 0, endSec: 1, targetMidi: 1e300 },
      ];
      for (const target of bad) {
        expectSonareError(
          () => assignNoteTargets({ notes, sampleRate, targets: [target] }),
          ErrorCode.InvalidParameter,
        );
      }
      // The nearest policy reaches a target through a second path, so it is
      // refused there too rather than only on the overlap comparison.
      expectSonareError(
        () =>
          assignNoteTargets({
            notes: [note(5, 6)],
            sampleRate,
            targets: [{ startSec: Number.NaN, endSec: 1, targetMidi: 72 }],
            unmatchedPolicy: 'nearest',
          }),
        ErrorCode.InvalidParameter,
      );
    });

    it('refuses a non-positive sample rate', () => {
      for (const rate of [0, -48000]) {
        expectSonareError(
          () =>
            assignNoteTargets({
              notes: [note(0, 1)],
              sampleRate: rate,
              targets: [{ startSec: 0, endSec: 1, targetMidi: 72 }],
            }),
          ErrorCode.InvalidParameter,
        );
      }
    });

    it('leaves the notes it was handed untouched and returns fresh objects', () => {
      const notes = threeNotes();
      const before = JSON.stringify(notes, (_key, value) =>
        value instanceof Float32Array ? Array.from(value) : value,
      );
      const result = assignNoteTargets({
        notes,
        sampleRate,
        targets: [{ startSec: 0, endSec: 1, targetMidi: 72 }],
        unmatchedPolicy: 'mute',
      });
      const after = JSON.stringify(notes, (_key, value) =>
        value instanceof Float32Array ? Array.from(value) : value,
      );
      expect(after).toBe(before);
      expect(result.notes[0]).not.toBe(notes[0]);
      expect(result.notes[0].edit).not.toBe(notes[0].edit);
      // The arrays too, or a host editing the result in place would reach back
      // into the notes it handed in.
      expect(result.notes[0].amplitude).not.toBe(notes[0].amplitude);
      expect(result.notes[0].edit.amplitudeEnvelope).not.toBe(notes[0].edit.amplitudeEnvelope);
      result.notes[0].amplitude[0] = 99;
      expect(notes[0].amplitude[0]).toBe(0.25);
      // The written fields did move, so the equality above is preservation and
      // not a call that did nothing.
      expect(result.notes[0].edit.pitchShiftSemitones).toBeCloseTo(3, 4);
      expect(result.notes[1].edit.muted).toBe(true);
    });

    it('returns every other field of a note exactly as it went in', () => {
      const notes = threeNotes();
      notes[0].edit.gainDb = -3;
      notes[0].edit.timeStretchRatio = 1.25;
      notes[0].edit.formantShiftSemitones = 2;
      notes[0].edit.timeOffsetSamples = 512;
      notes[0].edit.amplitudeEnvelope = new Float32Array([0.5, 1]);
      const result = assignNoteTargets({
        notes,
        sampleRate,
        targets: [{ startSec: 0, endSec: 1, targetMidi: 72 }],
      });
      const source = notes[0];
      const edited = result.notes[0];
      expect(edited.onsetSample).toBe(source.onsetSample);
      expect(edited.offsetSample).toBe(source.offsetSample);
      expect(edited.frameStart).toBe(source.frameStart);
      expect(edited.frameEnd).toBe(source.frameEnd);
      expect(edited.medianHz).toBe(source.medianHz);
      expect(edited.medianCents).toBe(source.medianCents);
      expect(edited.f0Stability).toBe(source.f0Stability);
      expect(Array.from(edited.amplitude)).toEqual(Array.from(source.amplitude));
      expect(edited.edit.gainDb).toBe(-3);
      expect(edited.edit.timeStretchRatio).toBe(1.25);
      expect(edited.edit.formantShiftSemitones).toBe(2);
      expect(edited.edit.timeOffsetSamples).toBe(512);
      expect(Array.from(edited.edit.amplitudeEnvelope)).toEqual([0.5, 1]);
      // Only the two fields the rule owns moved.
      expect(edited.edit.pitchShiftSemitones).toBeCloseTo(3, 4);
    });

    it('assigns a target to notes that came out of extractNotes', () => {
      const rate = 16000;
      const frameRate = 100;
      const frames = 40;
      const samples = new Float32Array(frames * (rate / frameRate));
      for (let i = 0; i < samples.length; i++) {
        samples[i] = 0.5 * Math.sin((2 * Math.PI * a4Hz * i) / rate);
      }
      const f0Hz = new Float32Array(frames).fill(a4Hz);
      const notes = extractNotes({
        samples,
        sampleRate: rate,
        f0Hz,
        voiced: new Int32Array(frames).fill(1),
        frameRate,
      });
      expect(notes.length).toBeGreaterThan(0);
      const durationSec = samples.length / rate;
      const result = assignNoteTargets({
        notes,
        sampleRate: rate,
        targets: [{ startSec: 0, endSec: durationSec, targetMidi: 72 }],
      });
      expect(result.assignedCount).toBe(notes.length);
      expect(result.notes[0].edit.pitchShiftSemitones).toBeCloseTo(3, 1);
      // A measured note carries an amplitude curve, and it comes back untouched.
      expect(Array.from(result.notes[0].amplitude)).toEqual(Array.from(notes[0].amplitude));
    });
  });

  describe('noteTargetsFromSmf', () => {
    /** 120 BPM, so one quarter note is half a second and a PPQ span halves. */
    function melodySmf(): Uint8Array {
      const project = new Project();
      try {
        project.setSampleRate(sampleRate);
        project.setTempoSegments([{ startPpq: 0, bpm: 120 }]);
        const { clipId } = project.addMidiClip(0, 8);
        project.setMidiEvents(clipId, [
          Project.midiNoteOn(0, 0, 0, 72, 100),
          Project.midiNoteOff(2, 0, 0, 72, 0),
          Project.midiNoteOn(2, 0, 0, 74, 100),
          Project.midiNoteOff(4, 0, 0, 74, 0),
        ]);
        return project.exportSmf();
      } finally {
        project.delete();
      }
    }

    it('reads the melody at track 0, because the conductor track carries no MIDI', () => {
      const targets = noteTargetsFromSmf({ data: melodySmf() });
      expect(targets).toHaveLength(2);
      expect(targets[0].targetMidi).toBeCloseTo(72, 4);
      expect(targets[1].targetMidi).toBeCloseTo(74, 4);
      // PPQ 0..2 and 2..4 at 120 BPM.
      expect(targets[0].startSec).toBeCloseTo(0, 3);
      expect(targets[0].endSec).toBeCloseTo(1, 3);
      expect(targets[1].startSec).toBeCloseTo(1, 3);
      expect(targets[1].endSec).toBeCloseTo(2, 3);
      // An explicit 0 reaches the same track as the default.
      expect(noteTargetsFromSmf({ data: melodySmf(), trackIndex: 0 })).toEqual(targets);
      // A caller counting the file's own tracks would land here and be one off.
      expectSonareError(
        () => noteTargetsFromSmf({ data: melodySmf(), trackIndex: 1 }),
        ErrorCode.InvalidParameter,
      );
    });

    it('feeds assignNoteTargets directly', () => {
      const targets = noteTargetsFromSmf({ data: melodySmf() });
      const result = assignNoteTargets({
        notes: [note(0, 1), note(1, 2)],
        sampleRate,
        targets,
      });
      expect(result.assignedCount).toBe(2);
      expect(result.notes[0].edit.pitchShiftSemitones).toBeCloseTo(3, 4);
      expect(result.notes[1].edit.pitchShiftSemitones).toBeCloseTo(5, 4);
    });

    it('refuses bytes that are not a readable SMF', () => {
      expectSonareError(
        () => noteTargetsFromSmf({ data: new Uint8Array([1, 2, 3, 4]) }),
        ErrorCode.InvalidFormat,
      );
    });
  });
});
