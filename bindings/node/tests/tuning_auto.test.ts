import { describe, expect, it } from 'vitest';
import {
  analyze,
  detectChords,
  detectKey,
  detectKeyCandidates,
  estimateTuning,
  referenceHzToTuning,
  transcribe,
  tuningToReferenceHz,
} from '../src/index.js';

const sampleRate = 22050;
/** A4 = 446 Hz, in cents. */
const detuneCents = 1200 * Math.log2(446 / 440);
/** The same detune as a semitone fraction, for the synthesis below. */
const detune = detuneCents / 100;

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
const measured = estimateTuning(loop, sampleRate);

describe("tuningCents: 'auto'", () => {
  it('measures the tuning, in cents, of an A4 = 446 Hz recording on every entry point', () => {
    expect(measured).toBeCloseTo(detuneCents, -1);
    const chords = detectChords({ samples: loop, sampleRate, tuningCents: 'auto' });
    expect(chords.tuningCents).toBeCloseTo(measured, 3);
    expect(detectChords(loop, sampleRate, { tuningCents: 'auto' }).tuningCents).toBe(
      chords.tuningCents,
    );
    const key = detectKey({ samples: loop, sampleRate, tuningCents: 'auto' });
    expect(key.tuningCents).toBeCloseTo(measured, 3);
    expect(key.tuningCents).toBeCloseTo(detuneCents, -1);
    const analysis = analyze({ samples: loop, sampleRate, tuningCents: 'auto' });
    expect(analysis.tuningCents).toBeCloseTo(measured, 3);
  });

  // Each new tuning value rebuilds the constant-Q kernels.
  it('reports a given tuning as given, 0 included, and the measured one by default', () => {
    expect(detectChords(loop, sampleRate, { tuningCents: 10 }).tuningCents).toBeCloseTo(10, 4);
    expect(detectChords(loop, sampleRate, { tuningCents: 0 }).tuningCents).toBe(0);
    expect(detectChords(loop, sampleRate).tuningCents).toBeCloseTo(detuneCents, -1);
    expect(detectKey(loop, sampleRate, { tuningCents: 20 }).tuningCents).toBeCloseTo(20, 4);
    expect(detectKey(loop, sampleRate, { tuningCents: 0 }).tuningCents).toBe(0);
    const byDefault = detectKey(loop, sampleRate);
    expect(byDefault.tuningCents).toBeCloseTo(detuneCents, -1);
    expect(byDefault.tuningCents).toBeCloseTo(measured, 3);
    expect([byDefault.root, byDefault.mode]).toEqual([0, 'major']);
    expect(analyze(loop, sampleRate, { tuningCents: -30 }).tuningCents).toBeCloseTo(-30, 4);
    expect(analyze(loop, sampleRate, { tuningCents: 0 }).tuningCents).toBe(0);
    expect(analyze(loop, sampleRate).tuningCents).toBeCloseTo(detuneCents, -1);
  }, 30_000);

  it('lets the key candidates take a tuning, measured by default', () => {
    const byDefault = detectKeyCandidates(loop, sampleRate);
    expect(byDefault[0].key.root).toBe(0);
    expect(byDefault[0].key.mode).toBe('major');
    expect(detectKeyCandidates({ samples: loop, sampleRate, tuningCents: 'auto' })).toEqual(
      byDefault,
    );
    expect(detectKeyCandidates({ samples: loop, sampleRate, tuningCents: measured })).toEqual(
      byDefault,
    );
    const off = detectKeyCandidates({ samples: loop, sampleRate, tuningCents: 0 });
    expect(off).not.toEqual(byDefault);
    expect(() => detectKeyCandidates({ samples: loop, sampleRate, tuningCents: 70 })).toThrow(
      RangeError,
    );
  });

  it('gives the result a measured tuning applied explicitly would give', () => {
    const auto = detectKey({ samples: loop, sampleRate, tuningCents: 'auto' });
    const explicit = detectKey({ samples: loop, sampleRate, tuningCents: measured });
    expect(auto).toEqual(explicit);
  });

  it('refuses a tuning that is neither a number nor auto, by name', () => {
    expect(() => detectKey({ samples: loop, sampleRate, tuningCents: 'other' as never })).toThrow(
      /tuningCents must be a number or 'auto'/,
    );
    expect(() => detectKey({ samples: loop, sampleRate, tuningCents: 70 })).toThrow(RangeError);
    // The core's guard, which the facade deliberately does not mirror.
    expect(() => detectKey({ samples: loop, sampleRate, tuningCents: 70 })).toThrow(
      /^detectKey: tuning_cents must be/,
    );
  });
});

describe('tuning converters', () => {
  it('round-trip and name the reference pitch', () => {
    expect(tuningToReferenceHz(0)).toBeCloseTo(440, 4);
    expect(tuningToReferenceHz(detuneCents)).toBeCloseTo(446, 1);
    expect(referenceHzToTuning(446)).toBeCloseTo(detuneCents, 1);
    expect(tuningToReferenceHz(0, 432)).toBeCloseTo(432, 4);
    for (const tuning of [-50, -20, 23.49, 49, 700]) {
      expect(referenceHzToTuning(tuningToReferenceHz(tuning, 432), 432)).toBeCloseTo(tuning, 1);
    }
  });

  it('refuse values outside their domain', () => {
    expect(() => tuningToReferenceHz(Number.NaN)).toThrow();
    // The core's plain refusal for each.
    expect(() => tuningToReferenceHz(0, 0)).toThrow(/^tuningToReferenceHz: Invalid parameter$/);
    expect(() => referenceHzToTuning(-1)).toThrow(/^referenceHzToTuning: Invalid parameter$/);
    expect(() => referenceHzToTuning(440, 0)).toThrow(/^referenceHzToTuning: Invalid parameter$/);
    for (const refuse of [
      () => tuningToReferenceHz(0, 0),
      () => referenceHzToTuning(-1),
      () => referenceHzToTuning(440, 0),
    ]) {
      expect(refuse).toThrow(RangeError);
    }
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
    const stretch = 2 ** (0.3 / 12);
    const result = transcribe({
      samples: tones(stretch),
      sampleRate,
      tempoBpm: 120,
      referenceHz: 'auto',
    });
    expect(result.noteCount).toBeGreaterThan(0);
    expect(result.tuningCents).toBeCloseTo(30, -1);
  });

  it('reports the tuning a given reference amounts to', () => {
    const result = transcribe({
      samples: tones(1),
      sampleRate,
      tempoBpm: 120,
      referenceHz: 432,
    });
    // The transcribe result reports the tuning in cents.
    expect(result.tuningCents).toBeCloseTo(referenceHzToTuning(432), 2);
    expect(transcribe({ samples: tones(1), sampleRate, tempoBpm: 120 }).tuningCents).toBe(0);
  });

  it('refuses a reference that is neither a number nor auto', () => {
    expect(() =>
      transcribe({ samples: tones(1), sampleRate, referenceHz: 'other' as never }),
    ).toThrow(/referenceHz must be a number or 'auto'/);
  });
});
