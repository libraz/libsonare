/**
 * Argument domains the C ABI defines and the WASM entries reproduce through the same core
 * validators: refused as RangeError on every entry form, accepted unchanged at the boundary.
 */

import { beforeAll, describe, expect, it } from 'vitest';
import {
  analyzeBpm,
  analyzeImpulseResponse,
  analyzeMelody,
  analyzeRhythm,
  chromaCqt,
  cqt,
  cqtToAudio,
  detectAcoustic,
  detectKey,
  detectKeyCandidates,
  detectOnsets,
  hybridCqt,
  init,
  melSpectrogram,
  melToAudio,
  melToStft,
  mfcc,
  mfccToAudio,
  nnlsChroma,
  pseudoCqt,
  spectralEdit,
  vqt,
  vqtToAudio,
} from '../src/index';
import { expectRefusalOf, unknownKeyText } from './_helpers';

const SR = 22050;

function tone(seconds: number, freq = 440): Float32Array {
  const out = new Float32Array(Math.floor(SR * seconds));
  for (let i = 0; i < out.length; i++) {
    out[i] = 0.5 * Math.sin((2 * Math.PI * freq * i) / SR);
  }
  return out;
}

beforeAll(async () => {
  await init();
});

describe('nnlsChroma STFT blend', () => {
  const samples = tone(0.5);

  it('refuses a weight outside [0, 1] and an odd or too-small FFT size', () => {
    for (const options of [
      { stftBlendWeight: -1 },
      { stftBlendWeight: 1.5 },
      { stftBlendNFft: 4095 },
      { stftBlendNFft: 0 },
    ]) {
      expectRefusalOf(() => nnlsChroma(samples, SR, options));
      expectRefusalOf(() => nnlsChroma({ samples, sampleRate: SR, ...options }));
    }
  });

  it('accepts the domain edges', () => {
    expect(nnlsChroma(samples, SR, { stftBlendWeight: 0, stftBlendNFft: 2 }).cols).toBeGreaterThan(
      0,
    );
    expect(nnlsChroma(samples, SR, { stftBlendWeight: 1 }).cols).toBeGreaterThan(0);
  });
});

describe('detectOnsets delta', () => {
  const samples = tone(0.5);

  it('refuses a negative delta on both entry forms', () => {
    expectRefusalOf(() => detectOnsets(samples, SR, { delta: -1 }));
    expectRefusalOf(() => detectOnsets({ samples, sampleRate: SR, delta: -1 }));
  });

  it('accepts delta 0', () => {
    expect(detectOnsets(samples, SR, { delta: 0 })).toBeInstanceOf(Float32Array);
  });
});

describe('startBpm', () => {
  const samples = tone(1);

  it('refuses a non-positive startBpm on analyzeRhythm', () => {
    for (const startBpm of [-1, 0]) {
      expectRefusalOf(() => analyzeRhythm(samples, SR, { startBpm }));
      expectRefusalOf(() => analyzeRhythm({ samples, sampleRate: SR, startBpm }));
    }
  });

  it('refuses startBpm as an undeclared analyzeBpm key on both entry forms', () => {
    const options = { startBpm: 120 };
    expect(
      expectRefusalOf(() => analyzeBpm(samples, SR, options as never), TypeError).message,
    ).toMatch(unknownKeyText('analyzeBpm: unknown key startBpm'));
    const request = { samples, sampleRate: SR, ...options } as never;
    expect(expectRefusalOf(() => analyzeBpm(request), TypeError).message).toMatch(
      unknownKeyText('analyzeBpm: unknown key startBpm'),
    );
  });
});

describe('key detection hop under HPSS', () => {
  const samples = tone(0.5);

  it('refuses hopLength below 16 with useHpss on both key entries', () => {
    for (const hopLength of [1, 8, 15]) {
      expectRefusalOf(() => detectKey(samples, SR, { useHpss: true, hopLength }));
      expectRefusalOf(() => detectKey({ samples, sampleRate: SR, useHpss: true, hopLength }));
      expectRefusalOf(() => detectKeyCandidates(samples, SR, { useHpss: true, hopLength }));
    }
  });

  it('accepts hopLength 16 with useHpss and a small hop without it', () => {
    expect(detectKey(samples, SR, { useHpss: true, hopLength: 16 }).name).toBeTypeOf('string');
    expect(detectKeyCandidates(samples, SR, { hopLength: 8 }).length).toBeGreaterThan(0);
  });
});

describe('chromaCqt bins per octave', () => {
  it('refuses a bin count that is not a multiple of nChroma, as the C ABI does', () => {
    expectRefusalOf(() =>
      chromaCqt(tone(0.5), SR, { hopLength: 512, nChroma: 7, binsPerOctave: 36 }),
    );
  });
});

describe('spectralEdit op fields', () => {
  const samples = tone(0.5);
  const whole = { startSample: 0, endSample: samples.length };

  it('refuses an overflowing gain and a band edge the reader lets through', () => {
    expectRefusalOf(() =>
      spectralEdit(samples, SR, { ops: [{ ...whole, gainDb: 1000, mode: 'gain' }] }),
    );
  });

  it('returns finite audio for a band edge far past the Nyquist', () => {
    const out = spectralEdit(samples, SR, {
      ops: [{ ...whole, lowHz: 1e30, gainDb: 12, mode: 'gain' }],
    });
    expect(out.every(Number.isFinite)).toBe(true);
  });
});

describe('mel band edges', () => {
  const samples = tone(0.25);
  const nMels = 16;
  const nFrames = 4;
  const power = { data: new Float32Array(nMels * nFrames).fill(1), rows: nMels, cols: nFrames };
  const coefficients = { data: new Float32Array(13 * nFrames), rows: 13, cols: nFrames };

  it('refuses a negative fmin or fmax on the forward and inverse entries', () => {
    for (const [fminHz, fmaxHz] of [
      [-1000, 0],
      [0, -1],
    ]) {
      expectRefusalOf(() => melSpectrogram({ samples, sampleRate: SR, fminHz, fmaxHz }));
      expectRefusalOf(() => mfcc({ samples, sampleRate: SR, fminHz, fmaxHz }));
      expectRefusalOf(() => melToStft({ power, sampleRate: SR, nFft: 512, fminHz, fmaxHz }));
      expectRefusalOf(() =>
        melToAudio({ power, sampleRate: SR, nFft: 512, hopLength: 128, fminHz, fmaxHz, nIter: 1 }),
      );
      expectRefusalOf(() =>
        mfccToAudio({
          coefficients,
          nMels,
          sampleRate: SR,
          nFft: 512,
          hopLength: 128,
          fminHz,
          fmaxHz,
          nIter: 1,
        }),
      );
    }
  });

  it('refuses an fmax at or below fmin and an explicit fmax of 0, and takes the Nyquist when it is omitted', () => {
    expectRefusalOf(() => melSpectrogram({ samples, sampleRate: SR, fminHz: 4000, fmaxHz: 2000 }));
    expectRefusalOf(() =>
      melToStft({ power, sampleRate: SR, nFft: 512, fminHz: 4000, fmaxHz: 4000 }),
    );
    expect(melSpectrogram({ samples, sampleRate: SR, fminHz: 0 }).fmaxHz).toBe(SR / 2);
    expect(melToStft({ power, sampleRate: SR, nFft: 512, fminHz: 0 }).cols).toBe(nFrames);
    expectRefusalOf(() => melSpectrogram({ samples, sampleRate: SR, fmaxHz: 0 }));
    expectRefusalOf(() => mfcc({ samples, sampleRate: SR, fmaxHz: 0 }));
    expectRefusalOf(() => melToStft({ power, sampleRate: SR, nFft: 512, fmaxHz: 0 }));
  });
});

describe('acoustic analysis options', () => {
  const ir = new Float32Array(4800);
  for (let i = 0; i < ir.length; i++) {
    ir[i] = Math.exp(-i / 400) * (i % 2 === 0 ? 1 : -1);
  }

  it('refuses a negative band count and a non-positive decay window on both entries', () => {
    expectRefusalOf(() => analyzeImpulseResponse(ir, 48000, { nOctaveBands: -1 }));
    expectRefusalOf(() => analyzeImpulseResponse(ir, 48000, { nOctaveBands: 6, minDecayDb: 0 }));
    expectRefusalOf(() =>
      analyzeImpulseResponse({ samples: ir, sampleRate: 48000, minDecayDb: -5 }),
    );
    expectRefusalOf(() => detectAcoustic(ir, 48000, { nOctaveBands: -1 }));
    expectRefusalOf(() => detectAcoustic(ir, 48000, { nThirdOctaveSubbands: -1 }));
    expectRefusalOf(() => detectAcoustic(ir, 48000, { minDecayDb: 0 }));
    expectRefusalOf(() => detectAcoustic(ir, 48000, { noiseFloorMarginDb: -1 }));
  });

  it('accepts zero bands', () => {
    expect(analyzeImpulseResponse(ir, 48000, { nOctaveBands: 0 }).rt60Bands?.length).toBe(0);
  });
});

describe('CQT family and melody frequency bounds', () => {
  const samples = tone(0.5);
  const named = (key: string) =>
    expect.objectContaining({ name: 'RangeError', message: expect.stringContaining(key) });

  it('refuses a non-positive fmin naming it on every forward and inverse entry', () => {
    const magnitude = { data: new Float32Array(84 * 4).fill(0.1), rows: 84, cols: 4 };
    for (const fminHz of [0, -1]) {
      const opts = { hopLength: 512, fminHz };
      expect(() => cqt(samples, SR, opts)).toThrow(named('fminHz'));
      expect(() => pseudoCqt(samples, SR, opts)).toThrow(named('fminHz'));
      expect(() => hybridCqt(samples, SR, opts)).toThrow(named('fminHz'));
      expect(() => vqt(samples, SR, opts)).toThrow(named('fminHz'));
      expect(() =>
        cqtToAudio({
          magnitude,
          sampleRate: SR,
          hopLength: 512,
          fminHz,
          binsPerOctave: 12,
          nIter: 1,
        }),
      ).toThrow(named('fminHz'));
      expect(() =>
        vqtToAudio({
          magnitude,
          sampleRate: SR,
          hopLength: 512,
          fminHz,
          binsPerOctave: 12,
          gamma: 0,
          nIter: 1,
        }),
      ).toThrow(named('fminHz'));
    }
  });

  it('refuses a melody band whose fmax is not above fmin, naming it', () => {
    expect(() => analyzeMelody(samples, SR, { f0MinHz: 400, f0MaxHz: 200 })).toThrow(
      named('f0MaxHz'),
    );
    expect(() => analyzeMelody(samples, SR, { f0MinHz: 0 })).toThrow(named('f0MinHz'));
    expect(() => analyzeMelody(samples, SR, { threshold: 0 })).toThrow(named('threshold'));
  });
});
