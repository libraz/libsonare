import { beforeAll, describe, expect, it } from 'vitest';
import {
  analyze,
  detectChords,
  detectKey,
  estimateTuning,
  init,
  isSonareError,
  referenceHzToTuning,
  transcribe,
  tuningToReferenceHz,
} from '../src/index';

const sampleRate = 22050;
/** A4 = 446 Hz, as a semitone fraction. */
const detune = 12 * Math.log2(446 / 440);

/** I-V-vi-IV in C major at 120 BPM, re-struck every beat, every partial detuned. */
function detunedPopLoop(detuneSemitones: number): Float32Array {
  const chords = [
    [48, 60, 64, 67],
    [43, 59, 62, 67],
    [45, 60, 64, 69],
    [41, 60, 65, 69],
  ];
  const beat = Math.floor(0.5 * sampleRate);
  const out = new Float32Array(chords.length * 4 * beat);
  let i = 0;
  for (const chord of chords) {
    const freqs = chord.map((m) => 440 * 2 ** ((m - 69 + detuneSemitones) / 12));
    for (let b = 0; b < 4; b++) {
      for (let n = 0; n < beat; n++) {
        const t = n / sampleRate;
        const env = Math.exp(-3 * t) * Math.min(1, t * 200);
        let value = 0;
        for (const f of freqs) {
          for (let h = 1; h <= 4; h++) {
            value += Math.sin(2 * Math.PI * f * h * t) / h;
          }
        }
        out[i++] = 0.08 * env * value;
      }
    }
  }
  return out;
}

const loop = detunedPopLoop(detune);
let measured = 0;

beforeAll(async () => {
  await init();
  measured = estimateTuning(loop, sampleRate);
});

describe("tuning: 'auto'", () => {
  it('measures the semitone fraction of an A4 = 446 Hz recording on every entry point', () => {
    expect(measured).toBeCloseTo(detune, 1);
    const chords = detectChords({ samples: loop, sampleRate, tuning: 'auto' });
    expect(chords.tuning).toBeCloseTo(measured, 5);
    expect(detectChords(loop, sampleRate, { tuning: 'auto' }).tuning).toBe(chords.tuning);
    const key = detectKey({ samples: loop, sampleRate, tuning: 'auto' });
    expect(key.tuning).toBeCloseTo(measured, 5);
    expect(key.tuning).toBeCloseTo(detune, 1);
    expect(analyze({ samples: loop, sampleRate, tuning: 'auto' }).tuning).toBeCloseTo(measured, 5);
  });

  it('reports a given tuning as given, and 0 by default', () => {
    expect(detectChords(loop, sampleRate, { tuning: 0.1 }).tuning).toBeCloseTo(0.1, 6);
    expect(detectChords(loop, sampleRate).tuning).toBe(0);
    expect(detectKey(loop, sampleRate, { tuning: 0.2 }).tuning).toBeCloseTo(0.2, 6);
    expect(detectKey(loop, sampleRate).tuning).toBe(0);
    expect(analyze(loop, sampleRate, { tuning: -0.3 }).tuning).toBeCloseTo(-0.3, 6);
  });

  it('gives the result a measured tuning applied explicitly would give', () => {
    const auto = detectKey({ samples: loop, sampleRate, tuning: 'auto' });
    const explicit = detectKey({ samples: loop, sampleRate, tuning: measured });
    expect(auto).toEqual(explicit);
  });

  it('refuses a tuning that is neither a number nor auto, by name', () => {
    expect(() => detectKey({ samples: loop, sampleRate, tuning: 'other' as never })).toThrow(
      /tuning must be a number or 'auto'/,
    );
    expect(() => detectKey({ samples: loop, sampleRate, tuning: 0.7 })).toThrow();
  });
});

describe('tuning converters', () => {
  it('round-trip and name the reference pitch', () => {
    expect(tuningToReferenceHz(0)).toBeCloseTo(440, 4);
    expect(tuningToReferenceHz(detune)).toBeCloseTo(446, 1);
    expect(referenceHzToTuning(446)).toBeCloseTo(detune, 3);
    expect(tuningToReferenceHz(0, 432)).toBeCloseTo(432, 4);
    for (const tuning of [-0.5, -0.2, 0.2349, 0.49, 7]) {
      expect(referenceHzToTuning(tuningToReferenceHz(tuning, 432), 432)).toBeCloseTo(tuning, 3);
    }
  });

  it('refuse values outside their domain', () => {
    expect(() => tuningToReferenceHz(Number.NaN)).toThrow();
    expect(() => tuningToReferenceHz(0, 0)).toThrow();
    expect(() => referenceHzToTuning(-1)).toThrow();
    let thrown: unknown;
    try {
      referenceHzToTuning(440, 0);
    } catch (error) {
      thrown = error;
    }
    expect(isSonareError(thrown)).toBe(true);
  });
});

describe("transcribe referenceHz: 'auto'", () => {
  function tones(stretch: number): Float32Array {
    const per = Math.round(sampleRate * 0.4);
    const gap = Math.round(sampleRate * 0.06);
    const notes = [261.626, 329.628, 391.995];
    const out = new Float32Array((per + gap) * notes.length);
    let write = 0;
    for (const hz of notes) {
      for (let i = 0; i < per; i++) {
        const envelope = Math.min(1, i / 200, (per - i) / 200);
        out[write++] = 0.5 * envelope * Math.sin((2 * Math.PI * hz * stretch * i) / sampleRate);
      }
      write += gap;
    }
    return out;
  }

  it('measures the reference and reports the tuning used', () => {
    const result = transcribe({
      samples: tones(2 ** (0.3 / 12)),
      sampleRate,
      tempoBpm: 120,
      referenceHz: 'auto',
    });
    expect(result.noteCount).toBeGreaterThan(0);
    expect(result.tuning).toBeCloseTo(0.3, 1);
  });

  it('reports the tuning a given reference amounts to', () => {
    const result = transcribe({ samples: tones(1), sampleRate, tempoBpm: 120, referenceHz: 432 });
    expect(result.tuning).toBeCloseTo(referenceHzToTuning(432), 4);
    expect(transcribe({ samples: tones(1), sampleRate, tempoBpm: 120 }).tuning).toBe(0);
  });

  it('refuses a reference that is neither a number nor auto', () => {
    expect(() =>
      transcribe({ samples: tones(1), sampleRate, referenceHz: 'other' as never }),
    ).toThrow(/referenceHz must be a positive number or 'auto'/);
  });
});
