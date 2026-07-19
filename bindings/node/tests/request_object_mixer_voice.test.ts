import { describe, expect, it } from 'vitest';
import { mixStereo, resample, voiceChange, voiceChangeRealtime } from '../src/index.js';

const sampleRate = 48000;
const samples = new Float32Array(2048);
for (let i = 0; i < samples.length; i += 1) {
  samples[i] = Math.sin((2 * Math.PI * 440 * i) / sampleRate) * 0.1;
}

describe('mixer and voice request objects', () => {
  it('keeps resample request and positional forms equivalent', () => {
    expect(resample({ samples, srcSr: sampleRate, targetSr: 44100 })).toEqual(
      resample(samples, sampleRate, 44100),
    );
  });

  it('keeps mixStereo request and positional forms equivalent', () => {
    const left = [samples];
    const right = [new Float32Array(samples.length)];
    const options = { inputTrimDb: 1, faderDb: -1 };
    expect(mixStereo({ leftChannels: left, rightChannels: right, sampleRate, ...options })).toEqual(
      mixStereo(left, right, sampleRate, options),
    );
  });

  it('keeps voiceChange request and positional forms equivalent', () => {
    const options = { pitchSemitones: 1.5, formantFactor: 1.1 };
    expect(voiceChange({ samples, sampleRate, ...options })).toEqual(
      voiceChange(samples, sampleRate, options),
    );
  });

  it('keeps realtime voice-change request and positional forms equivalent', () => {
    const options = { channels: 1 as const };
    expect(
      voiceChangeRealtime({ samples, sampleRate, preset: 'neutral-monitor', ...options }),
    ).toEqual(voiceChangeRealtime(samples, sampleRate, 'neutral-monitor', options));
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
    const reqResample = captureThrow(() => resample({ samples, srcSr: sampleRate, targetSr: 0 }));
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
