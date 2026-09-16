/**
 * Note-object extraction, rendering and reshaping on the WASM surface.
 *
 * The editing model rests on one property: a set of notes whose edits are all
 * identity renders back to the input bit for bit. Everything else here is an
 * edit applied to exactly one note's span, checked against the untouched
 * neighbour.
 *
 * The reshaping and curve fixtures are hand-built at 16 kHz with 10 ms frames
 * rather than measured with `pitchPyin`, so every expected span, median and
 * curve value is predictable.
 */

import { beforeAll, describe, expect, it } from 'vitest';
import {
  decomposeNotePitch,
  ErrorCode,
  extractNotes,
  init,
  isSonareError,
  mergeNotes,
  type NoteObject,
  type NoteObjectInput,
  type NoteSetEntry,
  pitchPyin,
  renderNotes,
  type SonareError,
  splitNote,
} from '../src/index';

const sampleRate = 22050;
const hopLength = 512;
const frameCount = 43;
const splitFrame = 22;
const frameRate = sampleRate / hopLength;
const totalSamples = frameCount * hopLength;

/** The hand-built fixtures' rate: 400 Hz is 40 samples a period, a frame four of them. */
const fixtureRate = 16000;
const fixtureFrameRate = 100;
const samplesPerFrame = 160;
const fixtureFrames = 40;

/** Two sustained tones a fifth apart, switching at `splitFrame`. */
function twoNoteSignal(): Float32Array {
  const samples = new Float32Array(totalSamples);
  const splitSample = splitFrame * hopLength;
  for (let i = 0; i < samples.length; i++) {
    const hz = i < splitSample ? 220 : 330;
    samples[i] = 0.5 * Math.sin((2 * Math.PI * hz * i) / sampleRate);
  }
  return samples;
}

function twoNoteF0(): Float32Array {
  const f0Hz = new Float32Array(frameCount);
  for (let frame = 0; frame < frameCount; frame++) {
    f0Hz[frame] = frame < splitFrame ? 220 : 330;
  }
  return f0Hz;
}

function voicedFlags(value: boolean): Int32Array {
  return new Int32Array(frameCount).fill(value ? 1 : 0);
}

function sine(hz: number, amplitude: number, n: number): Float32Array {
  const out = new Float32Array(n);
  for (let i = 0; i < n; i++) {
    out[i] = amplitude * Math.sin((2 * Math.PI * hz * i) / fixtureRate);
  }
  return out;
}

/**
 * A sine whose pitch swings `depthCents` either side of `centreHz` at `rateHz`,
 * so a note taken from it carries a vibrato a curve edit can act on.
 */
function fmTone(
  centreHz: number,
  depthCents: number,
  rateHz: number,
  amplitude: number,
  n: number,
): Float32Array {
  const out = new Float32Array(n);
  let phase = 0;
  for (let i = 0; i < n; i++) {
    out[i] = amplitude * Math.sin(phase);
    const cents = depthCents * Math.sin((2 * Math.PI * rateHz * i) / fixtureRate);
    phase += (2 * Math.PI * centreHz * 2 ** (cents / 1200)) / fixtureRate;
  }
  return out;
}

/** The F0 track describing {@link fmTone} frame by frame. */
function fmTrack(
  centreHz: number,
  depthCents: number,
  rateHz: number,
  frames: number,
): Float32Array {
  const out = new Float32Array(frames);
  for (let i = 0; i < frames; i++) {
    const cents = depthCents * Math.sin((2 * Math.PI * rateHz * i) / fixtureFrameRate);
    out[i] = centreHz * 2 ** (cents / 1200);
  }
  return out;
}

/**
 * Harmonics of `f0Hz` under a fixed resonance. A bare sine carries no spectral
 * envelope, so a formant warp needs a source that has one.
 */
function vowelTone(f0Hz: number, formantHz: number, amplitude: number, n: number): Float32Array {
  const bandwidthHz = 500;
  const out = new Float32Array(n);
  for (let h = 1; h * f0Hz < fixtureRate / 2; h++) {
    const harmonicHz = h * f0Hz;
    const weight = 1 / (1 + ((harmonicHz - formantHz) / bandwidthHz) ** 2);
    const partial = sine(harmonicHz, amplitude * weight, n);
    for (let i = 0; i < n; i++) {
      out[i] += partial[i];
    }
  }
  return out;
}

/**
 * A 400 Hz tone whose amplitude steps up once per frame, so each frame's RMS is
 * distinct — which is what a per-note amplitude comparison needs to be able to
 * fail. On a flat tone a curve belonging to a neighbour reads correct.
 */
function steppedTone(frames: number): Float32Array {
  const out = new Float32Array(frames * samplesPerFrame);
  for (let frame = 0; frame < frames; frame++) {
    const amplitude = 0.1 + 0.02 * frame;
    for (let k = 0; k < samplesPerFrame; k++) {
      const i = frame * samplesPerFrame + k;
      out[i] = amplitude * Math.sin((2 * Math.PI * 400 * i) / fixtureRate);
    }
  }
  return out;
}

/** F0 values whose cents against `centreHz` are exactly the sum of `components`. */
function injectedF0(
  centreHz: number,
  frames: number,
  components: readonly { hz: number; cents: number }[],
): Float32Array {
  const out = new Float32Array(frames);
  for (let i = 0; i < frames; i++) {
    let cents = 0;
    for (const component of components) {
      cents += component.cents * Math.sin((2 * Math.PI * component.hz * i) / fixtureFrameRate);
    }
    out[i] = centreHz * 2 ** (cents / 1200);
  }
  return out;
}

function centsAbove(hz: number, centreHz: number): number {
  return 1200 * Math.log2(hz / centreHz);
}

function rms(data: Float32Array, lo: number, hi: number): number {
  let sum = 0;
  for (let i = lo; i < hi; i++) {
    sum += data[i] * data[i];
  }
  return Math.sqrt(sum / (hi - lo));
}

function peak(data: Float32Array): number {
  let highest = 0;
  for (const value of data) {
    highest = Math.max(highest, Math.abs(value));
  }
  return highest;
}

function maxDifference(a: Float32Array, b: Float32Array): number {
  let worst = 0;
  for (let i = 0; i < a.length; i++) {
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

/** Asserts `actual` is within `rel` of `expected`, relative to `expected`. */
function expectWithinRel(actual: number, expected: number, rel: number): void {
  expect(Math.abs(actual - expected)).toBeLessThanOrEqual(Math.abs(expected) * rel);
}

/** Asserts that @p action throws a SonareError carrying InvalidParameter. */
function expectInvalidParameter(action: () => void): void {
  let caught: unknown;
  try {
    action();
  } catch (error) {
    caught = error;
  }
  expect(isSonareError(caught)).toBe(true);
  if (isSonareError(caught)) {
    expect(caught.code).toBe(ErrorCode.InvalidParameter);
  }
}

/** A renderable note carrying nothing but its span, its frames and its centre. */
function handNote(
  onsetSample: number,
  offsetSample: number,
  medianHz = 440,
): NoteObjectInput & { medianHz: number } {
  return {
    onsetSample,
    offsetSample,
    frameStart: onsetSample / samplesPerFrame,
    frameEnd: offsetSample / samplesPerFrame,
    medianHz,
  };
}

/** One voiced run over the whole reshaping fixture. */
function plainSource() {
  return {
    samples: steppedTone(fixtureFrames),
    f0Hz: new Float32Array(fixtureFrames).fill(400),
    voiced: new Int32Array(fixtureFrames).fill(1),
    sampleRate: fixtureRate,
    frameRate: fixtureFrameRate,
  };
}

/**
 * Two unvoiced gaps, so the segmenter emits three notes of 10, 13 and 13 frames
 * with two frames between each pair.
 */
function gappedSource() {
  const source = plainSource();
  for (const [lo, hi] of [
    [10, 12],
    [25, 27],
  ]) {
    for (let i = lo; i < hi; i++) {
      source.f0Hz[i] = 0;
      source.voiced[i] = 0;
    }
  }
  return source;
}

let samples: Float32Array;
let f0Hz: Float32Array;
let notes: NoteObject[];

beforeAll(async () => {
  await init();
  samples = twoNoteSignal();
  f0Hz = twoNoteF0();
  notes = extractNotes({ samples, sampleRate, f0Hz, voiced: voicedFlags(true), frameRate });
});

describe('extractNotes', () => {
  it('segments the track into one note per sustained pitch', () => {
    expect(notes).toHaveLength(2);
    expect(notes[0].frameStart).toBe(0);
    expect(notes[0].frameEnd).toBe(splitFrame);
    expect(notes[1].frameStart).toBe(splitFrame);
    expect(notes[1].frameEnd).toBe(frameCount);
    expect(notes[0].onsetSample).toBe(0);
    expect(notes[0].offsetSample).toBe(splitFrame * hopLength);
    expect(notes[1].offsetSample).toBe(totalSamples);
  });

  it('measures pitch and stability over each span', () => {
    expect(notes[0].medianHz).toBeCloseTo(220, 0);
    expect(notes[1].medianHz).toBeCloseTo(330, 0);
    // 220 Hz sits a whole tone below the 440 Hz reference: -1200 cents.
    expect(notes[0].medianCents).toBeCloseTo(-1200, 0);
    for (const note of notes) {
      // A perfectly steady contour has zero deviation from its median.
      expect(note.f0Stability).toBe(1);
    }
  });

  it('surfaces exactly the nine documented note fields', () => {
    expect(Object.keys(notes[0]).sort()).toEqual([
      'amplitude',
      'edit',
      'f0Stability',
      'frameEnd',
      'frameStart',
      'medianCents',
      'medianHz',
      'offsetSample',
      'onsetSample',
    ]);
  });

  it('gives each note its own amplitude curve, one RMS per F0 frame', () => {
    for (const note of notes) {
      expect(note.amplitude).toBeInstanceOf(Float32Array);
      expect(note.amplitude.length).toBe(note.frameEnd - note.frameStart);
      for (const value of note.amplitude) {
        // A half-amplitude sine measures 0.5 / sqrt(2) per frame.
        expect(value).toBeCloseTo(0.354, 1);
      }
    }
  });

  it('returns the identity edit on every extracted note', () => {
    for (const note of notes) {
      expect(note.edit).toEqual({
        timeOffsetSamples: 0,
        pitchShiftSemitones: 0,
        gainDb: 0,
        timeStretchRatio: 1,
        formantShiftSemitones: 0,
        vibratoDepthChange: 0,
        driftChange: 0,
        muted: false,
        amplitudeEnvelope: new Float32Array(0),
      });
    }
  });

  it('honours the voicedThreshold only when voiced is absent', () => {
    const voicedProb = new Float32Array(frameCount).fill(0.4);
    expect(
      extractNotes({ samples, sampleRate, f0Hz, voicedProb, frameRate, voicedThreshold: 0.3 }),
    ).toHaveLength(2);
    expect(
      extractNotes({ samples, sampleRate, f0Hz, voicedProb, frameRate, voicedThreshold: 0.9 }),
    ).toHaveLength(0);
  });

  it('returns an empty array when the track segments to nothing', () => {
    const empty = extractNotes({
      samples,
      sampleRate,
      f0Hz,
      voiced: voicedFlags(false),
      frameRate,
    });
    expect(empty).toEqual([]);
  });

  it('rejects a malformed request', () => {
    const voiced = voicedFlags(true);
    // Neither voicing input given.
    expectInvalidParameter(() => extractNotes({ samples, sampleRate, f0Hz, frameRate }));
    expectInvalidParameter(() => extractNotes({ samples, sampleRate, f0Hz, voiced, frameRate: 0 }));
    expectInvalidParameter(() =>
      extractNotes({ samples, sampleRate, f0Hz, voiced, frameRate, minNoteMs: -1 }),
    );
    expectInvalidParameter(() =>
      extractNotes({ samples, sampleRate, f0Hz, voiced, frameRate, voicedThreshold: 1.5 }),
    );
    expectInvalidParameter(() =>
      extractNotes({
        samples,
        sampleRate,
        f0Hz: new Float32Array([220, -1, 220]),
        voiced: new Int32Array([1, 1, 1]),
        frameRate,
      }),
    );
    expect(() =>
      extractNotes({ samples, sampleRate, f0Hz, voiced: new Int32Array(3), frameRate }),
    ).toThrow(RangeError);
    expect(() => extractNotes({ samples, sampleRate: 7999, f0Hz, voiced, frameRate })).toThrow(
      RangeError,
    );
    expect(() =>
      extractNotes({ samples: new Float32Array(0), sampleRate, f0Hz, voiced, frameRate }),
    ).toThrow(RangeError);
  });
});

describe('renderNotes', () => {
  it('reproduces the input exactly when every edit is the identity', () => {
    expect(renderNotes({ samples, sampleRate, notes })).toEqual(samples);
  });

  it('reproduces the input exactly for an empty note list', () => {
    expect(renderNotes({ samples, sampleRate, notes: [] })).toEqual(samples);
  });

  it('applies a gain edit to that note span only', () => {
    const edited = notes.map((note, index) =>
      index === 1 ? { ...note, edit: { gainDb: -20 } } : note,
    );
    const rendered = renderNotes({ samples, sampleRate, notes: edited });
    expect(rendered).toHaveLength(samples.length);

    // The untouched note passes through bit for bit.
    for (let i = 0; i < notes[0].offsetSample; i++) {
      expect(rendered[i]).toBe(samples[i]);
    }
    // Past the edge cross-fade the edited span is exactly the scaled source.
    for (let i = notes[1].onsetSample + 500; i < notes[1].offsetSample - 500; i += 97) {
      expect(rendered[i]).toBeCloseTo(samples[i] * 0.1, 5);
    }
  });

  it('silences a muted note span and leaves its neighbour alone', () => {
    const edited = notes.map((note, index) =>
      index === 0 ? { ...note, edit: { muted: true } } : note,
    );
    const rendered = renderNotes({ samples, sampleRate, notes: edited });

    // Math.abs, because erasing a negative sample leaves -0 and Object.is
    // separates that from 0.
    for (let i = 500; i < notes[0].offsetSample - 500; i++) {
      expect(Math.abs(rendered[i])).toBe(0);
    }
    for (let i = notes[1].onsetSample; i < notes[1].offsetSample; i++) {
      expect(rendered[i]).toBe(samples[i]);
    }
  });

  it('reads only the span and the edit of a hand-built note', () => {
    const hand: NoteObjectInput[] = [
      { onsetSample: 4096, offsetSample: 8192, edit: { gainDb: -20 } },
    ];
    const rendered = renderNotes({ samples, sampleRate, notes: hand });
    expect(rendered[0]).toBe(samples[0]);
    expect(rendered[6144]).toBeCloseTo(samples[6144] * 0.1, 5);
  });

  it('honours fadeMs at the edited note edges', () => {
    const edited = [{ onsetSample: 4096, offsetSample: 8192, edit: { muted: true } }];
    const shortFade = renderNotes({ samples, sampleRate, notes: edited, fadeMs: 1 });
    const longFade = renderNotes({ samples, sampleRate, notes: edited, fadeMs: 20 });
    // 1 ms is 22 samples, so the 200th sample of the span is already silent;
    // a 20 ms fade is still ramping there.
    expect(Math.abs(shortFade[4296])).toBe(0);
    expect(Math.abs(longFade[4296])).toBeGreaterThan(0);
  });

  it('rejects a malformed request', () => {
    expectInvalidParameter(() =>
      renderNotes({
        samples,
        sampleRate,
        notes: [{ offsetSample: 8192 } as unknown as NoteObjectInput],
      }),
    );
    expectInvalidParameter(() =>
      renderNotes({ samples, sampleRate, notes: [{ onsetSample: 8192, offsetSample: 4096 }] }),
    );
    expectInvalidParameter(() =>
      renderNotes({
        samples,
        sampleRate,
        notes: [
          { onsetSample: 0, offsetSample: 8192, edit: { gainDb: -3 } },
          { onsetSample: 4096, offsetSample: 12288 },
        ],
      }),
    );
    expectInvalidParameter(() => renderNotes({ samples, sampleRate, notes, fadeMs: -1 }));
    expectInvalidParameter(() =>
      renderNotes({
        samples,
        sampleRate,
        notes: [{ onsetSample: 0, offsetSample: 8192, edit: { timeStretchRatio: -1 } }],
      }),
    );
    expect(() => renderNotes({ samples, sampleRate: 7999, notes })).toThrow(RangeError);
  });
});

describe('renderNotes refuses a note that omits a sample bound', () => {
  // `NoteObjectInput` declares onsetSample and offsetSample mandatory, and the
  // reader enforces it rather than defaulting. An omitted bound used to read as
  // 0, which on a hand-built note makes onset and offset equal: a zero-length
  // span renders as nothing, so the note's edit was dropped in silence while the
  // call reported success.
  const render = (note: Record<string, unknown>): unknown => {
    try {
      renderNotes({ samples, sampleRate, notes: [note as unknown as NoteObjectInput] });
      return undefined;
    } catch (error) {
      return error;
    }
  };

  /** A malformed note, and the message the refusal has to carry. */
  const MALFORMED: ReadonlyArray<readonly [string, Record<string, unknown>, string]> = [
    [
      'onsetSample absent',
      { offsetSample: 8192, edit: { gainDb: -20 } },
      'onsetSample is required',
    ],
    [
      'offsetSample absent',
      { onsetSample: 4096, edit: { gainDb: -20 } },
      'offsetSample is required',
    ],
    // Present-but-undefined and present-but-null read as absent here, so the
    // three spellings of "not given" land on one answer instead of three.
    [
      'onsetSample undefined',
      { onsetSample: undefined, offsetSample: 8192 },
      'onsetSample is required',
    ],
    ['offsetSample null', { onsetSample: 4096, offsetSample: null }, 'offsetSample is required'],
    [
      'onsetSample a string',
      { onsetSample: '4096', offsetSample: 8192 },
      'onsetSample must be a number',
    ],
    [
      'offsetSample NaN',
      { onsetSample: 4096, offsetSample: Number.NaN },
      'offsetSample must be finite',
    ],
  ];

  for (const [name, note, fragment] of MALFORMED) {
    it(`refuses a note whose ${name}, naming the field`, () => {
      const caught = render(note);
      expect(isSonareError(caught), `expected a SonareError for ${name}`).toBe(true);
      const error = caught as SonareError;
      expect(error.code).toBe(ErrorCode.InvalidParameter);
      // The field, not merely "something threw". A refusal for an unrelated
      // reason is indistinguishable from this one without it, which is exactly
      // the case the defect produced.
      expect(error.message).toContain(`renderNotes note.${fragment}`);
    });
  }

  it('renders the very same note once its span is stated', () => {
    // The control, and it is mandatory: a reader that had started refusing every
    // note satisfies every assertion above. The gain has to be visible in the
    // output too, or a note that was silently dropped reads the same as one that
    // was applied.
    const rendered = renderNotes({
      samples,
      sampleRate,
      notes: [{ onsetSample: 4096, offsetSample: 8192, edit: { gainDb: -20 } }],
    });
    expect(rendered).toHaveLength(samples.length);
    // -20 dB is a factor of 0.1 over the span, measured clear of the edge fades,
    // and nothing outside the span moves at all.
    expect(rms(rendered, 5000, 7000)).toBeCloseTo(rms(samples, 5000, 7000) * 0.1, 4);
    expect(rms(samples, 5000, 7000)).toBeGreaterThan(0.1);
    expect(rendered[0]).toBe(samples[0]);
    expect(rendered[12288]).toBe(samples[12288]);
  });

  it('keeps the note-set entries splitNote and mergeNotes take free of the bounds', () => {
    // Intended, not an oversight: `NoteSetEntry` does not declare the sample
    // bounds at all, because both entry points re-derive every note from the
    // audio and the track. Requiring them here would be a new disagreement
    // between the surfaces rather than one fewer.
    const source = gappedSource();
    const entries: NoteSetEntry[] = extractNotes(source).map((note) => ({
      frameStart: note.frameStart,
      frameEnd: note.frameEnd,
    }));
    expect(entries).toHaveLength(3);
    expect(entries.every((entry) => !('onsetSample' in entry))).toBe(true);

    expect(splitNote({ ...source, notes: entries, index: 1, frame: 18 })).toHaveLength(
      entries.length + 1,
    );
    expect(mergeNotes({ ...source, notes: entries, first: 0, last: 1 })).toHaveLength(
      entries.length - 1,
    );
  });
});

describe('renderNotes amplitude envelope', () => {
  // 400 Hz at 16 kHz is 40 samples a period, so each window below holds a whole
  // number of them and the source's RMS is the same in all four.
  const source = sine(400, 0.5, 6400);

  it("applies each note's own envelope and leaves a note without one flat", () => {
    const edited: NoteObjectInput[] = [
      { ...handNote(0, 3200), edit: { amplitudeEnvelope: new Float32Array([0.25, 1]) } },
      // -6.0206 dB is exactly half, flat across the whole span.
      { ...handNote(3200, 6400), edit: { gainDb: -6.0206 } },
    ];
    const rendered = renderNotes({ samples: source, sampleRate: fixtureRate, notes: edited });
    expect(rendered).toHaveLength(source.length);

    // Inset past the 5 ms (80-sample) cross-fade at each span edge.
    const headSource = rms(source, 160, 800);
    const tailSource = rms(source, 2400, 3040);
    expect(headSource).toBeGreaterThan(0);
    expectWithinRel(tailSource, headSource, 1e-4);

    // 0.25 -> 1.0 stretched over the note leaves the envelope at 0.3625 and
    // 0.8875 at these two windows' centres; a window's RMS sits slightly above
    // its centre value because the ramp is squared into it. A one-point read, or
    // an envelope applied to the wrong note, would put the same number at both.
    expectWithinRel(rms(rendered, 160, 800) / headSource, 0.36508, 0.02);
    expectWithinRel(rms(rendered, 2400, 3040) / tailSource, 0.88856, 0.02);

    // The second note is half throughout, not a ramp, so the first note's
    // envelope did not leak into it.
    expectWithinRel(rms(rendered, 3360, 4000) / rms(source, 3360, 4000), 0.5, 0.01);
    expectWithinRel(rms(rendered, 5600, 6240) / rms(source, 5600, 6240), 0.5, 0.01);
  });

  it('reads a plain number array exactly as it reads a Float32Array', () => {
    // The points are copied into WASM memory either way, so the two spellings
    // are the same envelope rather than merely similar ones.
    const rendered = (amplitudeEnvelope: Float32Array | readonly number[]): Float32Array =>
      renderNotes({
        samples: source,
        sampleRate: fixtureRate,
        notes: [{ ...handNote(1920, 6080), edit: { amplitudeEnvelope } }],
      });
    const fromTyped = rendered(new Float32Array([0.25, 1]));
    expect(rendered([0.25, 1])).toEqual(fromTyped);
    // Non-vacuity: the envelope was read at all, so the equality above is not
    // two identical pass-throughs.
    expect(fromTyped).not.toEqual(source);
  });

  it('treats a one-entry envelope as a constant gain', () => {
    const edited: NoteObjectInput[] = [
      { ...handNote(0, 6400), edit: { amplitudeEnvelope: new Float32Array([0.5]) } },
    ];
    const rendered = renderNotes({ samples: source, sampleRate: fixtureRate, notes: edited });
    for (const [lo, hi] of [
      [160, 800],
      [3000, 3640],
      [5600, 6240],
    ]) {
      expectWithinRel(rms(rendered, lo, hi) / rms(source, lo, hi), 0.5, 0.01);
    }
  });

  it('rejects an envelope value that is not a usable linear gain', () => {
    for (const bad of [Number.NaN, Number.POSITIVE_INFINITY, Number.NEGATIVE_INFINITY, -1]) {
      expectInvalidParameter(() =>
        renderNotes({
          samples: source,
          sampleRate: fixtureRate,
          notes: [
            { ...handNote(1920, 6080), edit: { amplitudeEnvelope: new Float32Array([1, bad]) } },
          ],
        }),
      );
    }
    // Positive control: the same note under a usable envelope renders.
    expect(
      renderNotes({
        samples: source,
        sampleRate: fixtureRate,
        notes: [
          { ...handNote(1920, 6080), edit: { amplitudeEnvelope: new Float32Array([1, 0.25]) } },
        ],
      }),
    ).toHaveLength(source.length);
  });
});

describe('renderNotes formant shift', () => {
  const source = vowelTone(200, 1200, 0.1, 6400);

  const renderedWith = (formantShiftSemitones: number, gainDb: number): Float32Array =>
    renderNotes({
      samples: source,
      sampleRate: fixtureRate,
      notes: [{ ...handNote(0, 6400, 200), edit: { formantShiftSemitones, gainDb } }],
    });

  it('runs no warp at all at 0, so the edit is still the identity', () => {
    // An LPC analysis-resynthesis round at factor 1 would not come back bit for
    // bit, so this is the warp being skipped rather than being harmless.
    expect(renderedWith(0, 0)).toEqual(source);
  });

  it('warps the spectral envelope when the shift is set', () => {
    expectEdited(renderedWith(2, 0), source);
  });

  it('is read on a note the renderer was resynthesizing anyway', () => {
    // Without this the field could merely be deciding whether any work happens.
    expectEdited(renderedWith(2, -3), renderedWith(0, -3));
  });
});

describe('renderNotes pitch curve edits', () => {
  const source = fmTone(220, 30, 5.5, 0.4, fixtureFrames * samplesPerFrame);
  const track = fmTrack(220, 30, 5.5, fixtureFrames);

  it('rejects a curve edit with no track and applies it with one', () => {
    for (const edit of [{ vibratoDepthChange: -1 }, { driftChange: 1 }]) {
      const note = { ...handNote(0, 6400, 220), edit };
      // The curve the edit acts on is the caller's own track, and there is none.
      expectInvalidParameter(() =>
        renderNotes({ samples: source, sampleRate: fixtureRate, notes: [note] }),
      );

      // Its companion: the same edit with the track renders and moves the audio,
      // which is what makes the rejection about the track rather than the field.
      const rendered = renderNotes({
        samples: source,
        sampleRate: fixtureRate,
        notes: [note],
        f0Hz: track,
        frameRate: fixtureFrameRate,
      });
      expect(rendered).toHaveLength(source.length);
      expectEdited(rendered, source);
    }
  });

  it('rejects a track that does not cover the notes it is given', () => {
    const note = { ...handNote(0, 6400, 220), edit: { vibratoDepthChange: -1 } };
    // A note whose frames run past the end of the track: the curve it would be
    // edited on is not there to slice.
    expectInvalidParameter(() =>
      renderNotes({
        samples: source,
        sampleRate: fixtureRate,
        notes: [{ ...note, frameEnd: fixtureFrames + 5 }],
        f0Hz: track,
        frameRate: fixtureFrameRate,
      }),
    );
    for (const badRate of [0, -100, Number.NaN, Number.POSITIVE_INFINITY]) {
      expectInvalidParameter(() =>
        renderNotes({
          samples: source,
          sampleRate: fixtureRate,
          notes: [note],
          f0Hz: track,
          frameRate: badRate,
        }),
      );
    }
    // Positive control: a note ending exactly at the track's last frame is
    // inside it, so none of the above passes by rejecting every curve edit.
    expect(note.frameEnd).toBe(fixtureFrames);
    expect(
      renderNotes({
        samples: source,
        sampleRate: fixtureRate,
        notes: [note],
        f0Hz: track,
        frameRate: fixtureFrameRate,
      }),
    ).toHaveLength(source.length);
  });

  it('cuts the curve where vibratoCutoffHz says, defaulting at 0', () => {
    const renderedAt = (vibratoCutoffHz?: number): Float32Array =>
      renderNotes({
        samples: source,
        sampleRate: fixtureRate,
        notes: [{ ...handNote(0, 6400, 220), edit: { vibratoDepthChange: -1 } }],
        f0Hz: track,
        frameRate: fixtureFrameRate,
        vibratoCutoffHz,
      });

    // 0 and absent both keep the default 3 Hz.
    const fromDefault = renderedAt();
    expect(renderedAt(0)).toEqual(fromDefault);
    expect(renderedAt(3)).toEqual(fromDefault);

    // 5.5 Hz sits above a 3 Hz cut and below an 8 Hz one, so flattening the
    // vibrato takes most of the swing at 3 and little of it at 8. A cutoff that
    // was never read would render these two the same.
    const fromWide = renderedAt(8);
    expectEdited(fromWide, fromDefault);

    // And in the direction the filter dictates: at a 3 Hz cut the 5.5 Hz swing
    // is vibrato and flattening takes it, at 8 Hz it is mostly drift and
    // survives, so the 3 Hz render is the one that moved further from the
    // source. Wired backwards this inverts rather than merely shrinking.
    expect(maxDifference(source, fromDefault)).toBeGreaterThan(
      1.5 * maxDifference(source, fromWide),
    );
  });

  it('rejects a cutoff that cannot be one', () => {
    for (const bad of [Number.NaN, Number.POSITIVE_INFINITY, Number.NEGATIVE_INFINITY, -1]) {
      expectInvalidParameter(() =>
        renderNotes({
          samples: source,
          sampleRate: fixtureRate,
          notes: [{ ...handNote(0, 6400, 220), edit: { vibratoDepthChange: -1 } }],
          f0Hz: track,
          frameRate: fixtureFrameRate,
          vibratoCutoffHz: bad,
        }),
      );
    }
  });

  it('is still the identity once the edit carries the envelope and curve fields', () => {
    const extracted = extractNotes({
      samples: source,
      sampleRate: fixtureRate,
      f0Hz: track,
      voiced: new Int32Array(fixtureFrames).fill(1),
      frameRate: fixtureFrameRate,
    });
    expect(extracted.length).toBeGreaterThan(0);

    // The track is handed in, so the identity has to survive the path that reads
    // it rather than only the one that never looks.
    const withTrack = {
      samples: source,
      sampleRate: fixtureRate,
      f0Hz: track,
      frameRate: fixtureFrameRate,
    };
    expect(renderNotes({ ...withTrack, notes: extracted })).toEqual(source);

    // Non-vacuity, one new field at a time: each moves the output off the
    // source, so the equality above is the identity rather than an edit nobody
    // read.
    for (const edit of [
      { formantShiftSemitones: 2 },
      { vibratoDepthChange: -1 },
      { driftChange: 1 },
      { amplitudeEnvelope: new Float32Array([0.25, 1]) },
    ]) {
      const moved = extracted.map((note, index) => (index === 0 ? { ...note, edit } : note));
      expectEdited(renderNotes({ ...withTrack, notes: moved }), source);
    }
  });
});

describe('decomposeNotePitch', () => {
  const centreHz = 196;
  const curveFrames = 400;
  const components = [
    { hz: 0.5, cents: 60 },
    { hz: 5.5, cents: 40 },
  ];
  const curve = injectedF0(centreHz, curveFrames, components);

  it('splits a curve into two parts that add back up to it', () => {
    const split = decomposeNotePitch({
      f0Hz: curve,
      frameRate: fixtureFrameRate,
      medianHz: centreHz,
      vibratoCutoffHz: 3,
    });
    expect(split.centreHz).toBe(centreHz);
    expect(split.driftCents).toHaveLength(curveFrames);
    expect(split.vibratoCents).toHaveLength(curveFrames);

    let worst = 0;
    let driftPeak = 0;
    let vibratoPeak = 0;
    for (let i = 0; i < curveFrames; i++) {
      const cents = centsAbove(curve[i], centreHz);
      worst = Math.max(worst, Math.abs(split.driftCents[i] + split.vibratoCents[i] - cents));
      driftPeak = Math.max(driftPeak, Math.abs(split.driftCents[i]));
      vibratoPeak = Math.max(vibratoPeak, Math.abs(split.vibratoCents[i]));
    }
    expect(worst).toBeLessThan(1e-3);

    // Two zero curves satisfy the sum as well, so both have to carry something.
    // 0.5 Hz and 5.5 Hz sit either side of the 3 Hz cut, so each does.
    expect(driftPeak).toBeGreaterThan(10);
    expect(vibratoPeak).toBeGreaterThan(10);
  });

  it('takes the cutoff default at 0 and at absent', () => {
    const at = (vibratoCutoffHz?: number) =>
      decomposeNotePitch({
        f0Hz: curve,
        frameRate: fixtureFrameRate,
        medianHz: centreHz,
        vibratoCutoffHz,
      });
    const explicitDefault = at(3);
    expect(at()).toEqual(explicitDefault);
    expect(at(0)).toEqual(explicitDefault);
    // Non-vacuity: the cutoff does decide the split, so the equality above is
    // the default being applied rather than an argument nobody reads.
    expect(at(8)).not.toEqual(explicitDefault);
  });

  it('reports a note with no usable pitch as an empty result', () => {
    // A measurement that came up empty is not a bad argument, so it is reported
    // rather than rejected.
    const expectEmpty = (f0: Float32Array, medianHz: number): void => {
      const split = decomposeNotePitch({ f0Hz: f0, frameRate: fixtureFrameRate, medianHz });
      expect(split.centreHz).toBe(0);
      expect(split.driftCents).toHaveLength(0);
      expect(split.vibratoCents).toHaveLength(0);
    };
    // Not one usable frame to hold anything from.
    expectEmpty(new Float32Array(curveFrames), centreHz);
    // A curve, but no centre to measure it against.
    expectEmpty(curve, 0);

    // The same curve with a centre is not an empty measurement, so neither of
    // the two above passes by emptying every call.
    const usable = decomposeNotePitch({
      f0Hz: curve,
      frameRate: fixtureFrameRate,
      medianHz: centreHz,
    });
    expect(usable.centreHz).toBe(centreHz);
    expect(usable.driftCents).toHaveLength(curveFrames);
  });

  it('rejects malformed arguments', () => {
    const base = { f0Hz: curve, frameRate: fixtureFrameRate, medianHz: centreHz };
    expectInvalidParameter(() => decomposeNotePitch({ ...base, f0Hz: new Float32Array(0) }));
    // A frame carrying no pitch is spelled zero, negative or non-finite, and
    // all three are read rather than refused. The frame rate and the centre
    // below are still rejected, so this is not a blanket acceptance.
    for (const noPitch of [Number.NaN, Number.POSITIVE_INFINITY, Number.NEGATIVE_INFINITY, -1]) {
      const track = Float32Array.from(curve);
      track[7] = noPitch;
      expect(decomposeNotePitch({ ...base, f0Hz: track }).centreHz).toBeGreaterThan(0);
    }
    for (const badRate of [0, -100, Number.NaN, Number.POSITIVE_INFINITY]) {
      expectInvalidParameter(() => decomposeNotePitch({ ...base, frameRate: badRate }));
    }
    // 0 is the no-pitch spelling and 0 is the default cutoff, so only a value
    // that cannot be either at all is rejected.
    for (const bad of [-1, Number.NaN, Number.POSITIVE_INFINITY, Number.NEGATIVE_INFINITY]) {
      expectInvalidParameter(() => decomposeNotePitch({ ...base, medianHz: bad }));
      expectInvalidParameter(() => decomposeNotePitch({ ...base, vibratoCutoffHz: bad }));
    }
    // Positive control: the same call with nothing poisoned succeeds.
    expect(decomposeNotePitch(base).driftCents).toHaveLength(curveFrames);
  });
});

describe('splitNote', () => {
  it('keeps every note pointing at its own frames', () => {
    const source = gappedSource();
    const before = extractNotes(source);
    expect(before).toHaveLength(3);
    expect(before.map((note) => [note.frameStart, note.frameEnd])).toEqual([
      [0, 10],
      [12, 25],
      [27, 40],
    ]);
    // No two amplitude values agree, so comparing a curve against the wrong one
    // cannot pass.
    const everyValue = before.flatMap((note) => [...note.amplitude]).sort((a, b) => a - b);
    for (let i = 1; i < everyValue.length; i++) {
      expect(everyValue[i] - everyValue[i - 1]).toBeGreaterThan(1e-4);
    }

    const split = splitNote({ ...source, notes: before, index: 1, frame: 18 });
    expect(split).toHaveLength(4);

    // The cut lands where it was asked for and the spans stay contiguous.
    expect(split[1].frameStart).toBe(12);
    expect(split[1].frameEnd).toBe(18);
    expect(split[2].frameStart).toBe(18);
    expect(split[2].frameEnd).toBe(25);
    expect(split[1].offsetSample).toBe(split[2].onsetSample);
    expect(split[1].onsetSample).toBe(before[1].onsetSample);
    expect(split[2].offsetSample).toBe(before[1].offsetSample);

    // Every note, the untouched ones included, holds its own values — a wrapper
    // that cannot supply a pass-through note's curve produces spans that look
    // right over a neighbour's data, which only the values catch.
    expect(split[0].amplitude).toEqual(before[0].amplitude);
    expect(split[3].amplitude).toEqual(before[2].amplitude);
    expect(Float32Array.from([...split[1].amplitude, ...split[2].amplitude])).toEqual(
      before[1].amplitude,
    );

    // The untouched notes are re-derived rather than copied, and come back equal.
    for (const [after, original] of [
      [split[0], before[0]],
      [split[3], before[2]],
    ] as const) {
      expect(after.onsetSample).toBe(original.onsetSample);
      expect(after.offsetSample).toBe(original.offsetSample);
      expect(after.medianHz).toBe(original.medianHz);
      expect(after.medianCents).toBe(original.medianCents);
      expect(after.f0Stability).toBe(original.f0Stability);
    }

    // No note carried an envelope in, so none comes back with one.
    for (const note of split) {
      expect(note.edit.amplitudeEnvelope).toHaveLength(0);
    }
  });

  it('normalizes a timeStretchRatio of 0 to 1', () => {
    const source = plainSource();
    const before = extractNotes(source);
    expect(before).toHaveLength(1);

    // The identity ratio is spelled 0 on the way in, and the split re-derives
    // its notes, so what comes back is the 1.0 spelling of the same edit.
    const zeroed = [{ ...before[0], edit: { timeStretchRatio: 0 } }];
    const split = splitNote({ ...source, notes: zeroed, index: 0, frame: 20 });
    expect(split).toHaveLength(2);
    for (const half of split) {
      expect(half.edit.timeStretchRatio).toBe(1);
      expect(half.edit.gainDb).toBe(0);
      expect(half.edit.muted).toBe(false);
      expect(half.edit.amplitudeEnvelope).toHaveLength(0);
    }
  });

  it("cuts the source note's envelope at the same proportion as the span", () => {
    const source = plainSource();
    const before = extractNotes(source);
    expect(before[0].frameStart).toBe(0);
    expect(before[0].frameEnd).toBe(40);

    const withEnvelope = [
      { ...before[0], edit: { amplitudeEnvelope: new Float32Array([0.25, 1]) } },
    ];
    const split = splitNote({ ...source, notes: withEnvelope, index: 0, frame: 20 });
    expect(split).toHaveLength(2);
    const first = split[0].edit.amplitudeEnvelope;
    const second = split[1].edit.amplitudeEnvelope;
    expect(first.length).toBeGreaterThan(0);
    expect(second.length).toBeGreaterThan(0);

    // The cut is at the note's midpoint, so a 0.25 -> 1.0 ramp parts at 0.625.
    expect(first[0]).toBeCloseTo(0.25, 4);
    expect(first[first.length - 1]).toBeCloseTo(0.625, 1);
    expect(second[0]).toBeCloseTo(first[first.length - 1], 4);
    expect(second[second.length - 1]).toBeCloseTo(1, 4);
    // Nothing outside the source note's own two points reached either half.
    for (const value of [...first, ...second]) {
      expect(value).toBeGreaterThanOrEqual(0.25 - 1e-5);
      expect(value).toBeLessThanOrEqual(1 + 1e-5);
    }
  });

  it('gives both halves a one-entry envelope unchanged, because it is a constant', () => {
    const source = plainSource();
    const before = extractNotes(source);
    const withEnvelope = [{ ...before[0], edit: { amplitudeEnvelope: new Float32Array([0.5]) } }];
    const split = splitNote({ ...source, notes: withEnvelope, index: 0, frame: 20 });
    for (const half of split) {
      expect(half.edit.amplitudeEnvelope).toEqual(new Float32Array([0.5]));
    }
  });

  it('rejects an out-of-range index and a frame outside the note', () => {
    const source = gappedSource();
    const before = extractNotes(source);
    expect(before).toHaveLength(3);

    for (const index of [before.length, before.length + 4]) {
      expectInvalidParameter(() => splitNote({ ...source, notes: before, index, frame: 18 }));
    }
    expectInvalidParameter(() => splitNote({ ...source, notes: [], index: 0, frame: 18 }));

    // Strictly inside the note's own span, so neither of its boundaries is a
    // legal cut and neither is a frame belonging to another note.
    for (const frame of [12, 25, 5, 30, -1, 100]) {
      expectInvalidParameter(() => splitNote({ ...source, notes: before, index: 1, frame }));
    }

    // A note the set cannot describe: an empty span, and one running past the
    // track the whole set is re-derived against.
    const emptySpan = before.map((note, index) =>
      index === 1 ? { ...note, frameEnd: note.frameStart } : note,
    );
    expectInvalidParameter(() => splitNote({ ...source, notes: emptySpan, index: 0, frame: 5 }));
    const beyond = before.map((note, index) =>
      index === 2 ? { ...note, frameEnd: fixtureFrames + 1 } : note,
    );
    expectInvalidParameter(() => splitNote({ ...source, notes: beyond, index: 0, frame: 5 }));

    // The track arguments extraction itself rejects.
    expectInvalidParameter(() =>
      splitNote({ ...source, voiced: undefined, notes: before, index: 1, frame: 18 }),
    );
    expectInvalidParameter(() =>
      splitNote({ ...source, frameRate: 0, notes: before, index: 1, frame: 18 }),
    );
    expectInvalidParameter(() =>
      splitNote({
        ...source,
        notes: before.map((note) => ({
          ...note,
          edit: { amplitudeEnvelope: new Float32Array([1, -1]) },
        })),
        index: 1,
        frame: 18,
      }),
    );

    // Positive controls: one frame in from either end of the note is legal, and
    // both halves keep a span.
    for (const frame of [13, 24]) {
      const split = splitNote({ ...source, notes: before, index: 1, frame });
      expect(split).toHaveLength(4);
      expect(split[1].frameEnd).toBe(frame);
      expect(split[2].frameStart).toBe(frame);
    }
  });
});

describe('mergeNotes', () => {
  it('spans the gap it joins over and keeps every note aligned', () => {
    const source = gappedSource();
    const before = extractNotes(source);
    expect(before).toHaveLength(3);

    const merged = mergeNotes({ ...source, notes: before, first: 0, last: 1 });
    // last - first notes go away, so the count drops by exactly one here.
    expect(merged).toHaveLength(2);
    expect(merged[0].frameStart).toBe(0);
    expect(merged[0].frameEnd).toBe(25);
    expect(merged[0].onsetSample).toBe(before[0].onsetSample);
    expect(merged[0].offsetSample).toBe(before[1].offsetSample);
    expect(merged[1].amplitude).toEqual(before[2].amplitude);

    // The merged note's curve covers the gap the segmenter cut at, so it holds
    // the two frames neither neighbour carried.
    const joined = merged[0].amplitude;
    expect(joined).toHaveLength(25);
    expect(Float32Array.from(joined.subarray(0, 10))).toEqual(before[0].amplitude);
    expect(Float32Array.from(joined.subarray(12))).toEqual(before[1].amplitude);
    // The gap's own amplitude lives in the audio, not in either neighbour.
    expect(joined[10]).toBeGreaterThan(0);
    expect(joined[11]).toBeGreaterThan(0);
  });

  it("takes the first note's edit", () => {
    const source = gappedSource();
    const before = extractNotes(source);
    const edits = [{ gainDb: -3, pitchShiftSemitones: 2 }, { gainDb: 9, muted: true }, undefined];
    const edited = before.map((note, index) =>
      edits[index] ? { ...note, edit: edits[index] } : note,
    );

    const merged = mergeNotes({ ...source, notes: edited, first: 0, last: 1 });
    expect(merged).toHaveLength(2);
    expect(merged[0].edit.gainDb).toBe(-3);
    expect(merged[0].edit.pitchShiftSemitones).toBe(2);
    // The second note's edit does not survive: the rule is the first note's
    // edit, not a merge of the two.
    expect(merged[0].edit.muted).toBe(false);
  });

  it('undoes a split, returning the spans and the curves it started from', () => {
    const source = gappedSource();
    const before = extractNotes(source);
    const split = splitNote({ ...source, notes: before, index: 1, frame: 18 });
    expect(split).toHaveLength(4);

    const rejoined = mergeNotes({ ...source, notes: split, first: 1, last: 2 });
    expect(rejoined).toHaveLength(before.length);
    for (let i = 0; i < rejoined.length; i++) {
      expect(rejoined[i].onsetSample).toBe(before[i].onsetSample);
      expect(rejoined[i].offsetSample).toBe(before[i].offsetSample);
      expect(rejoined[i].frameStart).toBe(before[i].frameStart);
      expect(rejoined[i].frameEnd).toBe(before[i].frameEnd);
      expect(rejoined[i].medianHz).toBe(before[i].medianHz);
      expect(rejoined[i].amplitude).toEqual(before[i].amplitude);
    }
  });

  it('rejects a first/last pair that is not an ascending in-range run', () => {
    const source = gappedSource();
    const before = extractNotes(source);
    expect(before).toHaveLength(3);

    // A run of one is not a merge, and a run cannot run backwards.
    for (const [first, last] of [
      [1, 1],
      [2, 1],
      [0, before.length],
      [before.length, before.length + 1],
    ]) {
      expectInvalidParameter(() => mergeNotes({ ...source, notes: before, first, last }));
    }
    expectInvalidParameter(() => mergeNotes({ ...source, notes: [], first: 0, last: 1 }));

    // A note the set cannot describe, the same two ways a split rejects.
    const emptySpan = before.map((note, index) =>
      index === 2 ? { ...note, frameEnd: note.frameStart } : note,
    );
    expectInvalidParameter(() => mergeNotes({ ...source, notes: emptySpan, first: 0, last: 1 }));
    const beyond = before.map((note, index) =>
      index === 2 ? { ...note, frameEnd: fixtureFrames + 1 } : note,
    );
    expectInvalidParameter(() => mergeNotes({ ...source, notes: beyond, first: 0, last: 1 }));

    // The track arguments extraction itself rejects.
    expectInvalidParameter(() =>
      mergeNotes({ ...source, voiced: undefined, notes: before, first: 0, last: 1 }),
    );
    expectInvalidParameter(() =>
      mergeNotes({ ...source, frameRate: 0, notes: before, first: 0, last: 1 }),
    );

    // Positive control: merging the whole run collapses the list to one note
    // over the whole span, so none of the above passes by rejecting every merge.
    const all = mergeNotes({ ...source, notes: before, first: 0, last: 2 });
    expect(all).toHaveLength(1);
    expect(all[0].frameStart).toBe(before[0].frameStart);
    expect(all[0].frameEnd).toBe(before[2].frameEnd);
  });
});

/**
 * The one fixture here measured rather than hand-built: whether the shipped
 * pitchPyin-to-extractNotes example runs is a question about what pitchPyin
 * actually returns, so a hand-written track cannot answer it.
 */
describe('the documented pitchPyin to extractNotes pipeline', () => {
  const pipelineLength = sampleRate;

  // A tone with its middle third silenced, so pYIN has genuinely unvoiced
  // frames to report. A continuous tone has none, and the first case below
  // would then pass on an input too clean to tell.
  const gappedTone = (() => {
    const out = new Float32Array(pipelineLength);
    for (let i = 0; i < pipelineLength; i++) {
      out[i] = 0.4 * Math.sin((2 * Math.PI * 220 * i) / sampleRate);
    }
    out.fill(0, Math.floor(pipelineLength / 3), Math.floor((2 * pipelineLength) / 3));
    return out;
  })();

  it('segments the default track, whose unvoiced frames are NaN', () => {
    const pitch = pitchPyin({ samples: gappedTone, sampleRate });

    // Both halves of the track are real: the silence is NaN and the tone is
    // not, so the success below is the NaN frames being read as carrying no
    // pitch rather than an artefact of a degenerate fixture.
    const nanFrames = [...pitch.f0].filter((hz) => Number.isNaN(hz)).length;
    expect(nanFrames).toBeGreaterThan(0);
    expect(nanFrames).toBeLessThan(pitch.f0.length);
    expect(pitch.voicedFlag.filter(Boolean).length).toBeGreaterThan(0);

    const notes = extractNotes({
      samples: gappedTone,
      sampleRate,
      f0Hz: pitch.f0,
      voiced: pitch.voicedFlag,
      frameRate,
      minNoteMs: 40,
    });
    expect(notes.length).toBeGreaterThan(0);
    for (const note of notes) {
      expect(note.medianHz).toBeCloseTo(220, 0);
    }
  });

  it('segments the track fillNa produces', () => {
    const pitch = pitchPyin({ samples: gappedTone, sampleRate, fillNa: true });
    // The track lives on `f0`; `f0Hz` is the name extractNotes reads it under.
    expect(pitch.f0).toBeInstanceOf(Float32Array);
    expect([...pitch.f0].every((hz) => Number.isFinite(hz))).toBe(true);

    const notes = extractNotes({
      samples: gappedTone,
      sampleRate,
      f0Hz: pitch.f0,
      voiced: pitch.voicedFlag,
      frameRate,
      minNoteMs: 40,
    });
    expect(notes.length).toBeGreaterThan(0);
    for (const note of notes) {
      expect(note.offsetSample).toBeGreaterThan(note.onsetSample);
      expectWithinRel(note.medianHz, 220, 0.02);
    }

    // The notes go back where they came from: the whole point of extracting
    // them is to edit and render them.
    notes[0].edit.pitchShiftSemitones = 1;
    expect(renderNotes({ samples: gappedTone, sampleRate, notes })).toHaveLength(pipelineLength);
  });
});

describe('an index into a note set is refused above the addressable range', () => {
  // The shared count reader accepts the whole JS safe-integer range, and its
  // narrowing saturates on wasm32, so 2**32 and 2**40 both arrive as the last
  // address. For a position in a buffer that is not a clamp, it is a different
  // note -- and for a PAIR it is worse, because two saturated indices compare
  // equal and any ordering test they have to satisfy passes on two notes that
  // do not exist.
  //
  // ASSERTING A REFUSAL WOULD BE VACUOUS HERE, which is why these read the
  // message. A saturated index is out of the note set's range too, so the old
  // behaviour also threw -- from the set's bound check, about a note that was
  // never asked for. Only the sender of the refusal tells the two apart.
  const BEYOND = 2 ** 32;
  const ADDRESS_REFUSAL = /is larger than this build can address/;

  function expectAddressRefusal(action: () => void): string {
    let caught: unknown;
    try {
      action();
    } catch (error) {
      caught = error;
    }
    expect(caught, 'expected a refusal, got none').toBeDefined();
    expect(isSonareError(caught)).toBe(true);
    expect((caught as { code: number }).code).toBe(ErrorCode.InvalidParameter);
    const message = String((caught as Error).message);
    expect(message).toMatch(ADDRESS_REFUSAL);
    return message;
  }

  it('refuses a splitNote index past the address space, as an index', () => {
    const source = gappedSource();
    const before = extractNotes(source);
    expect(before).toHaveLength(3);
    // The control, first: a real index is accepted and does split.
    expect(splitNote({ ...source, notes: before, index: 1, frame: 18 })).toHaveLength(4);
    for (const bad of [BEYOND, BEYOND + 100, 2 ** 40]) {
      const message = expectAddressRefusal(() =>
        splitNote({ ...source, notes: before, index: bad, frame: 18 }),
      );
      expect(message).toContain('splitNote index');
    }
    // And an index merely past the end of the set is still the set's own
    // refusal, not this one -- the two failures stay distinguishable.
    expectInvalidParameter(() => splitNote({ ...source, notes: before, index: 99, frame: 18 }));
    let pastEnd: unknown;
    try {
      splitNote({ ...source, notes: before, index: 99, frame: 18 });
    } catch (error) {
      pastEnd = error;
    }
    expect(String((pastEnd as Error).message)).not.toMatch(ADDRESS_REFUSAL);
  });

  it('refuses a mergeNotes pair that is entirely out of range', () => {
    // The case no single-argument test can reach: saturated, BEYOND + 1 and
    // BEYOND + 2 are one number, so the ordering the pair must satisfy holds
    // and the pair reaches the set as a single in-range-looking index.
    const source = gappedSource();
    const before = extractNotes(source);
    expect(before).toHaveLength(3);
    // The control: a real pair merges and the set shrinks.
    expect(mergeNotes({ ...source, notes: before, first: 0, last: 1 })).toHaveLength(2);

    const pair = expectAddressRefusal(() =>
      mergeNotes({ ...source, notes: before, first: BEYOND + 1, last: BEYOND + 2 }),
    );
    expect(pair).toContain('mergeNotes first');
    const single = expectAddressRefusal(() =>
      mergeNotes({ ...source, notes: before, first: 0, last: BEYOND }),
    );
    expect(single).toContain('mergeNotes last');
  });
});
