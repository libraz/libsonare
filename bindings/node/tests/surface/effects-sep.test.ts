import { describe, expect, expectTypeOf, it } from 'vitest';
import type { Matrix2D } from '../../dist/index.js';
import * as sonare from '../../dist/index.js';
import {
  decompose,
  decomposeStems,
  decomposeStemsLinked,
  estimateRoom,
  harmonic,
  hpss,
  hpssWithResidual,
  nnFilter,
  normalize,
  normalizeStereo,
  percussive,
  phaseVocoder,
  remix,
  remixAlignedIntervals,
  roomGeometryFromEstimate,
  roomMorph,
  roomMorphStereo,
  synthesizeRir,
  trim,
} from '../../dist/index.js';

// biome-ignore lint/suspicious/noExplicitAny: the table drives many differently typed entry points
type AnyFn = (...args: any[]) => unknown;

const sr = 22050;

function sine(seconds: number, hz = 440): Float32Array {
  const out = new Float32Array(Math.floor(sr * seconds));
  for (let i = 0; i < out.length; i++) {
    out[i] = 0.4 * Math.sin((2 * Math.PI * hz * i) / sr);
  }
  return out;
}

/** Exponentially decaying noise from a fixed generator: a stand-in for a room tail. */
function decayNoise(seconds: number): Float32Array {
  const out = new Float32Array(Math.floor(sr * seconds));
  let state = 12345;
  for (let i = 0; i < out.length; i++) {
    state = (state * 1103515245 + 12345) & 0x7fffffff;
    out[i] = ((state / 0x7fffffff) * 2 - 1) * Math.exp((-6.9 * i) / (0.3 * sr));
  }
  return out;
}

const samples = sine(0.5);
const room = { lengthM: 7, widthM: 5, heightM: 3, absorption: 0.3 };

function matrix(rows: number, cols: number): Matrix2D {
  const data = new Float32Array(rows * cols);
  for (let i = 0; i < data.length; i++) {
    data[i] = 0.1 + ((i * 7) % 11) / 11;
  }
  return { data, rows, cols };
}

function caught(fn: () => unknown): unknown {
  try {
    fn();
  } catch (error) {
    return error;
  }
  return undefined;
}

function maxAbsDiff(a: Float32Array, b: Float32Array): number {
  let worst = 0;
  for (let i = 0; i < a.length; i++) {
    worst = Math.max(worst, Math.abs((a[i] as number) - (b[i] as number)));
  }
  return worst;
}

describe('effects separation: sample-taking one-shots', () => {
  const noise = decayNoise(0.5);
  const sampleTaking: Array<[string, AnyFn, Float32Array, Record<string, unknown>]> = [
    ['hpss', hpss, samples, {}],
    ['harmonic', harmonic, samples, {}],
    ['percussive', percussive, samples, {}],
    ['hpssWithResidual', hpssWithResidual, samples, {}],
    ['decomposeStems', decomposeStems, samples, { nComponents: 2 }],
    ['trim', trim, samples, {}],
    ['normalize', normalize, samples, {}],
    ['phaseVocoder', phaseVocoder, samples, { speedRatio: 1.25 }],
    ['remix', remix, samples, { intervals: [100, 3000] }],
    ['remixAlignedIntervals', remixAlignedIntervals, samples, { intervals: [100, 3000] }],
    ['estimateRoom', estimateRoom, noise, {}],
    ['roomMorph', roomMorph, noise, room],
  ];

  it.each(sampleTaking)('%s requires sampleRate in both forms', (_name, fn, input, extra) => {
    expect(caught(() => fn(input, undefined, extra))).toBeInstanceOf(TypeError);
    expect(caught(() => fn({ samples: input, ...extra }))).toBeInstanceOf(TypeError);
  });

  it.each(sampleTaking)('%s refuses a non-integer sample rate', (_name, fn, input, extra) => {
    expect(caught(() => fn({ samples: input, sampleRate: 22050.5, ...extra }))).toBeInstanceOf(
      RangeError,
    );
    expect(caught(() => fn(input, 0, extra))).toBeInstanceOf(RangeError);
  });

  it.each(sampleTaking)('%s refuses the old positional tail', (_name, fn, input) => {
    expect(caught(() => fn(input, sr, 31, 31, 2048))).toBeInstanceOf(TypeError);
    expect(caught(() => fn(input, sr, true))).toBeInstanceOf(TypeError);
  });

  it.each(sampleTaking)(
    '%s gives the request and positional forms one result',
    (_name, fn, input, extra) => {
      const viaRequest = fn({ samples: input, sampleRate: sr, ...extra }) as object;
      const viaPositional = fn(input, sr, extra) as object;
      expect(viaPositional).toEqual(viaRequest);
    },
  );

  it.each(sampleTaking)(
    '%s refuses a request followed by a positional argument',
    (_name, fn, input, extra) => {
      expect(caught(() => fn({ samples: input, sampleRate: sr, ...extra }, sr))).toBeInstanceOf(
        TypeError,
      );
    },
  );

  it.each(sampleTaking)('%s refuses non-finite samples', (_name, fn, input, extra) => {
    const bad = Float32Array.from(input);
    bad[3] = Number.NaN;
    expect(caught(() => fn({ samples: bad, sampleRate: sr, ...extra }))).toBeInstanceOf(RangeError);
  });

  it.each(sampleTaking)('%s refuses validate and other unknown keys', (_name, fn, input, extra) => {
    expect(caught(() => fn(input, sr, { ...extra, validate: false }))).toBeInstanceOf(TypeError);
    expect(caught(() => fn(input, sr, { ...extra, notAnOption: 1 }))).toBeInstanceOf(TypeError);
  });

  it.each([
    ['normalizeStereo', normalizeStereo, {}],
    ['decomposeStemsLinked', decomposeStemsLinked, { nComponents: 2 }],
    ['roomMorphStereo', roomMorphStereo, room],
  ] as Array<[string, AnyFn, Record<string, unknown>]>)(
    '%s takes only a request and requires sampleRate',
    (_name, fn, extra) => {
      const noise = decayNoise(0.25);
      const pair = { left: noise, right: noise, channels: [noise, noise] };
      const own = _name === 'decomposeStemsLinked' ? { channels: pair.channels } : pair;
      const request = { ...own, ...extra };
      if (_name !== 'decomposeStemsLinked') {
        Reflect.deleteProperty(request, 'channels');
      }
      expect(caught(() => fn(request))).toBeInstanceOf(TypeError);
      expect(caught(() => fn({ ...request, sampleRate: 22050.5 }))).toBeInstanceOf(RangeError);
      expect(caught(() => fn(noise, sr))).toBeInstanceOf(TypeError);
      expect(caught(() => fn({ ...request, sampleRate: sr, validate: false }))).toBeInstanceOf(
        TypeError,
      );
    },
  );
});

describe('effects separation: removed overloads and entry points', () => {
  it('drops decomposeWithInit', () => {
    expect((sonare as Record<string, unknown>).decomposeWithInit).toBeUndefined();
  });

  it.each([
    ['decompose flat', () => (decompose as AnyFn)(new Float32Array(8), 2, 4, 2)],
    ['nnFilter flat', () => (nnFilter as AnyFn)(new Float32Array(8), 2, 4)],
    ['normalize without a rate', () => (normalize as AnyFn)(samples)],
    ['synthesizeRir without a request', () => (synthesizeRir as AnyFn)()],
    [
      'roomGeometryFromEstimate positional',
      () => (roomGeometryFromEstimate as AnyFn)({ lengthM: 7, widthM: 5, heightM: 3 }, {}),
    ],
    ['hpss hardMask positional', () => (hpss as AnyFn)(samples, sr, 31, 31, 2048, 512, true)],
    ['remix old order', () => (remix as AnyFn)(samples, [100, 3000], sr, true)],
  ])('refuses %s with TypeError', (_label, call) => {
    expect(caught(call)).toBeInstanceOf(TypeError);
  });
});

describe('effects separation: Matrix2D results and closed sets', () => {
  const s = matrix(4, 16);

  it('decompose takes a Matrix2D and returns w and h as Matrix2D', () => {
    const result = decompose({ s, nComponents: 2 });
    expect(result.w.rows).toBe(4);
    expect(result.w.cols).toBe(2);
    expect(result.h.rows).toBe(2);
    expect(result.h.cols).toBe(16);
    expect(result.w.data.length).toBe(8);
    expect(result.h.data.length).toBe(32);
  });

  it('decompose carries init and refuses the removed dimension fields', () => {
    expect(decompose({ s, nComponents: 2, init: 'nndsvd' }).w.rows).toBe(4);
    expect(caught(() => decompose({ s, nComponents: 2, nFeatures: 4 } as never))).toBeInstanceOf(
      TypeError,
    );
    expect(caught(() => decompose({ s, nComponents: 2, nFrames: 16 } as never))).toBeInstanceOf(
      TypeError,
    );
    expect(caught(() => decompose({ s, nComponents: 2, init: 'svd' } as never))).toBeInstanceOf(
      RangeError,
    );
    expect(caught(() => decompose({ s, nComponents: 2, init: 1 } as never))).toBeInstanceOf(
      TypeError,
    );
  });

  it('decompose requires its defining parameter and a Matrix2D', () => {
    expect(caught(() => decompose({ s } as never))).toBeInstanceOf(TypeError);
    expect(caught(() => decompose({ s: s.data, nComponents: 2 } as never))).toBeInstanceOf(
      TypeError,
    );
    expect(caught(() => decompose({ s, nComponents: 0 }))).toBeInstanceOf(RangeError);
  });

  it('decompose runs the core default iteration count when nIter is omitted and refuses a non-positive one', () => {
    const omitted = decompose({ s, nComponents: 2 });
    const explicit = decompose({ s, nComponents: 2, nIter: 50 });
    expect(omitted.w.data).toEqual(explicit.w.data);
    expect(omitted.h.data).toEqual(explicit.h.data);
    expect(caught(() => decompose({ s, nComponents: 2, nIter: 0 }))).toBeInstanceOf(RangeError);
    expect(caught(() => decompose({ s, nComponents: 2, nIter: -1 }))).toBeInstanceOf(RangeError);
  });

  it('nnFilter takes the core neighbour count and exclusion width when they are omitted', () => {
    expect(nnFilter({ s }).data).toEqual(nnFilter({ s, k: 7, width: 1 }).data);
  });

  it('nnFilter refuses a zero or negative k and a negative width', () => {
    expect(caught(() => nnFilter({ s, k: 0 }))).toBeInstanceOf(RangeError);
    expect(caught(() => nnFilter({ s, k: -1 }))).toBeInstanceOf(RangeError);
    expect(caught(() => nnFilter({ s, width: -1 }))).toBeInstanceOf(RangeError);
  });

  it('nnFilter takes and returns a Matrix2D of the input shape', () => {
    const out = nnFilter({ s });
    expect(out.rows).toBe(4);
    expect(out.cols).toBe(16);
    expect(out.data.length).toBe(64);
    expect(caught(() => nnFilter({ s, nFeatures: 4, nFrames: 16 } as never))).toBeInstanceOf(
      TypeError,
    );
  });

  it.each(['mean', 'median', 'min', 'max'] as const)(
    'nnFilter accepts aggregate %s',
    (aggregate) => {
      expect(nnFilter({ s, aggregate }).rows).toBe(4);
    },
  );

  it.each([
    ['average', RangeError],
    ['avg', RangeError],
    ['MEAN', RangeError],
    [0, TypeError],
    [1, TypeError],
  ] as const)('nnFilter refuses aggregate %j', (aggregate, errorClass) => {
    expect(caught(() => nnFilter({ s, aggregate } as never))).toBeInstanceOf(errorClass);
  });

  it('declares the result and request types', () => {
    expectTypeOf(decompose).returns.toEqualTypeOf<{ w: Matrix2D; h: Matrix2D }>();
    expectTypeOf(nnFilter).returns.toEqualTypeOf<Matrix2D>();
    expectTypeOf<Parameters<typeof nnFilter>[0]['aggregate']>().toEqualTypeOf<
      'mean' | 'median' | 'min' | 'max' | undefined
    >();
    expectTypeOf<Parameters<typeof decompose>[0]['s']>().toEqualTypeOf<Matrix2D>();
    expectTypeOf<ReturnType<typeof hpssWithResidual>>().toEqualTypeOf<{
      harmonic: Float32Array;
      percussive: Float32Array;
      residual: Float32Array;
      sampleRate: number;
    }>();
    const typeOnly = () => {
      // @ts-expect-error sampleRate is required on the request
      hpss({ samples });
      // @ts-expect-error sampleRate is required on the request
      normalize({ samples });
      // @ts-expect-error speedRatio is required
      phaseVocoder(samples, sr, {});
    };
    expect(typeOnly).toBeTypeOf('function');
  });
});

describe('effects separation: renamed and moved fields', () => {
  it('phaseVocoder takes speedRatio, in the request and in the options, and refuses rate', () => {
    const viaRequest = phaseVocoder({ samples, sampleRate: sr, speedRatio: 2 });
    const viaPositional = phaseVocoder(samples, sr, { speedRatio: 2 });
    expect(viaRequest.length).toBeLessThan(samples.length);
    expect(maxAbsDiff(viaRequest, viaPositional)).toBeLessThan(1e-6);
    expect(
      caught(() => phaseVocoder({ samples, sampleRate: sr, rate: 2 } as never)),
    ).toBeInstanceOf(TypeError);
    expect(caught(() => phaseVocoder({ samples, sampleRate: sr } as never))).toBeInstanceOf(
      TypeError,
    );
    expect(
      caught(() => phaseVocoder({ samples, sampleRate: sr, speedRatio: Number.NaN })),
    ).toBeInstanceOf(RangeError);
  });

  it('hpss and hpssWithResidual take hardMask inside the options', () => {
    const hard = hpss(samples, sr, { hardMask: true });
    const viaRequest = hpss({ samples, sampleRate: sr, hardMask: true });
    expect(maxAbsDiff(hard.harmonic, viaRequest.harmonic)).toBeLessThan(1e-6);
    const residual = hpssWithResidual(samples, sr, { hardMask: true }).residual;
    expect(residual.length).toBe(samples.length);
    expect(caught(() => hpss(samples, sr, { hardMask: 1 } as never))).toBeInstanceOf(TypeError);
  });

  it('derives an omitted hop as a quarter of nFft', () => {
    const common = { samples, sampleRate: sr, nFft: 512 };
    const calls: Array<[string, (hop?: number) => Float32Array]> = [
      ['hpss', (hop) => hpss({ ...common, hopLength: hop }).harmonic],
      ['hpssWithResidual', (hop) => hpssWithResidual({ ...common, hopLength: hop }).residual],
      ['timeStretch', (hop) => sonare.timeStretch({ ...common, speedRatio: 1.25, hopLength: hop })],
      ['pitchShift', (hop) => sonare.pitchShift({ ...common, pitchSemitones: 2, hopLength: hop })],
    ];
    for (const [name, call] of calls) {
      expect(Array.from(call()), name).toEqual(Array.from(call(128)));
      expect(Array.from(call()), name).not.toEqual(Array.from(call(256)));
    }
  });

  it('remix aligns cuts to zero crossings by default', () => {
    const intervals = [101, 3001];
    const byDefault = remix({ samples, sampleRate: sr, intervals });
    const explicit = remix({ samples, sampleRate: sr, intervals, alignZeros: true });
    const unaligned = remix({ samples, sampleRate: sr, intervals, alignZeros: false });
    expect(Array.from(byDefault)).toEqual(Array.from(explicit));
    expect(Array.from(byDefault)).not.toEqual(Array.from(unaligned));
    expect(Array.from(remixAlignedIntervals({ samples, sampleRate: sr, intervals }))).toEqual(
      Array.from(remixAlignedIntervals({ samples, sampleRate: sr, intervals, alignZeros: true })),
    );
  });

  it('remix requires its intervals', () => {
    expect(caught(() => remix({ samples, sampleRate: sr } as never))).toBeInstanceOf(TypeError);
  });

  it('trim takes thresholdDb and cuts the silent edges', () => {
    const padded = new Float32Array(samples.length + 16000);
    padded.set(samples, 8000);
    const out = trim(padded, sr, { thresholdDb: -40 });
    expect(out.length).toBeLessThan(padded.length);
    expect(maxAbsDiff(out, trim({ samples: padded, sampleRate: sr, thresholdDb: -40 }))).toBe(0);
    expect(caught(() => trim(padded, sr, { thresholdDb: Number.NaN }))).toBeInstanceOf(RangeError);
  });

  it('normalize reaches the target without a default rate', () => {
    const out = normalize(samples, sr, { targetDb: -6 });
    const peak = out.reduce((acc, v) => Math.max(acc, Math.abs(v)), 0);
    expect(peak).toBeCloseTo(10 ** (-6 / 20), 3);
    const stereo = normalizeStereo({ left: samples, right: samples, sampleRate: sr, mode: 'rms' });
    expect(stereo.left.length).toBe(samples.length);
    expect(caught(() => normalize(samples, sr, { mode: 0 } as never))).toBeInstanceOf(TypeError);
    expect(caught(() => normalize(samples, sr, { mode: 'loud' } as never))).toBeInstanceOf(
      RangeError,
    );
  });
});

describe('effects separation: room requests', () => {
  const noise = decayNoise(0.5);

  it('synthesizeRir takes source and listener placements and a maxSec', () => {
    const result = synthesizeRir({
      sampleRate: sr,
      ...room,
      source: { x: 2, y: 2, z: 1.5 },
      listener: { x: 4, y: 3, z: 1.5 },
      maxSec: 0.3,
    });
    expect(result.sampleRate).toBe(sr);
    expect(result.rir.length).toBeLessThanOrEqual(Math.ceil(0.3 * sr) + 1);
  });

  it.each([
    ['sourceX', { sourceX: 2 }],
    ['listenerZ', { listenerZ: 1 }],
    ['maxSeconds', { maxSeconds: 0.3 }],
    ['rirSynthOptions sampleRate omitted', {}],
  ])('synthesizeRir refuses %s', (label, extra) => {
    const request = label.endsWith('omitted') ? { ...room } : { sampleRate: sr, ...room, ...extra };
    expect(caught(() => synthesizeRir(request as never))).toBeInstanceOf(TypeError);
  });

  it.each([
    ['synthesizeRir', (extra: object) => synthesizeRir({ sampleRate: sr, ...room, ...extra })],
    ['roomMorph', (extra: object) => roomMorph(decayNoise(0.25), sr, { ...room, ...extra })],
    [
      'roomMorphStereo',
      (extra: object) =>
        roomMorphStereo({
          left: decayNoise(0.25),
          right: decayNoise(0.25),
          sampleRate: sr,
          ...room,
          ...extra,
        }),
    ],
  ] as Array<[string, (extra: object) => unknown]>)(
    '%s throws RangeError for a source outside the room',
    (_name, call) => {
      expect(caught(() => call({ source: { x: 99, y: 1, z: 1 } }))).toBeInstanceOf(RangeError);
    },
  );

  it.each(['mixingTimeMs', 'maxSec'])(
    'synthesizeRir refuses an explicit 0 for %s and accepts omission',
    (field) => {
      expect(caught(() => synthesizeRir({ sampleRate: sr, ...room, [field]: 0 }))).toBeInstanceOf(
        RangeError,
      );
      expect(() => synthesizeRir({ sampleRate: sr, ...room })).not.toThrow();
    },
  );

  it.each(['mixingTimeMs', 'maxSec'])('roomMorph refuses an explicit 0 for %s', (field) => {
    expect(caught(() => roomMorph(decayNoise(0.25), sr, { ...room, [field]: 0 }))).toBeInstanceOf(
      RangeError,
    );
  });

  it.each([
    'aspectHintLw',
    'aspectHintLh',
    'referenceAbsorption',
    'nOctaveBands',
    'minDecayDb',
    'noiseFloorMarginDb',
  ])('estimateRoom refuses an explicit 0 for %s', (field) => {
    expect(caught(() => estimateRoom(noise, sr, { [field]: 0 }))).toBeInstanceOf(RangeError);
  });

  it.each([
    ['seed', 1],
    ['crossfadeMs', 5],
    ['airTemperatureC', 20],
    ['airHumidityPercent', 50],
  ])(
    'synthesizeRir reads an explicit 0 for %s literally, apart from the omitted default',
    (field, dflt) => {
      const rirOf = (extra: object): number[] =>
        Array.from(synthesizeRir({ sampleRate: sr, ...room, maxSec: 0.3, ...extra }).rir);
      const omitted = rirOf({});
      expect(rirOf({ [field]: dflt })).toEqual(omitted);
      expect(rirOf({ [field]: 0 })).not.toEqual(omitted);
    },
  );

  it('estimateRoom names the impulse-response mode impulseResponse', () => {
    expect(() => estimateRoom(noise, sr, { mode: 'impulseResponse' })).not.toThrow();
    expect(
      caught(() => estimateRoom(noise, sr, { mode: 'impulse_response' } as never)),
    ).toBeInstanceOf(RangeError);
    expect(caught(() => estimateRoom(noise, sr, { mode: 2 } as never))).toBeInstanceOf(TypeError);
  });

  it('roomGeometryFromEstimate is request-only and returns placements', () => {
    const estimate = { lengthM: 7, widthM: 5, heightM: 3 };
    const geometry = roomGeometryFromEstimate({ estimate });
    expect(geometry.source).toEqual({
      x: expect.any(Number),
      y: expect.any(Number),
      z: expect.any(Number),
    });
    expect(geometry.listener).toBeDefined();
    const moved = roomGeometryFromEstimate({ estimate, source: { x: 1, y: 1, z: 1 } });
    expect(moved.source).toEqual({ x: 1, y: 1, z: 1 });
    expect(moved.listener).toEqual(geometry.listener);
    expect((geometry as Record<string, unknown>).sourceX).toBeUndefined();
  });
});
