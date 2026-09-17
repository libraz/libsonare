import { describe, expect, it } from 'vitest';
import type { TranscribeRequest, TranscribeResult } from '../src/index.js';
import { Project, transcribe } from '../src/index.js';

const SR = 22050;

/** MIDI notes the fixture below is built from: C4, E4, G4. */
const C4 = 60;
const E4 = 64;
const G4 = 67;
const NOTE_HZ = [261.626, 329.628, 391.995];

/**
 * A monophonic line of `NOTE_HZ`, each note 0.4 s with a short silence after it.
 * The gap is what separates the notes: an unvoiced run ends one span and starts
 * the next, so the transcription has three notes to find rather than one glide.
 */
function line(seconds = 0.4, gapSeconds = 0.05): Float32Array {
  const per = Math.round(SR * seconds);
  const gap = Math.round(SR * gapSeconds);
  const out = new Float32Array((per + gap) * NOTE_HZ.length);
  let write = 0;
  for (const hz of NOTE_HZ) {
    for (let i = 0; i < per; i++) {
      // A few-ms ramp at each end, so the note starts and stops without a click
      // the onset side would read as a second event.
      const envelope = Math.min(1, i / 200, (per - i) / 200);
      out[write++] = 0.5 * envelope * Math.sin((2 * Math.PI * hz * i) / SR);
    }
    write += gap;
  }
  return out;
}

/** The UMP MIDI-1.0 channel-voice word, unpacked. */
function decode(event: { ppq: number; data0: number; data1?: number }) {
  return {
    ppq: event.ppq,
    group: (event.data0 >>> 24) & 0xf,
    status: (event.data0 >>> 20) & 0xf,
    channel: (event.data0 >>> 16) & 0xf,
    note: (event.data0 >>> 8) & 0x7f,
    velocity: event.data0 & 0x7f,
  };
}

const NOTE_ON = 0x9;
const NOTE_OFF = 0x8;

const noteNumbers = (result: TranscribeResult): number[] =>
  result.events.filter((e) => decode(e).status === NOTE_ON).map((e) => decode(e).note);

const audio = line();

function withProject<T>(body: (project: Project, clipId: number) => T): T {
  const project = Project.create();
  try {
    project.setSampleRate(SR);
    const { clipId } = project.addMidiClip(0, 8);
    return body(project, clipId);
  } finally {
    project.destroy();
  }
}

describe('transcribe', () => {
  it('finds the notes that were played, in order', () => {
    const result = transcribe({ samples: audio, sampleRate: SR, tempoBpm: 120 });

    expect(noteNumbers(result)).toEqual([C4, E4, G4]);
    expect(result.noteCount).toBe(3);
    expect(result.events).toHaveLength(2 * result.noteCount);
  });

  it('emits note-on/off pairs in canonical order', () => {
    const events = transcribe({ samples: audio, sampleRate: SR, tempoBpm: 120 }).events.map(decode);

    for (let i = 1; i < events.length; i++) {
      const previous = events[i - 1] as (typeof events)[number];
      const current = events[i] as (typeof events)[number];
      expect(current.ppq).toBeGreaterThanOrEqual(previous.ppq);
      // At a shared tick the note-off has to come first, or a consumer playing
      // the list in order starts the new note and immediately stops it.
      if (current.ppq === previous.ppq) {
        expect(previous.status).toBe(NOTE_OFF);
        expect(current.status).toBe(NOTE_ON);
      }
    }
    // Not vacuous: this fixture is legato at the note boundaries, so there are
    // shared ticks for the ordering rule to apply to.
    expect(events.filter((e, i) => i > 0 && e.ppq === events[i - 1]?.ppq).length).toBeGreaterThan(
      0,
    );
  });

  it('echoes a supplied tempo and detects a usable one otherwise', () => {
    expect(transcribe({ samples: audio, sampleRate: SR, tempoBpm: 132 }).tempoBpm).toBeCloseTo(132);

    const detected = transcribe({ samples: audio, sampleRate: SR }).tempoBpm;
    expect(Number.isFinite(detected)).toBe(true);
    expect(detected).toBeGreaterThan(0);
    // A tempo <= 0 is the documented "detect it", so it must land on the same
    // answer omitting the field does rather than on a zero-tempo grid.
    expect(transcribe({ samples: audio, sampleRate: SR, tempoBpm: 0 }).tempoBpm).toBe(detected);
  });

  it('scales the ppq grid with the supplied tempo', () => {
    // PPQ counts quarter notes, so ppq = seconds * bpm / 60: the same audio at
    // twice the tempo spans twice as many quarter notes.
    const slow = transcribe({ samples: audio, sampleRate: SR, tempoBpm: 120 }).events;
    const fast = transcribe({ samples: audio, sampleRate: SR, tempoBpm: 240 }).events;

    expect(fast).toHaveLength(slow.length);
    expect(slow).toHaveLength(6);
    for (let i = 0; i < slow.length; i++) {
      expect(fast[i]?.ppq).toBeCloseTo((slow[i]?.ppq ?? Number.NaN) * 2, 6);
      // Only the time axis may move: the packed word carries the note number,
      // the velocity, the status, the group and the channel, so comparing it
      // says every one of them survived the tempo change unchanged.
      expect(fast[i]?.data0).toBe(slow[i]?.data0);
      expect(decode(fast[i] as (typeof fast)[number]).note).toBe(
        decode(slow[i] as (typeof slow)[number]).note,
      );
      expect(decode(fast[i] as (typeof fast)[number]).velocity).toBe(
        decode(slow[i] as (typeof slow)[number]).velocity,
      );
    }
    // The ratio is trivially satisfied by a grid that is 0 everywhere, and the
    // first onset IS 0 in both. The last event is what the comparison rests on.
    expect(slow.at(-1)?.ppq).toBeGreaterThan(0);
  });

  it('measures the note numbers against referenceHz', () => {
    // A reference a semitone sharp puts every note one semitone lower, which is
    // the assertion a field read and then ignored cannot pass.
    const sharp = transcribe({
      samples: audio,
      sampleRate: SR,
      tempoBpm: 120,
      referenceHz: 440 * 2 ** (1 / 12),
    });

    expect(noteNumbers(sharp)).toEqual([C4 - 1, E4 - 1, G4 - 1]);
  });

  it('gives every note the fixed velocity when one is set', () => {
    const result = transcribe({
      samples: audio,
      sampleRate: SR,
      tempoBpm: 120,
      fixedVelocity: 100,
    });
    const events = result.events.map(decode);

    expect(events.filter((e) => e.status === NOTE_ON).map((e) => e.velocity)).toEqual([
      100, 100, 100,
    ]);
    // A MIDI 1.0 note-off is velocity 0; the fixed velocity must not reach it.
    expect(events.filter((e) => e.status === NOTE_OFF).map((e) => e.velocity)).toEqual([0, 0, 0]);
    // The measured path answers something else, so the fixed one is not simply
    // what this audio would have produced anyway.
    const measured = transcribe({ samples: audio, sampleRate: SR, tempoBpm: 120 }).events.map(
      decode,
    );
    expect(measured.filter((e) => e.status === NOTE_ON).map((e) => e.velocity)).not.toContain(100);
  });

  it('emits on the requested group and channel', () => {
    const result = transcribe({
      samples: audio,
      sampleRate: SR,
      tempoBpm: 120,
      group: 2,
      channel: 5,
    });

    for (const event of result.events.map(decode)) {
      expect(event.group).toBe(2);
      expect(event.channel).toBe(5);
    }
  });

  it('maps velocity from the level floor', () => {
    const wide = transcribe({ samples: audio, sampleRate: SR, velocityFloorDb: -48 });
    const narrow = transcribe({ samples: audio, sampleRate: SR, velocityFloorDb: -12 });

    // A floor closer to 0 dBFS spreads the same levels over a lower velocity
    // range, so the two cannot be the same answer.
    const velocities = (r: TranscribeResult) =>
      r.events
        .map(decode)
        .filter((e) => e.status === NOTE_ON)
        .map((e) => e.velocity);
    expect(velocities(narrow)).not.toEqual(velocities(wide));
    for (const velocity of velocities(narrow)) {
      expect(velocity).toBeLessThan(velocities(wide)[0] as number);
    }
    // Omitting the key is what asks for the default, so it must land on -48.
    expect(transcribe({ samples: audio, sampleRate: SR })).toEqual(wide);
  });

  it('refuses a zero the C ABI would have read as its default', () => {
    // The C ABI spells "keep the default" as 0, and cannot tell that from a
    // caller asking for 0. This surface can: omission already means the
    // default, so a written 0 is out of domain and returning the default for it
    // would be an answer measured against something nobody asked for.
    const zeroed = (extra: Record<string, number>) => () =>
      transcribe({ samples: audio, sampleRate: SR, ...extra });

    expect(zeroed({ velocityFloorDb: 0 })).toThrow(/velocityFloorDb must be negative/);
    expect(zeroed({ referenceHz: 0 })).toThrow(/referenceHz must be a positive number/);
    expect(zeroed({ fmin: 0 })).toThrow(/fmin must be a positive number/);
    expect(zeroed({ fmax: 0 })).toThrow(/fmax must be a positive number/);
    expect(zeroed({ minNoteMs: 0 })).toThrow(/minNoteMs must be a positive number/);
    expect(zeroed({ segmentationThresholdCents: 0 })).toThrow(
      /segmentationThresholdCents must be a positive number/,
    );
    // 0 is not a MIDI velocity either, so it is refused by its own domain rather
    // than read as the "measure it" the omitted key asks for.
    expect(zeroed({ fixedVelocity: 0 })).toThrow(/fixedVelocity must be an integer in \[1, 127\]/);
    // The refusal reports the same class the C ABI gives the same field one step
    // further out, so two adjacent out-of-domain values are not two error kinds.
    expect(zeroed({ velocityFloorDb: 0 })).toThrow(
      expect.objectContaining({ name: 'SonareError', codeName: 'InvalidParameter' }),
    );
    expect(zeroed({ fixedVelocity: 0 })).toThrow(
      expect.objectContaining({ name: 'SonareError', codeName: 'InvalidParameter' }),
    );

    // The two fields whose 0 IS a value a caller can mean stay accepted, and
    // that is the distinction deciding the whole set.
    expect(zeroed({ group: 0 })).not.toThrow();
    expect(zeroed({ channel: 0 })).not.toThrow();
    // Both ends of the velocity domain stay reachable, so the refusal above is a
    // bound rather than the field having stopped working.
    expect(zeroed({ fixedVelocity: 1 })).not.toThrow();
    expect(zeroed({ fixedVelocity: 127 })).not.toThrow();
  });

  it('answers silence with no notes rather than an error', () => {
    const result = transcribe({ samples: new Float32Array(SR), sampleRate: SR, tempoBpm: 100 });

    expect(result.events).toEqual([]);
    expect(result.noteCount).toBe(0);
    expect(result.tempoBpm).toBeCloseTo(100);
  });

  it('refuses a wrong-typed option by name instead of defaulting it', () => {
    const request = { samples: audio, sampleRate: SR };
    const bad = (extra: Record<string, unknown>) => () =>
      transcribe({ ...request, ...extra } as unknown as TranscribeRequest);

    expect(bad({ fixedVelocity: 'loud' })).toThrow(TypeError);
    expect(bad({ fixedVelocity: 'loud' })).toThrow(/fixedVelocity must be a number/);
    expect(bad({ referenceHz: 'high' })).toThrow(/referenceHz must be a number/);
    expect(bad({ polyphonic: 'yes' })).toThrow(/polyphonic must be a boolean/);
    expect(bad({ group: {} })).toThrow(/group must be a number/);
    // A fraction is refused as a fraction rather than as a magnitude: the
    // narrowing guards the C int, the [1, 127] bound guards the library domain,
    // and a caller can only act on the mistake they made.
    expect(bad({ fixedVelocity: 64.5 })).toThrow(RangeError);
    expect(bad({ fixedVelocity: 64.5 })).toThrow(/fixedVelocity must be an integer/);
  });

  it('rejects an out-of-domain request', () => {
    expect(() => transcribe({ samples: new Float32Array(0), sampleRate: SR })).toThrow(RangeError);
    expect(() => transcribe({ samples: audio, sampleRate: 0 })).toThrow(RangeError);
    expect(() => transcribe({ samples: audio, sampleRate: SR, fixedVelocity: 128 })).toThrow(
      /fixedVelocity must be an integer in \[1, 127\]/,
    );
    expect(() => transcribe({ samples: audio, sampleRate: SR, velocityFloorDb: 6 })).toThrow();
    expect(() => transcribe({ samples: audio, sampleRate: SR, fmin: 2000, fmax: 100 })).toThrow();
    expect(() => transcribe({ samples: audio, sampleRate: SR, group: 16 })).toThrow();
    expect(() => transcribe({ samples: audio, sampleRate: SR, channel: 16 })).toThrow();
  });

  it('surfaces the C ABI’s own field-naming refusals', () => {
    // These three carry no addon-side guard on purpose — one rule in one place.
    // The C ABI records which field it refused, and the addon's error path
    // prefers that recorded detail over the generic message for the code, so a
    // caller is told what to fix rather than "Invalid parameter". Pinned here
    // because nothing else fails if that preference is lost: the call still
    // throws, with the same code, carrying a message that names nothing.
    expect(() => transcribe({ samples: audio, sampleRate: SR, fmin: 900, fmax: 200 })).toThrow(
      /^fmax must be above fmin$/,
    );
    expect(() => transcribe({ samples: audio, sampleRate: SR, group: 99 })).toThrow(
      /^group must be an integer in \[0, 15\]$/,
    );
    expect(() => transcribe({ samples: audio, sampleRate: SR, channel: 99 })).toThrow(
      /^channel must be an integer in \[0, 15\]$/,
    );

    // The detail is a last-error slot, so a message that merely LOOKS right may
    // be the previous call's. Fail something else first and check the answer
    // moved: without this, a slot that had stopped being written would still
    // read as correct for whichever failure ran before it.
    expect(() => transcribe({ samples: audio, sampleRate: SR, group: 99 })).toThrow(/^group /);
    expect(() => transcribe({ samples: audio, sampleRate: SR, fmin: 900, fmax: 200 })).toThrow(
      /^fmax must be above fmin$/,
    );
    // ...and a success after a failure is still a success, i.e. the slot is not
    // consulted when there is nothing to report.
    expect(transcribe({ samples: audio, sampleRate: SR }).noteCount).toBeGreaterThan(0);
  });

  it('clears the detail slot, so an unannotated failure inherits nothing', () => {
    // The half the moving-answer check above cannot see. That one shows the slot
    // is still being WRITTEN; it says nothing about whether a failure the C ABI
    // does not annotate comes back generic, because a slot that were always
    // populated would look identical. So the two kinds are interleaved: an
    // unannotated failure run immediately after an annotated one must not carry
    // the annotated one's text.
    //
    // A bad clip id is the unannotated failure, and it crosses entry points on
    // purpose — the slot is thread-local and shared by every C-ABI entry, which
    // is exactly where a leak would show.
    withProject((project, clipId) => {
      const detail = (body: () => unknown): string => {
        try {
          body();
          return 'NO THROW';
        } catch (error) {
          return (error as Error).message;
        }
      };
      const annotated = () => transcribe({ samples: audio, sampleRate: SR, fmin: 900, fmax: 200 });
      const unannotated = () =>
        project.transcribeToClip({ clipId: clipId + 5000, samples: audio, sampleRate: SR });

      expect(detail(annotated)).toBe('fmax must be above fmin');
      expect(detail(unannotated)).toBe('Invalid parameter');
      expect(detail(() => transcribe({ samples: audio, sampleRate: SR, group: 99 }))).toBe(
        'group must be an integer in [0, 15]',
      );
      expect(detail(unannotated)).toBe('Invalid parameter');

      // Reversed, so the generic answer above cannot be "it is generic whatever
      // ran before it, including nothing".
      expect(detail(unannotated)).toBe('Invalid parameter');
      expect(detail(annotated)).toBe('fmax must be above fmin');

      // A success after a failure is still a success: the slot is not consulted
      // when there is nothing to report.
      expect(transcribe({ samples: audio, sampleRate: SR }).noteCount).toBeGreaterThan(0);
    });
  });
});

describe('Project.transcribeToClip', () => {
  /** The events a clip holds, read back out of the project's own serialization. */
  function storedEvents(project: Project, clipId: number) {
    const payload = JSON.parse(project.toJson()) as {
      midi_content: Record<string, Array<{ ppq: number; data0: number; data1: number }>>;
    };
    return (payload.midi_content[String(clipId)] ?? []).map(decode);
  }

  it('writes the transcribed notes into the clip', () => {
    withProject((project, clipId) => {
      const noteCount = project.transcribeToClip({
        clipId,
        samples: audio,
        sampleRate: SR,
        fixedVelocity: 90,
      });

      expect(noteCount).toBe(3);
      const stored = storedEvents(project, clipId);
      expect(stored).toHaveLength(2 * noteCount);
      expect(stored.filter((e) => e.status === NOTE_ON).map((e) => e.note)).toEqual([C4, E4, G4]);
      expect(stored.filter((e) => e.status === NOTE_ON).map((e) => e.velocity)).toEqual([
        90, 90, 90,
      ]);
    });
  });

  it('replaces the clip contents rather than appending to them', () => {
    withProject((project, clipId) => {
      project.setMidiEvents(clipId, [
        Project.midiNoteOn(0, 0, 0, 40, 100),
        Project.midiNoteOff(1, 0, 0, 40, 0),
      ]);
      expect(storedEvents(project, clipId)).toHaveLength(2);

      const noteCount = project.transcribeToClip({ clipId, samples: audio, sampleRate: SR });

      expect(noteCount).toBe(3);
      const stored = storedEvents(project, clipId);
      expect(stored).toHaveLength(2 * noteCount);
      // The seeded E1 is gone, not merged in front of the transcription.
      expect(stored.map((e) => e.note)).not.toContain(40);
    });
  });

  it('places the notes on the project tempo map', () => {
    const onProject = withProject((project, clipId) => {
      project.setTempoSegments([{ startPpq: 0, bpm: 240 }]);
      project.transcribeToClip({ clipId, samples: audio, sampleRate: SR });
      return storedEvents(project, clipId).map((e) => e.ppq);
    });
    const standalone = transcribe({ samples: audio, sampleRate: SR, tempoBpm: 240 }).events.map(
      (e) => e.ppq,
    );

    // Six events rather than "the same as each other": two empty lists agree.
    expect(standalone).toHaveLength(6);
    expect(onProject).toHaveLength(standalone.length);
    for (let i = 0; i < standalone.length; i++) {
      expect(onProject[i]).toBeCloseTo(standalone[i] as number, 6);
    }
  });

  it('rejects an out-of-domain request', () => {
    withProject((project, clipId) => {
      expect(() => project.transcribeToClip({ samples: audio, sampleRate: SR } as never)).toThrow(
        /clipId must be a number/,
      );
      expect(() =>
        project.transcribeToClip({ clipId: 9999, samples: audio, sampleRate: SR }),
      ).toThrow();
      expect(() =>
        project.transcribeToClip({ clipId, samples: new Float32Array(0), sampleRate: SR }),
      ).toThrow(RangeError);
      expect(() => project.transcribeToClip({ clipId, samples: audio, sampleRate: 0 })).toThrow(
        RangeError,
      );
      // The zero-is-not-the-default refusal is the same reader's, so it must
      // reach this entry point too rather than guarding only the free function.
      expect(() =>
        project.transcribeToClip({ clipId, samples: audio, sampleRate: SR, velocityFloorDb: 0 }),
      ).toThrow(/velocityFloorDb must be negative/);
      expect(() =>
        project.transcribeToClip({ clipId, samples: audio, sampleRate: SR, minNoteMs: 0 }),
      ).toThrow(/minNoteMs must be a positive number/);
      // The C ABI's field-naming refusals reach this door too, not only the
      // free function's.
      expect(() =>
        project.transcribeToClip({ clipId, samples: audio, sampleRate: SR, fmin: 900, fmax: 200 }),
      ).toThrow(/^fmax must be above fmin$/);
    });
  });
});
