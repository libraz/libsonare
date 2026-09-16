import { describe, expect, it } from 'vitest';
import type { NoteObject, NoteObjectInput, NoteSetEntry } from '../src/index.js';
import {
  decomposeNotePitch,
  ErrorCode,
  extractNotes,
  isSonareError,
  mergeNotes,
  pitchPyin,
  renderNotes,
  splitNote,
} from '../src/index.js';

const SR = 22050;
const HOP = 512;
const N_FRAMES = 43;
const LENGTH = N_FRAMES * HOP;
const FRAME_RATE = SR / HOP;
// The pitch steps here, so the segmenter splits into exactly two notes whose
// spans tile the buffer: [0, SPLIT_FRAME * HOP) and [SPLIT_FRAME * HOP, LENGTH).
const SPLIT_FRAME = 22;
const SPLIT_SAMPLE = SPLIT_FRAME * HOP;

// The note set the split and merge cases start from: 40 frames, two unvoiced
// gaps, and an amplitude that steps every frame.
const SET_FRAMES = 40;
const SET_LENGTH = SET_FRAMES * HOP;
// Half of that, for the formant cases: an LPC analysis-resynthesis round is the
// most expensive render here and nothing it asserts needs the length.
const SHORT_LENGTH = (SET_FRAMES / 2) * HOP;

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

function peak(samples: Float32Array, start = 0, end = samples.length): number {
  let highest = 0;
  for (let i = start; i < end; i += 1) {
    highest = Math.max(highest, Math.abs(samples[i]));
  }
  return highest;
}

function maxDifference(a: Float32Array, b: Float32Array): number {
  let worst = 0;
  for (let i = 0; i < a.length; i += 1) {
    worst = Math.max(worst, Math.abs(a[i] - b[i]));
  }
  return worst;
}

/**
 * Asserts that an edit meant to change the sound did: the output moved away from
 * `source`, and it is still a signal. The amplitude band is what a bare
 * difference check misses — silence differs from the source by its own peak, so
 * it passes one, and a blow-up passes it too.
 */
function expectEdited(out: Float32Array, source: Float32Array): void {
  expect(maxDifference(source, out)).toBeGreaterThan(0.05);
  const sourcePeak = peak(source);
  expect(peak(out)).toBeGreaterThan(0.5 * sourcePeak);
  expect(peak(out)).toBeLessThan(2 * sourcePeak);
}

/** A sine whose pitch swings `depthCents` either side of `centreHz` at `rateHz`. */
function fmTone(
  centreHz: number,
  depthCents: number,
  rateHz: number,
  length: number,
): Float32Array {
  const samples = new Float32Array(length);
  let phase = 0;
  for (let i = 0; i < length; i += 1) {
    samples[i] = 0.4 * Math.sin(phase);
    const cents = depthCents * Math.sin((2 * Math.PI * rateHz * i) / SR);
    phase += (2 * Math.PI * centreHz * 2 ** (cents / 1200)) / SR;
  }
  return samples;
}

/** The F0 track describing {@link fmTone} frame by frame. */
function fmTrack(
  centreHz: number,
  depthCents: number,
  rateHz: number,
  frames: number,
): Float32Array {
  const f0Hz = new Float32Array(frames);
  for (let frame = 0; frame < frames; frame += 1) {
    const cents = depthCents * Math.sin((2 * Math.PI * rateHz * frame) / FRAME_RATE);
    f0Hz[frame] = centreHz * 2 ** (cents / 1200);
  }
  return f0Hz;
}

/**
 * Harmonics of `f0Hz` under a fixed resonance. A bare sine carries no spectral
 * envelope, so a formant warp needs a source that has one.
 */
function vowelTone(f0Hz: number, formantHz: number, length: number): Float32Array {
  const bandwidthHz = 500;
  const samples = new Float32Array(length);
  for (let harmonic = 1; harmonic * f0Hz < SR / 2; harmonic += 1) {
    const hz = harmonic * f0Hz;
    const weight = 0.1 / (1 + ((hz - formantHz) / bandwidthHz) ** 2);
    for (let i = 0; i < length; i += 1) {
      samples[i] += weight * Math.sin((2 * Math.PI * hz * i) / SR);
    }
  }
  return samples;
}

/** A tone whose amplitude steps up once per frame, so no two frames share an RMS. */
function steppedTone(frames: number): Float32Array {
  const samples = new Float32Array(frames * HOP);
  for (let frame = 0; frame < frames; frame += 1) {
    const amplitude = 0.1 + 0.02 * frame;
    for (let k = 0; k < HOP; k += 1) {
      const i = frame * HOP + k;
      samples[i] = amplitude * Math.sin((2 * Math.PI * 441 * i) / SR);
    }
  }
  return samples;
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
        amplitudeEnvelope: new Float32Array(0),
        pitchShiftSemitones: 0,
        gainDb: 0,
        timeStretchRatio: 1,
        formantShiftSemitones: 0,
        vibratoDepthChange: 0,
        driftChange: 0,
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
    expect(() => extractNotes({ ...request, frameRate: Number.NaN })).toThrow(RangeError);
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

  it('rejects a note that omits a sample bound the type declares mandatory', () => {
    // The bound used to default to 0, which makes onset and offset equal: the
    // note's edit rendered as nothing while the call reported success. The same
    // request throws on the WASM surface, so this is the policy the exported
    // NoteObjectInput has always declared rather than a new restriction.
    const missing = (note: Record<string, unknown>): unknown => {
      try {
        renderNotes({
          samples: tone.samples,
          sampleRate: SR,
          notes: [note as unknown as NoteObjectInput],
        });
        return undefined;
      } catch (error) {
        return error;
      }
    };

    // Each row carries the message its refusal has to name, because a refusal
    // for an unrelated reason is indistinguishable from this one on the code
    // alone -- and the defect this replaced returned success, not another error.
    const malformed: ReadonlyArray<readonly [Record<string, unknown>, string]> = [
      [{ offsetSample: 5000, edit: { gainDb: -6 } }, 'note.onsetSample is required'],
      [{ onsetSample: 0, edit: { gainDb: -6 } }, 'note.offsetSample is required'],
      // Present-but-undefined and present-but-null both read as absent, so the
      // three spellings of "not given" land on one answer instead of three.
      [
        { onsetSample: 0, offsetSample: undefined, edit: { gainDb: -6 } },
        'note.offsetSample is required',
      ],
      [
        { onsetSample: 0, offsetSample: null, edit: { gainDb: -6 } },
        'note.offsetSample is required',
      ],
      [{ onsetSample: '0', offsetSample: 5000 }, 'note.onsetSample must be a number'],
    ];

    for (const [note, fragment] of malformed) {
      const caught = missing(note);
      expect(isSonareError(caught), `expected a SonareError for ${JSON.stringify(note)}`).toBe(
        true,
      );
      expect((caught as { code: number }).code).toBe(ErrorCode.InvalidParameter);
      expect((caught as { message: string }).message).toContain(`renderNotes ${fragment}`);
    }

    // A non-finite bound is refused too, but by the TS facade's own int64 range
    // check ahead of the addon, so it is a RangeError rather than a SonareError.
    // Stated rather than folded into the loop: the class is the surface's, and a
    // sweep that "unified" it would be changing behaviour, not tidying a test.
    expect(
      missing({ onsetSample: 0, offsetSample: Number.NaN, edit: { gainDb: -6 } }),
    ).toBeInstanceOf(RangeError);

    // The control. A reader that had started refusing every note would satisfy
    // every assertion above and fail here, and the gain has to be visible in the
    // output or a rejected span would read the same as an applied one.
    const stated = renderNotes({
      samples: tone.samples,
      sampleRate: SR,
      notes: [{ onsetSample: 0, offsetSample: 5000, edit: { gainDb: -20 } }],
    });
    expect(rms(stated, 2000, 4000)).toBeCloseTo(rms(tone.samples, 2000, 4000) * 0.1, 5);
  });

  it('keeps the note-set entries split and merge take free of the sample bounds', () => {
    // NoteSetEntry does not declare the bounds at all — both entry points
    // re-derive every note from the audio and the track — so requiring them
    // here would have been a new disagreement rather than one fewer.
    //
    // Two unvoiced gaps, so the set holds three notes -- a merge needs a
    // neighbour to join to, which a single-note track cannot supply.
    const f0Hz = new Float32Array(SET_FRAMES).fill(441);
    const voiced = new Int32Array(SET_FRAMES).fill(1);
    for (const start of [10, 25]) {
      for (let frame = start; frame < start + 2; frame += 1) {
        f0Hz[frame] = 0;
        voiced[frame] = 0;
      }
    }
    const source = {
      samples: steppedTone(SET_FRAMES),
      sampleRate: SR,
      f0Hz,
      voiced,
      frameRate: FRAME_RATE,
    };
    const entries: NoteSetEntry[] = extractNotes(source).map((note) => ({
      frameStart: note.frameStart,
      frameEnd: note.frameEnd,
    }));
    expect(entries).toHaveLength(3);
    expect(entries.every((entry) => !('onsetSample' in entry))).toBe(true);

    const split = splitNote({ ...source, notes: entries, index: 0, frame: 5 });
    expect(split).toHaveLength(entries.length + 1);
    const merged = mergeNotes({ ...source, notes: entries, first: 0, last: 1 });
    expect(merged).toHaveLength(entries.length - 1);
  });
});

describe('renderNotes amplitude envelope', () => {
  // 441 Hz at 22050 is 50 samples a period, so every window below holds a whole
  // number of them and the source's RMS is the same in all of them.
  const samples = new Float32Array(SET_LENGTH).map(
    (_, i) => 0.5 * Math.sin((2 * Math.PI * 441 * i) / SR),
  );
  const HALF = SET_LENGTH / 2;
  // Two 2000-sample windows near the ends of the first note, each inset well
  // past the 5 ms (110-sample) cross-fade at that edge.
  const head: [number, number] = [400, 2400];
  const tail: [number, number] = [HALF - 2400, HALF - 400];

  /** The first half carries the envelope; the second half is left at the identity. */
  const renderWith = (amplitudeEnvelope: readonly number[]): Float32Array =>
    renderNotes({
      samples,
      sampleRate: SR,
      notes: [
        { onsetSample: 0, offsetSample: HALF, edit: { amplitudeEnvelope } },
        { onsetSample: HALF, offsetSample: SET_LENGTH },
      ],
    });

  it('applies a one-entry envelope as a constant gain over the note', () => {
    const rendered = renderWith([0.5]);

    // Two windows at opposite ends of the note: a constant is the same in both,
    // which a ramp read in its place would not be.
    expect(rms(rendered, ...head) / rms(samples, ...head)).toBeCloseTo(0.5, 2);
    expect(rms(rendered, ...tail) / rms(samples, ...tail)).toBeCloseTo(0.5, 2);
    // The untouched neighbour keeps the identity's bit-for-bit pass-through.
    expect(rendered.subarray(HALF)).toEqual(samples.subarray(HALF));
  });

  it('stretches a two-point envelope over the note it belongs to', () => {
    const rendered = renderWith([0.25, 1]);

    const at = (window: [number, number]) => rms(rendered, ...window) / rms(samples, ...window);
    // 0.25 -> 1.0 stretched over the note's 10240 samples leaves the envelope at
    // 0.3525 and 0.8975 at these two windows' centres. A window's RMS sits
    // slightly above its centre value because the ramp is squared into it:
    // c*sqrt(1 + (mh/c)^2/3) for slope m over half-span h, which is +0.72% here
    // and +0.11% there. An envelope that was never read leaves both at 1.
    expect(at(head)).toBeCloseTo(0.355, 2);
    expect(at(tail)).toBeCloseTo(0.898, 2);
    expect(rendered.subarray(HALF)).toEqual(samples.subarray(HALF));
  });

  it('rejects an envelope value that is not a usable linear gain', () => {
    for (const bad of [Number.NaN, Number.POSITIVE_INFINITY, -1]) {
      expect(() => renderWith([0.5, bad])).toThrow();
    }
    // Positive control: the same call with nothing poisoned renders.
    expect(renderWith([0.5, 1])).toHaveLength(SET_LENGTH);
  });
});

describe('renderNotes formant shift', () => {
  const samples = vowelTone(200, 1200, SHORT_LENGTH);

  const renderWith = (formantShiftSemitones: number, gainDb: number): Float32Array =>
    renderNotes({
      samples,
      sampleRate: SR,
      notes: [
        {
          onsetSample: 0,
          offsetSample: SHORT_LENGTH,
          medianHz: 200,
          edit: { formantShiftSemitones, gainDb },
        },
      ],
    });

  it('runs no warp at all at 0, so the edit is still the identity', () => {
    // An LPC analysis-resynthesis round at factor 1 would not come back bit for
    // bit, so this is the field deciding not to run rather than running flat.
    expect(renderWith(0, 0)).toEqual(samples);
  });

  it('moves the spectral envelope without moving the level', () => {
    expectEdited(renderWith(2, 0), samples);
  });

  it('is read on a note the renderer was resynthesizing anyway', () => {
    // Not merely deciding whether any work happens at all: the gain edit already
    // forced a resynthesis, and the warp still changes the result.
    expectEdited(renderWith(2, -3), renderWith(0, -3));
  });
});

describe('renderNotes pitch curve edits', () => {
  // The whole 40 frames: the drift filter is asked to separate 5.5 Hz from
  // 3 Hz, which a track half this long gives it too few frames to do cleanly.
  const samples = fmTone(220, 30, 5.5, SET_LENGTH);
  const f0Hz = fmTrack(220, 30, 5.5, SET_FRAMES);
  const note = {
    onsetSample: 0,
    offsetSample: SET_LENGTH,
    frameStart: 0,
    frameEnd: SET_FRAMES,
    medianHz: 220,
  };

  const renderWith = (
    edit: { vibratoDepthChange?: number; driftChange?: number },
    vibratoCutoffHz?: number,
  ): Float32Array =>
    renderNotes({
      samples,
      sampleRate: SR,
      notes: [{ ...note, edit }],
      f0Hz,
      frameRate: FRAME_RATE,
      vibratoCutoffHz,
    });

  it('rejects a curve edit with no F0 track and applies it with one', () => {
    for (const edit of [{ vibratoDepthChange: -1 }, { driftChange: 1 }]) {
      // The curve the edit acts on is the caller's own track, and there is none.
      expect(() => renderNotes({ samples, sampleRate: SR, notes: [{ ...note, edit }] })).toThrow();
      // Its companion: the same edit with the track renders and moves the audio,
      // which is what makes the rejection about the track rather than the field.
      expectEdited(renderWith(edit), samples);
    }
  });

  it('rejects a track that does not cover the notes it is given', () => {
    const beyond = { ...note, frameEnd: SET_FRAMES + 5 };
    expect(() =>
      renderNotes({
        samples,
        sampleRate: SR,
        notes: [{ ...beyond, edit: { vibratoDepthChange: -1 } }],
        f0Hz,
        frameRate: FRAME_RATE,
      }),
    ).toThrow();
    expect(() =>
      renderNotes({
        samples,
        sampleRate: SR,
        notes: [{ ...note, edit: { vibratoDepthChange: -1 } }],
        f0Hz,
        frameRate: Number.NaN,
      }),
    ).toThrow(RangeError);
    // Positive control: a note ending exactly at the track's last frame is
    // inside it, so neither rejection above is refusing every curve edit.
    expect(renderWith({ vibratoDepthChange: -1 })).toHaveLength(SET_LENGTH);
  });

  it('leaves the identity alone on the path that reads the track', () => {
    expect(renderWith({})).toEqual(samples);
  });

  it('takes the vibrato cutoff as the boundary between the two curves', () => {
    const flattened = renderWith({ vibratoDepthChange: -1 });

    // 0 keeps the default 3 Hz, so an explicit 3 lands on what an omitted one
    // renders.
    expect(renderWith({ vibratoDepthChange: -1 }, 3)).toEqual(flattened);

    // 5.5 Hz sits above a 3 Hz cut and below an 8 Hz one, so flattening the
    // vibrato takes most of the swing at 3 and little of it at 8. A cutoff that
    // was never read would render these two the same, and one wired backwards
    // would invert the order rather than merely shrinking the difference.
    const wide = renderWith({ vibratoDepthChange: -1 }, 8);
    expectEdited(wide, flattened);
    expect(maxDifference(samples, flattened)).toBeGreaterThan(maxDifference(samples, wide));
    expect(maxDifference(samples, wide)).toBeGreaterThan(0.05);
  });

  it('rejects a cutoff that cannot be one', () => {
    for (const bad of [Number.NaN, Number.POSITIVE_INFINITY, -1]) {
      expect(() => renderWith({ vibratoDepthChange: -1 }, bad)).toThrow();
    }
  });
});

describe('decomposeNotePitch', () => {
  // A standalone curve: this entry point measures a track and never looks at
  // audio, so it is driven at its own frame rate and length.
  const CURVE_FRAMES = 400;
  const CURVE_RATE = 100;
  const CENTRE = 196;

  /** F0 whose cents against `CENTRE` are exactly the sum of the components. */
  function injectedF0(components: ReadonlyArray<{ hz: number; cents: number }>): Float32Array {
    const f0Hz = new Float32Array(CURVE_FRAMES);
    for (let i = 0; i < CURVE_FRAMES; i += 1) {
      let cents = 0;
      for (const component of components) {
        cents += component.cents * Math.sin((2 * Math.PI * component.hz * i) / CURVE_RATE);
      }
      f0Hz[i] = CENTRE * 2 ** (cents / 1200);
    }
    return f0Hz;
  }

  // 0.5 Hz and 5.5 Hz sit either side of the 3 Hz cut, so each curve has
  // something of its own to carry.
  const f0Hz = injectedF0([
    { hz: 0.5, cents: 60 },
    { hz: 5.5, cents: 40 },
  ]);

  const decomposeAt = (cutoff?: number) =>
    decomposeNotePitch({
      f0Hz,
      frameRate: CURVE_RATE,
      medianHz: CENTRE,
      vibratoCutoffHz: cutoff,
    });

  it('splits a curve into two parts that add back up to it', () => {
    const result = decomposeAt(3);

    expect(result.centreHz).toBe(CENTRE);
    expect(result.driftCents).toHaveLength(CURVE_FRAMES);
    expect(result.vibratoCents).toHaveLength(CURVE_FRAMES);

    let worst = 0;
    let driftPeak = 0;
    let vibratoPeak = 0;
    for (let i = 0; i < CURVE_FRAMES; i += 1) {
      const cents = 1200 * Math.log2(f0Hz[i] / CENTRE);
      worst = Math.max(worst, Math.abs(result.driftCents[i] + result.vibratoCents[i] - cents));
      driftPeak = Math.max(driftPeak, Math.abs(result.driftCents[i]));
      vibratoPeak = Math.max(vibratoPeak, Math.abs(result.vibratoCents[i]));
    }
    expect(worst).toBeLessThan(0.01);
    // Two zero curves satisfy the sum as well, so both have to carry something.
    expect(driftPeak).toBeGreaterThan(10);
    expect(vibratoPeak).toBeGreaterThan(10);
  });

  it('takes the cutoff default at 0 and an omitted one', () => {
    const explicitDefault = decomposeAt(3);
    expect(decomposeAt(0)).toEqual(explicitDefault);
    expect(decomposeAt()).toEqual(explicitDefault);
    // Non-vacuity: the cutoff does decide the split, so the equality above is
    // the default being applied rather than an argument nobody reads.
    expect(decomposeAt(8)).not.toEqual(explicitDefault);
  });

  it('reports a note with no usable pitch as an empty result', () => {
    // A measurement that came up empty is not a bad argument, so it is reported
    // rather than rejected.
    for (const empty of [
      decomposeNotePitch({
        f0Hz: new Float32Array(CURVE_FRAMES),
        frameRate: CURVE_RATE,
        medianHz: CENTRE,
      }),
      decomposeNotePitch({ f0Hz, frameRate: CURVE_RATE, medianHz: 0 }),
    ]) {
      expect(empty.centreHz).toBe(0);
      expect(empty.driftCents).toHaveLength(0);
      expect(empty.vibratoCents).toHaveLength(0);
    }
    // The same curve with a centre is not an empty measurement, so neither of
    // the two above passes by emptying every call.
    expect(decomposeAt().driftCents).toHaveLength(CURVE_FRAMES);
  });

  it('rejects malformed arguments', () => {
    expect(() => decomposeNotePitch({ f0Hz, frameRate: Number.NaN, medianHz: CENTRE })).toThrow(
      RangeError,
    );
    expect(() =>
      decomposeNotePitch({ f0Hz: new Float32Array(0), frameRate: CURVE_RATE, medianHz: CENTRE }),
    ).toThrow();
    for (const badRate of [0, -100]) {
      expect(() => decomposeNotePitch({ f0Hz, frameRate: badRate, medianHz: CENTRE })).toThrow();
    }
    // 0 is the no-pitch spelling, so only a value that cannot be a centre at all
    // is rejected.
    expect(() => decomposeNotePitch({ f0Hz, frameRate: CURVE_RATE, medianHz: -1 })).toThrow();
    for (const badCutoff of [-1, Number.NaN]) {
      expect(() => decomposeAt(badCutoff)).toThrow();
    }
    const poisoned = Float32Array.from(f0Hz);
    poisoned[7] = -1;
    expect(() =>
      decomposeNotePitch({ f0Hz: poisoned, frameRate: CURVE_RATE, medianHz: CENTRE }),
    ).toThrow();
  });
});

describe('splitNote and mergeNotes', () => {
  /** Two unvoiced gaps, so the segmenter emits three notes of 10, 13 and 13 frames. */
  function gappedSource() {
    const f0Hz = new Float32Array(SET_FRAMES).fill(441);
    const voiced = new Int32Array(SET_FRAMES).fill(1);
    for (const start of [10, 25]) {
      for (let frame = start; frame < start + 2; frame += 1) {
        f0Hz[frame] = 0;
        voiced[frame] = 0;
      }
    }
    return {
      samples: steppedTone(SET_FRAMES),
      sampleRate: SR,
      f0Hz,
      voiced,
      frameRate: FRAME_RATE,
    };
  }

  /** One voiced run over the whole track. */
  function plainSource() {
    return {
      samples: steppedTone(SET_FRAMES),
      sampleRate: SR,
      f0Hz: new Float32Array(SET_FRAMES).fill(441),
      voiced: new Int32Array(SET_FRAMES).fill(1),
      frameRate: FRAME_RATE,
    };
  }

  const measurements = (note: NoteObject) => ({
    onsetSample: note.onsetSample,
    offsetSample: note.offsetSample,
    frameStart: note.frameStart,
    frameEnd: note.frameEnd,
    medianHz: note.medianHz,
    medianCents: note.medianCents,
    f0Stability: note.f0Stability,
  });

  /** No two entries agree, so comparing a slice against the wrong one cannot pass. */
  function expectDistinct(values: number[]): void {
    const sorted = [...values].sort((a, b) => a - b);
    expect(sorted.length).toBeGreaterThan(1);
    for (let i = 1; i < sorted.length; i += 1) {
      expect(sorted[i] - sorted[i - 1]).toBeGreaterThan(1e-4);
    }
  }

  function expectSameValues(actual: Float32Array | number[], expected: Float32Array | number[]) {
    expect(actual.length).toBe(expected.length);
    for (let i = 0; i < expected.length; i += 1) {
      expect(actual[i]).toBeCloseTo(expected[i], 6);
    }
  }

  it('segments the gapped source into the three notes the cases below start from', () => {
    const notes = extractNotes(gappedSource());

    expect(notes.map((note) => [note.frameStart, note.frameEnd])).toEqual([
      [0, 10],
      [12, 25],
      [27, 40],
    ]);
    // Every frame's RMS is its own, which is what makes a misaligned slice
    // visible: on a flat tone a slice pointing at a neighbour reads correct.
    expectDistinct(notes.flatMap((note) => [...note.amplitude]));
  });

  it('keeps every note pointing at its own frames when one is split', () => {
    const source = gappedSource();
    const notes = extractNotes(source);
    const split = splitNote({ ...source, notes, index: 1, frame: 18 });

    expect(split).toHaveLength(4);
    // The cut lands where it was asked for and the spans stay contiguous.
    expect(split[1].frameStart).toBe(12);
    expect(split[1].frameEnd).toBe(18);
    expect(split[2].frameStart).toBe(18);
    expect(split[2].frameEnd).toBe(25);
    expect(split[1].offsetSample).toBe(split[2].onsetSample);
    expect(split[1].onsetSample).toBe(notes[1].onsetSample);
    expect(split[2].offsetSample).toBe(notes[1].offsetSample);

    // The two halves' curves are the source note's, cut at the same frame. A
    // wrapper that cannot supply a pass-through note's curve produces slices
    // that look the right length and hold a neighbour's data.
    expectSameValues([...split[1].amplitude, ...split[2].amplitude], notes[1].amplitude);
    expectSameValues(split[0].amplitude, notes[0].amplitude);
    expectSameValues(split[3].amplitude, notes[2].amplitude);

    // The untouched notes are re-derived rather than copied, and come back equal.
    expect(measurements(split[0])).toEqual(measurements(notes[0]));
    expect(measurements(split[3])).toEqual(measurements(notes[2]));
  });

  it('normalizes the identity edit and carries no envelope through', () => {
    const source = plainSource();
    const notes = extractNotes(source);
    expect(notes).toHaveLength(1);

    const split = splitNote({ ...source, notes, index: 0, frame: 20 });
    expect(split).toHaveLength(2);
    for (const half of split) {
      expect(half.edit.timeStretchRatio).toBe(1);
      expect(half.edit.gainDb).toBe(0);
      expect(half.edit.muted).toBe(false);
      expect(half.edit.amplitudeEnvelope).toEqual(new Float32Array(0));
    }
  });

  it('cuts the source note envelope at the same proportion as the span', () => {
    const source = plainSource();
    const notes = extractNotes(source);
    notes[0].edit.amplitudeEnvelope = Float32Array.from([0.25, 1]);

    // The cut is at the note's midpoint, so a 0.25 -> 1.0 ramp parts at 0.625.
    const split = splitNote({ ...source, notes, index: 0, frame: 20 });
    expect(split[0].edit.amplitudeEnvelope[0]).toBeCloseTo(0.25, 5);
    expect(split[0].edit.amplitudeEnvelope.at(-1)).toBeCloseTo(0.625, 1);
    expect(split[1].edit.amplitudeEnvelope[0]).toBeCloseTo(
      split[0].edit.amplitudeEnvelope.at(-1) as number,
      5,
    );
    expect(split[1].edit.amplitudeEnvelope.at(-1)).toBeCloseTo(1, 5);
    // Nothing outside the source note's own two points reached either half.
    for (const half of split) {
      for (const point of half.edit.amplitudeEnvelope) {
        expect(point).toBeGreaterThanOrEqual(0.25 - 1e-5);
        expect(point).toBeLessThanOrEqual(1 + 1e-5);
      }
    }
  });

  it('gives both halves a one-entry envelope unchanged, because it is a constant', () => {
    const source = plainSource();
    const notes = extractNotes(source);
    notes[0].edit.amplitudeEnvelope = Float32Array.from([0.5]);

    const split = splitNote({ ...source, notes, index: 0, frame: 20 });
    for (const half of split) {
      expect(half.edit.amplitudeEnvelope).toEqual(Float32Array.from([0.5]));
    }
  });

  it('spans the gap a merge joins over and keeps every slice aligned', () => {
    const source = gappedSource();
    const notes = extractNotes(source);
    const merged = mergeNotes({ ...source, notes, first: 0, last: 1 });

    // last - first notes go away, so the count drops by exactly one here.
    expect(merged).toHaveLength(2);
    expect(merged[0].frameStart).toBe(0);
    expect(merged[0].frameEnd).toBe(25);
    expect(merged[0].onsetSample).toBe(notes[0].onsetSample);
    expect(merged[0].offsetSample).toBe(notes[1].offsetSample);
    expect(measurements(merged[1])).toEqual(measurements(notes[2]));

    // The merged note's curve covers the gap the segmenter cut at, so it is
    // longer than the two it replaces by the two frames neither carried.
    const joined = merged[0].amplitude;
    expect(joined).toHaveLength(25);
    expectSameValues(joined.subarray(0, 10), notes[0].amplitude);
    expectSameValues(joined.subarray(12), notes[1].amplitude);
    // The gap's own amplitude lives in the audio, not in either neighbour.
    expect(joined[10]).toBeGreaterThan(0);
    expect(joined[11]).toBeGreaterThan(0);
  });

  it('takes the first note edit of the run it joins', () => {
    const source = gappedSource();
    const notes = extractNotes(source);
    notes[0].edit.gainDb = -3;
    notes[0].edit.pitchShiftSemitones = 2;
    notes[0].edit.amplitudeEnvelope = Float32Array.from([0.5]);
    notes[1].edit.gainDb = 9;
    notes[1].edit.muted = true;

    const merged = mergeNotes({ ...source, notes, first: 0, last: 1 });
    expect(merged[0].edit.gainDb).toBe(-3);
    expect(merged[0].edit.pitchShiftSemitones).toBe(2);
    expect(merged[0].edit.amplitudeEnvelope).toEqual(Float32Array.from([0.5]));
    // The second note's edit does not survive: the rule is the first note's
    // edit, not a merge of the two.
    expect(merged[0].edit.muted).toBe(false);
  });

  it('returns the spans and the curves a split started from when it is undone', () => {
    const source = gappedSource();
    const notes = extractNotes(source);
    const split = splitNote({ ...source, notes, index: 1, frame: 18 });
    const rejoined = mergeNotes({ ...source, notes: split, first: 1, last: 2 });

    expect(rejoined).toHaveLength(notes.length);
    for (let i = 0; i < notes.length; i += 1) {
      expect(measurements(rejoined[i])).toEqual(measurements(notes[i]));
      expectSameValues(rejoined[i].amplitude, notes[i].amplitude);
    }
  });

  it('rejects a split index or frame the set cannot describe', () => {
    const source = gappedSource();
    const notes = extractNotes(source);

    for (const index of [notes.length, notes.length + 4, -1]) {
      expect(() => splitNote({ ...source, notes, index, frame: 18 })).toThrow();
    }
    // Strictly inside the note's own span, so neither of its boundaries is a
    // legal cut and neither is a frame belonging to another note.
    for (const frame of [12, 25, 5, 30, -1, 100]) {
      expect(() => splitNote({ ...source, notes, index: 1, frame })).toThrow();
    }
    expect(() => splitNote({ ...source, notes: [], index: 0, frame: 18 })).toThrow();
    expect(() =>
      splitNote({
        ...source,
        voiced: undefined,
        voicedProb: undefined,
        notes,
        index: 1,
        frame: 18,
      }),
    ).toThrow(TypeError);
    expect(() => splitNote({ ...source, sampleRate: 0, notes, index: 1, frame: 18 })).toThrow(
      RangeError,
    );

    // Positive controls: one frame in from either end of the note is legal, and
    // both halves keep a span.
    for (const frame of [13, 24]) {
      const split = splitNote({ ...source, notes, index: 1, frame });
      expect(split).toHaveLength(4);
      expect(split[1].frameEnd).toBe(frame);
      expect(split[2].frameStart).toBe(frame);
    }
  });

  it('names the note whose frame bounds are missing', () => {
    const source = gappedSource();
    const notes = extractNotes(source);
    // A note is identified by its frame bounds alone here, so a missing bound
    // would otherwise read as an empty span and come back as a bare
    // INVALID_PARAMETER that names nothing.
    const broken = [notes[0], { edit: {} } as unknown as NoteSetEntry, notes[2]];

    expect(() => splitNote({ ...source, notes: broken, index: 0, frame: 5 })).toThrow(/notes\[1\]/);
    expect(() => mergeNotes({ ...source, notes: broken, first: 0, last: 1 })).toThrow(/notes\[1\]/);
  });

  it('rejects a merge run that is not ascending and in range', () => {
    const source = gappedSource();
    const notes = extractNotes(source);

    // A run of one is not a merge, and a run cannot run backwards.
    for (const [first, last] of [
      [1, 1],
      [2, 1],
      [0, notes.length],
      [notes.length, notes.length + 1],
    ]) {
      expect(() => mergeNotes({ ...source, notes, first, last })).toThrow();
    }
    expect(() => mergeNotes({ ...source, notes: [], first: 0, last: 1 })).toThrow();

    // Positive control: merging the whole run collapses the list to one note
    // over the whole span, so none of the above passes by rejecting every merge.
    const all = mergeNotes({ ...source, notes, first: 0, last: 2 });
    expect(all).toHaveLength(1);
    expect(all[0].frameStart).toBe(notes[0].frameStart);
    expect(all[0].frameEnd).toBe(notes[2].frameEnd);
  });

  it('renders a split set the same as the set it came from when nothing was edited', () => {
    const source = gappedSource();
    const notes = extractNotes(source);
    const split = splitNote({ ...source, notes, index: 1, frame: 18 });

    // A note whose edit is the identity is not resynthesized, and a split leaves
    // every edit at the identity, so the halves still reproduce the input.
    expect(renderNotes({ samples: source.samples, sampleRate: SR, notes: split })).toEqual(
      source.samples,
    );
  });
});

describe('the documented pitchPyin to extractNotes pipeline', () => {
  // A tone with its middle third silenced, so pYIN has genuinely unvoiced
  // frames to report. A continuous tone has none, and the first case below
  // would then pass on an input too clean to tell.
  const PIPELINE_LENGTH = SR;
  const gappedTone = (() => {
    const samples = new Float32Array(PIPELINE_LENGTH);
    for (let i = 0; i < PIPELINE_LENGTH; i += 1) {
      samples[i] = 0.4 * Math.sin((2 * Math.PI * 220 * i) / SR);
    }
    samples.fill(0, Math.floor(PIPELINE_LENGTH / 3), Math.floor((2 * PIPELINE_LENGTH) / 3));
    return samples;
  })();

  it('rejects the default track, whose unvoiced frames are NaN', () => {
    const pitch = pitchPyin({ samples: gappedTone, sampleRate: SR });

    // Both halves of the track are real: the silence is NaN and the tone is
    // not, so neither the rejection below nor the success in the next case is
    // an artefact of a degenerate fixture.
    const nanFrames = [...pitch.f0].filter((hz) => Number.isNaN(hz)).length;
    expect(nanFrames).toBeGreaterThan(0);
    expect(nanFrames).toBeLessThan(pitch.f0.length);
    expect(pitch.voicedFlag.filter(Boolean).length).toBeGreaterThan(0);

    let caught: unknown;
    try {
      extractNotes({
        samples: gappedTone,
        sampleRate: SR,
        f0Hz: pitch.f0,
        voiced: pitch.voicedFlag,
        frameRate: FRAME_RATE,
        minNoteMs: 40,
      });
    } catch (error) {
      caught = error;
    }
    expect(isSonareError(caught)).toBe(true);
    if (isSonareError(caught)) {
      expect(caught.code).toBe(ErrorCode.InvalidParameter);
    }
  });

  it('segments the track fillNa produces', () => {
    const pitch = pitchPyin({ samples: gappedTone, sampleRate: SR, fillNa: true });
    expect([...pitch.f0].every((hz) => Number.isFinite(hz))).toBe(true);

    const notes = extractNotes({
      samples: gappedTone,
      sampleRate: SR,
      f0Hz: pitch.f0,
      voiced: pitch.voicedFlag,
      frameRate: FRAME_RATE,
      minNoteMs: 40,
    });
    expect(notes.length).toBeGreaterThan(0);
    for (const note of notes) {
      expect(note.offsetSample).toBeGreaterThan(note.onsetSample);
      expect(note.medianHz).toBeCloseTo(220, 0);
    }

    // The notes go back where they came from: the whole point of extracting
    // them is to edit and render them.
    notes[0].edit.gainDb = -6;
    expect(renderNotes({ samples: gappedTone, sampleRate: SR, notes })).toHaveLength(
      PIPELINE_LENGTH,
    );
  });
});
