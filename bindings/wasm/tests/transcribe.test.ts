/**
 * Audio-to-MIDI transcription on the WASM surface: the standalone `transcribe`
 * and `Project.transcribeToClip`, both over the sonare_c_transcribe.h C ABI.
 *
 * The fixture is three 0.5 s tones at 44.1 kHz — C4, E4, G4 in that order, each
 * five partials at `1/h` so the tracker has a harmonic series to follow rather
 * than a bare sine. It is deliberately a sequence and not a chord: the default
 * chain is monophonic, and a positive claim about WHICH notes came back needs
 * notes that are unambiguous to the ear as well as to the detector.
 *
 * The note numbers and the reference shift below are the library's own measured
 * answers for that fixture, not values read off a run of this file after the
 * fact. The shared-tick pair comes from a separate legato fixture built for it.
 */

import { beforeAll, describe, expect, it } from 'vitest';
import type { ProjectMidiEvent, TranscribeOptions, TranscribeRequest } from '../src/index';
import { init, midiNoteOff, midiNoteOn, Project, transcribe } from '../src/index';
import { expectArgumentRefusal, unknownKeyText } from './_helpers';

const sampleRate = 44100;
const noteSeconds = 0.5;
/** C4, E4, G4. */
const toneHz = [261.63, 329.63, 392.0];
/** What the monophonic chain measures those three at. */
const expectedNotes = [60, 64, 67];

/** `harmonics` partials of `f0Hz` at `1/h`, with a 10 ms fade at each end. */
function richTone(f0Hz: number, seconds: number): Float32Array {
  const n = Math.round(sampleRate * seconds);
  const fade = Math.round(sampleRate * 0.01);
  const out = new Float32Array(n);
  for (let i = 0; i < n; i++) {
    let sum = 0;
    for (let h = 1; h <= 5; h++) {
      const partialHz = h * f0Hz;
      if (partialHz >= sampleRate / 2) {
        break;
      }
      sum += (0.3 / h) * Math.sin((2 * Math.PI * partialHz * i) / sampleRate);
    }
    const gain = i < fade ? i / fade : i > n - fade ? (n - i) / fade : 1;
    out[i] = sum * gain;
  }
  return out;
}

/** C4, E4 and G4 sounding together for one second. */
function triad(): Float32Array {
  const parts = toneHz.map((hz) => richTone(hz, 1.0));
  const out = new Float32Array(parts[0].length);
  for (const part of parts) {
    for (let i = 0; i < out.length; i++) {
      out[i] += part[i] / parts.length;
    }
  }
  return out;
}

/**
 * One pitch struck twice: each strike decays to a third of its peak over 0.4 s,
 * the first with a 5 ms attack and the second with a 20 ms ramp.
 */
function restruck(): Float32Array {
  const strikeSamples = Math.round(sampleRate * 0.4);
  const out = new Float32Array(2 * strikeSamples);
  const tone = richTone(toneHz[0], 0.8);
  for (let strike = 0; strike < 2; strike++) {
    const attack = Math.round(sampleRate * (strike === 0 ? 0.005 : 0.02));
    for (let i = 0; i < strikeSamples; i++) {
      const rise = i < attack ? i / attack : 1;
      const fall = 3 ** (-i / strikeSamples);
      out[strike * strikeSamples + i] = tone[i] * rise * fall;
    }
  }
  return out;
}

/** The most notes sounding at once, from a result's note-on / note-off order. */
function maxSimultaneous(result: ReturnType<typeof transcribe>): number {
  let open = 0;
  let most = 0;
  for (const event of result.events.map(decode)) {
    open += event.status === NOTE_ON ? 1 : -1;
    most = Math.max(most, open);
  }
  return most;
}

/**
 * C4, E4, G4 as plain sines of `noteSeconds` each, back to back with 5 ms raised-cosine edges:
 * the legato construction the C-ABI transcribe test pins. Each note's offset is the next one's
 * onset, so the two land on one tick at every input rate.
 */
function legato(): Float32Array {
  const n = Math.round(sampleRate * noteSeconds);
  const edge = 0.005 * sampleRate;
  const out = new Float32Array(n * toneHz.length);
  toneHz.forEach((hz, note) => {
    for (let i = 0; i < n; i++) {
      const ramp = Math.min(1, i / edge, (n - 1 - i) / edge);
      const envelope = 0.5 - 0.5 * Math.cos(Math.PI * ramp);
      out[note * n + i] = 0.5 * envelope * Math.sin((2 * Math.PI * hz * i) / sampleRate);
    }
  });
  return out;
}

function melody(): Float32Array {
  const parts = toneHz.map((hz) => richTone(hz, noteSeconds));
  const out = new Float32Array(parts.reduce((total, part) => total + part.length, 0));
  let offset = 0;
  for (const part of parts) {
    out.set(part, offset);
    offset += part.length;
  }
  return out;
}

interface DecodedEvent {
  ppq: number;
  /** 0x9 = note-on, 0x8 = note-off. */
  status: number;
  channel: number;
  group: number;
  note: number;
  velocity: number;
}

/** Unpacks a UMP MIDI-1.0 channel-voice word (message type 0x2). */
function decode(event: ProjectMidiEvent): DecodedEvent {
  const word = event.word0 >>> 0;
  return {
    ppq: event.ppq,
    status: (word >> 20) & 0xf,
    channel: (word >> 16) & 0xf,
    group: (word >> 24) & 0xf,
    note: (word >> 8) & 0x7f,
    velocity: word & 0x7f,
  };
}

const NOTE_ON = 0x9;
const NOTE_OFF = 0x8;
const A1 = 33;

function noteNumbers(result: ReturnType<typeof transcribe>): number[] {
  return result.events
    .map(decode)
    .filter((event) => event.status === NOTE_ON)
    .map((event) => event.note);
}

/** One fixture and one baseline run, shared by every case that does not vary an option. */
let samples: Float32Array;
let baseline: ReturnType<typeof transcribe>;

describe('WASM transcribe', () => {
  beforeAll(async () => {
    await init();
    samples = melody();
    baseline = transcribe({ samples, sampleRate, tempoBpm: 120 });
  });

  it('returns the notes of the source melody, in order', () => {
    expect(baseline.noteCount).toBe(toneHz.length);
    const onsets = baseline.events.map(decode).filter((event) => event.status === NOTE_ON);
    expect(onsets.map((event) => event.note)).toEqual(expectedNotes);
  });

  it('pairs every note and orders the pairs, note-off first at a shared tick', () => {
    expect(baseline.events.length).toBe(2 * baseline.noteCount);

    const decoded = baseline.events.map(decode);
    expect(decoded.filter((event) => event.status === NOTE_ON).length).toBe(baseline.noteCount);
    expect(decoded.filter((event) => event.status === NOTE_OFF).length).toBe(baseline.noteCount);
    for (let i = 1; i < decoded.length; i++) {
      expect(decoded[i].ppq).toBeGreaterThanOrEqual(decoded[i - 1].ppq);
    }

    // A note-off sharing a tick with the next note's on has to come first, or a
    // consumer playing the events in order starts a legato repeat of the same
    // pitch and immediately stops it. The legato fixture supplies the pairs that
    // make the check mean anything: without one, every ordering passes.
    const joined = transcribe({ samples: legato(), sampleRate, tempoBpm: 120 }).events.map(decode);
    expect(joined.filter((event) => event.status === NOTE_ON).map((event) => event.note)).toEqual(
      expectedNotes,
    );
    const sharedTicks = joined.filter((event, i) => i > 0 && event.ppq === joined[i - 1].ppq);
    expect(sharedTicks.length).toBeGreaterThan(0);
    for (const event of sharedTicks) {
      expect(event.status).toBe(NOTE_ON);
      expect(joined[joined.indexOf(event) - 1].status).toBe(NOTE_OFF);
    }

    // Every note-on is closed before the next one opens.
    let open = 0;
    for (const event of decoded) {
      open += event.status === NOTE_ON ? 1 : -1;
      expect(open).toBeGreaterThanOrEqual(0);
      expect(open).toBeLessThanOrEqual(1);
    }
    expect(open).toBe(0);
  });

  it('echoes a supplied tempo and detects a usable one when none is given', () => {
    expect(baseline.tempoBpm).toBe(120);

    const detected = transcribe({ samples, sampleRate });
    expect(Number.isFinite(detected.tempoBpm)).toBe(true);
    expect(detected.tempoBpm).toBeGreaterThan(0);
  });

  it('scales the ppq grid with the supplied tempo', () => {
    // Twice the tempo is twice as many beats per second (ppq = seconds * bpm /
    // 60), so the same audio lands on twice the ppq.
    const doubled = transcribe({ samples, sampleRate, tempoBpm: 240 });
    expect(doubled.noteCount).toBe(baseline.noteCount);

    // The ratio alone is satisfied by an all-zero grid, so the slow run has to
    // be a real one first.
    expect(baseline.events[baseline.events.length - 1].ppq).toBeGreaterThan(0);

    // Only the time axis moved: word0 carries status, group, channel, note and
    // velocity, so comparing it pins every part of the payload at once.
    expect(doubled.events.map((event) => event.word0)).toEqual(
      baseline.events.map((event) => event.word0),
    );
    const notesAndVelocities = (result: typeof baseline) =>
      result.events.map(decode).map((event) => [event.note, event.velocity]);
    expect(notesAndVelocities(doubled)).toEqual(notesAndVelocities(baseline));

    for (let i = 0; i < doubled.events.length; i++) {
      expect(doubled.events[i].ppq).toBeCloseTo(2 * baseline.events[i].ppq, 6);
    }
  });

  it('measures the note numbers against referenceHz', () => {
    // A reference a semitone above A440 puts every measured pitch one semitone
    // lower. The field being read and then ignored is what this catches.
    const sharp = transcribe({
      samples,
      sampleRate,
      tempoBpm: 120,
      referenceHz: 440 * 2 ** (1 / 12),
    });
    const shifted = sharp.events.map(decode).filter((event) => event.status === NOTE_ON);
    expect(shifted.map((event) => event.note)).toEqual(expectedNotes.map((note) => note - 1));
  });

  it('gives every note the fixed velocity when one is set', () => {
    const fixed = transcribe({ samples, sampleRate, tempoBpm: 120, fixedVelocity: 77 });
    const onsets = fixed.events.map(decode).filter((event) => event.status === NOTE_ON);
    expect(onsets.length).toBe(fixed.noteCount);
    expect(onsets.map((event) => event.velocity)).toEqual(onsets.map(() => 77));

    // Measured velocities are the thing being replaced, so they must not already
    // be 77 -- otherwise this passes on a fixedVelocity that was never read.
    const measured = baseline.events.map(decode).filter((event) => event.status === NOTE_ON);
    expect(measured.every((event) => event.velocity !== 77)).toBe(true);

    // Both ends of the domain are inside it, so the refusal below cannot be an
    // off-by-one that happens to exclude the boundary.
    for (const velocity of [1, 127]) {
      const at = transcribe({ samples, sampleRate, tempoBpm: 120, fixedVelocity: velocity });
      const ons = at.events.map(decode).filter((event) => event.status === NOTE_ON);
      expect(ons.map((event) => event.velocity)).toEqual(ons.map(() => velocity));
    }
  });

  it('reads an absent option as the default however absence is spelled', () => {
    // The control for the guard below: this reader zero-initializes the config
    // rather than seeding it from the C defaults, so an omitted field IS the 0
    // the guard refuses. Presence is what separates them, and nothing but a call
    // with the field left out can show that the separation works -- a value
    // check here would refuse omission and no rejection test would notice.
    const asKey = (result: typeof baseline) =>
      JSON.stringify([result.noteCount, result.events.map((event) => [event.ppq, event.word0])]);
    const expected = asKey(baseline);

    const guarded = [
      'referenceHz',
      'f0MinHz',
      'f0MaxHz',
      'minNoteMs',
      'minNoteDivision',
      'segmentationThresholdCents',
      'velocityFloorDb',
      'fixedVelocity',
      'maxPolyphony',
      'minFramePeakRatio',
      'minRidgePeakRatio',
      'reattackRatio',
    ] as const;
    for (const key of guarded) {
      for (const absent of [undefined, null]) {
        const result = transcribe({
          samples,
          sampleRate,
          tempoBpm: 120,
          [key]: absent,
        } as TranscribeRequest);
        expect(asKey(result), `${key}: ${String(absent)}`).toBe(expected);
      }
    }
  });

  it('resolves omitted tracker bounds from the selected source', () => {
    const lowNote = richTone(55, 1.0);
    const omitted = transcribe({
      samples: lowNote,
      sampleRate,
      polyphonic: true,
      tempoBpm: 120,
    });
    const explicit = transcribe({
      samples: lowNote,
      sampleRate,
      polyphonic: true,
      f0MinHz: 55,
      f0MaxHz: 1760,
      tempoBpm: 120,
    });

    // The explicit target makes equality meaningful: both calls must retain a
    // real A1 rather than agree on an empty transcription.
    expect(noteNumbers(explicit)).toContain(A1);
    expect(omitted).toEqual(explicit);
  });

  it('resolves a partial tracker range against the selected source', () => {
    // Mono uses its 2093 Hz upper default, so this is a valid high lower bound
    // even though the fixture contains no note in that range. Polyphonic uses
    // the salience tracker's 1760 Hz upper default and must refuse the same
    // partial request after resolving it.
    expect(() => transcribe({ samples, sampleRate, f0MinHz: 1800, tempoBpm: 120 })).not.toThrow();
    expect(() =>
      transcribe({ samples, sampleRate, polyphonic: true, f0MinHz: 1800, tempoBpm: 120 }),
    ).toThrow(/^transcribe: f0MaxHz must be greater than f0MinHz$/);
  });

  it('emits on the requested group and channel', () => {
    const routed = transcribe({ samples, sampleRate, tempoBpm: 120, group: 3, channel: 9 });
    for (const event of routed.events.map(decode)) {
      expect(event.group).toBe(3);
      expect(event.channel).toBe(9);
    }
  });

  it('refuses a buffer or a rate it cannot use', () => {
    expect(() => transcribe({ samples: new Float32Array(0), sampleRate })).toThrow(RangeError);
    expect(() => transcribe({ samples, sampleRate: 0 })).toThrow(RangeError);
    const withNan = samples.slice();
    withNan[100] = Number.NaN;
    expect(() => transcribe({ samples: withNan, sampleRate })).toThrow(RangeError);

    // The transcription C ABI checks nothing about the sample values, so the
    // facade's non-finite scan is the only refusal and is not optional.
    const bypass = {
      samples: withNan,
      sampleRate,
      validate: false,
    } as unknown as TranscribeRequest;
    expect(() => transcribe(bypass)).toThrow(unknownKeyText('transcribe: unknown key validate'));
    const allNan = new Float32Array(samples.length).fill(Number.NaN);
    expect(() => transcribe({ samples: allNan, sampleRate })).toThrow(RangeError);
  });

  it('refuses an option outside its domain, including a written zero', () => {
    // 0 is the C ABI's "use the default" on these fields, and omitting the field
    // already says that here -- so a 0 the caller wrote is a value, and it is out
    // of domain. Accepting it would answer with the default under a number they
    // did not choose.
    // The reader refuses the first group itself (a RangeError); the C ABI owns the
    // last three, which reach the caller as a RangeError as well.
    const rejected: Array<[string, TranscribeOptions]> = [
      ['fixedVelocity 0', { fixedVelocity: 0 }],
      ['fixedVelocity 128', { fixedVelocity: 128 }],
      ['velocityFloorDb 0', { velocityFloorDb: 0 }],
      ['velocityFloorDb positive', { velocityFloorDb: 6 }],
      ['referenceHz 0', { referenceHz: 0 }],
      ['minNoteMs 0', { minNoteMs: 0 }],
      ['minNoteDivision 0', { minNoteDivision: 0 }],
      ['minNoteDivision negative', { minNoteDivision: -1 }],
      ['minNoteDivision 129', { minNoteDivision: 129 }],
      ['minNoteDivision fractional', { minNoteDivision: 1.5 }],
      ['maxPolyphony 0', { polyphonic: true, maxPolyphony: 0 }],
      ['maxPolyphony 65', { polyphonic: true, maxPolyphony: 65 }],
      ['maxPolyphony fractional', { polyphonic: true, maxPolyphony: 2.5 }],
      ['minFramePeakRatio negative', { polyphonic: true, minFramePeakRatio: -0.1 }],
      ['minFramePeakRatio above 1', { polyphonic: true, minFramePeakRatio: 1.5 }],
      ['minRidgePeakRatio negative', { polyphonic: true, minRidgePeakRatio: -0.1 }],
      ['minRidgePeakRatio above 1', { polyphonic: true, minRidgePeakRatio: 1.5 }],
      ['reattackRatio negative', { polyphonic: true, reattackRatio: -1 }],
      ['reattackRatio 1', { polyphonic: true, reattackRatio: 1 }],
      ['reattackRatio 0.5', { polyphonic: true, reattackRatio: 0.5 }],
      ['segmentationThresholdCents 0', { segmentationThresholdCents: 0 }],
      ['fmax below fmin', { f0MinHz: 500, f0MaxHz: 400 }],
      ['channel above 15', { channel: 16 }],
      ['group above 15', { group: 16 }],
    ];
    for (const [name, options] of rejected) {
      let thrown: unknown;
      try {
        transcribe({ samples, sampleRate, tempoBpm: 120, ...options });
      } catch (error) {
        thrown = error;
      }
      expect(thrown, name).toBeDefined();
      expectArgumentRefusal(thrown, RangeError, name);
    }
  });

  it('names the offending field on a refusal the C ABI owns', () => {
    // `group` / `channel` out of [0, 15] are refused by the C ABI rather than by
    // this surface's reader -- one rule in one place. The F0 bounds are refused
    // by the facade under their public names.
    // What has to hold here is that the detail survives the boundary:
    // `throwCError` prefers `sonare_last_error_message()` over the generic text
    // for the code, so a caller is told which field rather than "Invalid
    // parameter". The field name is asserted rather than the whole sentence, so
    // this pins the seam without owning the C ABI's prose.
    const named: Array<[string, TranscribeOptions, RegExp]> = [
      [
        'f0MaxHz below f0MinHz',
        { f0MinHz: 500, f0MaxHz: 400 },
        /f0MaxHz must be greater than f0MinHz/,
      ],
      ['group above 15', { group: 16 }, /group/],
      ['channel above 15', { channel: 16 }, /channel/],
    ];
    for (const [name, options, field] of named) {
      let message = '';
      try {
        transcribe({ samples, sampleRate, tempoBpm: 120, ...options });
      } catch (error) {
        message = (error as Error).message;
      }
      expect(message, name).toMatch(field);
      // The failure this guards is the detail being dropped, which reads as the
      // generic message for the code and names no field at all.
      expect(message, name).not.toMatch(/:\s*Invalid parameter$/);
    }
  });

  it('reports the detail of THIS call, not whatever last wrote the error slot', () => {
    // The detail comes from a last-error slot, so a message that looks right may
    // be the previous call's: a slot that had stopped being written would still
    // read as correct for whichever failure ran before it, and every assertion
    // above would keep passing. Annotated and unannotated failures are therefore
    // interleaved -- the detail has to APPEAR when the C ABI names a field and
    // DISAPPEAR when it does not. Refusing a clip id is the unannotated one: it
    // fails past `read_config`, where nothing writes the slot.
    const project = new Project();
    project.setSampleRate(48000);
    project.addMidiClip(0, 8 * 960);
    const unknownClipId = 9999;

    const messageOf = (call: () => unknown): string => {
      try {
        call();
      } catch (error) {
        return (error as Error).message;
      }
      return '';
    };

    const annotated = () =>
      messageOf(() => transcribe({ samples, sampleRate, tempoBpm: 120, group: 16 }));
    const unannotated = () =>
      messageOf(() => project.transcribeToClip({ clipId: unknownClipId, samples, sampleRate }));

    expect(annotated()).toMatch(/group/);
    expect(unannotated()).toMatch(/: clipId must be the id of /);
    expect(unannotated()).not.toMatch(/group/);
    // Once more in the other order, so neither result can be the one the slot
    // happened to be holding when the test started.
    expect(annotated()).toMatch(/group/);
    expect(unannotated()).not.toMatch(/group/);

    // And a success after a failure is still a success -- the slot is not
    // consulted when there is nothing to report.
    expect(transcribe({ samples, sampleRate, tempoBpm: 120 }).noteCount).toBe(baseline.noteCount);
  });

  it('refuses a non-positive tempoBpm in both call forms and still detects when it is absent', () => {
    for (const tempoBpm of [0, -1]) {
      expect(() => transcribe({ samples, sampleRate, tempoBpm })).toThrow(RangeError);
      expect(() => transcribe({ samples, sampleRate, tempoBpm })).toThrow(/tempoBpm/);
      expect(() => transcribe(samples, sampleRate, { tempoBpm })).toThrow(RangeError);
      expect(() => transcribe(samples, sampleRate, { tempoBpm })).toThrow(/tempoBpm/);
    }
    const detected = transcribe({ samples, sampleRate });
    expect(Number.isFinite(detected.tempoBpm)).toBe(true);
    expect(detected.tempoBpm).toBeGreaterThan(0);
  });

  it('lets maxPolyphony bound the voices of a chord', () => {
    const chordSamples = triad();
    const open = transcribe({ samples: chordSamples, sampleRate, tempoBpm: 120, polyphonic: true });
    const single = transcribe({
      samples: chordSamples,
      sampleRate,
      tempoBpm: 120,
      polyphonic: true,
      maxPolyphony: 1,
    });
    expect(maxSimultaneous(open)).toBeGreaterThanOrEqual(3);
    expect(maxSimultaneous(single)).toBeLessThan(maxSimultaneous(open));
  });

  it('reads reattackRatio: omitted leaves a re-struck tone whole, an explicit ratio splits it', () => {
    const struck = restruck();
    const run = (extra: TranscribeOptions) =>
      transcribe({ samples: struck, sampleRate, tempoBpm: 120, polyphonic: true, ...extra });
    const omitted = run({});
    expect(omitted.noteCount).toBe(1);
    expect(run({ reattackRatio: 2 }).noteCount).toBe(2);
    for (const bad of [0, 1, -1]) {
      expect(() => run({ reattackRatio: bad })).toThrow(RangeError);
      expect(() => run({ reattackRatio: bad })).toThrow(/reattackRatio must be greater than 1/);
    }
    // A larger ratio than the second strike's rise also leaves the tone whole.
    expect(run({ reattackRatio: 50 }).noteCount).toBe(1);
  });

  it('reads minFramePeakRatio and minRidgePeakRatio, and takes 0 as a real 0', () => {
    const chordSamples = triad();
    const key = (result: typeof baseline) =>
      JSON.stringify(result.events.map((event) => [event.ppq, event.word0]));
    const run = (extra: TranscribeOptions) =>
      transcribe({
        samples: chordSamples,
        sampleRate,
        tempoBpm: 120,
        polyphonic: true,
        ...extra,
      });
    const omitted = key(run({}));
    expect(key(run({ minFramePeakRatio: 1 }))).not.toBe(omitted);
    expect(key(run({ minRidgePeakRatio: 1 }))).not.toBe(omitted);
    // 0 is accepted, and is the C ABI's negative (a real 0), not its default.
    expect(() => run({ minFramePeakRatio: 0 })).not.toThrow();
    expect(() => run({ minRidgePeakRatio: 0 })).not.toThrow();
    expect(key(run({ minFramePeakRatio: 0.2 }))).toBe(omitted);
    expect(key(run({ minRidgePeakRatio: 0.1 }))).toBe(omitted);
  });

  it('reads minNoteDivision as a note value at the transcription tempo', () => {
    // A sixteenth note at 120 bpm is 125 ms.
    for (const polyphonic of [false, true]) {
      const key = (result: ReturnType<typeof transcribe>) =>
        JSON.stringify([result.noteCount, result.events.map((event) => [event.ppq, event.word0])]);
      const byDivision = transcribe({
        samples,
        sampleRate,
        tempoBpm: 120,
        polyphonic,
        minNoteDivision: 16,
      });
      const byMs = transcribe({ samples, sampleRate, tempoBpm: 120, polyphonic, minNoteMs: 125 });
      expect(key(byDivision), `polyphonic ${polyphonic}`).toBe(key(byMs));
    }
  });

  it('refuses a wrong-typed minNoteDivision and both spellings at once', () => {
    expect(() =>
      transcribe({
        samples,
        sampleRate,
        tempoBpm: 120,
        minNoteDivision: '16' as unknown as number,
      }),
    ).toThrow(TypeError);
    let thrown: unknown;
    try {
      transcribe({ samples, sampleRate, tempoBpm: 120, minNoteDivision: 16, minNoteMs: 125 });
    } catch (error) {
      thrown = error;
    }
    expectArgumentRefusal(thrown, RangeError);
    expect((thrown as Error).message).toContain('minNoteDivision');
  });

  it('surfaces the C refusal of a polyphonic-only option on the monophonic path', () => {
    for (const options of [
      { maxPolyphony: 3 },
      { minFramePeakRatio: 0.5 },
      { minRidgePeakRatio: 0.5 },
      { reattackRatio: 2 },
    ] as TranscribeOptions[]) {
      let thrown: unknown;
      try {
        transcribe({ samples, sampleRate, tempoBpm: 120, ...options });
      } catch (error) {
        thrown = error;
      }
      const name = Object.keys(options)[0];
      expectArgumentRefusal(thrown, RangeError, name);
      expect((thrown as Error).message, name).toContain(name);
    }
  });

  it('accepts the zeros that are values rather than sentinels', () => {
    // The other half of the rule above, and what keeps it from becoming "refuse
    // every zero": group 0 and channel 0 are values a caller can mean, and they
    // are the two the refusal must not reach.
    const zeroed = transcribe({ samples, sampleRate, tempoBpm: 120, group: 0, channel: 0 });
    expect(zeroed.noteCount).toBe(baseline.noteCount);
    for (const event of zeroed.events.map(decode)) {
      expect(event.group).toBe(0);
      expect(event.channel).toBe(0);
    }
  });
});

describe('WASM Project.transcribeToClip', () => {
  beforeAll(async () => {
    await init();
    samples = melody();
  });

  /** A MIDI clip holding one note nothing in the fixture could produce. */
  function projectWithSeededClip(): { project: Project; clipId: number; sourceId: number } {
    const project = new Project();
    project.setSampleRate(48000);
    const { clipId } = project.addMidiClip(0, 8 * 960);
    project.setMidiEvents(clipId, [
      midiNoteOn({ ppq: 0, group: 0, channel: 0, note: 30, velocity: 100 }),
      midiNoteOff({ ppq: 480, group: 0, channel: 0, note: 30, velocity: 0 }),
    ]);
    return { project, clipId, sourceId: project.clips()[0].sourceId };
  }

  /** The clip's stored events, read back out of the serialized project. */
  function storedEvents(project: Project, sourceId: number): ProjectMidiEvent[] {
    const document = JSON.parse(project.toJson()) as {
      midiContent: Record<string, ProjectMidiEvent[]>;
    };
    return document.midiContent[String(sourceId)] ?? [];
  }

  it('writes the transcribed notes into the clip, replacing what it held', () => {
    const { project, clipId, sourceId } = projectWithSeededClip();
    expect(
      storedEvents(project, sourceId)
        .map(decode)
        .map((event) => event.note),
    ).toEqual([30, 30]);

    const noteCount = project.transcribeToClip({ clipId, samples, sampleRate });
    expect(noteCount).toBe(toneHz.length);

    const stored = storedEvents(project, sourceId);
    expect(stored.length).toBe(2 * noteCount);
    const onsets = stored.map(decode).filter((event) => event.status === NOTE_ON);
    expect(onsets.map((event) => event.note)).toEqual(expectedNotes);
    // Replaced, not appended: the seeded note is gone rather than sitting in front.
    expect(stored.map(decode).some((event) => event.note === 30)).toBe(false);
  });

  it('refuses an unusable buffer, rate, or option', () => {
    const { project, clipId } = projectWithSeededClip();
    expect(() =>
      project.transcribeToClip({ clipId, samples: new Float32Array(0), sampleRate }),
    ).toThrow(RangeError);
    expect(() => project.transcribeToClip({ clipId, samples, sampleRate: 0 })).toThrow(RangeError);

    // Both doors share one config reader, and that is the claim being checked:
    // a guard covering only the free function passes every case above it.
    const rejected: Array<[string, TranscribeOptions]> = [
      ['velocityFloorDb 0', { velocityFloorDb: 0 }],
      ['fixedVelocity 0', { fixedVelocity: 0 }],
      ['minNoteMs 0', { minNoteMs: 0 }],
      ['minNoteDivision 0', { minNoteDivision: 0 }],
    ];
    for (const [name, options] of rejected) {
      let thrown: unknown;
      try {
        project.transcribeToClip({ clipId, samples, sampleRate, ...options });
      } catch (error) {
        thrown = error;
      }
      expectArgumentRefusal(thrown, RangeError, name);
    }
  });

  it('accepts minNoteDivision on a project clip', () => {
    const { project, clipId } = projectWithSeededClip();
    expect(() =>
      project.transcribeToClip({ clipId, samples, sampleRate, minNoteDivision: 32 }),
    ).not.toThrow();
  });
});

/** A harmonic tone at `hz` with sinusoidal vibrato of `rateHz` and `depthCents`, at 22050 Hz. */
function vibratoTone(
  hz: number,
  seconds: number,
  rateHz: number,
  depthCents: number,
): Float32Array {
  const rate = 22050;
  const out = new Float32Array(Math.round(rate * seconds));
  const edge = 0.01 * rate;
  let phase = 0;
  for (let i = 0; i < out.length; i++) {
    const f = hz * 2 ** ((depthCents / 1200) * Math.sin((2 * Math.PI * rateHz * i) / rate));
    phase += (2 * Math.PI * f) / rate;
    const envelope = Math.min(1, i / edge, (out.length - 1 - i) / edge);
    out[i] =
      0.4 * envelope * (Math.sin(phase) + 0.5 * Math.sin(2 * phase) + 0.25 * Math.sin(3 * phase));
  }
  return out;
}

describe('WASM transcribe monophonicModel', () => {
  const G4 = 67;
  const sung = vibratoTone(391.995, 2, 6, 75);
  const run = (extra: Record<string, unknown> = {}) =>
    transcribe({ samples: sung, sampleRate: 22050, tempoBpm: 120, ...extra } as TranscribeRequest);

  beforeAll(async () => {
    await init();
  });

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
  });

  it("refuses 'singing' on the polyphonic path", () => {
    expect(() => run({ polyphonic: true, monophonicModel: 'singing' })).toThrow(RangeError);
  });
});
