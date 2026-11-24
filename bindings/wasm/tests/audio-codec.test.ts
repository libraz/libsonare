import { beforeAll, describe, expect, it } from 'vitest';
import {
  capabilities,
  capabilityCatalog,
  decodeChannels,
  ErrorCode,
  encodeAudio,
  init,
  istft,
  readAudioTags,
  SonareError,
  stft,
} from '../dist/index.js';

const SAMPLE_RATE = 48000;
const LENGTH = 4096;

function samples(frequency: number): Float32Array {
  return Float32Array.from(
    { length: LENGTH },
    (_, index) => 0.4 * Math.sin((2 * Math.PI * frequency * index) / SAMPLE_RATE),
  );
}

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

describe('audio codec and spectral phase', () => {
  beforeAll(async () => {
    await init();
  });

  it('encodes audio, writes tags, and decodes the samples', () => {
    const channels = [samples(440), samples(660)];
    const tags = { title: 'Round trip', artist: 'libsonare' };
    const requestBytes = encodeAudio({ channels, sampleRate: SAMPLE_RATE, format: 'wav', tags });
    const positionalBytes = encodeAudio(channels, SAMPLE_RATE, { format: 'wav', tags });

    expect(positionalBytes).toEqual(requestBytes);
    expect(readAudioTags({ data: requestBytes })).toEqual(tags);
    expect(readAudioTags(requestBytes)).toEqual(tags);

    const decoded = decodeChannels(requestBytes);
    expect(decoded.sampleRate).toBe(SAMPLE_RATE);
    expect(decoded.channels).toHaveLength(2);
    for (let channel = 0; channel < channels.length; channel++) {
      expect(decoded.channels[channel]).toHaveLength(LENGTH);
      for (let index = 0; index < LENGTH; index++) {
        expect(Math.abs(decoded.channels[channel][index] - channels[channel][index])).toBeLessThan(
          2 / 32768,
        );
      }
    }
  });

  it('returns phase only when requested and publishes encoding capabilities', () => {
    const input = samples(440);
    const plain = stft({ samples: input, sampleRate: SAMPLE_RATE, nFft: 512, hopLength: 128 });
    const withPhase = stft({
      samples: input,
      sampleRate: SAMPLE_RATE,
      nFft: 512,
      hopLength: 128,
      phase: true,
    });

    expect(plain.phase).toBeUndefined();
    expect(withPhase.phase?.data).toHaveLength(withPhase.magnitude.data.length);
    expect(capabilities().encode).toEqual({ builtin: ['wav'], ffmpeg: [] });
    expect(capabilityCatalog().encode).toEqual({ builtin: ['wav'], ffmpeg: [] });
  });

  it('reconstructs a phased STFT and rejects inverse geometry by name', () => {
    const input = samples(440);
    const spectrum = stft({
      samples: input,
      sampleRate: SAMPLE_RATE,
      nFft: 512,
      hopLength: 128,
      phase: true,
    });
    const phase = spectrum.phase;
    if (!phase) {
      throw new Error('phase was not returned');
    }

    const reconstructed = istft({
      magnitude: spectrum.magnitude,
      phase,
      lengthSamples: input.length,
    });
    const scale = Math.max(...input.map((sample) => Math.abs(sample)));
    let maxError = 0;
    for (let index = 256; index < input.length - 256; index++) {
      maxError = Math.max(maxError, Math.abs(reconstructed[index] - input[index]));
    }
    expect(maxError).toBeLessThan(scale * 1e-4);

    for (const lengthSamples of [0, 500_000_001, 2 ** 31, 2 ** 32]) {
      expect(() => istft({ magnitude: spectrum.magnitude, phase, lengthSamples })).toThrow(
        /lengthSamples/,
      );
    }
    expect(() =>
      istft({
        magnitude: spectrum.magnitude,
        phase,
        nFft: 512,
        hopLength: 257,
      }),
    ).toThrow(/hopLength/);
    expect(() => istft({ magnitude: spectrum.magnitude, phase, nFft: 1024 })).toThrow(/nFft/);
    expect(() =>
      istft({
        magnitude: spectrum.magnitude,
        phase: {
          rows: phase.rows,
          cols: phase.cols - 1,
          data: phase.data.slice(0, phase.rows * (phase.cols - 1)),
        },
      }),
    ).toThrow(/same shape/);
    expect(() =>
      istft({
        magnitude: spectrum.magnitude,
        phase: { ...phase, data: phase.data.subarray(1) },
      }),
    ).toThrow(RangeError);
  });

  it('uses the encoder format and lowered float32 quality domain in both call forms', () => {
    const channels = [samples(220)];
    const supported = capabilities().encode.ffmpeg.includes('mp3');
    for (const encode of [
      (format: 'wav' | 'flac' | 'mp3', quality: number) =>
        encodeAudio({ channels, sampleRate: SAMPLE_RATE, format, quality }),
      (format: 'wav' | 'flac' | 'mp3', quality: number) =>
        encodeAudio(channels, SAMPLE_RATE, { format, quality }),
    ]) {
      for (const format of ['wav', 'flac'] as const) {
        expect(() => encode(format, 1.1)).toThrow(
          new RangeError('encodeAudio: quality is not supported by this format'),
        );
      }
      expect(() => encode('mp3', 1.01)).toThrow(
        new RangeError('encodeAudio: quality must be finite and in [0, 1]'),
      );
      const rounded = 1 + Number.EPSILON;
      expect(Math.fround(rounded)).toBe(1);
      if (supported) {
        const encoded = encode('mp3', rounded);
        expect(encoded.length).toBeGreaterThan(0);
        expect(encoded).toEqual(encode('mp3', 1));
        expect(encode('mp3', -Number.MIN_VALUE)).toEqual(encode('mp3', 0));
      } else {
        for (const quality of [rounded, -Number.MIN_VALUE]) {
          const error = thrownBy(() => encode('mp3', quality));
          expect(error).toBeInstanceOf(SonareError);
          expect(error).toMatchObject({ codeName: 'NotSupported', reason: 'notCompiledIn' });
        }
      }
    }
  });

  it('names encoder refusals and carries the unavailable-format reason', () => {
    const channels = [samples(220)];

    expect(() => encodeAudio(channels, SAMPLE_RATE, { format: 'wav', bitDepth: 0 })).toThrow(
      /bitDepth/,
    );
    expect(() =>
      Reflect.apply(encodeAudio, undefined, [
        channels,
        SAMPLE_RATE,
        { tags: { title: 'missing format' } },
      ]),
    ).toThrow(/format is required/);
    expect(() =>
      encodeAudio({ channels, sampleRate: SAMPLE_RATE, format: 'wav', quality: Number.NaN }),
    ).toThrow(/quality/);
    expect(() =>
      encodeAudio(channels, SAMPLE_RATE, { format: 'wav', tags: { custom: 'value' } }),
    ).toThrow(/tags\.custom/);
    const underscoredTags = Object.create(null) as Record<string, string>;
    underscoredTags.private_key = 'value';
    expect(() =>
      encodeAudio(channels, SAMPLE_RATE, { format: 'wav', tags: underscoredTags }),
    ).toThrow(/tags\.private_key/);
    expect(() =>
      Reflect.apply(encodeAudio, undefined, [{ channels, sampleRate: SAMPLE_RATE, format: 'WAV' }]),
    ).toThrow(/format/);
    expect(() =>
      Reflect.apply(encodeAudio, undefined, [
        { channels, sampleRate: SAMPLE_RATE, format: 'wav\u0000' },
      ]),
    ).toThrow(/format/);
    const nulKeyTags = Object.create(null) as Record<string, string>;
    nulKeyTags['private\u0000key'] = 'value';
    expect(() => encodeAudio(channels, SAMPLE_RATE, { format: 'wav', tags: nulKeyTags })).toThrow(
      /tags\./,
    );
    const nulValueTags = Object.create(null) as Record<string, string>;
    nulValueTags.title = 'bad\u0000value';
    expect(() => encodeAudio(channels, SAMPLE_RATE, { format: 'wav', tags: nulValueTags })).toThrow(
      /tags\.title/,
    );

    let error: unknown;
    try {
      encodeAudio({ channels, sampleRate: SAMPLE_RATE, format: 'flac' });
    } catch (caught) {
      error = caught;
    }
    expect(error).toBeInstanceOf(SonareError);
    expect(error).toMatchObject({
      code: ErrorCode.NotSupported,
      reason: 'notCompiledIn',
      entry: 'encodeAudio',
      message: 'encodeAudio: format is unavailable for encoding in this build',
    });
    expect(Object.hasOwn(error as object, 'reason')).toBe(true);
    expect(Object.hasOwn(error as object, 'entry')).toBe(true);

    let localError: unknown;
    try {
      encodeAudio(channels, SAMPLE_RATE, { format: 'wav', bitDepth: 0 });
    } catch (caught) {
      localError = caught;
    }
    expect(localError).toBeInstanceOf(RangeError);
    expect(Object.hasOwn(localError as object, 'reason')).toBe(false);
  });

  it('uses the same shape-first refusals for request and positional encoding', () => {
    const encode = encodeAudio as unknown as (...args: unknown[]) => Uint8Array;
    const channels = [samples(220)];
    const cases: Array<[() => unknown, () => unknown, string]> = [
      [
        () => encode({ channels, sampleRate: 0 }),
        () => encode(channels, 0, {}),
        'encodeAudio: format is required',
      ],
      [
        () => encode({ sampleRate: SAMPLE_RATE, format: 'wav' }),
        () => encode(undefined, SAMPLE_RATE, { format: 'wav' }),
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
        () =>
          encode(
            Object.assign(Object.create({ format: 'wav' }), {
              channels,
              sampleRate: SAMPLE_RATE,
            }),
          ),
        () => encode(channels, SAMPLE_RATE, Object.create({ format: 'wav' })),
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

  it('rejects unrecognized audio bytes', () => {
    let error: unknown;
    try {
      readAudioTags(new Uint8Array([0, 1, 2, 3]));
    } catch (caught) {
      error = caught;
    }
    expect(error).toMatchObject({ code: ErrorCode.InvalidFormat, entry: 'readAudioTags' });
  });

  it('carries native unavailable-format reasons from decoding and tag reads', () => {
    const flacHeader = new Uint8Array([0x66, 0x4c, 0x61, 0x43]);
    for (const [entry, operation, call] of [
      ['decodeChannels', 'decoding', () => decodeChannels(flacHeader)],
      ['readAudioTags', 'tag reading', () => readAudioTags(flacHeader)],
    ] as const) {
      let error: unknown;
      try {
        call();
      } catch (caught) {
        error = caught;
      }
      expect(error).toBeInstanceOf(SonareError);
      expect(error).toMatchObject({
        code: ErrorCode.NotSupported,
        reason: 'notCompiledIn',
        entry,
        message: `${entry}: data uses a format unavailable for ${operation} in this build`,
      });
    }
  });
});
