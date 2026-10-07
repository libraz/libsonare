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
  it('reports the transform it came from, with fmax resolved', () => {
    const mel = melSpectrogram(samples, sampleRate, 1024, 256, 40, 100, 0, true);
    expect(mel).toMatchObject({
      nMels: 40,
      sampleRate,
      hopLength: 256,
      nFft: 1024,
      fmin: 100,
      fmax: sampleRate / 2,
      htk: true,
      isDb: false,
    });
  });

  it('MFCC results carry the Mel parameters and the lifter', () => {
    const result = mfcc(samples, sampleRate, 1024, 256, 40, 13, 0, 8000, false, 22);
    expect(result).toMatchObject({
      nMfcc: 13,
      nMels: 40,
      sampleRate,
      hopLength: 256,
      nFft: 1024,
      fmin: 0,
      fmax: 8000,
      htk: false,
      isDb: false,
      lifter: 22,
    });
  });
});

describe('inverse from a result equals the inverse with explicit arguments', () => {
  const mel = melSpectrogram(samples, sampleRate, 1024, 256, 40, 100, 4000, true);

  it('melToStft', () => {
    const fromResult = melToStft({ result: mel });
    const explicit = melToStft({
      mel: mel.power,
      nMels: mel.nMels,
      nFrames: mel.nFrames,
      sampleRate,
      nFft: 1024,
      fmin: 100,
      fmax: 4000,
      htk: true,
    });
    expect(fromResult).toEqual(explicit);
  });

  it('melToAudio, with an extra argument the result does not carry', () => {
    const fromResult = melToAudio({ result: mel, nIter: 2 });
    const explicit = melToAudio({
      mel: mel.power,
      nMels: mel.nMels,
      nFrames: mel.nFrames,
      sampleRate,
      nFft: 1024,
      hopLength: 256,
      fmin: 100,
      fmax: 4000,
      htk: true,
      nIter: 2,
    });
    expect(fromResult).toEqual(explicit);
  });

  it('accepts a redundant argument that agrees, including fmax 0 for the Nyquist', () => {
    const full = melSpectrogram(samples, sampleRate, 1024, 256, 40);
    expect(melToStft({ result: full, sampleRate, nFft: 1024, fmax: 0 })).toEqual(
      melToStft({ result: full }),
    );
  });

  it('mfccToMel and mfccToAudio undo the lifter the result carries', () => {
    const result = mfcc(samples, sampleRate, 1024, 256, 40, 13, 0, 8000, false, 22);
    const fromResult = mfccToMel({ result });
    const explicit = mfccToMel({
      mfcc: result.coefficients,
      nMfcc: 13,
      nFrames: result.nFrames,
      nMels: 40,
      lifter: 22,
    });
    expect(fromResult).toEqual(explicit);
    // The lifter is part of the answer: dropping it changes the Mel matrix.
    const unlifted = mfccToMel({
      mfcc: result.coefficients,
      nMfcc: 13,
      nFrames: result.nFrames,
      nMels: 40,
    });
    expect(unlifted).not.toEqual(fromResult);

    const audio = mfccToAudio({ result, nIter: 2 });
    expect(audio).toEqual(
      mfccToAudio({
        mfcc: result.coefficients,
        nMfcc: 13,
        nFrames: result.nFrames,
        nMels: 40,
        sampleRate,
        nFft: 1024,
        hopLength: 256,
        fmin: 0,
        fmax: 8000,
        htk: false,
        lifter: 22,
        nIter: 2,
      }),
    );
  });
});

describe('a result request is refused when it contradicts the result', () => {
  const mel = melSpectrogram(samples, sampleRate, 1024, 256, 40);
  const coefficients = mfcc(samples, sampleRate, 1024, 256, 40, 13, 0, 0, false, 22);

  it('refuses a disagreeing explicit argument with a RangeError naming it', () => {
    expect(() => melToStft({ result: mel, sampleRate: 44100 })).toThrow(RangeError);
    expect(() => melToStft({ result: mel, sampleRate: 44100 })).toThrow(/sampleRate 44100/);
    expect(() => melToStft({ result: mel, nFft: 2048 })).toThrow(/nFft 2048/);
    expect(() => melToAudio({ result: mel, hopLength: 512 })).toThrow(/hopLength 512/);
    expect(() => melToStft({ result: mel, htk: true })).toThrow(/htk true/);
    expect(() => mfccToMel({ result: coefficients, lifter: 0 })).toThrow(RangeError);
    expect(() => mfccToAudio({ result: coefficients, nMels: 64 })).toThrow(/nMels 64/);
  });

  it('refuses a result in dB with a message naming the conversion', () => {
    const inDb = { ...mel, power: mel.db, isDb: true };
    expect(() => melToStft({ result: inDb })).toThrow(RangeError);
    expect(() => melToAudio({ result: inDb })).toThrow(/dbToPower/);
  });

  it('names a hand-built result that lacks a parameter', () => {
    const { nFft: _nFft, ...incomplete } = mel;
    expect(() => melToStft({ result: incomplete as never })).toThrow(/result\.nFft is missing/);
  });
});
