import { describe, expect, it } from 'vitest';
import {
  melSpectrogram,
  melToAudio,
  melToStft,
  mfcc,
  mfccToAudio,
  mfccToMel,
} from '../src/index.js';

const sampleRate = 22050;

function tone(seconds = 0.5): Float32Array {
  const out = new Float32Array(Math.round(sampleRate * seconds));
  for (let i = 0; i < out.length; i++) {
    out[i] = 0.5 * Math.sin((2 * Math.PI * 440 * i) / sampleRate);
  }
  return out;
}

const samples = tone();

describe('mel results carry the forward parameters', () => {
  it('reports the transform it came from, with fmaxHz resolved', () => {
    const mel = melSpectrogram(samples, sampleRate, {
      nFft: 1024,
      hopLength: 256,
      nMels: 40,
      fminHz: 100,
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
  });

  it('MFCC results carry the Mel parameters and the lifter', () => {
    const result = mfcc(samples, sampleRate, {
      nFft: 1024,
      hopLength: 256,
      nMels: 40,
      nMfcc: 13,
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
  });
});

describe('an inverse fed the result parameters equals the inverse with literal arguments', () => {
  const mel = melSpectrogram(samples, sampleRate, {
    nFft: 1024,
    hopLength: 256,
    nMels: 40,
    fminHz: 100,
    fmaxHz: 4000,
    htk: true,
  });

  it('melToStft', () => {
    const fromResult = melToStft({
      power: mel.power,
      sampleRate: mel.sampleRate,
      nFft: mel.nFft,
      fminHz: mel.fminHz,
      fmaxHz: mel.fmaxHz,
      htk: mel.htk,
    });
    const explicit = melToStft({
      power: mel.power,
      sampleRate,
      nFft: 1024,
      fminHz: 100,
      fmaxHz: 4000,
      htk: true,
    });
    expect(fromResult).toEqual(explicit);
  });

  it('melToAudio', () => {
    const fromResult = melToAudio({
      power: mel.power,
      sampleRate: mel.sampleRate,
      nFft: mel.nFft,
      hopLength: mel.hopLength,
      fminHz: mel.fminHz,
      fmaxHz: mel.fmaxHz,
      htk: mel.htk,
      nIter: 2,
    });
    const explicit = melToAudio({
      power: mel.power,
      sampleRate,
      nFft: 1024,
      hopLength: 256,
      fminHz: 100,
      fmaxHz: 4000,
      htk: true,
      nIter: 2,
    });
    expect(fromResult).toEqual(explicit);
  });

  it('an omitted fmaxHz is the Nyquist the result reports', () => {
    const full = melSpectrogram(samples, sampleRate, { nFft: 1024, hopLength: 256, nMels: 40 });
    expect(full.fmaxHz).toBe(sampleRate / 2);
    expect(melToStft({ power: full.power, sampleRate, nFft: 1024, fmaxHz: full.fmaxHz })).toEqual(
      melToStft({ power: full.power, sampleRate, nFft: 1024 }),
    );
  });

  it('mfccToMel and mfccToAudio undo the lifter that is passed', () => {
    const result = mfcc(samples, sampleRate, {
      nFft: 1024,
      hopLength: 256,
      nMels: 40,
      nMfcc: 13,
      fmaxHz: 8000,
      htk: false,
      lifter: 22,
    });
    const fromResult = mfccToMel({
      coefficients: result.coefficients,
      nMels: result.nMels,
      lifter: result.lifter,
    });
    const explicit = mfccToMel({ coefficients: result.coefficients, nMels: 40, lifter: 22 });
    expect(fromResult).toEqual(explicit);
    // The lifter is part of the answer: dropping it changes the Mel matrix.
    const unlifted = mfccToMel({ coefficients: result.coefficients, nMels: 40 });
    expect(unlifted).not.toEqual(fromResult);

    const audio = mfccToAudio({
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
    });
    expect(audio).toEqual(
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

describe('the inverse requests no longer read the names that differed from the result', () => {
  const mel = melSpectrogram(samples, sampleRate, { nFft: 1024, hopLength: 256, nMels: 40 });
  const result = mfcc(samples, sampleRate, { nFft: 1024, hopLength: 256, nMels: 40, nMfcc: 13 });
  const { rows: nMels, cols: nFrames } = mel.power;

  it('refuses the old key spellings', () => {
    expect(() => melToStft({ mel: mel.power, nMels, nFrames } as never)).toThrow();
    expect(() => melToAudio({ mel: mel.power, nMels, nFrames } as never)).toThrow();
    expect(() =>
      mfccToMel({
        mfcc: result.coefficients,
        nMfcc: 13,
        nFrames: result.coefficients.cols,
      } as never),
    ).toThrow();
    expect(() =>
      mfccToAudio({
        mfcc: result.coefficients,
        nMfcc: 13,
        nFrames: result.coefficients.cols,
      } as never),
    ).toThrow();
  });
});
