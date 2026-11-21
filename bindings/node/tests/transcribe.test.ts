import { describe, expect, it } from 'vitest';
import type { TranscribeRequest, TranscribeResult } from '../src/index.js';
import { midiNoteOff, midiNoteOn, Project, transcribe } from '../src/index.js';

const SR = 22050;

/** MIDI notes the fixture below is built from: C4, E4, G4. */
const C4 = 60;
const E4 = 64;
const G4 = 67;
const A1 = 33;
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

/** A single sustained note, with the same raised-cosine edges as the core fixture. */
function sustained(hz: number, seconds = 0.5): Float32Array {
  const sampleCount = Math.round(SR * seconds);
  const edgeSamples = 0.005 * SR;
  const out = new Float32Array(sampleCount);
  for (let i = 0; i < sampleCount; i++) {
    const edge = Math.min(1, i / edgeSamples, (sampleCount - 1 - i) / edgeSamples);
    const envelope = 0.5 - 0.5 * Math.cos(2 * Math.PI * 0.5 * edge);
    out[i] = 0.5 * envelope * Math.sin((2 * Math.PI * hz * i) / SR);
  }
  return out;
}

/** The UMP MIDI-1.0 channel-voice word, unpacked. */
function decode(event: { ppq: number; word0: number; word1?: number }) {
  return {
    ppq: event.ppq,
    group: (event.word0 >>> 24) & 0xf,
    status: (event.word0 >>> 20) & 0xf,
    channel: (event.word0 >>> 16) & 0xf,
    note: (event.word0 >>> 8) & 0x7f,
    velocity: event.word0 & 0x7f,
  };
}

const NOTE_ON = 0x9;
const NOTE_OFF = 0x8;

const noteNumbers = (result: TranscribeResult): number[] =>
  result.events.filter((e) => decode(e).status === NOTE_ON).map((e) => decode(e).note);

const audio = line();

function withProject<T>(body: (project: Project, clipId: number) => T): T {
  const project = new Project();
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
    // A written tempo <= 0 is refused; omitting the field is how to ask for detection.
    for (const tempoBpm of [0, -1]) {
      expect(() => transcribe({ samples: audio, sampleRate: SR, tempoBpm })).toThrow(
        expect.objectContaining({
          name: 'RangeError',
          message: 'transcribe: tempoBpm must be finite and greater than 0',
        }),
      );
    }
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
      expect(fast[i]?.word0).toBe(slow[i]?.word0);
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

  it('resolves omitted tracker bounds from the selected source', () => {
    const lowNote = sustained(55);
    const polyphonic = transcribe({
      samples: lowNote,
      sampleRate: SR,
      polyphonic: true,
      tempoBpm: 120,
    });
    expect(noteNumbers(polyphonic)).toContain(A1);

    const explicitPolyphonic = transcribe({
      samples: lowNote,
      sampleRate: SR,
      polyphonic: true,
      f0MinHz: 55,
      f0MaxHz: 1760,
      tempoBpm: 120,
    });
    expect(polyphonic).toEqual(explicitPolyphonic);
  });

  it('resolves a partial tracker range against the selected source', () => {
    // Mono uses its 2093 Hz upper default, so this is a valid high lower bound
    // even though the fixture contains no note in that range. Polyphonic uses
    // the salience tracker's 1760 Hz upper default and must refuse the same
    // partial request after resolving it; the facade cannot see the resolved
    // bound, so the refusal is the addon's.
    expect(() =>
      transcribe({ samples: audio, sampleRate: SR, f0MinHz: 1800, tempoBpm: 120 }),
    ).not.toThrow();
    expect(() =>
      transcribe({
        samples: audio,
        sampleRate: SR,
        polyphonic: true,
        f0MinHz: 1800,
        tempoBpm: 120,
      }),
    ).toThrow(new RangeError('transcribe: f0MaxHz must be finite and greater than f0MinHz'));
  });

  it('refuses a zero the C ABI would have read as its default', () => {
    // The C ABI spells "keep the default" as 0, and cannot tell that from a
    // caller asking for 0. This surface can: omission already means the
    // default, so a written 0 is out of domain and returning the default for it
    // would be an answer measured against something nobody asked for.
    const zeroed = (extra: Record<string, number>) => () =>
      transcribe({ samples: audio, sampleRate: SR, ...extra });

    expect(zeroed({ velocityFloorDb: 0 })).toThrow(
      /velocityFloorDb must be finite and less than 0/,
    );
    expect(zeroed({ referenceHz: 0 })).toThrow(/referenceHz must be finite and greater than 0/);
    expect(zeroed({ f0MinHz: 0 })).toThrow(/f0MinHz must be finite and greater than 0/);
    expect(zeroed({ f0MaxHz: 0 })).toThrow(/f0MaxHz must be finite and greater than 0/);
    expect(zeroed({ minNoteMs: 0 })).toThrow(/minNoteMs must be finite and greater than 0/);
    expect(zeroed({ segmentationThresholdCents: 0 })).toThrow(
      /segmentationThresholdCents must be finite and greater than 0/,
    );
    // 0 is not a MIDI velocity either, so it is refused by its own domain rather
    // than read as the "measure it" the omitted key asks for.
    expect(zeroed({ fixedVelocity: 0 })).toThrow(/fixedVelocity must be an integer in \[1, 127\]/);
    // The refusal reports the same class the C ABI gives the same field one step
    // further out, so two adjacent out-of-domain values are not two error kinds.
    expect(zeroed({ velocityFloorDb: 0 })).toThrow(RangeError);
    expect(zeroed({ fixedVelocity: 0 })).toThrow(RangeError);

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
    expect(bad({ fixedVelocity: 'loud' })).toThrow(/fixedVelocity must be an integer$/);
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
    expect(() =>
      transcribe({ samples: audio, sampleRate: SR, f0MinHz: 2000, f0MaxHz: 100 }),
    ).toThrow();
    expect(() => transcribe({ samples: audio, sampleRate: SR, group: 16 })).toThrow();
    expect(() => transcribe({ samples: audio, sampleRate: SR, channel: 16 })).toThrow();
  });

  it('surfaces the C ABI’s own field-naming refusals', () => {
    // These carry no facade guard on purpose — one rule in one place.
    // The C ABI records which field it refused, and the addon's error path
    // prefers that recorded detail over the generic message for the code, so a
    // caller is told what to fix rather than "Invalid parameter". Pinned here
    // because nothing else fails if that preference is lost: the call still
    // throws, with the same code, carrying a message that names nothing.
    expect(() =>
      transcribe({ samples: audio, sampleRate: SR, f0MinHz: 900, f0MaxHz: 200 }),
    ).toThrow(/^transcribe: f0MaxHz must be finite and greater than f0MinHz$/);
    expect(() => transcribe({ samples: audio, sampleRate: SR, group: 99 })).toThrow(
      /^transcribe: group must be an integer in \[0, 15\]$/,
    );
    expect(() => transcribe({ samples: audio, sampleRate: SR, channel: 99 })).toThrow(
      /^transcribe: channel must be an integer in \[0, 15\]$/,
    );

    // The detail is a last-error slot, so a message that merely LOOKS right may
    // be the previous call's. Fail something else first and check the answer
    // moved: without this, a slot that had stopped being written would still
    // read as correct for whichever failure ran before it.
    expect(() => transcribe({ samples: audio, sampleRate: SR, channel: 99 })).toThrow(
      /^transcribe: channel /,
    );
    expect(() => transcribe({ samples: audio, sampleRate: SR, group: 99 })).toThrow(
      /^transcribe: group /,
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
    // A bad clip id is the other failure, with a text of its own, and it crosses entry points on
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
      const annotated = () => transcribe({ samples: audio, sampleRate: SR, channel: 99 });
      const unannotated = () =>
        project.transcribeToClip({ clipId: clipId + 5000, samples: audio, sampleRate: SR });

      expect(detail(annotated)).toBe('transcribe: channel must be an integer in [0, 15]');
      expect(detail(unannotated)).toBe(
        'Project.transcribeToClip: clipId must be the id of a MIDI clip in this project',
      );
      expect(detail(() => transcribe({ samples: audio, sampleRate: SR, group: 99 }))).toBe(
        'transcribe: group must be an integer in [0, 15]',
      );
      expect(detail(unannotated)).toBe(
        'Project.transcribeToClip: clipId must be the id of a MIDI clip in this project',
      );

      // Reversed, so the generic answer above cannot be "it is generic whatever
      // ran before it, including nothing".
      expect(detail(unannotated)).toBe(
        'Project.transcribeToClip: clipId must be the id of a MIDI clip in this project',
      );
      expect(detail(annotated)).toBe('transcribe: channel must be an integer in [0, 15]');

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
      midiContent: Record<string, Array<{ ppq: number; word0: number; word1: number }>>;
    };
    return (payload.midiContent[String(clipId)] ?? []).map(decode);
  }

  it('accepts minNoteDivision and refuses it beside minNoteMs', () => {
    withProject((project, clipId) => {
      expect(
        project.transcribeToClip({ clipId, samples: audio, sampleRate: SR, minNoteDivision: 32 }),
      ).toBe(3);
      expect(() =>
        project.transcribeToClip({
          clipId,
          samples: audio,
          sampleRate: SR,
          minNoteDivision: 32,
          minNoteMs: 50,
        }),
      ).toThrow(
        new RangeError(
          'Project.transcribeToClip: minNoteDivision and minNoteMs cannot both be set',
        ),
      );
      expect(() =>
        project.transcribeToClip({ clipId, samples: audio, sampleRate: SR, minNoteDivision: 0 }),
      ).toThrow(/minNoteDivision/);
    });
  });

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
        midiNoteOn({ ppq: 0, group: 0, channel: 0, note: 40, velocity: 100 }),
        midiNoteOff({ ppq: 1, group: 0, channel: 0, note: 40, velocity: 0 }),
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
      ).toThrow(/velocityFloorDb must be finite and less than 0/);
      expect(() =>
        project.transcribeToClip({ clipId, samples: audio, sampleRate: SR, minNoteMs: 0 }),
      ).toThrow(/minNoteMs must be finite and greater than 0/);
      // The C ABI's field-naming refusals reach this door too, not only the
      // free function's.
      expect(() =>
        project.transcribeToClip({ clipId, samples: audio, sampleRate: SR, group: 99 }),
      ).toThrow(/^Project\.transcribeToClip: group must be an integer in \[0, 15\]$/);
      // The facade checks the F0 bounds by their public names before the addon.
      expect(() =>
        project.transcribeToClip({
          clipId,
          samples: audio,
          sampleRate: SR,
          f0MinHz: 900,
          f0MaxHz: 200,
        }),
      ).toThrow(/^Project\.transcribeToClip: f0MaxHz must be finite and greater than f0MinHz$/);
    });
  });
});

/** Sum of sines at `hzs`, held for `seconds` with 5 ms edges. */
function chordTone(hzs: number[], seconds = 0.8): Float32Array {
  const out = new Float32Array(Math.round(SR * seconds));
  const edge = 0.005 * SR;
  for (let i = 0; i < out.length; i++) {
    const envelope = Math.min(1, i / edge, (out.length - 1 - i) / edge);
    let sum = 0;
    for (const hz of hzs) {
      sum += Math.sin((2 * Math.PI * hz * i) / SR);
    }
    out[i] = (0.4 / hzs.length) * envelope * sum;
  }
  return out;
}

/** One pitch struck twice: the second strike ramps in over `attackMs`. */
function restruck(hz = 440, attackMs = 20): Float32Array {
  const strike = Math.round(SR * 0.5);
  const out = new Float32Array(strike * 2);
  const ramp = Math.round((attackMs / 1000) * SR);
  for (let i = 0; i < out.length; i++) {
    const local = i % strike;
    const rise = i < strike ? Math.min(1, i / 110) : Math.min(1, local / ramp);
    out[i] = 0.5 * rise * Math.exp(-2 * (local / strike)) * Math.sin((2 * Math.PI * hz * i) / SR);
  }
  return out;
}

function maxSimultaneous(result: TranscribeResult): number {
  let active = 0;
  let peakActive = 0;
  for (const event of result.events) {
    const status = decode(event).status;
    if (status === NOTE_ON) {
      active++;
    }
    if (status === NOTE_OFF) {
      active--;
    }
    peakActive = Math.max(peakActive, active);
  }
  return peakActive;
}

describe('transcribe polyphonic tuning', () => {
  const chord = chordTone([261.626, 329.628, 391.995, 523.251]);
  const poly = (extra: Record<string, number> = {}) =>
    transcribe({ samples: chord, sampleRate: SR, tempoBpm: 120, polyphonic: true, ...extra });

  it('maxPolyphony caps the simultaneous notes', () => {
    expect(maxSimultaneous(poly())).toBeGreaterThan(1);
    expect(maxSimultaneous(poly({ maxPolyphony: 1 }))).toBe(1);
  });

  it('minFramePeakRatio and minRidgePeakRatio reach the core', () => {
    const base = poly();
    // A frame floor of 1 keeps only the strongest voice per frame.
    expect(noteNumbers(poly({ minFramePeakRatio: 1 })).length).toBeLessThan(
      noteNumbers(base).length,
    );
    // A ridge floor of 1 ends a ridge at its first frame below its own peak.
    expect(poly({ minRidgePeakRatio: 1 }).events).not.toEqual(base.events);
  });

  it('an absent reattackRatio does not split a re-struck tone, a given one does', () => {
    const tone = restruck();
    const run = (extra: Record<string, number> = {}) =>
      transcribe({ samples: tone, sampleRate: SR, tempoBpm: 120, polyphonic: true, ...extra });
    const whole = run();
    const split = run({ reattackRatio: 1.5 });
    expect(split.noteCount).toBeGreaterThan(whole.noteCount);
    expect(whole.noteCount).toBe(1);
  });

  it('accepts a written 0 for the two peak ratios as a real zero', () => {
    expect(() => poly({ minFramePeakRatio: 0 })).not.toThrow();
    expect(() => poly({ minRidgePeakRatio: 0 })).not.toThrow();
    expect(poly({ minFramePeakRatio: 0 }).events).not.toEqual(poly().events);
  });

  it('refuses a written value outside the domain', () => {
    const bad = (extra: Record<string, number>) => () => poly(extra);
    expect(bad({ maxPolyphony: 0 })).toThrow(/maxPolyphony must be an integer in \[1, 64\]/);
    expect(bad({ maxPolyphony: 65 })).toThrow(/maxPolyphony must be an integer in \[1, 64\]/);
    expect(bad({ minFramePeakRatio: -0.1 })).toThrow(/minFramePeakRatio must be finite and in \[0, 1\]/);
    expect(bad({ minFramePeakRatio: 1.5 })).toThrow(/minFramePeakRatio must be finite and in \[0, 1\]/);
    expect(bad({ minRidgePeakRatio: -1 })).toThrow(/minRidgePeakRatio must be finite and in \[0, 1\]/);
    expect(bad({ minRidgePeakRatio: 2 })).toThrow(/minRidgePeakRatio must be finite and in \[0, 1\]/);
    expect(bad({ reattackRatio: 1 })).toThrow(/reattackRatio must be finite and greater than 1/);
    expect(bad({ reattackRatio: 0.5 })).toThrow(/reattackRatio must be finite and greater than 1/);
    expect(bad({ reattackRatio: -1 })).toThrow(/reattackRatio must be finite and greater than 1/);
  });

  it('surfaces the C refusal of a polyphonic field on the monophonic tracker', () => {
    expect(() =>
      transcribe({ samples: audio, sampleRate: SR, tempoBpm: 120, maxPolyphony: 3 }),
    ).toThrow(new RangeError('transcribe: maxPolyphony requires polyphonic transcription'));
  });
});

describe('transcribe minNoteDivision', () => {
  const run = (extra: Record<string, unknown>, samples = audio) =>
    transcribe({ samples, sampleRate: SR, tempoBpm: 120, ...extra });

  it('a 1/16 note at 120 bpm equals minNoteMs 125, for both trackers', () => {
    for (const polyphonic of [false, true]) {
      expect(run({ polyphonic, minNoteDivision: 16 }).events).toEqual(
        run({ polyphonic, minNoteMs: 125 }).events,
      );
    }
  });

  it('a coarser division drops a note a finer one keeps', () => {
    const fine = run({ minNoteDivision: 128 }).noteCount;
    const coarse = run({ minNoteDivision: 1 }).noteCount;
    expect(fine).toBeGreaterThan(0);
    expect(coarse).toBeLessThan(fine);
  });

  it('refuses a value outside the domain, naming the field', () => {
    for (const value of [0, -1, 129, 1.5]) {
      expect(() => run({ minNoteDivision: value })).toThrow(/minNoteDivision/);
    }
  });

  it('refuses a wrong type as a TypeError naming the field', () => {
    expect(() => run({ minNoteDivision: '16' })).toThrow(TypeError);
    expect(() => run({ minNoteDivision: '16' })).toThrow(/minNoteDivision/);
  });

  it('surfaces the C refusal of both spellings together', () => {
    expect(() => run({ minNoteDivision: 16, minNoteMs: 100 })).toThrow(
      new RangeError('transcribe: minNoteDivision and minNoteMs cannot both be set'),
    );
  });
});

/** A harmonic tone at `hz` with sinusoidal vibrato of `rateHz` and `depthCents`. */
function vibrato(hz: number, seconds: number, rateHz: number, depthCents: number): Float32Array {
  const out = new Float32Array(Math.round(SR * seconds));
  const edge = 0.01 * SR;
  let phase = 0;
  for (let i = 0; i < out.length; i++) {
    const t = i / SR;
    const f = hz * 2 ** ((depthCents / 1200) * Math.sin(2 * Math.PI * rateHz * t));
    phase += (2 * Math.PI * f) / SR;
    const envelope = Math.min(1, i / edge, (out.length - 1 - i) / edge);
    out[i] =
      0.4 * envelope * (Math.sin(phase) + 0.5 * Math.sin(2 * phase) + 0.25 * Math.sin(3 * phase));
  }
  return out;
}

describe('transcribe monophonicModel', () => {
  const sung = vibrato(391.995, 2, 6, 75);
  const run = (extra: Record<string, unknown> = {}) =>
    transcribe({ samples: sung, sampleRate: SR, tempoBpm: 120, ...extra } as TranscribeRequest);

  it("'singing' holds vibrato inside one note and differs from the default", () => {
    const singing = run({ monophonicModel: 'singing' });
    expect(singing.noteCount).toBe(1);
    expect(noteNumbers(singing)).toEqual([G4]);
    expect(singing.events).not.toEqual(run().events);
  });

  it("an explicit 'segments' equals an omitted field", () => {
    expect(run({ monophonicModel: 'segments' }).events).toEqual(run().events);
  });

  it('refuses an unknown name as a RangeError and a non-string as a TypeError', () => {
    expect(() => run({ monophonicModel: 'choir' })).toThrow(RangeError);
    expect(() => run({ monophonicModel: 'choir' })).toThrow(/monophonicModel/);
    expect(() => run({ monophonicModel: 1 })).toThrow(TypeError);
    expect(() => run({ monophonicModel: 1 })).toThrow(/monophonicModel/);
  });

  it("refuses 'singing' on the polyphonic source", () => {
    expect(() => run({ polyphonic: true, monophonicModel: 'singing' })).toThrow(RangeError);
  });
});
