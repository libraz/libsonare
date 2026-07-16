import { beforeAll, describe, expect, it } from 'vitest';
import {
  harmonic,
  hpss,
  init,
  mixStereo,
  normalize,
  pitchCorrectTimevarying,
  pitchCorrectToMidi,
  pitchCorrectToMidiTimevarying,
  pitchShift,
  spectralEdit,
  timeStretch,
  voiceChange,
  voiceChangeRealtime,
} from '../src/index';

const sampleRate = 22050;

function sine(length = 8192): Float32Array {
  const samples = new Float32Array(length);
  for (let i = 0; i < samples.length; i++) {
    samples[i] = 0.35 * Math.sin((2 * Math.PI * 220 * i) / sampleRate);
  }
  return samples;
}

beforeAll(async () => {
  await init();
});

describe('effects request-object compatibility (WASM)', () => {
  it('preserves positional results for basic transforms', () => {
    const samples = sine();
    expect(hpss({ samples, sampleRate, kernelHarmonic: 17, kernelPercussive: 17 })).toEqual(
      hpss(samples, sampleRate, 17, 17),
    );
    expect(harmonic({ samples, sampleRate })).toEqual(harmonic(samples, sampleRate));
    expect(timeStretch({ samples, sampleRate, rate: 1.1 })).toEqual(
      timeStretch(samples, sampleRate, 1.1),
    );
    expect(pitchShift({ samples, sampleRate, semitones: 2 })).toEqual(
      pitchShift(samples, sampleRate, 2),
    );
    expect(normalize({ samples, sampleRate, targetDb: -3 })).toEqual(
      normalize(samples, sampleRate, -3),
    );
  });

  it('preserves positional results for pitch and spectral editing requests', () => {
    const samples = sine();
    const f0Hz = new Float32Array(Math.ceil(samples.length / 512)).fill(220);
    expect(pitchCorrectToMidi({ samples, sampleRate, currentMidi: 57, targetMidi: 60 })).toEqual(
      pitchCorrectToMidi(samples, sampleRate, 57, 60),
    );
    expect(
      pitchCorrectToMidiTimevarying({ samples, f0Hz, targetMidi: 60, sampleRate, hopLength: 512 }),
    ).toEqual(pitchCorrectToMidiTimevarying(samples, f0Hz, 60, sampleRate, 512));
    expect(
      pitchCorrectTimevarying({ samples, f0Hz, sampleRate, hopLength: 512, mode: 'midi' }),
    ).toEqual(pitchCorrectTimevarying(samples, f0Hz, sampleRate, 512, { mode: 'midi' }));
    expect(spectralEdit({ samples, sampleRate, ops: [] })).toEqual(
      spectralEdit(samples, sampleRate, []),
    );
  });

  it('preserves positional results for one-shot voice-change requests', () => {
    const samples = sine(4096);
    expect(voiceChange({ samples, sampleRate, pitchSemitones: 2, formantFactor: 1.1 })).toEqual(
      voiceChange(samples, sampleRate, { pitchSemitones: 2, formantFactor: 1.1 }),
    );
    expect(
      voiceChangeRealtime({
        samples,
        sampleRate,
        preset: 'neutral-monitor',
        blockSize: 256,
      }),
    ).toEqual(voiceChangeRealtime(samples, sampleRate, 'neutral-monitor', { blockSize: 256 }));
  });

  it('preserves positional results for one-shot stereo mix requests', () => {
    const left = sine(1024);
    const right = sine(1024);
    expect(
      mixStereo({
        leftChannels: [left],
        rightChannels: [right],
        sampleRate,
        inputTrimDb: 1.5,
        faderDb: -2,
      }),
    ).toEqual(mixStereo([left], [right], sampleRate, { inputTrimDb: 1.5, faderDb: -2 }));
  });
});
