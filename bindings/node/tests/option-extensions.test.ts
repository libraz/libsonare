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
    const explicit = hpss(samples, sampleRate, {
      kernelHarmonic: 31,
      kernelPercussive: 31,
      nFft: 2048,
      hopLength: 512,
      hardMask: false,
    });
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

    const stretched = timeStretch(samples, sampleRate, { speedRatio: 1.2 });
    const changedStretch = timeStretch(samples, sampleRate, {
      speedRatio: 1.2,
      nFft: 1024,
      hopLength: 256,
    });
    expect(Array.from(changedStretch)).not.toEqual(Array.from(stretched));

    const shifted = pitchShift(samples, sampleRate, { pitchSemitones: 3 });
    const changedShift = pitchShift(samples, sampleRate, {
      pitchSemitones: 3,
      nFft: 1024,
      hopLength: 256,
    });
    expect(Array.from(changedShift)).not.toEqual(Array.from(shifted));
  });

  it('forwards residual, NNLS hop, and impulse-response options', () => {
    const residual = hpssWithResidual(samples, sampleRate);
    const changedResidual = hpssWithResidual(samples, sampleRate, {
      kernelHarmonic: 31,
      kernelPercussive: 31,
      nFft: 1024,
      hopLength: 256,
      hardMask: true,
    });
    expect(Array.from(changedResidual.residual)).not.toEqual(Array.from(residual.residual));

    const chroma = nnlsChroma(samples, sampleRate);
    const changedChroma = nnlsChroma(samples, sampleRate, { hopLength: 256 });
    expect(changedChroma.cols).toBeGreaterThan(chroma.cols);

    expect(() =>
      analyzeImpulseResponse(samples, sampleRate, { nOctaveBands: 6, minDecayDb: 0 }),
    ).toThrow(RangeError);
  });

  it('rejects invalid option types and ranges before native dispatch', () => {
    expect(() => hpss({ samples, sampleRate, nFft: 3 })).toThrow(RangeError);
    expect(() => hpss({ samples, sampleRate, hardMask: 'yes' as never })).toThrow(TypeError);
    expect(() =>
      timeStretch(samples, sampleRate, { speedRatio: 1, nFft: 2048, hopLength: 0 }),
    ).toThrow(RangeError);
    // Fractional but a number: the out-of-domain half of the split, not the
    // wrong-type half that `hardMask: 'yes'` above stays on.
    expect(() => nnlsChroma(samples, sampleRate, { hopLength: 1.5 })).toThrow(RangeError);
  });

  it('rejects a hop below the half-window overlap contract', () => {
    // The core validator is the single gate, so every spectral effect that
    // resynthesizes by overlap-add rejects the same geometry.
    expect(() => hpss({ samples, sampleRate, nFft: 1024, hopLength: 1024 })).toThrow();
    expect(() => hpssWithResidual({ samples, sampleRate, nFft: 1024, hopLength: 1024 })).toThrow();
    expect(() =>
      timeStretch({ samples, sampleRate, speedRatio: 1.2, nFft: 512, hopLength: 2048 }),
    ).toThrow();
    expect(() =>
      pitchShift({ samples, sampleRate, pitchSemitones: 3, nFft: 1024, hopLength: 1024 }),
    ).toThrow();
    expect(() =>
      phaseVocoder({ samples, sampleRate, speedRatio: 1.2, nFft: 1024, hopLength: 1024 }),
    ).toThrow();
  });

  it('accepts an even nFft that is not a power of two, matching the C ABI', () => {
    // The core FFT is mixed-radix; only the facades used to require a power of
    // two, which made the same call succeed on the C ABI and fail here.
    const geometry = { nFft: 1500, hopLength: 250 };
    expect(hpss({ samples, sampleRate, ...geometry }).harmonic.length).toBe(samples.length);
    expect(
      timeStretch({ samples, sampleRate, speedRatio: 1.2, ...geometry }).length,
    ).toBeGreaterThan(0);
    expect(
      pitchShift({ samples, sampleRate, pitchSemitones: 3, ...geometry }).length,
    ).toBeGreaterThan(0);
    expect(
      phaseVocoder({ samples, sampleRate, speedRatio: 1.2, ...geometry }).length,
    ).toBeGreaterThan(0);
  });
});

// One FFT-size rule, not one per module. `hpss` accepted any even size (the
// core FFT is mixed-radix) while `hpssWithResidual` went through a second copy
// that demanded a power of two, so reaching for the third stem forced an
// unrelated change of FFT size — and the message asserted a constraint this
// project had already written down as untrue.
describe('STFT entry points share one nFft rule', () => {
  const stftEntryPoints = {
    hpss: (nFft: number) => hpss({ samples, sampleRate, nFft, hopLength: 256 }),
    hpssWithResidual: (nFft: number) =>
      hpssWithResidual({ samples, sampleRate, nFft, hopLength: 256 }),
    timeStretch: (nFft: number) => timeStretch({ samples, sampleRate, speedRatio: 1.0, nFft }),
    pitchShift: (nFft: number) => pitchShift({ samples, sampleRate, pitchSemitones: 0, nFft }),
    phaseVocoder: (nFft: number) => phaseVocoder({ samples, sampleRate, speedRatio: 1.0, nFft }),
  };
  const domain = (name: string) =>
    new RangeError(`${name}: nFft must be an even integer in [4, 2147483646]`);

  it.each(Object.keys(stftEntryPoints))('%s accepts a non-power-of-two even nFft', (name) => {
    expect(() => stftEntryPoints[name as keyof typeof stftEntryPoints](1536)).not.toThrow();
  });

  it.each(Object.keys(stftEntryPoints))('%s rejects a two-point nFft', (name) => {
    // A two-point symmetric Hann synthesis window is all zeros.
    expect(() => stftEntryPoints[name as keyof typeof stftEntryPoints](2)).toThrow(domain(name));
  });

  it.each(Object.keys(stftEntryPoints))('%s rejects an odd nFft the same way', (name) => {
    expect(() => stftEntryPoints[name as keyof typeof stftEntryPoints](511)).toThrow(domain(name));
  });

  it.each(Object.keys(stftEntryPoints))(
    '%s rejects an nFft past the largest even C int',
    (name) => {
      expect(() => stftEntryPoints[name as keyof typeof stftEntryPoints](2 ** 31)).toThrow(
        domain(name),
      );
    },
  );

  it('runs an omitted phaseVocoder hop as a quarter of nFft', () => {
    const omitted = phaseVocoder({ samples, sampleRate, speedRatio: 1.5, nFft: 1024 });
    const quarter = phaseVocoder({
      samples,
      sampleRate,
      speedRatio: 1.5,
      nFft: 1024,
      hopLength: 256,
    });
    expect(Array.from(omitted)).toEqual(Array.from(quarter));
    expect(() => phaseVocoder({ samples, sampleRate, speedRatio: 1.5, nFft: 8 })).not.toThrow();
  });
});
