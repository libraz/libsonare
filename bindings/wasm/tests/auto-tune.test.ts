/**
 * Tests for scaleMaskForMode, the named scaleModeMask and autoTune (WASM).
 */

import { beforeAll, describe, expect, it } from 'vitest';
import {
  autoTune,
  detectKey,
  init,
  type KeyMode,
  PitchClass,
  type PitchCorrectTimevaryingRequest,
  pitchCorrectTimevarying,
  pitchPyin,
  scaleMaskForMode,
} from '../src/index';

const SR = 22050;
const NOTE_SECONDS = 0.5;
const DETUNE_CENTS = 35;
// pYIN quantizes pitch to 10-cent bins; the corrected pitch is judged to within one bin.
const TOLERANCE_CENTS = 10;
const MELODY = [60, 64, 67, 64, 62, 65, 69, 67, 60, 64, 67, 72];

const MASKS: Record<KeyMode, number> = {
  major: 0b101010110101,
  minor: 0b010110101101,
  dorian: 0b011010101101,
  phrygian: 0b010110101011,
  lydian: 0b101011010101,
  mixolydian: 0b011010110101,
  locrian: 0b010101101011,
};

function detunedMelody(detuneCents: number): Float32Array {
  const noteSamples = Math.floor(NOTE_SECONDS * SR);
  const out = new Float32Array(noteSamples * MELODY.length);
  MELODY.forEach((midi, note) => {
    const hz = 440 * 2 ** ((midi - 69 + detuneCents / 100) / 12);
    for (let i = 0; i < noteSamples; i++) {
      const t = i / SR;
      let v = 0;
      for (let h = 1; h <= 3; h++) {
        v += Math.sin(2 * Math.PI * hz * h * t) / h;
      }
      const fade = Math.min(1, i / 200, (noteSamples - i) / 200);
      out[note * noteSamples + i] = 0.3 * fade * v;
    }
  });
  return out;
}

function medianMidi(audio: Float32Array, note: number): number {
  const noteSamples = Math.floor(NOTE_SECONDS * SR);
  const begin = note * noteSamples + noteSamples / 2;
  const end = (note + 1) * noteSamples - noteSamples / 8;
  const pitch = pitchPyin({ samples: audio.slice(begin, end), sampleRate: SR });
  const midi: number[] = [];
  pitch.f0.forEach((f0, i) => {
    if (pitch.voicedFlag[i] && Number.isFinite(f0)) {
      midi.push(69 + 12 * Math.log2(f0 / 440));
    }
  });
  midi.sort((a, b) => a - b);
  return midi[Math.floor(midi.length / 2)];
}

describe('scaleMaskForMode (WASM)', () => {
  beforeAll(async () => {
    await init();
  });

  it('returns each mode as its 12-bit mask, for every root', () => {
    for (const [name, mask] of Object.entries(MASKS)) {
      for (const root of [0, 5, 11] as const) {
        expect(scaleMaskForMode(root, name as KeyMode)).toBe(mask);
      }
    }
  });

  it('refuses an unknown mode and an out-of-range root', () => {
    expect(() => scaleMaskForMode(0, 'whole-tone' as KeyMode)).toThrow();
    expect(() => scaleMaskForMode(12 as PitchClass, 'major')).toThrow();
    expect(() => scaleMaskForMode(-1 as PitchClass, 'major')).toThrow();
  });
});

describe('scaleModeMask as a mode name (WASM)', () => {
  const samples = detunedMelody(DETUNE_CENTS);
  let base: PitchCorrectTimevaryingRequest;

  beforeAll(async () => {
    await init();
    const pitch = pitchPyin({ samples, sampleRate: SR });
    base = {
      samples,
      sampleRate: SR,
      f0Hz: pitch.f0,
      voiced: Uint8Array.from(pitch.voicedFlag, (flag) => (flag ? 1 : 0)),
      targetMode: 'scale',
    };
  });

  it('corrects exactly as the numeric mask does', () => {
    for (const name of ['major', 'dorian'] as const) {
      const byName = pitchCorrectTimevarying({ ...base, scaleModeMask: name });
      const byMask = pitchCorrectTimevarying({ ...base, scaleModeMask: MASKS[name] });
      expect(byName).toEqual(byMask);
    }
  });

  it('refuses an unknown name', () => {
    expect(() => pitchCorrectTimevarying({ ...base, scaleModeMask: 'nope' as KeyMode })).toThrow();
  });
});

describe('autoTune (WASM)', () => {
  const input = detunedMelody(DETUNE_CENTS);

  beforeAll(async () => {
    await init();
  });

  it('lands a detuned melody on the key scale', () => {
    const { samples, key } = autoTune({
      samples: input,
      sampleRate: SR,
      key: { root: PitchClass.C, mode: 'major' },
    });
    expect(samples.length).toBe(input.length);
    expect(key).toMatchObject({ root: PitchClass.C, mode: 'major', confidence: 1 });
    let worstBefore = 0;
    let worstAfter = 0;
    MELODY.forEach((target, note) => {
      worstBefore = Math.max(worstBefore, Math.abs(medianMidi(input, note) - target));
      worstAfter = Math.max(worstAfter, Math.abs(medianMidi(samples, note) - target));
    });
    expect(worstBefore * 100).toBeGreaterThan(DETUNE_CENTS - 10);
    expect(worstAfter * 100).toBeLessThan(TOLERANCE_CENTS);
  });

  it("key 'detect' equals passing detectKey's result", () => {
    const detected = autoTune({ samples: input, sampleRate: SR });
    const reference = detectKey({ samples: input, sampleRate: SR });
    expect(detected.key.root).toBe(reference.root);
    expect(detected.key.mode).toBe(reference.mode);
    expect(detected.key.confidence).toBe(reference.confidence);
    const named = autoTune({ samples: input, sampleRate: SR, key: reference });
    expect(named.samples).toEqual(detected.samples);
    expect(autoTune({ samples: input, sampleRate: SR, key: 'detect' }).samples).toEqual(
      detected.samples,
    );
  });

  it('takes the key mode by name and refuses an ordinal', () => {
    const byName = autoTune({
      samples: input,
      sampleRate: SR,
      key: { root: PitchClass.D, mode: 'dorian' },
    });
    expect(byName.key.mode).toBe('dorian');
    expect(() =>
      autoTune({
        samples: input,
        sampleRate: SR,
        key: { root: PitchClass.D, mode: 2 as never },
      }),
    ).toThrow(TypeError);
  });

  it('retuneAmount 0 leaves the audio unchanged', () => {
    const { samples } = autoTune({
      samples: input,
      sampleRate: SR,
      key: { root: PitchClass.C, mode: 'major' },
      retuneAmount: 0,
    });
    let worst = 0;
    for (let i = 0; i < input.length; i++) {
      worst = Math.max(worst, Math.abs(samples[i] - input[i]));
    }
    expect(worst).toBeLessThan(1e-3);
  });

  it('refuses a bad key and an out-of-range retuneAmount', () => {
    expect(() => autoTune({ samples: input, sampleRate: SR, key: 'C major' as never })).toThrow();
    expect(() =>
      autoTune({
        samples: input,
        sampleRate: SR,
        key: { root: 12 as PitchClass, mode: 'major' },
      }),
    ).toThrow();
    expect(() =>
      autoTune({
        samples: input,
        sampleRate: SR,
        key: { root: PitchClass.C, mode: 'major' },
        retuneAmount: 2,
      }),
    ).toThrow();
  });
});
