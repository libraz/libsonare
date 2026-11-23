import { describe, expect, expectTypeOf, it } from 'vitest';
import {
  type AudioEncodeOptions,
  capabilities,
  decodeChannels,
  type EncodeAudioRequest,
  ErrorCode,
  type ErrorReason,
  encodeAudio,
  isSonareError,
  Project,
  readAudioTags,
  SonareError,
} from '../src/index.js';

const sampleRate = 22050;
const channels = [
  new Float32Array(256).map((_, i) => 0.5 * Math.sin((2 * Math.PI * 440 * i) / sampleRate)),
  new Float32Array(256).map((_, i) => 0.3 * Math.cos((2 * Math.PI * 220 * i) / sampleRate)),
];
const tags = { title: 'codec round trip', artist: 'libsonare test' };

function thrownBy(call: () => unknown): unknown {
  try {
    call();
  } catch (error) {
    return error;
  }
  throw new Error('expected a refusal');
}

function expectEquivalentTypeError(
  requestCall: () => unknown,
  positionalCall: () => unknown,
  message: string,
): void {
  const requestError = thrownBy(requestCall);
  const positionalError = thrownBy(positionalCall);
  expect(requestError).toBeInstanceOf(TypeError);
  expect(positionalError).toBeInstanceOf(TypeError);
  expect(requestError).toMatchObject({ message });
  expect(positionalError).toMatchObject({ message });
  expect((requestError as Error).constructor).toBe((positionalError as Error).constructor);
}

describe('audio tags and in-memory encoding', () => {
  it('round trips WAV samples and tags, and normalizes both call forms', () => {
    const request = { channels, sampleRate, format: 'wav' as const, tags };
    const bytes = encodeAudio(request);
    const positional = encodeAudio(channels, sampleRate, { format: 'wav', tags });
    expect(Array.from(positional)).toEqual(Array.from(bytes));

    const decoded = decodeChannels(bytes);
    expect(decoded.sampleRate).toBe(sampleRate);
    expect(decoded.channels).toHaveLength(channels.length);
    for (let channel = 0; channel < channels.length; channel += 1) {
      const actual = decoded.channels[channel];
      for (let i = 0; i < channels[channel].length; i += 1) {
        expect(Math.abs(actual[i] - channels[channel][i])).toBeLessThan(1 / 32768 + 1e-6);
      }
    }

    expect(readAudioTags({ data: bytes })).toEqual(tags);
    expect(readAudioTags(bytes)).toEqual(tags);
  });

  it('requires an explicit format in both call forms', () => {
    expect(() => encodeAudio({ channels, sampleRate } as unknown as EncodeAudioRequest)).toThrow(
      /^encodeAudio: format is required$/,
    );
    const call = encodeAudio as unknown as (...args: unknown[]) => Uint8Array;
    expect(() => call(channels, sampleRate)).toThrow(/^encodeAudio: format is required$/);
    expect(() => call(channels, sampleRate, {})).toThrow(/^encodeAudio: format is required$/);
  });

  it('uses the same shape-first refusals for request and positional encoding', () => {
    const encode = encodeAudio as unknown as (...args: unknown[]) => Uint8Array;
    const cases: Array<[() => unknown, () => unknown, string]> = [
      [
        () => encode({ channels, sampleRate: 0 }),
        () => encode(channels, 0, {}),
        'encodeAudio: format is required',
      ],
      [
        () => encode({ sampleRate, format: 'wav' }),
        () => encode(undefined, sampleRate, { format: 'wav' }),
        'encodeAudio: channels is required',
      ],
      [
        () => encode({ channels, format: 'wav' }),
        () => encode(channels, undefined, { format: 'wav' }),
        'encodeAudio: sampleRate is required',
      ],
      [
        () => encode({ channels, sampleRate: 0, format: null }),
        () => encode(channels, 0, { format: null }),
        'encodeAudio: format is required',
      ],
      [
        () => encode(Object.assign(Object.create({ format: 'wav' }), { channels, sampleRate })),
        () => encode(channels, sampleRate, Object.create({ format: 'wav' })),
        'encodeAudio: format is required',
      ],
      [
        () => encode({ channels, sampleRate: 0, format: 7 }),
        () => encode(channels, 0, { format: 7 }),
        'encodeAudio: format must be a string',
      ],
      [
        () => encode({ channels: 5, sampleRate: 0, format: 'wav' }),
        () => encode(5, 0, { format: 'wav' }),
        'encodeAudio: channels must be an array of Float32Array',
      ],
      [
        () => encode({ channels: [5], sampleRate: 0, format: 'wav' }),
        () => encode([5], 0, { format: 'wav' }),
        'encodeAudio: channels[0] must be a Float32Array',
      ],
      [
        () => encode({ channels, sampleRate: 0, format: 'wav', bitDepth: '16' }),
        () => encode(channels, 0, { format: 'wav', bitDepth: '16' }),
        'encodeAudio: bitDepth must be an integer',
      ],
      [
        () => encode({ channels, sampleRate: 0, format: 'wav', quality: 'high' }),
        () => encode(channels, 0, { format: 'wav', quality: 'high' }),
        'encodeAudio: quality must be a number',
      ],
      [
        () => encode({ channels, sampleRate: 0, format: 'wav', tags: 'bad' }),
        () => encode(channels, 0, { format: 'wav', tags: 'bad' }),
        'encodeAudio: tags must be an object of string values',
      ],
      [
        () => encode({ channels, sampleRate: 0, format: 'wav', tags: { title: 7 } }),
        () => encode(channels, 0, { format: 'wav', tags: { title: 7 } }),
        'encodeAudio: tags.title must be a string',
      ],
      [
        () => encode({ channels, sampleRate: 'bad', format: 'wav' }),
        () => encode(channels, 'bad', { format: 'wav' }),
        'encodeAudio: sampleRate must be an integer',
      ],
    ];

    for (const [requestCall, positionalCall, message] of cases) {
      expectEquivalentTypeError(requestCall, positionalCall, message);
    }
  });

  it('names readAudioTags data consistently for both call forms', () => {
    const readTags = readAudioTags as unknown as (...args: unknown[]) => unknown;
    const wrongData = new Float32Array(4);
    expectEquivalentTypeError(
      () => readTags({ data: wrongData }),
      () => readTags(wrongData),
      'readAudioTags: data must be a Uint8Array',
    );
    const rawBuffer = new ArrayBuffer(8);
    expectEquivalentTypeError(
      () => readTags({ data: rawBuffer }),
      () => readTags(rawBuffer),
      'readAudioTags: data must be a Uint8Array',
    );
    if (typeof SharedArrayBuffer !== 'undefined') {
      const sharedBuffer = new SharedArrayBuffer(8);
      expectEquivalentTypeError(
        () => readTags({ data: sharedBuffer }),
        () => readTags(sharedBuffer),
        'readAudioTags: data must be a Uint8Array',
      );
    }
    expectEquivalentTypeError(
      () => readTags({}),
      () => readTags(undefined),
      'readAudioTags: data is required',
    );
    expectEquivalentTypeError(
      () => readTags({ data: null }),
      () => readTags(null),
      'readAudioTags: data is required',
    );
  });

  it('reports the named format, bit-depth, quality, sample-rate, and tag refusals', () => {
    expect(() => encodeAudio({ channels, sampleRate, format: 'WAV' as never })).toThrow(/format/);
    expect(() => encodeAudio({ channels, sampleRate, format: 'wav', bitDepth: 8 })).toThrow(
      /bitDepth/,
    );
    expect(() => encodeAudio({ channels, sampleRate, format: 'wav', bitDepth: 0 })).toThrow(
      /bitDepth/,
    );
    expect(() => encodeAudio({ channels, sampleRate, format: 'wav', quality: 0.5 })).toThrow(
      /quality/,
    );
    expect(() => encodeAudio({ channels, sampleRate, format: 'wav', quality: Number.NaN })).toThrow(
      /quality/,
    );
    expect(() =>
      encodeAudio({ channels, sampleRate, format: 'mp3', quality: 1 + Number.EPSILON }),
    ).toThrow(/quality/);
    expect(() =>
      encodeAudio({ channels, sampleRate, format: 'mp3', quality: -Number.MIN_VALUE }),
    ).toThrow(/quality/);
    expect(() => encodeAudio({ channels, sampleRate: 400000, format: 'wav' })).toThrow(
      /sampleRate/,
    );
    expect(() =>
      encodeAudio({ channels, sampleRate, format: 'wav', tags: { private_key: 'value' } }),
    ).toThrow(/tags\.private_key/);
    expect(() =>
      encodeAudio({ channels, sampleRate, format: 'wav', tags: { title: 7 } as never }),
    ).toThrow(/tags\.title/);
    expect(() => encodeAudio({ channels, sampleRate, format: 'wav\u0000' as never })).toThrow(
      /format/,
    );
    expect(() =>
      encodeAudio({ channels, sampleRate, format: 'wav', tags: { title: 'a\u0000b' } }),
    ).toThrow(/tags\.title/);
  });

  it('keeps coded format refusals and their reason on SonareError', () => {
    const supported = new Set([...capabilities().encode.builtin, ...capabilities().encode.ffmpeg]);
    const unavailable = ['flac', 'mp3', 'ogg', 'opus', 'm4a'].find(
      (format) => !supported.has(format),
    );
    if (unavailable !== undefined) {
      let caught: unknown;
      try {
        encodeAudio({ channels, sampleRate, format: unavailable as never });
      } catch (error) {
        caught = error;
      }
      expect(isSonareError(caught)).toBe(true);
      const error = caught as SonareError;
      expect(error.code).toBe(ErrorCode.NotSupported);
      expect(error.reason).toBe('notCompiledIn');
      expect(Object.hasOwn(error, 'reason')).toBe(true);
      expect(error.entry).toBe('encodeAudio');
    }
  });

  it('carries a deterministic core reason and the public entry on a coded error', () => {
    let caught: unknown;
    try {
      Project.fromJson('{');
    } catch (error) {
      caught = error;
    }
    expect(isSonareError(caught)).toBe(true);
    const error = caught as SonareError;
    expect(error.code).toBe(ErrorCode.InvalidFormat);
    expect(error.reason).toBe('malformedJson');
    expect(Object.hasOwn(error, 'reason')).toBe(true);
    expect(error.entry).toBe('Project.fromJson');
  });

  it('does not carry a native reason onto a later facade refusal', () => {
    let native: unknown;
    try {
      Project.fromJson('{');
    } catch (error) {
      native = error;
    }
    expect(native).toMatchObject({ reason: 'malformedJson' });

    let caught: unknown;
    try {
      encodeAudio({ channels, sampleRate, format: 'wav', quality: Number.NaN });
    } catch (error) {
      caught = error;
    }
    expect(caught).toBeInstanceOf(RangeError);
    expect(Object.hasOwn(caught as object, 'reason')).toBe(false);

    const direct = new SonareError(ErrorCode.Unknown, 'direct');
    expect(Object.hasOwn(direct, 'reason')).toBe(false);
    expect(Object.hasOwn(direct, 'entry')).toBe(false);
  });

  it('types reasons and validates request objects', () => {
    expectTypeOf<SonareError['reason']>().toEqualTypeOf<ErrorReason | undefined>();
    const options: AudioEncodeOptions = { format: 'wav', quality: 0.5 };
    expect(options.quality).toBe(0.5);
    expect(() => readAudioTags({ data: new Uint8Array(4), extra: true } as never)).toThrow(
      TypeError,
    );
    expect(() => readAudioTags({} as never)).toThrow(/^readAudioTags: data is required$/);
    expect(() => readAudioTags({ data: new Float32Array(4) } as never)).toThrow(
      /^readAudioTags: data must be a Uint8Array$/,
    );
    const readTags = readAudioTags as (...args: unknown[]) => unknown;
    expect(() => readTags(new Uint8Array(1), undefined)).toThrow(TypeError);
  });
});
