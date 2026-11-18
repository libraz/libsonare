import { describe, expect, it } from 'vitest';
import { mixStereo, resample, voiceChange } from '../src/index.js';

const sampleRate = 48000;
const samples = new Float32Array(2048);
for (let i = 0; i < samples.length; i += 1) {
  samples[i] = Math.sin((2 * Math.PI * 440 * i) / sampleRate) * 0.1;
}

describe('mixer and voice request objects', () => {
  it('keeps resample request and positional forms equivalent', () => {
    expect(resample({ samples, sampleRate, targetSampleRate: 44100 })).toEqual(
      resample(samples, sampleRate, 44100),
    );
  });

  it('takes mixStereo as a request only and refuses an extra positional argument', () => {
    const strips = [{ left: samples, right: new Float32Array(samples.length), faderDb: -1 }];
    const result = mixStereo({ strips, sampleRate });
    expect(result.left).toHaveLength(samples.length);
    expect(() => (mixStereo as (...args: unknown[]) => unknown)({ strips, sampleRate }, 1)).toThrow(
      TypeError,
    );
    expect(() =>
      (mixStereo as (...args: unknown[]) => unknown)([samples], [samples], sampleRate),
    ).toThrow(TypeError);
  });

  it('keeps voiceChange request and positional forms equivalent', () => {
    const options = { pitchSemitones: 1.5, formantFactor: 1.1 };
    expect(voiceChange({ samples, sampleRate, ...options })).toEqual(
      voiceChange(samples, sampleRate, options),
    );
    // Absolute mode, so a dropped formantMode field cannot pass.
    const absolute = { ...options, formantMode: 'absolute' as const };
    const positional = voiceChange(samples, sampleRate, absolute);
    expect(voiceChange({ samples, sampleRate, ...absolute })).toEqual(positional);
    expect(positional).not.toEqual(voiceChange(samples, sampleRate, options));
  });

  // Both call shapes funnel through one private normalizer, so an invalid input
  // must fail identically either way — the positive-path equivalence above does
  // not prove the error path stays in lockstep.
  it('throws identically on invalid input in both call forms', () => {
    const empty = new Float32Array(0);
    const posVoice = captureThrow(() => voiceChange(empty, sampleRate));
    const reqVoice = captureThrow(() => voiceChange({ samples: empty, sampleRate }));
    expect(posVoice.threw).toBe(true);
    expect(reqVoice.threw).toBe(true);
    expect(reqVoice.message).toBe(posVoice.message);

    const posResample = captureThrow(() => resample(samples, sampleRate, 0));
    const reqResample = captureThrow(() => resample({ samples, sampleRate, targetSampleRate: 0 }));
    expect(posResample.threw).toBe(true);
    expect(reqResample.threw).toBe(true);
    expect(reqResample.message).toBe(posResample.message);
  });
});

function captureThrow(fn: () => unknown): { threw: boolean; message: string } {
  try {
    fn();
    return { threw: false, message: '' };
  } catch (error) {
    return { threw: true, message: error instanceof Error ? error.message : String(error) };
  }
}
