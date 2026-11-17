import { beforeAll, describe, expect, it } from 'vitest';
import {
  init,
  melSpectrogram,
  melToAudio,
  melToStft,
  mfcc,
  mfccToAudio,
  mfccToMel,
} from '../src/index';

const sampleRate = 22050;

function tone(seconds = 0.5): Float32Array {
  const out = new Float32Array(Math.round(sampleRate * seconds));
  for (let i = 0; i < out.length; i++) {
    out[i] = 0.5 * Math.sin((2 * Math.PI * 440 * i) / sampleRate);
  }
  return out;
}

const samples = tone();

beforeAll(async () => {
  await init();
});

describe('mel results carry the forward parameters', () => {
  it('reports the transform it came from, with fmax resolved', () => {
    const mel = melSpectrogram(samples, sampleRate, {
      nFft: 1024,
      hopLength: 256,
      nMels: 40,
      fminHz: 100,
      fmaxHz: 0,
      htk: true,
    });
    expect(mel).toMatchObject({
      sampleRate,
      hopLength: 256,
      nFft: 1024,
      fminHz: 100,
      fmaxHz: sampleRate / 2,
      htk: true,
    });
    expect(mel.power.rows).toBe(40);
    expect(mel).not.toHaveProperty('isDb');
  });

  it('MFCC results carry the Mel parameters and the lifter', () => {
    const result = mfcc(samples, sampleRate, {
      nFft: 1024,
      hopLength: 256,
      nMels: 40,
      nMfcc: 13,
      fminHz: 0,
      fmaxHz: 8000,
      htk: false,
      lifter: 22,
    });
    expect(result).toMatchObject({
      nMels: 40,
      sampleRate,
      hopLength: 256,
      nFft: 1024,
      fminHz: 0,
      fmaxHz: 8000,
      htk: false,
      lifter: 22,
    });
    expect(result.coefficients.rows).toBe(13);
    expect(result).not.toHaveProperty('isDb');
  });
});

describe('inverse from the parameters a result carries', () => {
  const mel = () =>
    melSpectrogram(samples, sampleRate, {
      nFft: 1024,
      hopLength: 256,
      nMels: 40,
      fminHz: 100,
      fmaxHz: 4000,
      htk: true,
    });

  it('melToStft with the carried parameters equals the explicit call', () => {
    const result = mel();
    const explicit = melToStft({
      power: result.power,
      sampleRate,
      nFft: 1024,
      fminHz: 100,
      fmaxHz: 4000,
      htk: true,
    });
    expect(
      melToStft({
        power: result.power,
        sampleRate: result.sampleRate,
        nFft: result.nFft,
        fminHz: result.fminHz,
        fmaxHz: result.fmaxHz,
        htk: result.htk,
      }),
    ).toEqual(explicit);
    expect(explicit.rows).toBe(1024 / 2 + 1);
    expect(explicit.cols).toBe(result.power.cols);
  });

  it('melToAudio with the carried parameters equals the explicit call', () => {
    const result = mel();
    const explicit = melToAudio({
      power: result.power,
      sampleRate,
      nFft: 1024,
      hopLength: 256,
      fminHz: 100,
      fmaxHz: 4000,
      htk: true,
      nIter: 2,
    });
    expect(
      melToAudio({
        power: result.power,
        sampleRate: result.sampleRate,
        nFft: result.nFft,
        hopLength: result.hopLength,
        fminHz: result.fminHz,
        fmaxHz: result.fmaxHz,
        htk: result.htk,
        nIter: 2,
      }),
    ).toEqual(explicit);
  });

  it('a resolved Nyquist fmaxHz and an absent one give the same inverse', () => {
    const full = melSpectrogram(samples, sampleRate, { nFft: 1024, hopLength: 256, nMels: 40 });
    expect(melToStft({ power: full.power, sampleRate, nFft: 1024, fmaxHz: full.fmaxHz })).toEqual(
      melToStft({ power: full.power, sampleRate, nFft: 1024 }),
    );
  });

  it('mfccToMel and mfccToAudio undo the lifter the result carries', () => {
    const result = mfcc(samples, sampleRate, {
      nFft: 1024,
      hopLength: 256,
      nMels: 40,
      nMfcc: 13,
      fminHz: 0,
      fmaxHz: 8000,
      htk: false,
      lifter: 22,
    });
    const fromCarried = mfccToMel({
      coefficients: result.coefficients,
      nMels: result.nMels,
      lifter: result.lifter,
    });
    expect(fromCarried).toEqual(
      mfccToMel({ coefficients: result.coefficients, nMels: 40, lifter: 22 }),
    );
    expect(fromCarried.rows).toBe(40);
    expect(fromCarried.cols).toBe(result.coefficients.cols);
    // The lifter is part of the answer: dropping it changes the Mel matrix.
    expect(mfccToMel({ coefficients: result.coefficients, nMels: 40 })).not.toEqual(fromCarried);

    expect(
      mfccToAudio({
        coefficients: result.coefficients,
        nMels: result.nMels,
        sampleRate: result.sampleRate,
        nFft: result.nFft,
        hopLength: result.hopLength,
        fminHz: result.fminHz,
        fmaxHz: result.fmaxHz,
        htk: result.htk,
        lifter: result.lifter,
        nIter: 2,
      }),
    ).toEqual(
      mfccToAudio({
        coefficients: result.coefficients,
        nMels: 40,
        sampleRate,
        nFft: 1024,
        hopLength: 256,
        fminHz: 0,
        fmaxHz: 8000,
        htk: false,
        lifter: 22,
        nIter: 2,
      }),
    );
  });
});

describe('the inverse requests', () => {
  it('refuse the removed request names', () => {
    const mel = melSpectrogram(samples, sampleRate, { nFft: 1024, hopLength: 256, nMels: 40 });
    const coefficients = mfcc(samples, sampleRate, { nFft: 1024, hopLength: 256, nMels: 40 });
    expect(() => melToStft({ melPower: mel.power } as never)).toThrow();
    expect(() => melToAudio({ melPower: mel.power } as never)).toThrow();
    expect(() => mfccToMel({ mfccCoefficients: coefficients.coefficients } as never)).toThrow();
    expect(() => mfccToAudio({ mfccCoefficients: coefficients.coefficients } as never)).toThrow();
  });
});
