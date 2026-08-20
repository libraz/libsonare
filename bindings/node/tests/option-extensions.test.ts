import { describe, expect, it } from 'vitest';
import {
  analyzeImpulseResponse,
  hpss,
  hpssWithResidual,
  nnlsChroma,
  phaseVocoder,
  pitchShift,
  timeStretch,
} from '../src/index.js';

const sampleRate = 22050;
const samples = Float32Array.from(
  { length: 12000 },
  (_, i) => Math.sin(i * 0.071) * (0.25 + 0.2 * Math.sin(i * 0.003)),
);

describe('additive effect and feature options', () => {
  it('keeps explicit defaults byte-compatible and applies nondefault DSP settings', () => {
    const legacy = hpss(samples, sampleRate);
    const explicit = hpss(samples, sampleRate, 31, 31, 2048, 512, false);
    expect(Array.from(explicit.harmonic)).toEqual(Array.from(legacy.harmonic));
    expect(Array.from(explicit.percussive)).toEqual(Array.from(legacy.percussive));

    const changed = hpss({
      samples,
      sampleRate,
      nFft: 1024,
      hopLength: 256,
      hardMask: true,
    });
    expect(Array.from(changed.harmonic)).not.toEqual(Array.from(legacy.harmonic));

    const stretched = timeStretch(samples, sampleRate, 1.2);
    const changedStretch = timeStretch(samples, sampleRate, 1.2, 1024, 256);
    expect(Array.from(changedStretch)).not.toEqual(Array.from(stretched));

    const shifted = pitchShift(samples, sampleRate, 3);
    const changedShift = pitchShift(samples, sampleRate, 3, 1024, 256);
    expect(Array.from(changedShift)).not.toEqual(Array.from(shifted));
  });

  it('forwards residual, NNLS hop, and impulse-response options', () => {
    const residual = hpssWithResidual(samples, sampleRate);
    const changedResidual = hpssWithResidual(samples, sampleRate, 31, 31, 1024, 256, true);
    expect(Array.from(changedResidual.residual)).not.toEqual(Array.from(residual.residual));

    const chroma = nnlsChroma(samples);
    const changedChroma = nnlsChroma(samples, sampleRate, { hopLength: 256 });
    expect(changedChroma.nFrames).toBeGreaterThan(chroma.nFrames);

    expect(() => analyzeImpulseResponse(samples, sampleRate, 6, 0)).toThrow(RangeError);
  });

  it('rejects invalid option types and ranges before native dispatch', () => {
    expect(() => hpss({ samples, nFft: 3 })).toThrow(RangeError);
    expect(() => hpss({ samples, hardMask: 'yes' as never })).toThrow(TypeError);
    expect(() => timeStretch(samples, sampleRate, 1, 2048, 0)).toThrow(RangeError);
    expect(() => nnlsChroma(samples, sampleRate, { hopLength: 1.5 })).toThrow(TypeError);
  });

  it('rejects a hop below the half-window overlap contract', () => {
    // The core validator is the single gate, so every spectral effect that
    // resynthesizes by overlap-add rejects the same geometry.
    expect(() => hpss({ samples, sampleRate, nFft: 1024, hopLength: 1024 })).toThrow();
    expect(() => hpssWithResidual({ samples, sampleRate, nFft: 1024, hopLength: 1024 })).toThrow();
    expect(() =>
      timeStretch({ samples, sampleRate, rate: 1.2, nFft: 512, hopLength: 2048 }),
    ).toThrow();
    expect(() =>
      pitchShift({ samples, sampleRate, semitones: 3, nFft: 1024, hopLength: 1024 }),
    ).toThrow();
    expect(() =>
      phaseVocoder({ samples, sampleRate, rate: 1.2, nFft: 1024, hopLength: 1024 }),
    ).toThrow();
  });

  it('accepts an even nFft that is not a power of two, matching the C ABI', () => {
    // The core FFT is mixed-radix; only the facades used to require a power of
    // two, which made the same call succeed on the C ABI and fail here.
    const geometry = { nFft: 1500, hopLength: 250 };
    expect(hpss({ samples, sampleRate, ...geometry }).harmonic.length).toBe(samples.length);
    expect(timeStretch({ samples, sampleRate, rate: 1.2, ...geometry }).length).toBeGreaterThan(0);
    expect(pitchShift({ samples, sampleRate, semitones: 3, ...geometry }).length).toBeGreaterThan(
      0,
    );
    expect(phaseVocoder({ samples, sampleRate, rate: 1.2, ...geometry }).length).toBeGreaterThan(0);
  });
});
