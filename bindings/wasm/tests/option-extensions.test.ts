import { beforeAll, describe, expect, it } from 'vitest';
import {
  analyzeImpulseResponse,
  chroma as chromaStft,
  griffinLim,
  hpss,
  hpssWithResidual,
  init,
  melSpectrogram,
  melToAudio,
  melToStft,
  mfcc,
  mfccToAudio,
  nnlsChroma,
  normalize,
  onsetEnvelope,
  onsetStrengthMulti,
  phaseVocoder,
  pitchShift,
  stft,
  stftDb,
  timeStretch,
  trim,
} from '../src/index';

const sampleRate = 22050;
const samples = Float32Array.from(
  { length: 12000 },
  (_, i) => Math.sin(i * 0.071) * (0.25 + 0.2 * Math.sin(i * 0.003)),
);

describe('additive effect and feature options', () => {
  beforeAll(async () => {
    await init();
  });

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

  it('forwards normalize, trim, residual, NNLS, and impulse-response options', () => {
    const peak = normalize(samples, sampleRate, 0, 'peak');
    const rms = normalize(samples, sampleRate, -10, 'rms');
    expect(Array.from(rms)).not.toEqual(Array.from(peak));

    const trimInput = new Float32Array(12000);
    for (let i = 2500; i < 6500; i++) {
      trimInput[i] = Math.sin(i * 0.1) * 0.5;
    }
    const defaultTrim = trim(trimInput, sampleRate);
    const changedTrim = trim(trimInput, sampleRate, -60, 1024, 128);
    expect(changedTrim.length).not.toBe(defaultTrim.length);

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
    expect(() => normalize(samples, sampleRate, 0, 'RMS' as never)).toThrow(RangeError);
    expect(() => trim(samples, sampleRate, -60, 0, 512)).toThrow(RangeError);
    expect(() => nnlsChroma(samples, sampleRate, { hopLength: 1.5 })).toThrow(RangeError);
  });

  it('rejects a hop below the half-window overlap contract', () => {
    // The core validator is the single gate, and WASM calls the core directly
    // rather than through the C ABI, so the rejection is inherited here too.
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
    timeStretch: (nFft: number) => timeStretch({ samples, sampleRate, rate: 1.0, nFft }),
    pitchShift: (nFft: number) => pitchShift({ samples, sampleRate, semitones: 0, nFft }),
  };

  it.each(Object.keys(stftEntryPoints))('%s accepts a non-power-of-two even nFft', (name) => {
    expect(() => stftEntryPoints[name as keyof typeof stftEntryPoints](1536)).not.toThrow();
  });

  it.each(Object.keys(stftEntryPoints))('%s rejects an odd nFft the same way', (name) => {
    expect(() => stftEntryPoints[name as keyof typeof stftEntryPoints](511)).toThrow(RangeError);
    expect(() => stftEntryPoints[name as keyof typeof stftEntryPoints](511)).toThrow(
      /nFft must be an even integer/,
    );
  });
});

// The same one rule across the feature facades. These used to route `nFft`
// through a positivity-only helper, so an identical fault reported as three
// different classes depending on which entry point the caller reached, and an
// odd size was refused by the core rather than by the argument check.
describe('feature entry points share one nFft rule', () => {
  const nMels = 64;
  const nMfcc = 20;
  type Framing = { nFft?: number; hopLength?: number };
  let featureEntryPoints: Record<string, (framing: Framing) => unknown>;
  let names: string[];

  beforeAll(async () => {
    await init();
    // Every fixture carries the geometry nFft 2048 produces, so the default
    // framing is a call each entry point completes -- the control the refusals
    // below are measured against.
    const mel = melSpectrogram({ samples, sampleRate, nFft: 2048, hopLength: 512, nMels });
    const spectrum = stft({ samples, sampleRate, nFft: 2048, hopLength: 512 });
    const cepstrum = mfcc({ samples, sampleRate, nFft: 2048, hopLength: 512, nMels, nMfcc });

    featureEntryPoints = {
      stft: (f) => stft({ samples, sampleRate, ...f }),
      stftDb: (f) => stftDb({ samples, sampleRate, ...f }),
      melSpectrogram: (f) => melSpectrogram({ samples, sampleRate, nMels, ...f }),
      mfcc: (f) => mfcc({ samples, sampleRate, nMels, nMfcc, ...f }),
      chroma: (f) => chromaStft({ samples, sampleRate, ...f }),
      onsetEnvelope: (f) => onsetEnvelope({ samples, sampleRate, nMels, ...f }),
      onsetStrengthMulti: (f) => onsetStrengthMulti({ samples, sampleRate, nMels, ...f }),
      melToAudio: (f) =>
        melToAudio({
          melPower: mel.power,
          nMels,
          nFrames: mel.nFrames,
          sampleRate,
          nIter: 2,
          ...f,
        }),
      griffinLim: (f) =>
        griffinLim({
          magnitude: spectrum.magnitude,
          nBins: spectrum.nBins,
          nFrames: spectrum.nFrames,
          sampleRate,
          nIter: 2,
          ...f,
        }),
      mfccToAudio: (f) =>
        mfccToAudio({
          mfccCoefficients: cepstrum.coefficients,
          nMfcc,
          nFrames: cepstrum.nFrames,
          nMels,
          sampleRate,
          nIter: 2,
          ...f,
        }),
    };
    names = Object.keys(featureEntryPoints);
    expect(names).toHaveLength(10);
  });

  // Named rather than derived from the map, because `it.each` is collected
  // before `beforeAll` fills it; a name dropped from the map fails here as an
  // undefined entry point instead of silently shrinking the population.
  const entryPointNames = [
    'stft',
    'stftDb',
    'melSpectrogram',
    'mfcc',
    'chroma',
    'onsetEnvelope',
    'onsetStrengthMulti',
    'melToAudio',
    'griffinLim',
    'mfccToAudio',
  ];

  it.each(entryPointNames)('%s completes at the framing the refusals use', (name) => {
    expect(() => featureEntryPoints[name]({})).not.toThrow();
  });

  it.each(entryPointNames)('%s reports a fractional nFft as a TypeError, as Node does', (name) => {
    expect(() => featureEntryPoints[name]({ nFft: 2048.9 })).toThrow(TypeError);
    expect(() => featureEntryPoints[name]({ nFft: 2048.9 })).toThrow(/nFft must be an integer/);
  });

  it.each(entryPointNames)('%s reports a fractional hopLength as a TypeError', (name) => {
    expect(() => featureEntryPoints[name]({ hopLength: 512.5 })).toThrow(TypeError);
    expect(() => featureEntryPoints[name]({ hopLength: 512.5 })).toThrow(
      /hopLength must be an integer/,
    );
  });

  it.each(entryPointNames)('%s rejects an odd nFft the same way', (name) => {
    expect(() => featureEntryPoints[name]({ nFft: 2049 })).toThrow(RangeError);
    expect(() => featureEntryPoints[name]({ nFft: 2049 })).toThrow(/nFft must be an even integer/);
  });

  it('accepts a non-power-of-two even nFft', () => {
    expect(() => stft({ samples, sampleRate, nFft: 1536, hopLength: 256 })).not.toThrow();
    expect(() => melSpectrogram({ samples, sampleRate, nFft: 1536, hopLength: 256 })).not.toThrow();
  });

  // melToStft inverts a filterbank instead of running a transform, so its nFft
  // only sizes the output; an odd value is a supported geometry here, in the
  // core, and on the Node surface, and the parity rule would refuse it.
  it('keeps melToStft off the parity rule, where an odd nFft is a distinct result', () => {
    const mel = melSpectrogram({ samples, sampleRate, nFft: 2048, hopLength: 512, nMels });
    const shared = { melPower: mel.power, nMels, nFrames: mel.nFrames, sampleRate };
    const even = melToStft({ ...shared, nFft: 2048 });
    const odd = melToStft({ ...shared, nFft: 2049 });
    expect(odd.nBins).toBe(even.nBins);
    expect(Array.from(odd.power)).not.toEqual(Array.from(even.power));
  });
});
