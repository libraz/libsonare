import { beforeAll, describe, expect, it } from 'vitest';
import {
  analyzeImpulseResponse,
  hpss,
  hpssWithResidual,
  init,
  nnlsChroma,
  normalize,
  pitchShift,
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
});
