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
});
