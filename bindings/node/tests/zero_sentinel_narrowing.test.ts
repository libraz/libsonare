/**
 * Fractional values on the Node fields whose zero is not a quantity.
 *
 * The addon's integer readers truncate, so `0.5` reaches the callee as 0. On a
 * field whose 0 means "keep the library default" -- or, for an edit offset,
 * "make no change" -- that is a category change reported as success: the call
 * returns exactly what a caller who asked for nothing would have got. The
 * facade refuses such a value instead.
 *
 * Every case carries its own positive control. `differs` is asserted against a
 * second legitimate value that moves the result, because "the fractional
 * request equals the omitted one" is equally consistent with the field doing
 * nothing at all. The omitted, zero and control outcomes are asserted too, so a
 * guard that also changed what the accepted values do fails here.
 *
 * The last block is the other half of the measurement: fields sitting in the
 * same options bag whose control did NOT move the result, which is why they are
 * left truncating.
 */

import { describe, expect, it } from 'vitest';
import type { NoteObject, PercussiveEvent } from '../src/index.js';
import {
  analyzePolyphonic,
  decomposeStems,
  estimateRoom,
  extractNotes,
  extractPercussiveEvents,
  meteringSpectrum,
  meteringSpectrumFrame,
  Project,
  renderNotes,
  renderPercussiveEvents,
  roomMorph,
  spectralEdit,
  synthesizeRir,
} from '../src/index.js';

const SR = 22050;
const HOP = 512;
const N_FRAMES = 43;
const LENGTH = N_FRAMES * HOP;
const FRAME_RATE = SR / HOP;
// The pitch steps here, so the note segmenter splits the buffer into two.
const SPLIT_FRAME = 22;
const SPLIT_SAMPLE = SPLIT_FRAME * HOP;

const PROJECT_SAMPLE_RATE = 48000;

/** Tone plus periodic transients, so both a spectrum and an onset set are observable. */
function mixedTone(length: number): Float32Array {
  const out = new Float32Array(length);
  for (let i = 0; i < length; i += 1) {
    out[i] =
      0.5 * Math.sin((2 * Math.PI * 220 * i) / SR) +
      0.3 * Math.sin((2 * Math.PI * 990 * i) / SR) +
      (i % 2205 < 120 ? 0.6 * Math.sin((2 * Math.PI * 3000 * i) / SR) : 0);
  }
  return out;
}

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

const MIXED = mixedTone(SR);
const SHORT = MIXED.subarray(0, 8192);
const EDIT_INPUT = MIXED.subarray(0, 16384);
const PITCHED = twoNoteTone();

/** Order-sensitive digest, so a moved or rescaled sample changes it. */
function digest(samples: Float32Array | number[]): string {
  let accumulator = 2166136261;
  for (let i = 0; i < samples.length; i += 1) {
    accumulator =
      Math.imul((accumulator ^ (Math.round(samples[i] * 1e6) | 0)) >>> 0, 16777619) >>> 0;
  }
  return `${samples.length}:${accumulator.toString(16)}`;
}

/**
 * Drives one field through the four cases: omitted, the documented zero, a
 * control value that has to move the result, and the two fractional requests.
 */
function expectSentinelRefused<T>(
  call: (value?: number) => T,
  read: (result: T) => string,
  control: number,
): void {
  const omitted = read(call());
  expect(read(call(0))).toBe(omitted);
  expect(read(call(control))).not.toBe(omitted);
  // Matched on the message, so an unrelated throw from further down the call
  // cannot stand in for the refusal this asserts.
  expect(() => call(0.5)).toThrow(/must be an integer/);
  expect(() => call(1.5)).toThrow(/must be an integer/);
}

describe('decomposeStems counts', () => {
  const base = { samples: SHORT, sampleRate: SR, nComponents: 2, nIter: 5 };
  const read = (result: ReturnType<typeof decomposeStems>) =>
    `${result.components.length}/${digest(result.w)}`;

  it('refuses a fractional nComponents rather than separating into four', () => {
    expectSentinelRefused(
      (nComponents) => decomposeStems({ ...base, nComponents, nIter: 8 }),
      read,
      3,
    );
  });

  it('refuses a fractional nIter rather than running the default schedule', () => {
    expectSentinelRefused((nIter) => decomposeStems({ ...base, nIter }), read, 3);
  });

  it('refuses a fractional nFft rather than transforming at the default size', () => {
    expectSentinelRefused((nFft) => decomposeStems({ ...base, nFft }), read, 1024);
  });

  it('refuses a fractional hopLength rather than framing at the default hop', () => {
    expectSentinelRefused((hopLength) => decomposeStems({ ...base, hopLength }), read, 256);
  });
});

describe('spectralEdit framing and heal radius', () => {
  const muteOps = [
    { startSample: 1000, endSample: 6000, lowHz: 2500, highHz: 3500, mode: 'mute' as const },
  ];
  const healOps = [
    { startSample: 3000, endSample: 8000, lowHz: 2500, highHz: 3500, mode: 'heal' as const },
  ];
  const base = { samples: EDIT_INPUT, sampleRate: SR, ops: muteOps };

  it('refuses a fractional nFft rather than transforming at the default size', () => {
    expectSentinelRefused((nFft) => spectralEdit({ ...base, nFft }), digest, 1024);
  });

  it('refuses a fractional hopLength rather than framing at the default hop', () => {
    expectSentinelRefused((hopLength) => spectralEdit({ ...base, hopLength }), digest, 256);
  });

  it('refuses a fractional healRadiusFrames rather than healing over two frames', () => {
    expectSentinelRefused(
      (healRadiusFrames) => spectralEdit({ ...base, ops: healOps, healRadiusFrames }),
      digest,
      9,
    );
  });
});

describe('metering spectrum sizes', () => {
  const read = (result: { nFft: number; magnitude: Float32Array }) =>
    `${result.nFft}/${digest(result.magnitude)}`;

  it('refuses a fractional nFft rather than analysing at the default size', () => {
    expectSentinelRefused(
      (nFft) => meteringSpectrum({ samples: MIXED, sampleRate: SR, nFft }),
      read,
      512,
    );
  });

  it('refuses a fractional octaveFraction rather than smoothing at a third octave', () => {
    expectSentinelRefused(
      (octaveFraction) =>
        meteringSpectrum({
          samples: MIXED,
          sampleRate: SR,
          applyOctaveSmoothing: true,
          octaveFraction,
        }),
      read,
      12,
    );
  });

  it('refuses a fractional single-frame nFft ahead of the windowed pre-scan', () => {
    expectSentinelRefused(
      (nFft) => meteringSpectrumFrame({ samples: MIXED, sampleRate: SR, frameOffset: 0, nFft }),
      read,
      512,
    );
  });

  it('refuses a fractional single-frame octaveFraction', () => {
    expectSentinelRefused(
      (octaveFraction) =>
        meteringSpectrumFrame({
          samples: MIXED,
          sampleRate: SR,
          frameOffset: 0,
          applyOctaveSmoothing: true,
          octaveFraction,
        }),
      read,
      12,
    );
  });
});

describe('acoustic room options', () => {
  it('refuses a fractional RIR seed rather than seeding the late tail with one', () => {
    expectSentinelRefused(
      (seed) => synthesizeRir({ seed, maxSeconds: 0.4 }),
      (result) => digest(result.rir),
      987654,
    );
  });

  it('refuses a fractional morph seed', () => {
    expectSentinelRefused(
      (seed) => roomMorph({ samples: SHORT, sampleRate: SR, seed }),
      digest,
      987654,
    );
  });

  it('refuses a fractional nOctaveBands rather than estimating over six', () => {
    expectSentinelRefused(
      (nOctaveBands) => estimateRoom({ samples: MIXED, sampleRate: SR, nOctaveBands }),
      (result) => String(result.rt60Bands.length),
      8,
    );
  });
});

describe('note and event edit offsets', () => {
  it('refuses a fractional event shift rather than rendering the hit unmoved', () => {
    const events = extractPercussiveEvents({ samples: MIXED, sampleRate: SR });
    expect(events.length).toBeGreaterThan(0);
    const shifted = (timeOffsetSamples?: number): PercussiveEvent[] =>
      events.map((event, index) =>
        index === 0
          ? {
              ...event,
              edit: {
                ...event.edit,
                ...(timeOffsetSamples === undefined ? {} : { timeOffsetSamples }),
              },
            }
          : event,
      );
    expectSentinelRefused(
      (timeOffsetSamples) =>
        renderPercussiveEvents({
          samples: MIXED,
          sampleRate: SR,
          events: shifted(timeOffsetSamples),
        }),
      digest,
      512,
    );
  });

  it('refuses a fractional note shift rather than rendering the note unmoved', () => {
    const notes = extractNotes({
      samples: PITCHED.samples,
      sampleRate: SR,
      f0Hz: PITCHED.f0Hz,
      frameRate: FRAME_RATE,
      voiced: PITCHED.voiced,
    });
    expect(notes.length).toBeGreaterThan(0);
    const shifted = (timeOffsetSamples?: number): NoteObject[] =>
      notes.map((note, index) =>
        index === 0
          ? {
              ...note,
              edit: {
                ...note.edit,
                ...(timeOffsetSamples === undefined ? {} : { timeOffsetSamples }),
              },
            }
          : note,
      );
    expectSentinelRefused(
      (timeOffsetSamples) =>
        renderNotes({
          samples: PITCHED.samples,
          sampleRate: SR,
          notes: shifted(timeOffsetSamples),
          f0Hz: PITCHED.f0Hz,
          frameRate: FRAME_RATE,
        }),
      digest,
      1024,
    );
  });

  it('refuses a fractional shift on a held polyphonic analysis', () => {
    expectSentinelRefused(
      (timeOffsetSamples) => {
        const analysis = analyzePolyphonic({ samples: PITCHED.samples, sampleRate: SR });
        try {
          expect(analysis.notes().length).toBeGreaterThan(0);
          analysis.setNoteEdit(0, timeOffsetSamples === undefined ? {} : { timeOffsetSamples });
          return analysis.render();
        } finally {
          analysis.destroy();
        }
      },
      digest,
      1024,
    );
  });
});

/** A project with one audio track, one clip and a second source to switch takes to. */
function buildProject(): { project: Project; clipId: number; altSourceId: number } {
  const project = Project.create();
  project.setSampleRate(PROJECT_SAMPLE_RATE);
  const trackId = project.addTrack({ kind: 'audio', name: 'lead' });
  const audio = new Float32Array(4800);
  for (let i = 0; i < audio.length; i += 1) {
    audio[i] = Math.sin(i * 0.05) * 0.25;
  }
  const clipId = project.addClip({
    trackId,
    startPpq: 0,
    lengthPpq: 4,
    gain: 0.8,
    audio,
    audioChannels: 1,
    audioSampleRate: PROJECT_SAMPLE_RATE,
  });
  const alternate = new Float32Array(4800);
  for (let i = 0; i < alternate.length; i += 1) {
    alternate[i] = Math.sin(i * 0.11) * 0.4;
  }
  project.addClip({
    trackId,
    startPpq: 8,
    lengthPpq: 4,
    audio: alternate,
    audioChannels: 1,
    audioSampleRate: PROJECT_SAMPLE_RATE,
  });
  return { project, clipId, altSourceId: project.sourceByIndex(1).id };
}

/** Runs `body` against a fresh project, since each case mutates persistent state. */
function onProject<T>(body: (fixture: ReturnType<typeof buildProject>) => T): T {
  const fixture = buildProject();
  try {
    return body(fixture);
  } finally {
    fixture.project.destroy();
  }
}

describe('project bounce and take selection', () => {
  it('refuses a fractional totalFrames rather than auto-deriving the length', () => {
    expectSentinelRefused(
      (totalFrames) =>
        onProject(({ project }) =>
          project.bounce(totalFrames === undefined ? {} : { totalFrames }),
        ),
      digest,
      9600,
    );
  });

  it('refuses a fractional numChannels rather than rendering stereo', () => {
    expectSentinelRefused(
      (numChannels) =>
        onProject(({ project }) =>
          project.bounce(numChannels === undefined ? {} : { numChannels }),
        ),
      digest,
      1,
    );
  });

  it('refuses a fractional take sourceId rather than reusing the clip source', () => {
    const control = onProject(({ altSourceId }) => altSourceId);
    expectSentinelRefused(
      (sourceId) =>
        onProject(({ project, clipId }) => {
          project.setClipTakes(
            clipId,
            [{ id: 1, sourceOffsetPpq: 0, ...(sourceId === undefined ? {} : { sourceId }) }],
            1,
          );
          return project.toJson();
        }),
      (json) => json.match(/"takes":\[[^\]]*\]/)?.[0] ?? 'absent',
      control,
    );
  });

  it('refuses a fractional activeTakeId rather than selecting the base source', () => {
    expectSentinelRefused(
      (activeTakeId) =>
        onProject(({ project, clipId }) => {
          const takes = [
            { id: 1, sourceOffsetPpq: 0 },
            { id: 2, sourceOffsetPpq: 1 },
          ];
          if (activeTakeId === undefined) {
            project.setClipTakes(clipId, takes);
          } else {
            project.setClipTakes(clipId, takes, activeTakeId);
          }
          return project.toJson();
        }),
      (json) => json.match(/"active_take_id":\d+/)?.[0] ?? 'absent',
      2,
    );
  });

  it('refuses a fractional comp-segment takeId rather than falling back to the active take', () => {
    expectSentinelRefused(
      (takeId) =>
        onProject(({ project, clipId }) => {
          project.setClipTakes(
            clipId,
            [
              { id: 1, sourceOffsetPpq: 0 },
              { id: 2, sourceOffsetPpq: 1 },
            ],
            1,
          );
          project.setClipCompSegments(clipId, [
            { startPpq: 0, endPpq: 4, ...(takeId === undefined ? {} : { takeId }) },
          ]);
          return project.toJson();
        }),
      (json) => json.match(/"comp_segments":\[[^\]]*\]/)?.[0] ?? 'absent',
      2,
    );
  });
});

describe('bounce fields left truncating', () => {
  it('renders identically whichever block size the caller asks for', () => {
    const omitted = onProject(({ project }) => digest(project.bounce()));
    expect(onProject(({ project }) => digest(project.bounce({ blockSize: 0 })))).toBe(omitted);
    expect(onProject(({ project }) => digest(project.bounce({ blockSize: 64 })))).toBe(omitted);
    expect(onProject(({ project }) => digest(project.bounce({ blockSize: 0.5 })))).toBe(omitted);
  });

  it('accepts no sample rate but the project rate, which is what zero selects', () => {
    const omitted = onProject(({ project }) => digest(project.bounce()));
    expect(onProject(({ project }) => digest(project.bounce({ sampleRate: 0 })))).toBe(omitted);
    expect(
      onProject(({ project }) => digest(project.bounce({ sampleRate: PROJECT_SAMPLE_RATE }))),
    ).toBe(omitted);
    expect(() => onProject(({ project }) => project.bounce({ sampleRate: 1.5 }))).toThrow();
  });
});
