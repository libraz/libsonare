import { beforeAll, describe, expect, expectTypeOf, it } from 'vitest';
import { Audio, ErrorCode, init, SonareError, type SonareErrorOptions } from '../../dist/index.js';

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

describe('SonareError constructor', () => {
  it('derives codeName from (code, message, options)', () => {
    expectTypeOf<ConstructorParameters<typeof SonareError>>().toEqualTypeOf<
      [code: ErrorCode, message: string, options?: SonareErrorOptions]
    >();
    expectTypeOf<Extract<keyof SonareErrorOptions, 'codeName'>>().toEqualTypeOf<never>();
    const error = new SonareError(ErrorCode.InvalidParameter, 'bad input', { cause: 'test' });
    expect(error.codeName).toBe('InvalidParameter');
    expect(error.message).toBe('bad input');
    expect(error.cause).toBe('test');
  });
});

describe('Audio getters', () => {
  beforeAll(async () => {
    await init();
  });

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

describe('Audio browser decode fallback', () => {
  const notAudio = new Uint8Array([1, 2, 3, 4, 5, 6, 7, 8]);

  beforeAll(async () => {
    await init();
  });

  it('refuses with NotSupported when no decoding context exists, keeping the native error as cause', async () => {
    const caught = await Audio.fromMemoryWithBrowserFallback(notAudio, {
      createAudioContext: undefined,
    }).catch((error: unknown) => error);
    expect(caught).toBeInstanceOf(SonareError);
    expect((caught as SonareError).code).toBe(ErrorCode.NotSupported);
    expect((caught as Error).cause).toBeInstanceOf(Error);
  });

  it('reports a failed browser decode as DecodeFailed with the decoder error as cause', async () => {
    const decoderError = new Error('unsupported container');
    const context = {
      sampleRate,
      decodeAudioData: () => Promise.reject(decoderError),
    };
    const caught = await Audio.fromMemoryWithBrowserFallback(notAudio, {
      audioContext: context as never,
    }).catch((error: unknown) => error);
    expect(caught).toBeInstanceOf(SonareError);
    expect((caught as SonareError).code).toBe(ErrorCode.DecodeFailed);
    expect((caught as Error).cause).toBe(decoderError);
  });
});
