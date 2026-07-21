import { describe, expect, it } from 'vitest';
import {
  harmonic,
  hpss,
  noteStretch,
  percussive,
  pitchCorrectTimevarying,
  pitchCorrectToMidi,
  pitchCorrectToMidiTimevarying,
  pitchShift,
  spectralEdit,
  timeStretch,
} from '../src/index.js';

const sampleRate = 22050;
const samples = new Float32Array(sampleRate / 2);
for (let i = 0; i < samples.length; i++) {
  samples[i] = Math.sin((2 * Math.PI * 440 * i) / sampleRate);
}

describe('effects transform request-object compatibility', () => {
  it('preserves HPSS and basic transform calls', () => {
    expect(hpss({ samples, sampleRate, kernelHarmonic: 17, kernelPercussive: 19 })).toEqual(
      hpss(samples, sampleRate, 17, 19),
    );
    expect(harmonic({ samples, sampleRate })).toEqual(harmonic(samples, sampleRate));
    expect(percussive({ samples, sampleRate })).toEqual(percussive(samples, sampleRate));
    expect(timeStretch({ samples, sampleRate, rate: 1.25 })).toEqual(
      timeStretch(samples, sampleRate, 1.25),
    );
    expect(pitchShift({ samples, sampleRate, semitones: 2 })).toEqual(
      pitchShift(samples, sampleRate, 2),
    );
  });

  it('preserves spectral edit options', () => {
    const ops = [{ lowHz: 2000, highHz: 6000, gainDb: -6, mode: 'attenuate' as const }];
    const options = { nFft: 1024, hopLength: 256, window: 'hann' as const };
    expect(spectralEdit({ samples, sampleRate, ops, ...options })).toEqual(
      spectralEdit(samples, sampleRate, ops, options),
    );
  });

  it('preserves pitch correction and note stretch options', () => {
    const hopLength = 512;
    const f0Hz = new Float32Array(Math.floor(samples.length / hopLength) + 1).fill(440);
    const voiced = new Int32Array(f0Hz.length).fill(1);
    const voicedProb = new Float32Array(f0Hz.length).fill(1);

    expect(pitchCorrectToMidi({ samples, sampleRate, currentMidi: 69, targetMidi: 71 })).toEqual(
      pitchCorrectToMidi(samples, sampleRate, 69, 71),
    );
    expect(
      pitchCorrectToMidiTimevarying({
        samples,
        sampleRate,
        f0Hz,
        targetMidi: 71,
        hopLength,
        voiced,
        voicedProb,
      }),
    ).toEqual(
      pitchCorrectToMidiTimevarying(samples, f0Hz, 71, sampleRate, hopLength, voiced, voicedProb),
    );
    const correction = { mode: 'scale' as const, scaleRoot: 0, retuneAmount: 0.5 };
    expect(
      pitchCorrectTimevarying({ samples, sampleRate, f0Hz, hopLength, ...correction }),
    ).toEqual(pitchCorrectTimevarying(samples, f0Hz, sampleRate, hopLength, correction));
    const stretch = { onsetSample: 0, offsetSample: samples.length, stretchRatio: 1.25 };
    expect(noteStretch({ samples, sampleRate, ...stretch })).toEqual(
      noteStretch(samples, sampleRate, stretch),
    );
  });

  // Both call shapes funnel through one private normalizer, so an invalid input
  // must fail identically either way — the positive-path equivalence above does
  // not prove the error path stays in lockstep.
  it('throws identically on invalid input in both call forms', () => {
    const empty = new Float32Array(0);
    const posShift = captureThrow(() => pitchShift(empty, sampleRate, 2));
    const reqShift = captureThrow(() => pitchShift({ samples: empty, sampleRate, semitones: 2 }));
    expect(posShift.threw).toBe(true);
    expect(reqShift.threw).toBe(true);
    expect(reqShift.message).toBe(posShift.message);

    const posStretch = captureThrow(() => timeStretch(empty, sampleRate, 1.25));
    const reqStretch = captureThrow(() => timeStretch({ samples: empty, sampleRate, rate: 1.25 }));
    expect(posStretch.threw).toBe(true);
    expect(reqStretch.threw).toBe(true);
    expect(reqStretch.message).toBe(posStretch.message);
  });
});

describe('spectralEdit requires an explicit sample rate', () => {
  const ops = [{ lowHz: 2000, highHz: 6000, gainDb: -6, mode: 'attenuate' as const }];

  it('throws when sampleRate is omitted instead of silently applying 22050', () => {
    // biome-ignore lint/suspicious/noExplicitAny: exercising the runtime guard with the required field omitted
    expect(captureThrow(() => spectralEdit({ samples, ops } as any)).threw).toBe(true);
    // biome-ignore lint/suspicious/noExplicitAny: positional form with an undefined sample rate
    expect(captureThrow(() => spectralEdit(samples, undefined as any, ops)).threw).toBe(true);
  });

  it('accepts an explicit sample rate', () => {
    expect(spectralEdit({ samples, sampleRate, ops })).toBeInstanceOf(Float32Array);
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
