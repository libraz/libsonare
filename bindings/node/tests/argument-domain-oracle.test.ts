/**
 * Argument domains the C ABI defines and the Node surface must reproduce exactly:
 * refusing what the oracle refuses (as RangeError) and accepting what it accepts.
 */

import { describe, expect, it } from 'vitest';
import {
  analyzeBpm,
  detectChords,
  detectOnsets,
  Mixer,
  melSpectrogram,
  melToAudio,
  melToStft,
  mfcc,
  mfccToAudio,
  mixingScenePresetJson,
  spectralEdit,
} from '../src/index.js';
import { expectRefusal } from './_helpers.js';

const SR = 22050;

function tone(seconds: number, freq = 440): Float32Array {
  const out = new Float32Array(Math.floor(SR * seconds));
  for (let i = 0; i < out.length; i++) {
    out[i] = 0.5 * Math.sin((2 * Math.PI * freq * i) / SR);
  }
  return out;
}

describe('mel band edges', () => {
  const samples = tone(0.25);

  it('refuses a negative or non-finite fmin/fmax on melSpectrogram and mfcc', () => {
    for (const [fmin, fmax] of [
      [-1000, 0],
      [0, -1],
      [Number.NaN, 0],
      [0, Number.POSITIVE_INFINITY],
    ]) {
      expectRefusal(() => melSpectrogram({ samples, sampleRate: SR, fmin, fmax }), RangeError);
      expectRefusal(() => mfcc({ samples, sampleRate: SR, fmin, fmax }), RangeError);
    }
  });

  it('keeps 0 as the default spelling of both edges', () => {
    const mel = melSpectrogram({ samples, sampleRate: SR, fmin: 0, fmax: 0 });
    expect(mel.fmin).toBe(0);
    expect(mel.fmax).toBe(SR / 2);
    expect(mfcc({ samples, sampleRate: SR, fmin: 0, fmax: 0 }).nFrames).toBeGreaterThan(0);
  });
});

describe('inverse mel band edges', () => {
  const nMels = 16;
  const nFrames = 4;
  const power = new Float32Array(nMels * nFrames).fill(1);
  const coefficients = new Float32Array(13 * nFrames);

  it('refuses a negative or non-finite fmin/fmax on every inverse entry', () => {
    for (const [fmin, fmax] of [
      [-1000, 0],
      [0, -1],
      [Number.NaN, 0],
      [0, Number.POSITIVE_INFINITY],
    ]) {
      expectRefusal(() => melToStft(power, nMels, nFrames, SR, 512, fmin, fmax), RangeError);
      expectRefusal(
        () => melToAudio(power, nMels, nFrames, SR, 512, 128, fmin, fmax, 1),
        RangeError,
      );
      expectRefusal(
        () => mfccToAudio(coefficients, 13, nFrames, nMels, SR, 512, 128, fmin, fmax, 1),
        RangeError,
      );
    }
  });

  it('keeps 0 as the default spelling of both edges', () => {
    expect(melToStft(power, nMels, nFrames, SR, 512, 0, 0).nFrames).toBe(nFrames);
    expect(melToAudio(power, nMels, nFrames, SR, 512, 128, 0, 0, 1).length).toBeGreaterThan(0);
  });
});

describe('detectOnsets options', () => {
  const samples = tone(0.5);

  it('refuses a negative window or delta naming the key the caller wrote', () => {
    for (const key of [
      'preMax',
      'postMax',
      'preAvg',
      'postAvg',
      'wait',
      'backtrackRange',
      'delta',
    ]) {
      expect(() => detectOnsets({ samples, sampleRate: SR, [key]: -1 })).toThrow(
        expect.objectContaining({ name: 'RangeError', message: expect.stringContaining(key) }),
      );
    }
  });
});

describe('zero counts the C ABI accepts', () => {
  const samples = tone(1);

  it('analyzeBpm accepts maxCandidates 0 and returns no candidates', () => {
    expect(analyzeBpm({ samples, sampleRate: SR, maxCandidates: 0 }).candidates).toHaveLength(0);
  });

  it('detectChords accepts hmmBeamWidth 0', () => {
    expect(() =>
      detectChords({ samples, sampleRate: SR, useHmm: true, hmmBeamWidth: 0 }),
    ).not.toThrow();
  });

  it('still refuses the negatives and fractions the C ABI refuses', () => {
    expectRefusal(() => analyzeBpm({ samples, sampleRate: SR, maxCandidates: -1 }), RangeError);
    expectRefusal(() => analyzeBpm({ samples, sampleRate: SR, maxCandidates: 1.5 }), RangeError);
    expectRefusal(
      () => detectChords({ samples, sampleRate: SR, useHmm: true, hmmBeamWidth: -1 }),
      RangeError,
    );
  });
});

describe('spectralEdit op fields', () => {
  const samples = tone(0.5);
  const whole = { startSample: 0, endSample: samples.length };

  it('refuses a non-finite or overflowing gain and a non-finite band edge', () => {
    for (const op of [
      { ...whole, gainDb: Number.NaN, mode: 'gain' as const },
      { ...whole, gainDb: Number.POSITIVE_INFINITY, mode: 'gain' as const },
      { ...whole, gainDb: 1000, mode: 'gain' as const },
      { ...whole, lowHz: Number.NaN, mode: 'attenuate' as const },
      { ...whole, highHz: Number.NaN, mode: 'mute' as const },
    ]) {
      expectRefusal(() => spectralEdit(samples, SR, [op]), RangeError);
    }
  });

  it('returns finite audio for an in-range gain and a band edge past the Nyquist', () => {
    const out = spectralEdit(samples, SR, [
      { ...whole, lowHz: 1e30, gainDb: 12, mode: 'gain' },
      { ...whole, lowHz: 100, highHz: 2000, gainDb: 6, mode: 'gain' },
    ]);
    expect(out.every(Number.isFinite)).toBe(true);
  });
});

describe('Mixer send index', () => {
  function mixerWithTwoSends(): Mixer {
    const mixer = Mixer.fromSceneJson(mixingScenePresetJson('commentaryDucking'), 48000, 8);
    expect(mixer.addSend('host', 's0', 'master', -6, 'postFader')).toBe(0);
    expect(mixer.addSend('host', 's1', 'master', -6, 'postFader')).toBe(1);
    return mixer;
  }

  function sendIds(mixer: Mixer): string[] {
    const scene = JSON.parse(mixer.toSceneJson());
    const host = scene.strips.find((strip: { id: string }) => strip.id === 'host');
    return (host.sends ?? []).map((send: { id: string }) => send.id);
  }

  it('refuses an index outside the uint32 domain instead of wrapping it', () => {
    const mixer = mixerWithTwoSends();
    for (const index of [2 ** 32, 2 ** 32 + 1, -1, 0.5]) {
      expectRefusal(() => mixer.removeSend('host', index), RangeError);
      expectRefusal(() => mixer.setSendDb('host', index, -3), RangeError);
    }
    expect(sendIds(mixer)).toEqual(['s0', 's1']);
  });

  it('refuses a strip or send index outside the uint32 domain by its name', () => {
    const mixer = mixerWithTwoSends();
    for (const index of [-1, 2 ** 32, 0.5]) {
      expect(() => mixer.scheduleSendAutomation('host', index, 0, -3)).toThrow(
        expect.objectContaining({
          name: 'RangeError',
          message: expect.stringMatching(/sendIndex/),
        }),
      );
      expect(() => mixer.scheduleInsertAutomation(index, 0, 0, 0, 0)).toThrow(
        expect.objectContaining({
          name: 'RangeError',
          message: expect.stringMatching(/stripIndex/),
        }),
      );
      expect(() => mixer.setInputTrimDb(index, 0)).toThrow(
        expect.objectContaining({ name: 'RangeError', message: expect.stringMatching(/strip/) }),
      );
    }
    expect(sendIds(mixer)).toEqual(['s0', 's1']);
  });

  it('removes exactly the send the caller named', () => {
    const mixer = mixerWithTwoSends();
    mixer.removeSend('host', 1);
    expect(sendIds(mixer)).toEqual(['s0']);
  });
});
