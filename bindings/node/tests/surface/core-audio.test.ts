import { describe, expect, it } from 'vitest';
import { Audio, ErrorCode, SonareError } from '../../dist/index.js';

const sampleRate = 48000;

function ramp(): Float32Array {
  const out = new Float32Array(sampleRate / 2);
  for (let i = 0; i < out.length; i++) {
    out[i] = (i % 100) / 200;
  }
  return out;
}

function expectInvalidState(read: () => unknown): void {
  let caught: unknown;
  try {
    read();
  } catch (error) {
    caught = error;
  }
  expect(caught).toBeInstanceOf(SonareError);
  expect((caught as SonareError).code).toBe(ErrorCode.InvalidState);
  expect((caught as SonareError).codeName).toBe('InvalidState');
}

describe('Audio.fromBuffer', () => {
  it("requires the sample rate of the caller's samples", () => {
    const fromBuffer = Audio.fromBuffer as (samples: Float32Array, sampleRate?: number) => Audio;
    expect(() => fromBuffer(new Float32Array(16))).toThrow(
      new TypeError('Audio.fromBuffer: sampleRate must be an integer'),
    );
  });
});

describe('Audio getters', () => {
  it('reads data, length, sampleRate and durationSec as getters', () => {
    const samples = ramp();
    const audio = Audio.fromBuffer(samples, sampleRate);
    expect(audio.length).toBe(samples.length);
    expect(audio.sampleRate).toBe(sampleRate);
    expect(audio.durationSec).toBeCloseTo(samples.length / sampleRate, 12);
    expect(Array.from(audio.data)).toEqual(Array.from(samples));
  });

  it('returns the same shared data array on every read', () => {
    const audio = Audio.fromBuffer(ramp(), sampleRate);
    expect(audio.data).toBe(audio.data);
  });

  it('has no duration or get* accessors', () => {
    const audio = Audio.fromBuffer(ramp(), sampleRate);
    for (const name of ['duration', 'getData', 'getLength', 'getSampleRate', 'getDuration']) {
      expect(name in audio).toBe(false);
    }
  });

  it('refuses every read after destroy with InvalidState', () => {
    const audio = Audio.fromBuffer(ramp(), sampleRate);
    audio.destroy();
    expectInvalidState(() => audio.data);
    expectInvalidState(() => audio.length);
    expectInvalidState(() => audio.sampleRate);
    expectInvalidState(() => audio.durationSec);
    expectInvalidState(() => audio.peakDb());
  });

  it('treats a second destroy and Symbol.dispose after destroy as no-ops', () => {
    const audio = Audio.fromBuffer(ramp(), sampleRate);
    audio.destroy();
    expect(() => audio.destroy()).not.toThrow();
    expect(() => audio[Symbol.dispose]()).not.toThrow();
  });

  it('releases the same way through Symbol.dispose', () => {
    const audio = Audio.fromBuffer(ramp(), sampleRate);
    audio[Symbol.dispose]();
    expectInvalidState(() => audio.length);
  });
});
